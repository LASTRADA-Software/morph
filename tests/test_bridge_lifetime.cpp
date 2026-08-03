// SPDX-License-Identifier: Apache-2.0
//
// Regression tests for three Bridge memory-safety / robustness fixes:
//   FIX 2 — ~Bridge clears the active backend's reconnect handler and the
//           handler guards on the bridge's liveness token, so a reconnect fired
//           by a co-owned backend after the Bridge is destroyed is a safe no-op
//           rather than a use-after-free on the freed Bridge.
//   FIX 4 — executeVia's value-forwarding `.then` closure catches an exception
//           thrown while moving the result into the typed completion and routes
//           it to onError, instead of letting it escape the callback executor
//           (which would hang the completion or terminate the Qt loop).
//   FIX 5 — executeVia's `.then` closure checks the bridge's liveness token
//           BEFORE touching anything that reaches into the bridge (`onResult`
//           and `hasSubscribers()`), so a backend completion that resolves
//           after `~Bridge()` has run does not dereference the dangling
//           `Bridge`. Two test cases cover the two guarded call sites with
//           different strength: the `onResult` case is a deterministic,
//           sanitizer-free regression test (pre-fix, `onResult` ran with no
//           guard at all, so a flag set as its first statement differs
//           pre/post fix regardless of memory contents); the
//           `hasSubscribers()` case is a best-effort probe only -- see its
//           comment for why a sequential, single-threaded test structurally
//           cannot observe a behavioural difference there without a working
//           sanitizer.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>

#include "test_support.hpp"

namespace {

// ── FIX 2 fixtures ───────────────────────────────────────────────────────────

// A minimal fake backend that records the reconnect handler and lets the test
// fire it on demand, simulating a transport reconnect. It is co-owned via a
// shared_ptr in the test so it can outlive the Bridge.
class FakeReconnectBackend : public morph::backend::detail::IBackend {
public:
    morph::exec::detail::ModelId registerModel(
        const std::string&, std::function<std::unique_ptr<morph::model::detail::IModelHolder>()>) override {
        return morph::exec::detail::ModelId{++_nextId};
    }
    void deregisterModel(morph::exec::detail::ModelId) override {}
    morph::async::Completion<std::shared_ptr<void>> execute(morph::exec::detail::ModelId,
                                                            morph::backend::detail::ActionCall,
                                                            morph::exec::IExecutor*) override {
        return {};
    }
    void notifyBackendChanged() override {}
    void cancelPending(const std::exception_ptr&) override {}
    void setReconnectHandler(const std::function<void()>& handler) override { _handler = handler; }

    // Test hooks.
    void fireReconnect() {
        if (_handler) {
            _handler();
        }
    }
    [[nodiscard]] bool hasHandler() const { return static_cast<bool>(_handler); }
    // Snapshot the currently-installed handler so a test can invoke it AFTER the
    // Bridge is destroyed (models a reconnect already latched on the transport
    // thread). The snapshot still captures the now-dangling `this`, so it must
    // rely on the handler's internal liveness guard to be safe.
    [[nodiscard]] std::function<void()> snapshotHandler() const { return _handler; }

private:
    std::function<void()> _handler;
    uint64_t _nextId{0};
};

// A shim so the Bridge takes ownership of a unique_ptr while the test keeps a
// shared_ptr to the same object (making the backend co-owned / able to outlive
// the Bridge). The shim forwards every call to the shared target.
class BackendShim : public morph::backend::detail::IBackend {
public:
    explicit BackendShim(std::shared_ptr<FakeReconnectBackend> target) : _target{std::move(target)} {}
    morph::exec::detail::ModelId registerModel(
        const std::string& typeId,
        std::function<std::unique_ptr<morph::model::detail::IModelHolder>()> factory) override {
        return _target->registerModel(typeId, std::move(factory));
    }
    void deregisterModel(morph::exec::detail::ModelId mid) override { _target->deregisterModel(mid); }
    morph::async::Completion<std::shared_ptr<void>> execute(morph::exec::detail::ModelId mid,
                                                            morph::backend::detail::ActionCall call,
                                                            morph::exec::IExecutor* cbExec) override {
        return _target->execute(mid, std::move(call), cbExec);
    }
    void notifyBackendChanged() override { _target->notifyBackendChanged(); }
    void cancelPending(const std::exception_ptr& exc) override { _target->cancelPending(exc); }
    void setReconnectHandler(const std::function<void()>& handler) override { _target->setReconnectHandler(handler); }

private:
    std::shared_ptr<FakeReconnectBackend> _target;
};

// ── FIX 4 fixtures ───────────────────────────────────────────────────────────

// A result type whose move constructor throws. Default construction is cheap
// and does not throw, so the fake backend can build the result once; the throw
// only fires when executeVia's `.then` closure moves it into the typed state.
struct ThrowOnMove {
    ThrowOnMove() = default;  // NOLINT (used by model factory)
    ThrowOnMove(const ThrowOnMove&) = default;
    ThrowOnMove& operator=(const ThrowOnMove&) = default;
    // NOLINTNEXTLINE(performance-noexcept-move-constructor)
    ThrowOnMove(ThrowOnMove&&) { throw std::runtime_error("move threw during result forwarding"); }
    ThrowOnMove& operator=(ThrowOnMove&&) { throw std::runtime_error("move-assign threw"); }
};

struct ThrowAction {};
struct ThrowModel {
    ThrowOnMove execute(const ThrowAction&) const { return {}; }
};

// A backend that resolves the completion with a pre-built shared_ptr<ThrowOnMove>
// so the ONLY move of the value happens inside executeVia's `.then` forwarding.
class PrebuiltResultBackend : public morph::backend::detail::IBackend {
public:
    morph::exec::detail::ModelId registerModel(
        const std::string&, std::function<std::unique_ptr<morph::model::detail::IModelHolder>()>) override {
        return morph::exec::detail::ModelId{1};
    }
    void deregisterModel(morph::exec::detail::ModelId) override {}
    morph::async::Completion<std::shared_ptr<void>> execute(morph::exec::detail::ModelId,
                                                            morph::backend::detail::ActionCall,
                                                            morph::exec::IExecutor* cbExec) override {
        auto state = std::make_shared<morph::async::detail::CompletionState<std::shared_ptr<void>>>();
        morph::async::Completion<std::shared_ptr<void>> comp{state, cbExec};
        // Resolve with a shared_ptr holding a ThrowOnMove; setValue moves the
        // shared_ptr (cheap, cannot throw), never the ThrowOnMove itself.
        state->setValue(std::static_pointer_cast<void>(std::make_shared<ThrowOnMove>()));
        return comp;
    }
    void notifyBackendChanged() override {}
    void cancelPending(const std::exception_ptr&) override {}
};

// ── FIX 5 fixtures ───────────────────────────────────────────────────────────

// A plain, copy-constructible result -- unlike ThrowOnMove above, this must
// move/copy cleanly so the test isolates FIX 5's ordering bug (hasSubscribers()
// reading a dangling `this`) rather than FIX 4's move-exception path.
struct DeferredAction {};
struct DeferredModel {
    int execute(const DeferredAction&) const { return 0; }
};

// A backend whose execute() never resolves on its own: it hands back a fresh
// CompletionState and stashes it, so the test can destroy the Bridge while the
// completion is still pending and only resolve it afterward -- reproducing the
// exact race executeVia's liveness guard exists for.
class DeferredResultBackend : public morph::backend::detail::IBackend {
public:
    morph::exec::detail::ModelId registerModel(
        const std::string&, std::function<std::unique_ptr<morph::model::detail::IModelHolder>()>) override {
        return morph::exec::detail::ModelId{1};
    }
    void deregisterModel(morph::exec::detail::ModelId) override {}
    morph::async::Completion<std::shared_ptr<void>> execute(morph::exec::detail::ModelId,
                                                            morph::backend::detail::ActionCall,
                                                            morph::exec::IExecutor* cbExec) override {
        state = std::make_shared<morph::async::detail::CompletionState<std::shared_ptr<void>>>();
        return {state, cbExec};
    }
    void notifyBackendChanged() override {}
    void cancelPending(const std::exception_ptr&) override {}

    // Left un-resolved by execute(); the test resolves it directly once it
    // wants the completion to fire.
    std::shared_ptr<morph::async::detail::CompletionState<std::shared_ptr<void>>> state;
};

}  // namespace

template <>
struct morph::model::ModelTraits<DeferredModel> {
    static constexpr std::string_view typeId() { return "BL_DeferredModel"; }
};
template <>
struct morph::model::ActionTraits<DeferredAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "BL_DeferredAction"; }
    static std::string toJson(const DeferredAction&) { return "{}"; }
    [[maybe_unused]] static DeferredAction fromJson(std::string_view) { return {}; }
    static std::string resultToJson(const int&) { return "0"; }
    static int resultFromJson(std::string_view) { return 0; }
};

template <>
struct morph::model::ModelTraits<ThrowModel> {
    static constexpr std::string_view typeId() { return "BL_ThrowModel"; }
};
template <>
struct morph::model::ActionTraits<ThrowAction> {
    using Result = ThrowOnMove;
    static constexpr std::string_view typeId() { return "BL_ThrowAction"; }
    static std::string toJson(const ThrowAction&) { return "{}"; }
    [[maybe_unused]] static ThrowAction fromJson(std::string_view) { return {}; }
    static std::string resultToJson(const ThrowOnMove&) { return "{}"; }
    static ThrowOnMove resultFromJson(std::string_view) { return {}; }
};

// ── FIX 2 ────────────────────────────────────────────────────────────────────

// NOTE: the case name must not begin with a Catch2 test-spec operator (e.g. a
// leading '~'). catch_discover_tests passes each case name to the binary as a
// filter, and Catch2 reads a leading '~' as an *exclusion*, so a "~Bridge ..."
// case would run the whole suite minus itself in one process — masking the real
// result and, on some platforms, tripping latent cross-test crashes/hangs.
TEST_CASE("Bridge destructor clears the reconnect handler so a later reconnect is a safe no-op",
          "[bridge][lifetime]") {
    auto shared = std::make_shared<FakeReconnectBackend>();
    {
        auto bridge = std::make_unique<morph::bridge::Bridge>(std::make_unique<BackendShim>(shared));
        REQUIRE(shared->hasHandler());  // installed by the Bridge ctor
        bridge.reset();                 // ~Bridge runs here
    }
    // The Bridge is gone but the backend is still alive (co-owned by `shared`).
    // The destructor cleared the handler, so it is now empty; firing is a no-op.
    REQUIRE_FALSE(shared->hasHandler());
    shared->fireReconnect();  // must not crash / touch the freed Bridge
    SUCCEED("reconnect after ~Bridge did not touch the freed Bridge");
}

TEST_CASE("reconnect handler guards on the bridge liveness token", "[bridge][lifetime]") {
    // Simulate a reconnect already latched onto the transport thread: snapshot
    // the handler BEFORE destroying the Bridge, then invoke the snapshot AFTER.
    // The snapshot still captures the now-dangling Bridge `this`, so the ONLY
    // thing keeping this safe is the handler's internal liveness guard.
    auto shared = std::make_shared<FakeReconnectBackend>();
    std::function<void()> latched;
    {
        auto bridge = std::make_unique<morph::bridge::Bridge>(std::make_unique<BackendShim>(shared));
        latched = shared->snapshotHandler();  // the real installed handler, capturing `this`
        REQUIRE(static_cast<bool>(latched));
        bridge.reset();  // ~Bridge expires the liveness token (and clears the backend's copy)
    }
    // The latched handler's captured `this` is now dangling; the liveness guard
    // must short-circuit before it is dereferenced.
    latched();  // must not crash / touch the freed Bridge
    SUCCEED("latched reconnect after destruction was a no-op via the liveness guard");
}

// ── FIX 4 ────────────────────────────────────────────────────────────────────

TEST_CASE("executeVia routes a throwing result move to onError instead of hanging", "[bridge][completion]") {
    morph::bridge::Bridge bridge{std::make_unique<PrebuiltResultBackend>()};
    morph::testing::InlineExecutor cbExec;

    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = "BL_ThrowModel";
    binding->modelFactory = [] { return morph::model::detail::ModelFactory::create<ThrowModel>(); };
    bridge.registerHandler(binding);

    std::atomic<bool> okFired{false};
    std::atomic<bool> errFired{false};
    std::string errWhat;

    bridge.executeVia<ThrowModel, ThrowAction>(binding, ThrowAction{}, &cbExec)
        .then([&](ThrowOnMove) { okFired.store(true); })
        .onError([&](const std::exception_ptr& err) {
            errFired.store(true);
            try {
                std::rethrow_exception(err);
            } catch (const std::exception& exc) {
                errWhat = exc.what();
            }
        });

    // The typed completion must resolve — with the error, not the value, and it
    // must not hang (InlineExecutor runs everything synchronously above).
    REQUIRE(errFired.load());
    REQUIRE_FALSE(okFired.load());
    REQUIRE(errWhat.find("move threw") != std::string::npos);
}

// ── FIX 5 ────────────────────────────────────────────────────────────────────

TEST_CASE("Bridge: onResult does not run once the bridge is destroyed", "[bridge][lifetime]") {
    // Mirrors BridgeHandler::execute's ResultKeyed branch (bridge.hpp, the
    // `if constexpr (kShared && ResultKeyed<Action>)` case): a non-empty
    // `onResult` callback that calls back into the bridge through a captured
    // raw pointer to adopt a result-sourced primary key
    // (`bridgePtr->assignHandlerPrimary<Model>(...)`) -- the exact shape of
    // the second bridge-touching side effect FIX 5 guards, and the more
    // severe half of the original bug: pre-fix, `onResult` ran completely
    // unconditionally, with no liveness check at all.
    //
    // This half is fully deterministic to detect even without a sanitizer.
    // The lambda's first statement -- `onResultRan.store(true)` -- touches
    // only a local test variable, not the bridge, so it is always safely
    // observable if `onResult` is invoked at all, regardless of what the
    // subsequent (genuinely dangerous) bridge access does. Pre-fix, the
    // unconditional call means this flag always ends up `true`. Post-fix, the
    // `onResult && bridgeAlive` guard means `onResult` -- and therefore this
    // lambda -- never runs at all once the bridge is gone, so the flag stays
    // `false`. If the dangerous call after it crashes the process on a
    // pre-fix build (locking/copying the destroyed bridge's `_mtx`/`_backend`
    // members), Catch2's fatal-signal handling reports that as a failure too
    // -- either outcome (assertion failure or a crash) correctly fails this
    // test against the pre-fix ordering.
    auto backendOwner = std::make_unique<DeferredResultBackend>();
    auto* const backendPtr = backendOwner.get();
    morph::testing::InlineExecutor cbExec;

    // Heap-allocated (not stack-local via RAII scope exit) so the freed
    // memory actually goes back to the allocator instead of merely leaving a
    // stack frame whose bytes nothing has touched yet.
    auto* bridge = new morph::bridge::Bridge(std::move(backendOwner));

    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = "BL_DeferredModel";
    binding->modelFactory = [] { return morph::model::detail::ModelFactory::create<DeferredModel>(); };
    bridge->registerHandler(binding);

    std::atomic<bool> onResultRan{false};
    auto completion = bridge->executeVia<DeferredModel, DeferredAction>(
        binding, DeferredAction{}, &cbExec, [bridge, binding, &onResultRan](const int&) {
            onResultRan.store(true);
            bridge->assignHandlerPrimary<DeferredModel>(binding, "42");
        });
    (void)completion;

    auto pending = backendPtr->state;
    REQUIRE(pending);

    delete bridge;  // ~Bridge runs here; `bridge` is now a dangling pointer.
    bridge = nullptr;

    pending->setValue(std::static_pointer_cast<void>(std::make_shared<int>(42)));
    REQUIRE_FALSE(onResultRan.load());
}

TEST_CASE("Bridge: hasSubscribers is not read once the bridge is destroyed (best-effort probe)",
          "[bridge][lifetime]") {
    // Unlike the onResult case above, this half of FIX 5 cannot be turned
    // into a deterministic plain-build regression test. `hasSubscribers()`
    // only reads a trivially-destructible `std::atomic<size_t>`, and -- once
    // the bridge really is fully destroyed before the completion resolves --
    // the surviving `!alive.expired()` half of the old
    // `hasSubscribers() && !alive.expired()` condition is `false` regardless
    // of which operand is evaluated first. So the *observable branch
    // outcome* (whether `publishResult` runs) is identical pre- and post-fix
    // in a sequential, single-threaded destroy-then-resolve test like this
    // one: no postcondition assertion can tell the two orderings apart here.
    // The actual bug this guard exists for is either (a) the read of freed
    // memory being itself undefined behaviour -- detectable only by a
    // memory-safety tool such as ASan -- or (b) a genuine data race where the
    // completion's callback runs concurrently with `~Bridge()` on another
    // thread, which a sequential test cannot reproduce at all (that needs
    // TSan plus real concurrency).
    //
    // We investigated using a sanitizer here: both the whole `morph_tests`
    // binary and this file's tests in isolation hang indefinitely under this
    // machine's AppleClang ASan+UBSan combination (confirmed against
    // unrelated, already-passing tests too -- see the task report), so that
    // route is not available in this environment. This test is therefore a
    // best-effort probe, not a regression guard: heap-allocating the bridge
    // and aggressively overwriting freed memory with a recognizable,
    // non-zero pattern raises -- but does not guarantee -- the odds that a
    // pre-fix read of the dangling `this` behaves observably differently
    // (e.g. a crash while walking a corrupted `_subscriptions` vector, if
    // `hasSubscribers()` happens to read back a nonzero count from reused
    // memory). Confirmed empirically that it does NOT reliably fail against
    // the pre-fix ordering (see the report); do not read this test as proof
    // of coverage the way the onResult test above is.
    auto backendOwner = std::make_unique<DeferredResultBackend>();
    auto* const backendPtr = backendOwner.get();
    morph::testing::InlineExecutor cbExec;

    auto* bridge = new morph::bridge::Bridge(std::move(backendOwner));
    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = "BL_DeferredModel";
    binding->modelFactory = [] { return morph::model::detail::ModelFactory::create<DeferredModel>(); };
    bridge->registerHandler(binding);

    // No onResult here -- this case isolates the hasSubscribers()/
    // publishResult side effect from the onResult side effect covered above.
    auto completion = bridge->executeVia<DeferredModel, DeferredAction>(binding, DeferredAction{}, &cbExec);
    (void)completion;

    auto pending = backendPtr->state;
    REQUIRE(pending);

    delete bridge;  // ~Bridge runs here; `bridge` is now a dangling pointer.
    bridge = nullptr;

    // Raise the odds the allocator hands the freed Bridge's memory back for
    // one of these same-size allocations, filled with a distinct, non-zero
    // byte pattern rather than bytes nothing has ever written to.
    constexpr std::size_t kBridgeSize = sizeof(morph::bridge::Bridge);
    constexpr int kFillAttempts = 16;
    for (int i = 0; i < kFillAttempts; ++i) {
        auto* const filler = static_cast<unsigned char*>(::operator new(kBridgeSize));
        std::memset(filler, 0xAA, kBridgeSize);
        // Deliberately leaked for the rest of the test process -- freeing it
        // immediately would just hand the same memory straight back on the
        // next iteration, defeating the point of trying several attempts.
    }

    // Must not crash even if hasSubscribers() does read back the freed,
    // now-scribbled memory. See the comment above for why this is the
    // strongest available check without a working sanitizer.
    pending->setValue(std::static_pointer_cast<void>(std::make_shared<int>(42)));
    SUCCEED("resolving after destruction did not crash (best-effort probe; see comment above)");
}

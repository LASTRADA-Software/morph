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
//           `Bridge`.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <exception>
#include <functional>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
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

TEST_CASE("Bridge: a completion resolving after the bridge is destroyed does not touch the dead bridge",
          "[bridge][lifetime]") {
    auto backendOwner = std::make_unique<DeferredResultBackend>();
    auto* const backendPtr = backendOwner.get();
    morph::testing::InlineExecutor cbExec;

    std::shared_ptr<morph::async::detail::CompletionState<std::shared_ptr<void>>> pending;
    {
        morph::bridge::Bridge bridge{std::move(backendOwner)};

        auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
        binding->typeId = "BL_DeferredModel";
        binding->modelFactory = [] { return morph::model::detail::ModelFactory::create<DeferredModel>(); };
        bridge.registerHandler(binding);

        // Kicks off executeVia's `.then` chain against `bridge`, but the fake
        // backend never resolves it -- the completion (and the `this`-capturing
        // closure attached to it) is still pending when `bridge` goes out of
        // scope below.
        auto completion = bridge.executeVia<DeferredModel, DeferredAction>(binding, DeferredAction{}, &cbExec);
        (void)completion;

        pending = backendPtr->state;
        REQUIRE(pending);
        // ~Bridge runs here, with the completion still unresolved.
    }
    // The Bridge above is already destroyed. Resolving the backend's completion
    // now must not crash or touch freed memory -- this is exactly the
    // "completion resolves after the Bridge is gone" race executeVia's liveness
    // guard exists for. Catch2 reports a crash (e.g. under ASan) as a test
    // failure/signal, so no further assertion is needed beyond reaching SUCCEED.
    pending->setValue(std::static_pointer_cast<void>(std::make_shared<int>(42)));
    SUCCEED("resolving a completion after Bridge destruction did not touch the dead bridge");
}

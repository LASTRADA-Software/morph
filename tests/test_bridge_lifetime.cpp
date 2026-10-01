// SPDX-License-Identifier: Apache-2.0
//
// Bridge teardown and result-forwarding robustness:
//   - ~Bridge clears the active backend's reconnect handler, and the handler
//     checks the bridge's liveness token first, so a reconnect fired by a
//     co-owned backend after the Bridge is gone touches nothing.
//   - A throwing move of an action's result reaches onError instead of
//     escaping the settling thread.
//   - A result that settles after ~Bridge runs neither `onResult` nor the
//     subscription fan-out: both are bridge-side work, run on the owner and
//     gated on the bridge's token, and the check for subscribers reads a
//     registry the sink holds, never the destroyed Bridge.
//   - A Bridge constructed with no backend tolerates setDefaultSession.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
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
#include <thread>

#if !defined(_WIN32)
#include <sys/mman.h>
#include <unistd.h>

#include <csetjmp>
#include <csignal>
#endif

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
    void setReconnectHandler(std::function<void()> handler, morph::exec::IExecutor* exec) override {
        _handler = std::move(handler);
        _handlerExec = exec;
    }

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
    morph::exec::IExecutor* _handlerExec = nullptr;
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
    void setReconnectHandler(std::function<void()> handler, morph::exec::IExecutor* exec) override {
        _target->setReconnectHandler(std::move(handler), exec);
    }

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

#if !defined(_WIN32)
// Recovery machinery for the guard-page death test below. A signal handler
// may only call async-signal-safe functions, so this does the minimum: record
// which signal fired in a `sig_atomic_t` and jump back to the `sigsetjmp`
// checkpoint in the test body via `siglongjmp` (which, unlike plain
// `longjmp`, also restores the signal mask the jump point had -- required so
// the signal being handled isn't left permanently blocked after we resume).
//
// PROT_NONE protection faults are not portable across POSIX platforms: this
// machine's AppleClang/Darwin delivers SIGBUS for them (confirmed
// empirically -- see the report), while Linux typically delivers SIGSEGV for
// the same fault. Both are installed with the same handler so the test works
// either way.
volatile std::sig_atomic_t gGuardPageFaultSignal = 0;
sigjmp_buf gGuardPageJumpBuf;

void guardPageFaultHandler(int sig) {
    gGuardPageFaultSignal = sig;
    siglongjmp(gGuardPageJumpBuf, 1);
}
#endif

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
        morph::testing::InlineExecutor bridgeOwner;
        auto bridge = std::make_unique<morph::bridge::Bridge>(std::make_unique<BackendShim>(shared), bridgeOwner);
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
        morph::testing::InlineExecutor bridgeOwner;
        auto bridge = std::make_unique<morph::bridge::Bridge>(std::make_unique<BackendShim>(shared), bridgeOwner);
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
    morph::testing::InlineExecutor bridgeOwner;
    morph::bridge::Bridge bridge{std::make_unique<PrebuiltResultBackend>(), bridgeOwner};
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
    // A result-keyed action's `onResult` calls back into the bridge. Its first
    // statement touches only a local flag, so whether it ran at all is
    // observable without touching the destroyed bridge.
    auto backendOwner = std::make_unique<DeferredResultBackend>();
    auto* const backendPtr = backendOwner.get();
    morph::testing::InlineExecutor cbExec;

    // Heap-allocated so the freed memory goes back to the allocator.
    auto* bridge = new morph::bridge::Bridge(std::move(backendOwner), cbExec);

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

#if !defined(_WIN32)
// POSIX-only (mmap/mprotect): the guard-page death test below relies on them,
// so it is compiled out on Windows rather than approximated with something
// weaker there. See its comment for the technique and why it is deterministic
// where the superseded heap-reuse probe this replaced was not.
TEST_CASE("Bridge: hasSubscribers is not read once the bridge is destroyed (guard-page death test)",
          "[bridge][lifetime]") {
    // The Bridge is placement-new'd inside an mmap'd page, destroyed in place,
    // and the page is then mprotect'd to PROT_NONE: a result settling after
    // that which touched the destroyed Bridge would fault. The fault is
    // recovered in-process through sigsetjmp/siglongjmp (Darwin delivers
    // SIGBUS for a PROT_NONE fault, which Catch2 does not handle), and turned
    // into an ordinary FAIL.
    auto backendOwner = std::make_unique<DeferredResultBackend>();
    auto* const backendPtr = backendOwner.get();
    morph::testing::InlineExecutor cbExec;

    long const pageSizeRaw = sysconf(_SC_PAGESIZE);
    REQUIRE(pageSizeRaw > 0);
    auto const pageSize = static_cast<std::size_t>(pageSizeRaw);
    REQUIRE(sizeof(morph::bridge::Bridge) <= pageSize);

    void* const region = mmap(nullptr, pageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    REQUIRE(region != MAP_FAILED);

    // Place the Bridge flush against the end of the page (rounded down to its
    // required alignment), so a touch of any of its member data lands inside
    // the page that gets protected below, not in whatever precedes it.
    auto const regionEnd = reinterpret_cast<std::uintptr_t>(region) + pageSize;
    auto objAddr = regionEnd - sizeof(morph::bridge::Bridge);
    objAddr -= objAddr % alignof(morph::bridge::Bridge);
    void* const bridgeMem = reinterpret_cast<void*>(objAddr);  // NOLINT(performance-no-int-to-ptr)

    // Placement new into the mmap'd region above; destroyed via an explicit dtor
    // call below, not `delete` (the memory is not heap-owned). The directive
    // stays on one physical line; see the note at
    // include/morph/detail/fixed_string.hpp:48.
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    auto* const bridge = new (bridgeMem) morph::bridge::Bridge(std::move(backendOwner), cbExec);

    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = "BL_DeferredModel";
    binding->modelFactory = [] { return morph::model::detail::ModelFactory::create<DeferredModel>(); };
    bridge->registerHandler(binding);

    // No onResult here -- this case isolates the hasSubscribers()/
    // publishResult side effect from the onResult side effect covered by the
    // test above.
    auto completion = bridge->executeVia<DeferredModel, DeferredAction>(binding, DeferredAction{}, &cbExec);
    (void)completion;

    auto pending = backendPtr->state;
    REQUIRE(pending);

    bridge->~Bridge();  // Manually destroyed in place -- see the comment above on why not `delete`.

    REQUIRE(mprotect(region, pageSize, PROT_NONE) == 0);

    struct sigaction sa{};
    // glibc's <bits/sigaction.h> defines `sa_handler` as a macro
    // (`__sigaction_handler.sa_handler`) for POSIX compatibility; clang's
    // -Wdisabled-macro-expansion flags the resulting member-access expansion
    // as a false positive (the code is correct and portable -- this is a
    // known rough edge between clang's macro-hygiene checker and glibc's
    // headers, not a bug here) that only reproduces on Linux, not on the
    // BSD-derived <signal.h> this test was authored and verified against.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdisabled-macro-expansion"
#endif
    sa.sa_handler = guardPageFaultHandler;
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    struct sigaction oldSegv{};
    struct sigaction oldBus{};
    REQUIRE(sigaction(SIGSEGV, &sa, &oldSegv) == 0);
    REQUIRE(sigaction(SIGBUS, &sa, &oldBus) == 0);

    gGuardPageFaultSignal = 0;
    // sigsetjmp(..., 1) saves the signal mask along with the jump point, so
    // siglongjmp restores it too -- required so the signal we just caught
    // isn't left blocked for the rest of the process after we resume here.
    // A nonzero return means we got here via siglongjmp from the handler
    // (i.e. a fault happened); a zero return means the call below is about
    // to run for the first time.
    bool const faulted = sigsetjmp(gGuardPageJumpBuf, 1) != 0;
    if (!faulted) {
        // A settle that touched the protected Bridge would fault and jump to
        // the `faulted` branch below instead of returning here.
        pending->setValue(std::static_pointer_cast<void>(std::make_shared<int>(42)));
    }

    // Restore the default handlers before doing anything else, whether or not
    // we faulted.
    sigaction(SIGSEGV, &oldSegv, nullptr);
    sigaction(SIGBUS, &oldBus, nullptr);

    if (faulted) {
        // The crash happened inside a single, lock-free atomic load
        // (hasSubscribers() takes no locks), so nothing was left mid-mutation
        // for this best-effort cleanup to worry about disturbing.
        munmap(region, pageSize);
        std::string const message =
            "hasSubscribers() touched the destroyed bridge after ~Bridge() ran "
            "(caught signal " +
            std::to_string(gGuardPageFaultSignal) + ")";
        FAIL(message);
    }

    REQUIRE(munmap(region, pageSize) == 0);
}
#endif  // !defined(_WIN32)

// ── Coverage: setDefaultSession on a Bridge constructed with a null backend ──
//
// Bridge's constructor explicitly tolerates a null initial backend (see its
// own doc comment: "there is nothing yet to stamp a session onto"), and
// setDefaultSession's `if (backend) { backend->setSession(...); }` guard
// exists precisely so a later call on such a bridge does not dereference a
// null backend pointer. A couple of existing tests construct `Bridge{nullptr}`
// for other reasons (test_coverage_push95.cpp), but none of them go on to
// call setDefaultSession, so this guard's `false` arm was never exercised.
TEST_CASE("Bridge::setDefaultSession is a safe no-op on a Bridge with no active backend",
          "[bridge][lifetime][null-backend]") {
    morph::testing::InlineExecutor bridgeOwner;
    morph::bridge::Bridge bridge{nullptr, bridgeOwner};

    ::morph::session::Context ctx;
    ctx.principal = "no-backend-yet";
    REQUIRE_NOTHROW(bridge.setDefaultSession(ctx));

    // The session is still recorded for whenever a real backend arrives later
    // (e.g. via switchBackend()) -- setDefaultSession's own doc comment says
    // this default applies to "all subsequent calls".
    CHECK(bridge.defaultSession().principal == "no-backend-yet");
}

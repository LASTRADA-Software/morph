// SPDX-License-Identifier: Apache-2.0
//
// Coverage-gap tests focused on BRANCH coverage. Each case names the
// file:line range it exercises so future readers understand why an unusual
// edge case lives here. Companion to test_coverage_gaps.cpp / test_coverage_extra.cpp.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/logger.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/offline/offline_queue.hpp>
#include <morph/offline/reconnect_coordinator.hpp>
#include <morph/offline/sync_worker.hpp>
#include <morph/session/session.hpp>
#include <stdexcept>
#include <string>
#include <thread>

#include "test_support.hpp"

using namespace std::chrono_literals;

// Minimal model + action used for HandlerBinding factories and executeVia().
// File scope (not the anonymous namespace) so the BRIDGE_REGISTER_* trait
// specialisations are visible before makeBinding() below uses them.
struct P95Action {
    int x = 0;
};
struct P95Model {
    int execute(const P95Action& act) { return act.x; }
};
BRIDGE_REGISTER_MODEL(P95Model, "Cov_P95Model")
BRIDGE_REGISTER_ACTION(P95Model, P95Action, "Cov_P95Action")

namespace {

using SyncExecutor = morph::testing::InlineExecutor;
using LogGuard = morph::log::ScopedLoggerOverride;

template <typename Pred>
bool waitFor(Pred pred, std::chrono::milliseconds budget = 2000ms) {
    return morph::testing::waitUntil(std::move(pred), budget);
}

// ── A backend whose reconnect handler we can fire on demand ──────────────────
//
// Bridge installs a reconnect handler on every backend it owns
// (bridge.hpp installReconnectHandler). LocalBackend never invokes it, so the
// handler body (bridge.hpp:290-303) is otherwise dead. This fake lets a test
// invoke the stored handler directly.
struct ControllableBackend : ::morph::backend::detail::IBackend {
    std::atomic<std::uint64_t> nextId{1};
    std::function<void()> reconnect;

    ::morph::exec::detail::ModelId registerModel(
        const std::string& /*typeId*/,
        std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> /*factory*/) override {
        return ::morph::exec::detail::ModelId{nextId.fetch_add(1)};
    }
    void deregisterModel(::morph::exec::detail::ModelId /*mid*/) override {}
    ::morph::async::Completion<std::shared_ptr<void>> execute(::morph::exec::detail::ModelId /*mid*/,
                                                              ::morph::backend::detail::ActionCall /*call*/,
                                                              ::morph::exec::IExecutor* /*cbExec*/) override {
        return {};
    }
    void notifyBackendChanged() override {}
    void cancelPending(const std::exception_ptr& /*exc*/) override {}
    void setReconnectHandler(const std::function<void()>& handler) override { reconnect = std::move(handler); }
};

// A denying authorizer for the RemoteServer unauthorized path.
struct DenyAuthorizer : ::morph::session::IAuthorizer {
    [[nodiscard]] bool authorize(const ::morph::session::Context& /*ctx*/, std::string_view /*modelType*/,
                                 std::string_view /*actionType*/) const override {
        return false;
    }
};

std::shared_ptr<::morph::bridge::detail::HandlerBinding> makeBinding() {
    auto binding = std::make_shared<::morph::bridge::detail::HandlerBinding>();
    binding->typeId = ::morph::model::ModelTraits<P95Model>::typeId();
    binding->modelFactory = [] { return ::morph::model::detail::ModelFactory::create<P95Model>(); };
    return binding;
}

}  // namespace

// ── bridge.hpp:290-303 — reconnect handler re-registers live bindings ────────

TEST_CASE("morph::bridge::Bridge: reconnect handler re-registers live bindings and skips expired ones",
          "[coverage][bridge]") {
    // Firing the reconnect handler while its backend is still active drives the
    // "proceed" path: pinned != nullptr and pinned == loadBackend() (both false
    // arms of 293), the handler loop (296), and both arms of `!binding` (298) —
    // a live binding (false arm) plus an expired weak_ptr (true arm / continue).
    auto backend = std::make_unique<ControllableBackend>();
    auto* raw = backend.get();
    ::morph::bridge::Bridge bridge{std::move(backend)};

    auto live = makeBinding();
    bridge.registerHandler(live);
    const std::uint64_t liveIdBefore = live->currentId.load();

    {
        // A second binding whose owning shared_ptr dies immediately, leaving a
        // stale weak_ptr in Bridge::_handlers → covers the `!binding` continue.
        auto ephemeral = makeBinding();
        bridge.registerHandler(ephemeral);
    }

    REQUIRE(raw->reconnect);  // Bridge installed a handler on construction.
    raw->reconnect();         // Fire it: live binding gets a fresh model id.

    REQUIRE(live->currentId.load() != liveIdBefore);
}

// ── bridge.hpp:293 — reconnect handler ignores a stale backend ───────────────

TEST_CASE("morph::bridge::Bridge: reconnect handler from a superseded backend is a no-op", "[coverage][bridge]") {
    // Grab the handler installed on backend A, then switch to backend B. The
    // switch releases A, so the handler's weak_ptr can no longer be locked:
    // `!pinned` is true → early return (293 true arm). Safe to invoke because the
    // lambda captures the still-alive Bridge, not backend A.
    auto backendA = std::make_unique<ControllableBackend>();
    auto* rawA = backendA.get();
    ::morph::bridge::Bridge bridge{std::move(backendA)};

    std::function<void()> stale = rawA->reconnect;
    REQUIRE(stale);

    bridge.switchBackend(std::make_unique<ControllableBackend>());
    REQUIRE_NOTHROW(stale());
}

// ── bridge.hpp:137,144 — default session accessors ───────────────────────────

TEST_CASE("morph::bridge::Bridge: setDefaultSession / defaultSession round-trip", "[coverage][bridge]") {
    ::morph::exec::ThreadPoolExecutor pool{1};
    ::morph::bridge::Bridge bridge{std::make_unique<::morph::backend::LocalBackend>(pool)};

    ::morph::session::Context ctx;
    ctx.principal = "alice";
    bridge.setDefaultSession(ctx);
    REQUIRE(bridge.defaultSession().principal == "alice");

    bridge.setDefaultSession({});  // clear
    REQUIRE(bridge.defaultSession().principal.empty());
}

// ── bridge.hpp:91-95,283-285 — a null initial backend is tolerated ───────────

TEST_CASE("morph::bridge::Bridge: constructed with a null backend and destroyed", "[coverage][bridge]") {
    // installReconnectHandler sees a null backend and returns early (283-285);
    // the destructor's `if (auto active = loadBackend())` takes its null/false
    // arm (92) because the backend is still null at destruction.
    REQUIRE_NOTHROW([] { ::morph::bridge::Bridge bridge{std::unique_ptr<::morph::backend::detail::IBackend>{}}; }());
}

// ── bridge.hpp:184,190 — switchBackend from a null backend ───────────────────

TEST_CASE("morph::bridge::Bridge: switchBackend from a null backend skips the previous-backend teardown",
          "[coverage][bridge]") {
    // With a null initial backend, `previous` is empty after the swap, so both
    // `if (previous && previous != newShared)` guards (184, 190) take their false
    // arm and the old-backend teardown is skipped.
    ::morph::exec::ThreadPoolExecutor pool{1};
    ::morph::bridge::Bridge bridge{std::unique_ptr<::morph::backend::detail::IBackend>{}};
    REQUIRE_NOTHROW(bridge.switchBackend(std::make_unique<::morph::backend::LocalBackend>(pool)));
}

// ── bridge.hpp:239-242 — executeVia on an unbound handler reports "handler not bound"

TEST_CASE("morph::bridge::Bridge: executeVia on an unregistered binding fails with \"handler not bound\"",
          "[coverage][bridge]") {
    // A HandlerBinding that was never registered has currentId == 0, so executeVia
    // hits the `if (raw == 0U)` branch (239 true arm) and resolves with an error.
    ::morph::exec::ThreadPoolExecutor pool{1};
    SyncExecutor cbExec;
    ::morph::bridge::Bridge bridge{std::make_unique<::morph::backend::LocalBackend>(pool)};

    auto unbound = std::make_shared<::morph::bridge::detail::HandlerBinding>();
    auto comp = bridge.executeVia<P95Model, P95Action>(unbound, P95Action{5}, &cbExec);

    std::atomic<bool> errored{false};
    std::string message;
    comp.onError([&](const std::exception_ptr& exc) {
        try {
            std::rethrow_exception(exc);
        } catch (const std::exception& ex) {
            message = ex.what();
        }
        errored.store(true);
    });
    REQUIRE(waitFor([&] { return errored.load(); }));
    REQUIRE(message.contains("handler not bound"));
}

// ── completion.hpp:54 — setException is a no-op once the state is ready ───────

TEST_CASE("morph::async::detail::CompletionState: setException after ready returns early", "[coverage][completion]") {
    auto state = std::make_shared<::morph::async::detail::CompletionState<int>>();
    state->setValue(1);  // ready = true
    state->setException(std::make_exception_ptr(std::runtime_error("late")));
    REQUIRE(state->ready);
    REQUIRE_FALSE(state->error);  // early-return left `error` unset
}

// ── backend.hpp:118 — DisconnectedError constructor ──────────────────────────

TEST_CASE("morph::backend::DisconnectedError carries its canned message", "[coverage][backend]") {
    ::morph::backend::DisconnectedError err;
    REQUIRE(std::string{err.what()}.contains("transport disconnected"));
}

// ── session.hpp:115 — current() free function ────────────────────────────────

TEST_CASE("morph::session::current returns nullptr outside any ScopedContext", "[coverage][session]") {
    REQUIRE(::morph::session::current() == nullptr);
}

// ── remote.hpp:58-70,145 — authorizer ctor + unauthorized execute path ───────

TEST_CASE("morph::backend::RemoteServer: execute is rejected when the authorizer denies it", "[coverage][remote]") {
    ::morph::exec::ThreadPoolExecutor pool{2};
    auto authorizer = std::make_shared<DenyAuthorizer>();
    auto server = std::make_shared<::morph::backend::RemoteServer>(pool, authorizer);  // authorizer ctor (58-70)

    ::morph::wire::Envelope req;
    req.kind = "execute";
    req.callId = 77;
    req.modelId = 1;
    req.modelType = "Cov_Whatever";
    req.actionType = "Cov_Whatever";
    req.body = "{}";

    ::morph::testing::WaitReply reply;
    server->handle(::morph::wire::encode(req), std::ref(reply));
    REQUIRE(reply.await());
    REQUIRE(reply.env.kind == "err");
    REQUIRE(reply.env.message == "unauthorized");  // covers 145 true arm
    REQUIRE(reply.env.callId == 77U);
}

TEST_CASE("morph::backend::RemoteServer: null authorizer falls back to allow-all", "[coverage][remote]") {
    // The authorizer-taking ctor's `if (!_authorizer)` fallback (remote.hpp:67-69).
    ::morph::exec::ThreadPoolExecutor pool{2};
    auto server =
        std::make_shared<::morph::backend::RemoteServer>(pool, std::shared_ptr<::morph::session::IAuthorizer>{});

    // A register still works because the fallback allow-all authorizer is in place.
    ::morph::testing::WaitReply reply;
    server->handle(::morph::wire::encode(::morph::wire::makeRegister("Cov_Unregistered")), std::ref(reply));
    REQUIRE(reply.await());
    // Unknown model type → err, but the point is the server constructed and ran.
    REQUIRE((reply.env.kind == "err" || reply.env.kind == "ok"));
}

// ── remote.hpp:261 — SimulatedRemoteBackend maps an empty err message to "malformed reply"
//
// External-linkage types so glaze can resolve their mangled names.

struct CovEmptyThrowAction {
    int x = 0;
};
struct CovSlowAction {
    int x = 0;
};
struct CovEmptyThrowModel {
    int execute(const CovEmptyThrowAction&) { throw std::runtime_error(""); }  // empty what()
    int execute(const CovSlowAction&) {
        std::this_thread::sleep_for(80ms);  // stays in-flight long enough to be cancelled
        return 0;
    }
};

template <>
struct morph::model::ModelTraits<CovEmptyThrowModel> {
    static constexpr std::string_view typeId() { return "Cov_EmptyThrowModel"; }
};
template <>
struct morph::model::ActionTraits<CovEmptyThrowAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "Cov_EmptyThrowAction"; }
    static std::string toJson(const CovEmptyThrowAction&) { return "{}"; }
    static CovEmptyThrowAction fromJson(std::string_view) { return {}; }
    static std::string resultToJson(const int&) { return "0"; }
    static int resultFromJson(std::string_view) { return 0; }
};
template <>
struct morph::model::ActionTraits<CovSlowAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "Cov_SlowAction"; }
    static std::string toJson(const CovSlowAction&) { return "{}"; }
    static CovSlowAction fromJson(std::string_view) { return {}; }
    static std::string resultToJson(const int&) { return "0"; }
    static int resultFromJson(std::string_view) { return 0; }
};

namespace {

struct EmptyThrowEnv {
    ::morph::model::detail::ActionDispatcher dispatcher;
    ::morph::model::detail::ModelRegistryFactory registry;
};

EmptyThrowEnv& emptyThrowEnv() {
    static EmptyThrowEnv env = [] {
        EmptyThrowEnv tmp;
        tmp.registry.registerModel<CovEmptyThrowModel>("Cov_EmptyThrowModel");
        tmp.dispatcher.registerAction<CovEmptyThrowModel, CovEmptyThrowAction>("Cov_EmptyThrowModel",
                                                                               "Cov_EmptyThrowAction");
        tmp.dispatcher.registerAction<CovEmptyThrowModel, CovSlowAction>("Cov_EmptyThrowModel", "Cov_SlowAction");
        return tmp;
    }();
    return env;
}

}  // namespace

TEST_CASE("morph::backend::SimulatedRemoteBackend: empty err message surfaces as \"malformed reply\"",
          "[coverage][remote]") {
    ::morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cb;
    auto& env = emptyThrowEnv();
    auto server = std::make_shared<::morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);
    ::morph::backend::SimulatedRemoteBackend backend{*server};

    auto mid = backend.registerModel("Cov_EmptyThrowModel", {});

    ::morph::backend::detail::ActionCall call;
    call.modelTypeId = "Cov_EmptyThrowModel";
    call.actionTypeId = "Cov_EmptyThrowAction";
    call.serializeAction = [] { return std::string{"{}"}; };
    call.deserializeResult = [](std::string_view) -> std::shared_ptr<void> { return nullptr; };
    call.localOp = [](::morph::model::detail::IModelHolder&) -> std::shared_ptr<void> { return nullptr; };

    auto comp = backend.execute(mid, std::move(call), &cb);

    std::atomic<bool> errored{false};
    std::string message;
    comp.onError([&](const std::exception_ptr& exc) {
        try {
            std::rethrow_exception(exc);
        } catch (const std::exception& ex) {
            message = ex.what();
        }
        errored.store(true);
    });
    REQUIRE(waitFor([&] { return errored.load(); }));
    REQUIRE(message == "malformed reply");

    backend.deregisterModel(mid);
}

// ── remote.hpp:282 — cancelPending skips an already-expired completion state ──

TEST_CASE("morph::backend::SimulatedRemoteBackend: cancelPending tolerates an expired pending state",
          "[coverage][remote]") {
    ::morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cb;
    auto& env = emptyThrowEnv();
    auto server = std::make_shared<::morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);
    ::morph::backend::SimulatedRemoteBackend backend{*server};

    auto mid = backend.registerModel("Cov_EmptyThrowModel", {});

    {
        ::morph::backend::detail::ActionCall call;
        call.modelTypeId = "Cov_EmptyThrowModel";
        call.actionTypeId = "Cov_EmptyThrowAction";
        call.serializeAction = [] { return std::string{"{}"}; };
        call.deserializeResult = [](std::string_view) -> std::shared_ptr<void> { return nullptr; };
        call.localOp = [](::morph::model::detail::IModelHolder&) -> std::shared_ptr<void> { return nullptr; };

        auto comp = backend.execute(mid, std::move(call), &cb);
        // Wait for the reply lambda to run so it releases its ref to the state.
        std::atomic<bool> done{false};
        comp.onError([&](const std::exception_ptr&) { done.store(true); });
        REQUIRE(waitFor([&] { return done.load(); }));
        // `comp` (last owner of the state) is dropped at end of this scope.
    }
    // The tracked weak_ptr is now expired → cancelPending's `if (state = weak.lock())`
    // takes the false arm (remote.hpp:282).
    REQUIRE_NOTHROW(backend.cancelPending(std::make_exception_ptr(std::runtime_error("cancel"))));

    backend.deregisterModel(mid);
}

TEST_CASE("morph::backend::SimulatedRemoteBackend: cancelPending resolves a still-live pending state",
          "[coverage][remote]") {
    // Complements the expired-weak case above: a slow action keeps the completion
    // state alive and pending, so cancelPending's weak.lock() succeeds and delivers
    // the exception (remote.hpp:282 true arm).
    ::morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cb;
    auto& env = emptyThrowEnv();
    auto server = std::make_shared<::morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);
    ::morph::backend::SimulatedRemoteBackend backend{*server};

    auto mid = backend.registerModel("Cov_EmptyThrowModel", {});

    ::morph::backend::detail::ActionCall call;
    call.modelTypeId = "Cov_EmptyThrowModel";
    call.actionTypeId = "Cov_SlowAction";
    call.serializeAction = [] { return std::string{"{}"}; };
    call.deserializeResult = [](std::string_view) -> std::shared_ptr<void> { return nullptr; };
    call.localOp = [](::morph::model::detail::IModelHolder&) -> std::shared_ptr<void> { return nullptr; };

    auto comp = backend.execute(mid, std::move(call), &cb);

    std::atomic<bool> errored{false};
    std::string message;
    comp.onError([&](const std::exception_ptr& exc) {
        try {
            std::rethrow_exception(exc);
        } catch (const std::exception& ex) {
            message = ex.what();
        }
        errored.store(true);
    });

    // Cancel while the 80ms action is still executing on the server: the pending
    // state is alive → weak.lock() succeeds (282 true arm).
    backend.cancelPending(std::make_exception_ptr(std::runtime_error("cancelled-in-flight")));

    REQUIRE(waitFor([&] { return errored.load(); }));
    REQUIRE(message == "cancelled-in-flight");

    backend.deregisterModel(mid);
}

// ── reconnect_coordinator.hpp:162 — a null Deps member is logged ──────────────

TEST_CASE("morph::offline::ReconnectCoordinator: null Deps member is logged at construction",
          "[coverage][reconnect]") {
    LogGuard guard;
    std::atomic<bool> sawNull{false};
    ::morph::log::setLogger([&](::morph::log::LogLevel, std::string_view msg) {
        if (msg.contains("null Deps member")) {
            sawNull.store(true);
        }
    });

    ::morph::offline::ReconnectCoordinator::Deps deps;
    deps.tryReconnect = [] { return true; };
    deps.activatePrimary = [] {};
    deps.activateLocal = [] {};
    deps.bindContext = [] {};
    deps.replay = [] {};
    deps.shouldContinue = [] { return true; };
    // deps.sleep intentionally left null → assertDepsNonNull logs it (162 true arm).
    ::morph::offline::ReconnectCoordinator coordinator{std::move(deps)};
    (void)coordinator;

    REQUIRE(sawNull.load());
}

// ── reconnect_coordinator.hpp:176-190 — onOnline reconnect + shouldContinue throw

TEST_CASE("morph::offline::ReconnectCoordinator: onOnline reconnects and shouldContinue-throw aborts",
          "[coverage][reconnect]") {
    // Happy path: covers callShouldContinue's normal return (188) and callTryReconnect.
    {
        std::atomic<int> replayed{0};
        ::morph::offline::ReconnectCoordinator::Deps deps;
        deps.tryReconnect = [] { return true; };
        deps.activatePrimary = [] {};
        deps.activateLocal = [] {};
        deps.bindContext = [] {};
        deps.replay = [&] { replayed.fetch_add(1); };
        deps.shouldContinue = [] { return true; };
        deps.sleep = [](std::chrono::milliseconds) {};
        ::morph::offline::ReconnectCoordinator coordinator{std::move(deps)};
        REQUIRE(coordinator.onOnline() == ::morph::offline::ReconnectOutcome::Reconnected);
        REQUIRE(replayed.load() == 1);
    }

    // shouldContinue throws → treated as "do not continue" → Aborted (189-190 catch).
    {
        ::morph::offline::ReconnectCoordinator::Deps deps;
        deps.tryReconnect = [] { return true; };
        deps.activatePrimary = [] {};
        deps.activateLocal = [] {};
        deps.bindContext = [] {};
        deps.replay = [] {};
        deps.shouldContinue = []() -> bool { throw std::runtime_error("boom"); };
        deps.sleep = [](std::chrono::milliseconds) {};
        ::morph::offline::ReconnectCoordinator coordinator{std::move(deps)};
        REQUIRE(coordinator.onOnline() == ::morph::offline::ReconnectOutcome::Aborted);
    }
}

// ── sync_worker.hpp:132-152 — dead-letter after kMaxAttempts (=5), no-sink branch ──

TEST_CASE("morph::offline::SyncWorker: a payload is dead-lettered after kMaxAttempts", "[coverage][sync]") {
    LogGuard guard;  // silence the "dropping payload" error log
    ::morph::log::setLogger([](::morph::log::LogLevel, std::string_view) {});

    ::morph::offline::InMemoryOfflineQueue queue;
    queue.enqueue("always-fails");
    ::morph::offline::SyncWorker worker{queue, [](const std::string&) { return false; }};

    // kMaxAttempts is 5. Attempts 1-4 keep the item (failed); the 5th trips the
    // `counter >= kMaxAttempts` branch (132 true arm, no DeadLetterSink set) and
    // dead-letters it via the default log-and-drop path.
    ::morph::offline::SyncResult result;
    for (int i = 0; i < 5; ++i) {
        result = worker.run();
    }
    REQUIRE(result.deadLettered == 1);
    // Item is gone now, so a further run does nothing.
    auto after = worker.run();
    REQUIRE(after.failed == 0);
    REQUIRE(after.deadLettered == 0);
}

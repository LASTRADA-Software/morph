// SPDX-License-Identifier: Apache-2.0
//
// Tests that target specific coverage gaps identified by llvm-cov. Each test
// case names the file:line range it is meant to cover so future readers can
// understand why an unusual edge case is being exercised here.

#include <any>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/logger.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/offline/network_monitor.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeindex>
#include <variant>

#include "test_support.hpp"

using namespace std::chrono_literals;

namespace {

using SyncExecutor = morph::testing::InlineExecutor;
using LogGuard = morph::log::ScopedLoggerOverride;

template <typename Pred>
bool waitFor(Pred pred, std::chrono::milliseconds budget = 2000ms) {
    return morph::testing::waitUntil(std::move(pred), budget);
}

}  // namespace

// ── registry.hpp: BRIDGE_REGISTER_ACTION parse-error paths (lines 242-243, 256-257)

// Action types local to this file so they get their own BRIDGE_REGISTER_ACTION
// instantiations — we need the macro-expanded fromJson/resultFromJson failure
// paths to be exercised.
struct CovParseAction {
    int x = 0;
};
struct CovParseModel {
    int execute(const CovParseAction& act) { return act.x; }
};

BRIDGE_REGISTER_MODEL(CovParseModel, "Cov_ParseModel")
BRIDGE_REGISTER_ACTION(CovParseModel, CovParseAction, "Cov_ParseAction")

TEST_CASE("morph::model::ActionTraits (macro-expanded): fromJson throws morph::model::detail::ParseError on garbage",
          "[coverage][registry]") {
    REQUIRE_THROWS_AS(morph::model::ActionTraits<CovParseAction>::fromJson("not-json"),
                      morph::model::detail::ParseError);
}

TEST_CASE(
    "morph::model::ActionTraits (macro-expanded): resultFromJson throws morph::model::detail::ParseError on garbage",
    "[coverage][registry]") {
    REQUIRE_THROWS_AS(morph::model::ActionTraits<CovParseAction>::resultFromJson("not-a-number"),
                      morph::model::detail::ParseError);
}

TEST_CASE("morph::model::ActionTraits (macro-expanded): toJson/resultToJson round-trip succeeds",
          "[coverage][registry]") {
    auto json = morph::model::ActionTraits<CovParseAction>::toJson(CovParseAction{7});
    REQUIRE_FALSE(json.empty());
    auto back = morph::model::ActionTraits<CovParseAction>::fromJson(json);
    REQUIRE(back.x == 7);

    auto resJson = morph::model::ActionTraits<CovParseAction>::resultToJson(42);
    REQUIRE(morph::model::ActionTraits<CovParseAction>::resultFromJson(resJson) == 42);
}

// ── registry.hpp: BRIDGE_REGISTER_ACTION toJson/resultToJson write-error paths (lines 445, 463)
//
// glz::write_json's only reachable failure for a plain (non-partial) write is
// error_code::invalid_variant_object: a *tagged* variant whose active
// alternative is a null smart pointer (see glaze's docs/variant-handling.md,
// "Variants with Smart Pointers" -- "Null smart pointers during serialization
// will result in an error").

struct CovWriteFailPayload {
    std::string message;
};

using CovWriteFailVariant = std::variant<std::shared_ptr<CovWriteFailPayload>>;

template <>
struct glz::meta<CovWriteFailVariant> {
    static constexpr std::string_view tag = "type";
    static constexpr auto ids = std::array{"payload"};
};

struct CovWriteFailAction {
    // Default-constructed to index 0 holding a null shared_ptr -- exactly the
    // state glaze's writer rejects.
    CovWriteFailVariant data;
};
struct CovWriteFailModel {
    CovWriteFailVariant execute(const CovWriteFailAction& act) { return act.data; }
};

BRIDGE_REGISTER_MODEL(CovWriteFailModel, "Cov_WriteFailModel")
BRIDGE_REGISTER_ACTION(CovWriteFailModel, CovWriteFailAction, "Cov_WriteFailAction")

TEST_CASE(
    "morph::model::ActionTraits (macro-expanded): toJson throws morph::model::detail::ParseError on a null tagged "
    "variant alternative",
    "[coverage][registry]") {
    REQUIRE_THROWS_AS(morph::model::ActionTraits<CovWriteFailAction>::toJson(CovWriteFailAction{}),
                      morph::model::detail::ParseError);
}

TEST_CASE(
    "morph::model::ActionTraits (macro-expanded): resultToJson throws morph::model::detail::ParseError on a null "
    "tagged variant alternative",
    "[coverage][registry]") {
    CovWriteFailVariant const result{};
    REQUIRE_THROWS_AS(morph::model::ActionTraits<CovWriteFailAction>::resultToJson(result),
                      morph::model::detail::ParseError);
}

// ── remote.hpp: execute err reply envelope echoes the callId
//
// These types must have external linkage so glaze's reflection can resolve
// their mangled names — keep them at file scope, not in an anonymous namespace.

struct CovRemoteAction {
    int x = 0;
};
struct CovRemoteFail {};
struct CovRemoteModel {
    int execute(const CovRemoteAction& act) { return act.x; }
    int execute(const CovRemoteFail&) { throw std::runtime_error("remote action failed"); }
};

template <>
struct morph::model::ModelTraits<CovRemoteModel> {
    static constexpr std::string_view typeId() { return "Cov_RemoteModel"; }
};
template <>
struct morph::model::ActionTraits<CovRemoteAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "Cov_RemoteAction"; }
    static std::string toJson(const CovRemoteAction& act) {
        std::string out;
        (void)glz::write_json(act, out);
        return out;
    }
    static CovRemoteAction fromJson(std::string_view json) {
        CovRemoteAction action{};
        (void)glz::read_json(action, json);
        return action;
    }
    static std::string resultToJson(const int& res) {
        std::string out;
        (void)glz::write_json(res, out);
        return out;
    }
    static int resultFromJson(std::string_view json) {
        int result{};
        (void)glz::read_json(result, json);
        return result;
    }
};
template <>
struct morph::model::ActionTraits<CovRemoteFail> {
    using Result = int;
    static constexpr std::string_view typeId() { return "Cov_RemoteFail"; }
    static std::string toJson(const CovRemoteFail&) { return "{}"; }
    static CovRemoteFail fromJson(std::string_view) { return {}; }
    static std::string resultToJson(const int&) { return "0"; }
    static int resultFromJson(std::string_view) { return 0; }
};

namespace {

struct CovRemoteEnv {
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
};

CovRemoteEnv& covRemoteEnv() {
    static CovRemoteEnv env = [] {
        CovRemoteEnv tmp;
        tmp.registry.registerModel<CovRemoteModel>("Cov_RemoteModel");
        tmp.dispatcher.registerAction<CovRemoteModel, CovRemoteAction>("Cov_RemoteModel", "Cov_RemoteAction");
        tmp.dispatcher.registerAction<CovRemoteModel, CovRemoteFail>("Cov_RemoteModel", "Cov_RemoteFail");
        return tmp;
    }();
    return env;
}

}  // namespace

TEST_CASE("morph::backend::RemoteServer: execute err reply preserves callId", "[coverage][remote]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = covRemoteEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    // Register a model first
    std::string regReplyRaw;
    std::atomic<bool> regDone{false};
    server->handle(morph::wire::encode(morph::wire::makeRegister("Cov_RemoteModel")), [&](const std::string& reply) {
        regReplyRaw = reply;
        regDone.store(true);
    });
    REQUIRE(waitFor([&] { return regDone.load(); }));
    auto regReply = morph::wire::decode(regReplyRaw);
    REQUIRE(regReply.kind == "ok");

    // Execute with a callId; failure must echo the same callId in the err envelope.
    morph::wire::Envelope req;
    req.kind = "execute";
    req.callId = 4242;
    req.modelId = regReply.modelId;
    req.modelType = "Cov_RemoteModel";
    req.actionType = "Cov_RemoteFail";
    req.body = "{}";

    std::atomic<bool> replyReceived{false};
    std::string replyMsg;
    server->handle(morph::wire::encode(req), [&](const std::string& reply) {
        replyMsg = reply;
        replyReceived.store(true);
    });

    REQUIRE(waitFor([&] { return replyReceived.load(); }));
    auto resp = morph::wire::decode(replyMsg);
    REQUIRE(resp.kind == "err");
    REQUIRE(resp.callId == 4242U);
}

// ── remote.hpp: morph::backend::SimulatedRemoteBackend register failure (lines 196-197)

TEST_CASE("morph::backend::SimulatedRemoteBackend: registerModel propagates server err reply", "[coverage][remote]") {
    // The real morph::backend::RemoteServer reports "err|unknown model type: ..." when the
    // registry has no factory for the type. morph::backend::SimulatedRemoteBackend should
    // turn that into a thrown runtime_error.
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = covRemoteEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);
    morph::backend::SimulatedRemoteBackend backend{*server};

    REQUIRE_THROWS_AS(backend.registerModel("Cov_DefinitelyNotRegistered", {}), std::runtime_error);
}

// ── remote.hpp: morph::backend::SimulatedRemoteBackend execute error reply path

TEST_CASE("morph::backend::SimulatedRemoteBackend: execute error reply is delivered via onError",
          "[coverage][remote]") {
    // Exercises the err-prefixed reply branch (line 234) and confirms the
    // catch(...) -> setException path is wired up end-to-end.
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cb;
    auto& env = covRemoteEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);
    morph::backend::SimulatedRemoteBackend backend{*server};

    auto mid = backend.registerModel("Cov_RemoteModel", {});

    morph::backend::detail::ActionCall call;
    call.modelTypeId = "Cov_RemoteModel";
    call.actionTypeId = "Cov_RemoteFail";
    call.serializeAction = [] { return std::string{"{}"}; };
    call.deserializeResult = [](std::string_view) -> std::shared_ptr<void> { return nullptr; };
    call.localOp = [](morph::model::detail::IModelHolder&) -> std::shared_ptr<void> { return nullptr; };

    auto comp = backend.execute(mid, std::move(call), &cb);

    std::atomic<bool> errored{false};
    std::string errMsg;
    comp.onError([&](const std::exception_ptr& exc) {
        try {
            std::rethrow_exception(exc);
        } catch (const std::exception& ex) {
            errMsg = ex.what();
        }
        errored.store(true);
    });
    REQUIRE(waitFor([&] { return errored.load(); }));
    REQUIRE(errMsg.contains("remote action failed"));

    backend.deregisterModel(mid);
}

// ── completion.hpp: orphan destructor's inner catch on a throwing logger (lines 105, 110)

namespace {
struct NotAStdException {};
}  // namespace

TEST_CASE("CompletionState orphan dtor swallows exceptions from a throwing logger", "[coverage][completion]") {
    LogGuard guard;
    // Throwing sink covers the inner catch(...) in the std::exception branch (line 105).
    morph::log::setLogger([](morph::log::LogLevel, std::string_view) { throw std::runtime_error("sink-fail"); });

    {
        auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
        state->ready = true;
        try {
            throw std::runtime_error{"orphan std"};
        } catch (...) {
            state->error = std::current_exception();
        }
        // onErrAttached stays false → dtor enters the rethrow branch → morph::log::logError
        // throws → inner catch(...) swallows.
    }

    // And the same for the catch(...) non-std::exception branch (line 110).
    {
        auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
        state->ready = true;
        try {
            throw NotAStdException{};
        } catch (...) {
            state->error = std::current_exception();
        }
    }

    REQUIRE(true);  // no crash, no terminate
}

// ── logger.hpp: morph::log::detail::levelName fallback for an out-of-range enum value (line 41)

TEST_CASE("morph::log::detail::levelName returns ?    for an out-of-range morph::log::LogLevel",
          "[coverage][logger]") {
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) — exercises the default-branch fallback.
    auto bogus = static_cast<morph::log::LogLevel>(static_cast<std::uint8_t>(99));
    REQUIRE(morph::log::detail::levelName(bogus) == "?    ");
}

// ── logger.hpp: default sink lambda body (line 53)

TEST_CASE("Default logger sink lambda body is invoked", "[coverage][logger]") {
    LogGuard guard;
    // Reset the global sink to a fresh default-constructed LogState's sink,
    // which is the lambda defined inline at logger.hpp:53. Invoking it via
    // morph::log::logError() ensures its body executes regardless of prior test order.
    {
        std::scoped_lock lock{morph::log::detail::logState().mtx};
        morph::log::detail::logState().sink = morph::log::detail::LogState{}.sink;
        morph::log::detail::logState().minLevel = morph::log::LogLevel::error;
    }
    morph::log::logError("default-sink-coverage");
}

// ── network_monitor.hpp: stop() called from inside the probe → destructor spin-wait (lines 81-82, 113)

TEST_CASE("morph::offline::NetworkMonitor: stop() from inside probe detaches and dtor spin-waits",
          "[coverage][monitor]") {
    // The probe is invoked on the monitor's own thread. If the probe calls
    // stop(), the thread cannot join itself — stop() detaches instead, and the
    // destructor spin-yields on _runExited until run() finishes.
    //
    // The probe sleeps AFTER calling stop() to widen the window during which
    // the destructor's spin-wait loop actually runs (covering the yield body).
    std::atomic<bool> stopCalled{false};
    std::atomic<morph::offline::NetworkMonitor*> monPtr{nullptr};

    auto probe = [&] {
        if (auto* mon = monPtr.load()) {
            if (!stopCalled.exchange(true)) {
                mon->stop();
                // Sleep here so the test's destructor enters its spin-wait
                // while run() is still inside this probe call.
                std::this_thread::sleep_for(30ms);
            }
        }
        return true;
    };

    {
        morph::offline::NetworkMonitor monitor{
            std::move(probe), [] {}, [] {},
            morph::offline::NetworkMonitor::Config{.probeInterval = 5ms, .failureThreshold = 1}};
        monPtr.store(&monitor);
        REQUIRE(waitFor([&] { return stopCalled.load(); }));
        // Destructor here — stopCalled is true, run() is still sleeping inside
        // the probe, so _runExited is still false → spin-yield body executes.
    }

    REQUIRE(stopCalled.load());
}

// ── bridge.hpp: Bridge's subscription and handler bookkeeping on the paths
// where nothing matches — removeSubscription's erase_if predicate (977-980),
// publishResult's fan-out and dispatch loops (1026-1031, 1033-1039), and
// deregisterHandler's find_if predicate (1307-1310).
//
// The identifiers are the key here and the line numbers are a hint. This marker
// previously claimed a weak-lock failure "when handler dies mid-flight" at
// 489-490, 518-519 and 532-533, and named a type to go with it. By the time
// anyone checked, that name matched nothing anywhere in the tree and all three
// ranges had drifted onto unrelated code — mid-sentence in a doc comment, a
// ModelId load, a parkIfInFrame guard (morph#419, after morph#349 and morph#355
// in the same shape). The dead name is not repeated here on purpose: a grep for
// it must come back empty, or the comment reads as a live reference to whoever
// runs that grep next. Nothing verifies either half of such a citation, so when
// a name and a number disagree, believe the name.
//
// The type that does exist is Bridge::InstanceSubscription (bridge.hpp:1848) —
// a weak_ptr to a detail::HandlerBinding, the result type it wants, and where to
// deliver it.
//
// What the three cases below reach is the empty and non-matching arms: an
// unsubscribe with no entry to erase, a publishResult with no subscription to
// fan out to, and a deregisterHandler walking past a live binding that is not
// the one being removed. What they do not reach is the weak-lock-failure arms
// the old text named — `!owner` (979), `entry.binding.expired()` (1024) and a
// falsy `sptr` (1309) — each of which needs a binding destroyed while an entry
// for it is still listed, which nothing here arranges.
//
// File-scope (not anonymous namespace) for glaze reflection.

struct SlowSubAction {
    int seq = 0;
};
struct ThrowSubAction {
    int trigger = 0;
};
struct WeirdSubAction {
    int trigger = 0;
};
struct SubModel {
    int execute(SlowSubAction act) {
        std::this_thread::sleep_for(40ms);
        return act.seq;
    }
    int execute(ThrowSubAction) { throw std::runtime_error("threw inside action"); }
    int execute(WeirdSubAction) { throw NotAStdException{}; }
};

BRIDGE_REGISTER_MODEL(SubModel, "Cov_SubModel")
BRIDGE_REGISTER_ACTION(SubModel, SlowSubAction, "Cov_SlowSubAction")
BRIDGE_REGISTER_ACTION(SubModel, ThrowSubAction, "Cov_ThrowSubAction")
BRIDGE_REGISTER_ACTION(SubModel, WeirdSubAction, "Cov_WeirdSubAction")

BRIDGE_REGISTER_VALIDATOR(SlowSubAction, [](const SlowSubAction& act) { return act.seq != 0; })
BRIDGE_REGISTER_VALIDATOR(ThrowSubAction, [](const ThrowSubAction& act) { return act.trigger != 0; })
BRIDGE_REGISTER_VALIDATOR(WeirdSubAction, [](const WeirdSubAction& act) { return act.trigger != 0; })

TEST_CASE("morph::bridge::BridgeHandler: unsubscribe with no entry is a no-op", "[coverage][bridge]") {
    morph::exec::ThreadPoolExecutor pool{1};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<SubModel> handler{bridge, &cbExec};

    REQUIRE_NOTHROW(handler.unsubscribe<int>());
}

TEST_CASE("morph::bridge::Bridge: publishResult with no subscribers is a no-op", "[coverage][bridge]") {
    morph::exec::ThreadPoolExecutor pool{1};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<SubModel> handler{bridge, &cbExec};

    // Nothing is subscribed, so the fan-out loop finds no matching entry.
    REQUIRE_NOTHROW(
        bridge.publishResult(::morph::exec::detail::ModelId{1}, std::type_index{typeid(int)}, std::any{int{0}}));
}

TEST_CASE("morph::bridge::Bridge: deregisterHandler skips other bindings", "[coverage][bridge]") {
    // With two live handlers, deregistering one walks past the other in the
    // find_if predicate — the lambda returns false for the non-matching binding,
    // exercising the false arm of `sptr.get() == binding.get()`.
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<SubModel> handlerA{bridge, &cbExec};
    morph::bridge::BridgeHandler<SubModel> handlerB{bridge, &cbExec};
    (void)handlerA;
    (void)handlerB;
    // Destructors run in reverse: handlerB first (finds itself, lambda returns
    // false for A's binding, true for B's). Then handlerA.
}

// ── network_monitor.hpp: empty onOffline/onOnline callbacks (lines 146, 155)

TEST_CASE("morph::offline::NetworkMonitor: transitions work with empty callbacks", "[coverage][monitor]") {
    // Pass empty std::function<void()> for onOffline/onOnline — covers the
    // false arms of `if (_onOffline)` and `if (_onOnline)`.
    std::atomic<int> probeResult{0};  // 0 = fail, 1 = succeed
    morph::offline::NetworkMonitor mon{
        [&] { return probeResult.load() == 1; },
        morph::offline::NetworkMonitor::Callback{},  // empty onOffline → covers 146 false arm
        morph::offline::NetworkMonitor::Callback{},  // empty onOnline → covers 155 false arm
        morph::offline::NetworkMonitor::Config{.probeInterval = 5ms, .failureThreshold = 1, .onlineThreshold = 1}};
    REQUIRE(waitFor([&] { return !mon.isOnline(); }));
    probeResult.store(1);
    REQUIRE(waitFor([&] { return mon.isOnline(); }));
}

// ── bridge.hpp:163: deregisterHandler walks past a stale weak_ptr (`!sptr` arm)

TEST_CASE("morph::bridge::Bridge: deregisterHandler skips an expired weak_ptr in _handlers", "[coverage][bridge]") {
    // Use the shared_ptr overload of registerHandler so we can drop our ref
    // and leave a stale weak inside morph::bridge::Bridge::_handlers. Then deregister a
    // different binding — the find_if predicate locks the stale weak first,
    // gets nullptr, exercises the `sptr` false arm at line 163.
    morph::exec::ThreadPoolExecutor pool{2};
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};

    {
        auto stale = std::make_shared<morph::bridge::detail::HandlerBinding>();
        stale->typeId = morph::model::ModelTraits<SubModel>::typeId();
        stale->modelFactory = [] { return morph::model::detail::ModelFactory::create<SubModel>(); };
        bridge.registerHandler(stale);
    }  // stale goes out of scope → weak in _handlers becomes expired

    auto live = std::make_shared<morph::bridge::detail::HandlerBinding>();
    live->typeId = morph::model::ModelTraits<SubModel>::typeId();
    live->modelFactory = [] { return morph::model::detail::ModelFactory::create<SubModel>(); };
    bridge.registerHandler(live);

    REQUIRE_NOTHROW(bridge.deregisterHandler(live));
}

// ── completion.hpp:142 / 176: null-state constructor + onError early-return

TEST_CASE("morph::async::Completion: null-state ctor with non-null executor is a no-op", "[coverage][completion]") {
    // The morph::async::Completion(std::shared_ptr<State>, morph::exec::IExecutor*) ctor's `if (_state)`
    // false arm fires when statePtr is null.
    SyncExecutor exec;
    morph::async::Completion<int> compNull{std::shared_ptr<morph::async::detail::CompletionState<int>>{}, &exec};
    REQUIRE(compNull.state() == nullptr);

    morph::async::Completion<std::shared_ptr<void>> compNullErased{
        std::shared_ptr<morph::async::detail::CompletionState<std::shared_ptr<void>>>{}, &exec};
    REQUIRE(compNullErased.state() == nullptr);

    // onError on a null-state morph::async::Completion exercises the false arm of `_state != nullptr`
    bool fired = false;
    compNull.then([&](int) { fired = true; }).onError([&](const std::exception_ptr&) { fired = true; });
    REQUIRE_FALSE(fired);
}

// ── backend.hpp:138: morph::backend::LocalBackend::execute with an unregistered morph::exec::detail::ModelId

TEST_CASE("morph::backend::LocalBackend: execute with an unknown morph::exec::detail::ModelId reports model-not-found",
          "[coverage][backend]") {
    // Drives the false arm of `if (iter != _models.end())` at line 138 by
    // passing a morph::exec::detail::ModelId that was never registered.
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cb;
    morph::backend::LocalBackend backend{pool};

    morph::backend::detail::ActionCall call;
    call.modelTypeId = "Cov_NotRegisteredModel";
    call.actionTypeId = "Cov_NotRegisteredAction";
    call.serializeAction = [] { return std::string{"{}"}; };
    call.deserializeResult = [](std::string_view) -> std::shared_ptr<void> { return nullptr; };
    call.localOp = [](morph::model::detail::IModelHolder&) -> std::shared_ptr<void> { return nullptr; };

    auto comp = backend.execute(morph::exec::detail::ModelId{9999U}, std::move(call), &cb);
    std::atomic<bool> errored{false};
    std::string errMsg;
    comp.onError([&](const std::exception_ptr& exc) {
        try {
            std::rethrow_exception(exc);
        } catch (const std::exception& ex) {
            errMsg = ex.what();
        }
        errored.store(true);
    });
    REQUIRE(errored.load());
    REQUIRE(errMsg.contains("model not found"));
}

// ── bridge.hpp: switchBackend purges stale weak bindings (lines 127-128)

TEST_CASE("morph::bridge::Bridge: switchBackend purges weak_ptr bindings whose owner is gone", "[coverage][bridge]") {
    // Register a binding via the shared_ptr overload, then release it before
    // switchBackend runs. The weak_ptr in _handlers cannot be locked, so the
    // `continue` arm fires (lines 126-128).
    morph::exec::ThreadPoolExecutor pool{2};
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};

    {
        auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
        binding->typeId = morph::model::ModelTraits<SubModel>::typeId();
        binding->modelFactory = [] { return morph::model::detail::ModelFactory::create<SubModel>(); };
        bridge.registerHandler(binding);
        // Drop the local ref so the weak_ptr inside the bridge expires.
    }

    REQUIRE_NOTHROW(bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool)));
}

// ── model.hpp: morph::model::detail::IModelHolder::into<Wrong>() throws std::bad_cast (lines 70-71)

TEST_CASE("morph::model::detail::IModelHolder::into<Wrong>() throws std::bad_cast", "[coverage][model]") {
    auto holder = morph::model::detail::ModelFactory::create<SubModel>();
    REQUIRE_THROWS_AS(holder->template into<CovRemoteModel>(), std::bad_cast);
    // sanity: into<Correct>() still works
    REQUIRE_NOTHROW(holder->template into<SubModel>());
}

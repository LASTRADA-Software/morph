// SPDX-License-Identifier: Apache-2.0
//
// Coverage for the ordered action log (issue #3, phase 1): action_log.hpp,
// journal.hpp, and the touched lines in model.hpp/registry.hpp/bridge.hpp —
// Loggable default-on, ActionLogPolicy::coalesce, IModelHolder::attachActionLog/
// recordIfAttached, the two real `Model::execute()` sites (ActionDispatcher's
// runner and Bridge::executeVia's localOp), SessionLog checkpoint/undo, and
// journal::replay.

#include <morph/journal/action_log.hpp>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/journal/journal.hpp>
#include <morph/core/model.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/session/session.hpp>

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "test_support.hpp"

using SyncExec = morph::testing::InlineExecutor;
using morph::journal::IActionLog;
using morph::journal::InMemoryActionLog;
using morph::journal::LogEntry;
using morph::journal::SessionLog;

// Fully-initialised LogEntry construction — entityKey/payload/result default to
// empty so call sites only spell out what a given test actually cares about.
namespace {
LogEntry makeEntry(std::string modelType, std::string entityKey, std::string actionType, std::string payload = {},
                   std::string result = {}) {
    return LogEntry{
        .seq = 0,
        .modelType = std::move(modelType),
        .entityKey = std::move(entityKey),
        .actionType = std::move(actionType),
        .payload = std::move(payload),
        .result = std::move(result),
        .principal = {},
        .timestampMs = 0,
    };
}
}  // namespace

// ── Test model, registered via the macros (global registry/dispatcher) ─────────
//
// ALDeposit/ALSetNickname default to Loggable::Yes (no 4th macro argument).
// ALGetBalance opts out explicitly — a pure query, the case the design doc
// calls out as the reason the default is "log everything, opt specific actions
// out" rather than the other way around.

struct ALDeposit {
    int amount = 0;
};
struct ALGetBalance {};
struct ALSetNickname {
    std::string name;
};
// Throws when overdrawn -- the case issue #23 is about: a rejected action must
// still leave a journal entry, not silence.
struct ALWithdraw {
    int amount = 0;
};

struct ALModel {
    int balance = 0;
    std::string nickname;
    int execute(const ALDeposit& a) {
        balance += a.amount;
        return balance;
    }
    int execute(const ALGetBalance& /*a*/) { return balance; }
    std::string execute(const ALSetNickname& a) {
        nickname = a.name;
        return nickname;
    }
    int execute(const ALWithdraw& a) {
        if (a.amount > balance) {
            throw std::runtime_error("insufficient funds");
        }
        balance -= a.amount;
        return balance;
    }
};

// SetNickname is the "same field edited repeatedly" case — only the latest
// occurrence should survive a checkpoint. Must be visible before the
// BRIDGE_REGISTER_ACTION call below (explicit specialisations must precede the
// point where registerAction<ALModel, ALSetNickname> is implicitly instantiated).
template <>
struct morph::model::ActionLogPolicy<ALSetNickname> {
    static constexpr bool coalesce = true;
};

BRIDGE_REGISTER_MODEL(ALModel, "AL_Model")
BRIDGE_REGISTER_ACTION(ALModel, ALDeposit, "AL_Deposit")
BRIDGE_REGISTER_ACTION(ALModel, ALGetBalance, "AL_GetBalance", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(ALModel, ALSetNickname, "AL_SetNickname")
BRIDGE_REGISTER_ACTION(ALModel, ALWithdraw, "AL_Withdraw")

// ── Hand-written ActionTraits, no `loggable` member — mirrors test_dispatch_di.cpp.
// Exercises actionLoggable<A>()'s "member absent" branch: defaults to Yes without
// requiring every pre-existing manual ActionTraits specialisation to be touched.

struct ALLegacyAction {
    int x = 0;
};
struct ALLegacyModel {
    int execute(const ALLegacyAction& a) { return a.x; }
};

template <>
struct morph::model::ActionTraits<ALLegacyAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "AL_LegacyAction"; }
    static std::string toJson(const ALLegacyAction& a) { return R"({"x":)" + std::to_string(a.x) + "}"; }
    static ALLegacyAction fromJson(std::string_view json) {
        ALLegacyAction action{};
        auto pos = json.find(':');
        if (pos != std::string_view::npos) {
            action.x = std::stoi(std::string{json.substr(pos + 1)});
        }
        return action;
    }
    static std::string resultToJson(const int& r) { return std::to_string(r); }
    static int resultFromJson(std::string_view s) { return std::stoi(std::string{s}); }
};
template <>
struct morph::model::ModelTraits<ALLegacyModel> {
    static constexpr std::string_view typeId() { return "AL_LegacyModel"; }
};

// A model that reads morph::journal::isReplaying() from inside execute() --
// the shape Phase 6's rules engine will use to suppress rule evaluation.
struct RMModel {
    bool sawReplayingDuringExecute = false;
    int execute(const ALDeposit& a) {
        sawReplayingDuringExecute = morph::journal::isReplaying();
        return a.amount;
    }
};
template <>
struct morph::model::ModelTraits<RMModel> {
    static constexpr std::string_view typeId() { return "RM_Model"; }
};

// ── InMemoryActionLog ────────────────────────────────────────────────────────

TEST_CASE("morph::journal::InMemoryActionLog: append assigns increasing seq, preserves order", "[action_log]") {
    InMemoryActionLog log;
    log.append(makeEntry("M", "", "A1"));
    log.append(makeEntry("M", "", "A2"));
    auto all = log.entries();
    REQUIRE(all.size() == 2);
    REQUIRE(all[0].actionType == "A1");
    REQUIRE(all[0].seq == 1);
    REQUIRE(all[1].actionType == "A2");
    REQUIRE(all[1].seq == 2);
}

TEST_CASE("morph::journal::InMemoryActionLog: entries(entityKey) filters, empty key returns all", "[action_log]") {
    InMemoryActionLog log;
    log.append(makeEntry("M", "acct-1", "A"));
    log.append(makeEntry("M", "acct-2", "A"));
    log.append(makeEntry("M", "acct-1", "B"));

    REQUIRE(log.entries().size() == 3);
    auto acct1 = log.entries("acct-1");
    REQUIRE(acct1.size() == 2);
    REQUIRE(acct1[0].actionType == "A");
    REQUIRE(acct1[1].actionType == "B");
    REQUIRE(log.entries("no-such-account").empty());
}

TEST_CASE("morph::journal::InMemoryActionLog: flush is a callable no-op", "[action_log]") {
    InMemoryActionLog log;
    log.append(makeEntry("M", "", "A"));
    log.flush();
    REQUIRE(log.entries().size() == 1);
}

// ── Loggable default / opt-out, actionLoggable<A>() ─────────────────────────

TEST_CASE("Loggable: macro defaults to Yes, explicit 4th argument opts out", "[action_log][loggable]") {
    STATIC_REQUIRE(morph::model::ActionTraits<ALDeposit>::loggable == morph::model::Loggable::Yes);
    STATIC_REQUIRE(morph::model::ActionTraits<ALSetNickname>::loggable == morph::model::Loggable::Yes);
    STATIC_REQUIRE(morph::model::ActionTraits<ALGetBalance>::loggable == morph::model::Loggable::No);
}

TEST_CASE("actionLoggable<A>(): defaults to Yes when ActionTraits has no loggable member", "[action_log][loggable]") {
    // Runtime REQUIRE (not STATIC_REQUIRE) so the call is actually emitted and
    // instrumented — a compile-time-only check would never show up in coverage.
    auto legacy = morph::model::detail::actionLoggable<ALLegacyAction>();
    auto deposit = morph::model::detail::actionLoggable<ALDeposit>();
    auto getBalance = morph::model::detail::actionLoggable<ALGetBalance>();
    REQUIRE(legacy == morph::model::Loggable::Yes);
    REQUIRE(deposit == morph::model::Loggable::Yes);
    REQUIRE(getBalance == morph::model::Loggable::No);
}

// ── IModelHolder::attachActionLog / hasActionLog / recordIfAttached ─────────

TEST_CASE("IModelHolder: recordIfAttached is a no-op with no log attached", "[action_log][holder]") {
    auto holder = morph::model::detail::ModelFactory::create<ALModel>();
    REQUIRE_FALSE(holder->hasActionLog());
    holder->recordIfAttached(makeEntry("AL_Model", "", "AL_Deposit"));  // must not throw
    SUCCEED("no crash with no log attached");
}

TEST_CASE("IModelHolder: attachActionLog stamps entityKey and timestamp automatically", "[action_log][holder]") {
    auto holder = morph::model::detail::ModelFactory::create<ALModel>();
    auto log = std::make_shared<InMemoryActionLog>();
    holder->attachActionLog(log, "acct-42");
    REQUIRE(holder->hasActionLog());

    holder->recordIfAttached(makeEntry("AL_Model", "", "AL_Deposit", "{\"amount\":5}", "5"));

    auto entries = log->entries();
    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].entityKey == "acct-42");
    REQUIRE(entries[0].payload == "{\"amount\":5}");
    REQUIRE(entries[0].result == "5");
    REQUIRE(entries[0].principal.empty());
    REQUIRE(entries[0].timestampMs > 0);
}

// ── ModelHolder<Model>::onActionLogAttached forwarding ──────────────────────
//
// ALModel above has no model-level attachActionLog of its own, so every test
// above exercises only the ModelLevelActionLogAttachable<Model> == false arm
// of ModelHolder<Model>::onActionLogAttached's `if constexpr`. ALLoggingModel
// declares one matching the concept's exact shape, to exercise the == true
// arm: IModelHolder::attachActionLog forwarding log/contextKey to the
// wrapped model's own attachActionLog, the mechanism a keyed/shared model
// (e.g. kanban::BoardModel) needs to read its own journal back for an
// activity-stream-shaped view, since the type-erased holder's _actionLog is
// otherwise never visible to Model::execute() itself.

struct ALLoggingDeposit {
    int amount = 0;
};

struct ALLoggingModel {
    std::shared_ptr<IActionLog> log;
    std::string contextKey;
    int callCount = 0;

    void attachActionLog(std::shared_ptr<IActionLog> attachedLog, std::string attachedContextKey) {
        log = std::move(attachedLog);
        contextKey = std::move(attachedContextKey);
        ++callCount;
    }

    int execute(const ALLoggingDeposit& a) { return a.amount; }
};

BRIDGE_REGISTER_MODEL(ALLoggingModel, "AL_LoggingModel")
BRIDGE_REGISTER_ACTION(ALLoggingModel, ALLoggingDeposit, "AL_LoggingDeposit")

TEST_CASE("ModelHolder<Model>::onActionLogAttached forwards to a model-level attachActionLog when the model "
          "declares one matching ModelLevelActionLogAttachable",
          "[action_log][holder]") {
    static_assert(morph::model::detail::ModelLevelActionLogAttachable<ALLoggingModel>);
    static_assert(!morph::model::detail::ModelLevelActionLogAttachable<ALModel>);

    auto holder = morph::model::detail::ModelFactory::create<ALLoggingModel>();
    auto log = std::make_shared<InMemoryActionLog>();
    holder->attachActionLog(log, "board-7");

    // The holder's own auto-append state (recordIfAttached/hasActionLog)
    // still works exactly as before -- this hook adds a second, independent
    // forward, it doesn't replace the holder's own bookkeeping.
    REQUIRE(holder->hasActionLog());

    auto& typed = static_cast<morph::model::detail::ModelHolder<ALLoggingModel>&>(*holder);
    REQUIRE(typed.model.callCount == 1);
    REQUIRE(typed.model.log == log);
    REQUIRE(typed.model.contextKey == "board-7");
}

TEST_CASE("IModelHolder::onActionLogAttached: the no-op default runs when a holder wraps a model with no "
          "model-level attachActionLog",
          "[action_log][holder]") {
    // ALModel doesn't declare attachActionLog, so ModelHolder<ALModel>::
    // onActionLogAttached resolves its `if constexpr` false and never calls
    // into the model -- attachActionLog must still succeed and populate the
    // holder's own state exactly as it did before this hook existed.
    auto holder = morph::model::detail::ModelFactory::create<ALModel>();
    auto log = std::make_shared<InMemoryActionLog>();
    REQUIRE_NOTHROW(holder->attachActionLog(log, "acct-noop"));
    REQUIRE(holder->hasActionLog());
}

TEST_CASE("IModelHolder: recordIfAttached captures the active session principal", "[action_log][holder]") {
    auto holder = morph::model::detail::ModelFactory::create<ALModel>();
    auto log = std::make_shared<InMemoryActionLog>();
    holder->attachActionLog(log, "acct-7");

    ::morph::session::Context ctx;
    ctx.principal = "alice";
    {
        ::morph::session::detail::ScopedContext scoped{ctx};
        holder->recordIfAttached(makeEntry("AL_Model", "", "AL_Deposit"));
    }
    holder->recordIfAttached(makeEntry("AL_Model", "", "AL_Deposit"));  // outside the scope

    auto entries = log->entries();
    REQUIRE(entries.size() == 2);
    REQUIRE(entries[0].principal == "alice");
    REQUIRE(entries[1].principal.empty());
}

// ── ActionDispatcher: the server-side execution site (registry.hpp) ────────
//
// This is the exact code path RemoteServer::dispatchExecute uses for every
// remote/Qt topology — exercised directly here (isolated dispatcher/registry,
// matching test_dispatch_di.cpp's style) rather than through a live socket.

TEST_CASE("ActionDispatcher: records loggable actions, skips opted-out ones, tracks coalesce", "[action_log][dispatch]") {
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    registry.registerModel<ALModel>("AL_Model");
    dispatcher.registerAction<ALModel, ALDeposit>("AL_Model", "AL_Deposit");
    dispatcher.registerAction<ALModel, ALGetBalance>("AL_Model", "AL_GetBalance");
    dispatcher.registerAction<ALModel, ALSetNickname>("AL_Model", "AL_SetNickname");

    REQUIRE_FALSE(dispatcher.coalesce("AL_Model", "AL_Deposit"));
    REQUIRE(dispatcher.coalesce("AL_Model", "AL_SetNickname"));
    REQUIRE_FALSE(dispatcher.coalesce("AL_Model", "no-such-action"));

    auto holder = registry.create("AL_Model");
    auto log = std::make_shared<InMemoryActionLog>();
    holder->attachActionLog(log, "acct-1");

    auto depositJson = morph::model::ActionTraits<ALDeposit>::toJson(ALDeposit{.amount = 10});
    REQUIRE(dispatcher.dispatch("AL_Model", "AL_Deposit", *holder, depositJson) == "10");
    REQUIRE(dispatcher.dispatch("AL_Model", "AL_GetBalance", *holder, "{}") == "10");

    auto entries = log->entries();
    REQUIRE(entries.size() == 1);  // GetBalance opted out — not recorded
    REQUIRE(entries[0].modelType == "AL_Model");
    REQUIRE(entries[0].actionType == "AL_Deposit");
    REQUIRE(entries[0].entityKey == "acct-1");
    REQUIRE(entries[0].payload == depositJson);
    REQUIRE(entries[0].result == "10");
    REQUIRE(entries[0].outcome == morph::journal::Outcome::Succeeded);
    REQUIRE(entries[0].error.empty());
}

TEST_CASE("ActionDispatcher: dispatch against a holder with no log attached does not crash", "[action_log][dispatch]") {
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    registry.registerModel<ALModel>("AL_Model");
    dispatcher.registerAction<ALModel, ALDeposit>("AL_Model", "AL_Deposit");

    auto holder = registry.create("AL_Model");
    REQUIRE_FALSE(holder->hasActionLog());
    auto depositJson = morph::model::ActionTraits<ALDeposit>::toJson(ALDeposit{.amount = 3});
    REQUIRE(dispatcher.dispatch("AL_Model", "AL_Deposit", *holder, depositJson) == "3");
}

TEST_CASE("ActionDispatcher: records outcome=Failed with error text when Model::execute throws, still rethrows",
         "[action_log][dispatch][issue23]") {
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    registry.registerModel<ALModel>("AL_Model");
    dispatcher.registerAction<ALModel, ALWithdraw>("AL_Model", "AL_Withdraw");

    auto holder = registry.create("AL_Model");
    auto log = std::make_shared<InMemoryActionLog>();
    holder->attachActionLog(log, "acct-fail");

    auto withdrawJson = morph::model::ActionTraits<ALWithdraw>::toJson(ALWithdraw{.amount = 50});
    REQUIRE_THROWS_AS(dispatcher.dispatch("AL_Model", "AL_Withdraw", *holder, withdrawJson), std::runtime_error);

    auto entries = log->entries();
    REQUIRE(entries.size() == 1);  // the rejected attempt is still journaled
    REQUIRE(entries[0].modelType == "AL_Model");
    REQUIRE(entries[0].actionType == "AL_Withdraw");
    REQUIRE(entries[0].entityKey == "acct-fail");
    REQUIRE(entries[0].payload == withdrawJson);
    REQUIRE(entries[0].result.empty());
    REQUIRE(entries[0].outcome == morph::journal::Outcome::Failed);
    REQUIRE(entries[0].error == "insufficient funds");
}

TEST_CASE("ActionDispatcher: runner records for hand-written ActionTraits with no loggable member",
         "[action_log][dispatch]") {
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    registry.registerModel<ALLegacyModel>("AL_LegacyModel");
    dispatcher.registerAction<ALLegacyModel, ALLegacyAction>("AL_LegacyModel", "AL_LegacyAction");

    auto holder = registry.create("AL_LegacyModel");
    auto log = std::make_shared<InMemoryActionLog>();
    holder->attachActionLog(log, "legacy-1");

    REQUIRE(dispatcher.dispatch("AL_LegacyModel", "AL_LegacyAction", *holder, R"({"x":9})") == "9");
    REQUIRE(log->entries().size() == 1);
}

// ── Bridge::executeVia's localOp — the local-mode execution site (bridge.hpp) ──

TEST_CASE("Bridge/LocalBackend: local-mode execution records loggable actions, skips opted-out ones",
         "[action_log][bridge]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};

    auto log = std::make_shared<InMemoryActionLog>();
    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = "AL_Model";
    binding->modelFactory = [log] {
        auto holder = morph::model::detail::ModelFactory::create<ALModel>();
        holder->attachActionLog(log, "acct-99");
        return holder;
    };
    morph::bridge::BridgeHandler<ALModel> handler{bridge, &cbExec, binding};

    std::atomic<int> depositResult{-1};
    handler.execute(ALDeposit{.amount = 20})
        .then([&](int v) { depositResult.store(v); })
        .onError([](const std::exception_ptr&) {});
    REQUIRE(morph::testing::waitUntil([&] { return depositResult.load() != -1; }));
    REQUIRE(depositResult.load() == 20);

    std::atomic<int> balanceResult{-1};
    handler.execute(ALGetBalance{})
        .then([&](int v) { balanceResult.store(v); })
        .onError([](const std::exception_ptr&) {});
    REQUIRE(morph::testing::waitUntil([&] { return balanceResult.load() != -1; }));

    auto entries = log->entries();
    REQUIRE(entries.size() == 1);  // GetBalance opted out
    REQUIRE(entries[0].actionType == "AL_Deposit");
    REQUIRE(entries[0].entityKey == "acct-99");
}

TEST_CASE("Bridge/LocalBackend: local-mode execution records outcome=Failed when Model::execute throws",
         "[action_log][bridge][issue23]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};

    auto log = std::make_shared<InMemoryActionLog>();
    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = "AL_Model";
    binding->modelFactory = [log] {
        auto holder = morph::model::detail::ModelFactory::create<ALModel>();
        holder->attachActionLog(log, "acct-fail-local");
        return holder;
    };
    morph::bridge::BridgeHandler<ALModel> handler{bridge, &cbExec, binding};

    std::atomic<bool> errored{false};
    handler.execute(ALWithdraw{.amount = 50})
        .then([&](int) {})
        .onError([&](const std::exception_ptr&) { errored.store(true); });
    REQUIRE(morph::testing::waitUntil([&] { return errored.load(); }));

    auto entries = log->entries();
    REQUIRE(entries.size() == 1);  // the rejected attempt is still journaled
    REQUIRE(entries[0].actionType == "AL_Withdraw");
    REQUIRE(entries[0].entityKey == "acct-fail-local");
    REQUIRE(entries[0].result.empty());
    REQUIRE(entries[0].outcome == morph::journal::Outcome::Failed);
    REQUIRE(entries[0].error == "insufficient funds");
}

TEST_CASE("Bridge/LocalBackend: local-mode execution without an attached log does not crash",
         "[action_log][bridge]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<ALModel> handler{bridge, &cbExec};  // default factory — no log

    std::atomic<int> result{-1};
    handler.execute(ALDeposit{.amount = 4}).then([&](int v) { result.store(v); }).onError([](const std::exception_ptr&) {
    });
    REQUIRE(morph::testing::waitUntil([&] { return result.load() != -1; }));
    REQUIRE(result.load() == 4);
}

// ── SimulatedRemoteBackend: recording never happens on the client side ─────
//
// SimulatedRemoteBackend::registerModel's factory parameter is documented as
// ignored — the server's own ModelRegistryFactory constructs the instance.
// This proves that guarantee holds for logging specifically: a log attached
// via the client's HandlerBinding factory is never populated, because the
// factory itself is never even invoked for a remote backend.

TEST_CASE("SimulatedRemoteBackend: client-side factory (and its attached log) is never invoked",
         "[action_log][remote]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExec cbExec;

    morph::model::detail::ModelRegistryFactory serverRegistry;
    morph::model::detail::ActionDispatcher serverDispatcher;
    serverRegistry.registerModel<ALModel>("AL_Model");
    serverDispatcher.registerAction<ALModel, ALDeposit>("AL_Model", "AL_Deposit");

    auto server = std::make_shared<morph::backend::RemoteServer>(pool, serverDispatcher, serverRegistry);
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server)};

    std::atomic<bool> factoryCalled{false};
    auto clientLog = std::make_shared<InMemoryActionLog>();
    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = "AL_Model";
    binding->modelFactory = [&factoryCalled, clientLog] {
        factoryCalled.store(true);
        auto holder = morph::model::detail::ModelFactory::create<ALModel>();
        holder->attachActionLog(clientLog, "acct-client");
        return holder;
    };
    morph::bridge::BridgeHandler<ALModel> handler{bridge, &cbExec, binding};

    std::atomic<int> result{-1};
    handler.execute(ALDeposit{.amount = 7}).then([&](int v) { result.store(v); }).onError([](const std::exception_ptr&) {
    });
    REQUIRE(morph::testing::waitUntil([&] { return result.load() != -1; }));
    REQUIRE(result.load() == 7);  // executed correctly, server-side, with no log attached there

    REQUIRE_FALSE(factoryCalled.load());
    REQUIRE(clientLog->entries().empty());
}

// ── journal::replay ──────────────────────────────────────────────────────────

TEST_CASE("journal::replay: reconstructs state by re-executing entries in order", "[action_log][journal]") {
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    registry.registerModel<ALModel>("AL_Model");
    dispatcher.registerAction<ALModel, ALDeposit>("AL_Model", "AL_Deposit");

    std::vector<LogEntry> entries{
        makeEntry("AL_Model", "", "AL_Deposit", morph::model::ActionTraits<ALDeposit>::toJson(ALDeposit{.amount = 10})),
        makeEntry("AL_Model", "", "AL_Deposit", morph::model::ActionTraits<ALDeposit>::toJson(ALDeposit{.amount = 5})),
    };

    auto holder = morph::journal::replay("AL_Model", entries, registry, dispatcher);
    REQUIRE(holder->into<ALModel>().balance == 15);
}

TEST_CASE("journal::replay: skips Failed entries instead of re-dispatching them", "[action_log][journal][issue23]") {
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    registry.registerModel<ALModel>("AL_Model");
    dispatcher.registerAction<ALModel, ALDeposit>("AL_Model", "AL_Deposit");
    dispatcher.registerAction<ALModel, ALWithdraw>("AL_Model", "AL_Withdraw");

    auto depositEntry =
        makeEntry("AL_Model", "", "AL_Deposit", morph::model::ActionTraits<ALDeposit>::toJson(ALDeposit{.amount = 10}));
    // A Failed entry for an over-large withdrawal: if replay() dispatched this
    // (instead of skipping it), ALModel::execute would throw the very same
    // "insufficient funds" again, aborting reconstruction.
    auto failedWithdraw =
        makeEntry("AL_Model", "", "AL_Withdraw", morph::model::ActionTraits<ALWithdraw>::toJson(ALWithdraw{.amount = 999}));
    failedWithdraw.outcome = morph::journal::Outcome::Failed;
    failedWithdraw.error = "insufficient funds";

    std::vector<LogEntry> entries{depositEntry, failedWithdraw};

    std::unique_ptr<morph::model::detail::IModelHolder> holder;
    REQUIRE_NOTHROW(holder = morph::journal::replay("AL_Model", entries, registry, dispatcher));
    REQUIRE(holder->into<ALModel>().balance == 10);  // only the successful deposit was replayed
}

TEST_CASE("journal::replay: propagates the registry's unknown-model error", "[action_log][journal]") {
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    REQUIRE_THROWS_AS(morph::journal::replay("NoSuchModel", {}, registry, dispatcher), std::runtime_error);
}

// ── journal::SessionLog ──────────────────────────────────────────────────────

TEST_CASE("SessionLog: append/entries/flush behave like InMemoryActionLog", "[action_log][journal]") {
    SessionLog log;
    log.append(makeEntry("M", "e1", "A"));
    log.append(makeEntry("M", "e2", "A"));
    log.flush();
    REQUIRE(log.entries().size() == 2);
    REQUIRE(log.entries("e1").size() == 1);
    REQUIRE(log.entries("nope").empty());
}

TEST_CASE("SessionLog::checkpoint: coalesces by (modelType, entityKey, actionType), keeps distinct actions",
         "[action_log][journal]") {
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    registry.registerModel<ALModel>("AL_Model");
    dispatcher.registerAction<ALModel, ALDeposit>("AL_Model", "AL_Deposit");
    dispatcher.registerAction<ALModel, ALSetNickname>("AL_Model", "AL_SetNickname");

    SessionLog session;
    session.append(makeEntry("AL_Model", "acct-1", "AL_Deposit", "", "10"));
    session.append(makeEntry("AL_Model", "acct-1", "AL_SetNickname", "", "bob"));
    session.append(makeEntry("AL_Model", "acct-1", "AL_Deposit", "", "15"));
    session.append(makeEntry("AL_Model", "acct-1", "AL_SetNickname", "", "bobby"));

    InMemoryActionLog durable;
    session.checkpoint(durable, dispatcher);

    auto out = durable.entries();
    REQUIRE(out.size() == 3);  // two Deposits kept distinct, two SetNickname coalesced into one
    REQUIRE(out[0].actionType == "AL_Deposit");
    REQUIRE(out[0].result == "10");
    REQUIRE(out[1].actionType == "AL_SetNickname");
    REQUIRE(out[1].result == "bobby");  // latest occurrence, original (first-seen) position
    REQUIRE(out[2].actionType == "AL_Deposit");
    REQUIRE(out[2].result == "15");
}

TEST_CASE("SessionLog::checkpoint: no-op when nothing new since the last checkpoint", "[action_log][journal]") {
    morph::model::detail::ActionDispatcher dispatcher;
    SessionLog session;
    session.append(makeEntry("M", "e", "A"));

    InMemoryActionLog durable;
    session.checkpoint(durable, dispatcher);
    REQUIRE(durable.entries().size() == 1);

    session.checkpoint(durable, dispatcher);  // nothing appended since — must not duplicate
    REQUIRE(durable.entries().size() == 1);
}

TEST_CASE("SessionLog::undoLast: replays the prefix, reconstructing pre-undo state", "[action_log][journal]") {
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    registry.registerModel<ALModel>("AL_Model");
    dispatcher.registerAction<ALModel, ALDeposit>("AL_Model", "AL_Deposit");

    auto session = std::make_shared<SessionLog>();
    auto holder = registry.create("AL_Model");
    holder->attachActionLog(session, "acct-undo");

    auto deposit10 = morph::model::ActionTraits<ALDeposit>::toJson(ALDeposit{.amount = 10});
    auto deposit5 = morph::model::ActionTraits<ALDeposit>::toJson(ALDeposit{.amount = 5});
    dispatcher.dispatch("AL_Model", "AL_Deposit", *holder, deposit10);
    dispatcher.dispatch("AL_Model", "AL_Deposit", *holder, deposit5);
    REQUIRE(holder->into<ALModel>().balance == 15);
    REQUIRE(session->entries().size() == 2);

    auto afterUndo1 = session->undoLast("AL_Model", registry, dispatcher);
    REQUIRE(afterUndo1->into<ALModel>().balance == 10);
    REQUIRE(session->entries().size() == 1);

    auto afterUndo2 = session->undoLast("AL_Model", registry, dispatcher);
    REQUIRE(afterUndo2->into<ALModel>().balance == 0);
    REQUIRE(session->entries().empty());

    // Undo on an already-empty log is a no-op that still returns a fresh holder.
    auto afterUndo3 = session->undoLast("AL_Model", registry, dispatcher);
    REQUIRE(afterUndo3->into<ALModel>().balance == 0);
}

TEST_CASE("SessionLog::undoLast: clamps the checkpoint position when undoing past it",
         "[action_log][journal]") {
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    registry.registerModel<ALModel>("AL_Model");
    dispatcher.registerAction<ALModel, ALDeposit>("AL_Model", "AL_Deposit");

    auto session = std::make_shared<SessionLog>();
    auto holder = registry.create("AL_Model");
    holder->attachActionLog(session, "acct-clamp");

    auto deposit1 = morph::model::ActionTraits<ALDeposit>::toJson(ALDeposit{.amount = 1});
    dispatcher.dispatch("AL_Model", "AL_Deposit", *holder, deposit1);
    dispatcher.dispatch("AL_Model", "AL_Deposit", *holder, deposit1);

    InMemoryActionLog durable;
    session->checkpoint(durable, dispatcher);  // committedUpTo == 2
    REQUIRE(durable.entries().size() == 2);

    session->undoLast("AL_Model", registry, dispatcher);  // pops entry #2 — committedUpTo must clamp to 1
    REQUIRE(session->entries().size() == 1);

    // A subsequent checkpoint must see nothing pending (checkpoint already
    // covered the one remaining entry before the undo) rather than re-sending it.
    session->checkpoint(durable, dispatcher);
    REQUIRE(durable.entries().size() == 2);
}

// ── Bug B: undoLast after a coalescing checkpoint must not resurrect a
// forwarded entry ────────────────────────────────────────────────────────────
//
// SetNickname coalesces, so a checkpoint over [SetNickname(a), SetNickname(b)]
// forwards a single entry (b). The full-fidelity history still holds two raw
// entries. undoLast pops the raw tail, but a subsequent checkpoint must never
// re-forward the coalesced-away entry `a`: durable state stays monotonic.

TEST_CASE("SessionLog::undoLast: a coalescing checkpoint never re-forwards an undone entry",
          "[action_log][journal]") {
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    registry.registerModel<ALModel>("AL_Model");
    dispatcher.registerAction<ALModel, ALSetNickname>("AL_Model", "AL_SetNickname");

    auto nickA = morph::model::ActionTraits<ALSetNickname>::toJson(ALSetNickname{.name = "a"});
    auto nickB = morph::model::ActionTraits<ALSetNickname>::toJson(ALSetNickname{.name = "b"});

    auto session = std::make_shared<SessionLog>();
    session->append(makeEntry("AL_Model", "acct-b", "AL_SetNickname", nickA, "a"));
    session->append(makeEntry("AL_Model", "acct-b", "AL_SetNickname", nickB, "b"));

    InMemoryActionLog durable;
    session->checkpoint(durable, dispatcher);  // coalesces [a,b] -> forwards one entry (b)
    REQUIRE(durable.entries().size() == 1);
    REQUIRE(durable.entries()[0].result == "b");

    // undoLast pops the raw tail (b), leaving one raw entry (a) in history.
    session->undoLast("AL_Model", registry, dispatcher);
    REQUIRE(session->entries().size() == 1);

    // The coalescing batch already committed the SetNickname; `a` must NOT be
    // re-forwarded — durable state is forward-only and never regresses.
    session->checkpoint(durable, dispatcher);
    REQUIRE(durable.entries().size() == 1);  // still just the one committed entry
    REQUIRE(durable.entries()[0].result == "b");

    // A genuinely new action appended after the undo must still be forwarded
    // exactly once. This is the case a raw-index watermark gets wrong: the new
    // entry reuses a vector index that a coalesced-away, already-committed entry
    // previously occupied, so an index-based watermark would wrongly skip it (or
    // an over-eager clamp would re-forward the old one). Committed state must be
    // tracked by entry identity, not by position in the mutable history.
    auto nickC = morph::model::ActionTraits<ALSetNickname>::toJson(ALSetNickname{.name = "c"});
    session->append(makeEntry("AL_Model", "acct-b", "AL_SetNickname", nickC, "c"));
    session->checkpoint(durable, dispatcher);

    auto out = durable.entries();
    REQUIRE(out.size() == 2);       // the original committed entry + the new one
    REQUIRE(out[0].result == "b");  // unchanged, never regressed
    REQUIRE(out[1].result == "c");  // the new entry, forwarded exactly once
}

// ── Bug A: concurrent checkpoint() must forward in append order ──────────────
//
// Two threads checkpoint the same SessionLog concurrently. Each grabs a
// disjoint pending slice under the lock; the forward phase must be serialized
// so entries reach the durable sink in strictly nondecreasing append order —
// the slice taken first is forwarded first, with no interleaving.

namespace {
/// Durable sink that records the (original, pre-restamp) `seq` of every entry it
/// receives, in the exact order append() is called. Used to assert the forward
/// phase preserves append order under concurrency.
class OrderRecordingSink : public IActionLog {
public:
    void append(LogEntry entry) override {
        std::scoped_lock const lock{_mtx};
        _received.push_back(entry.seq);
    }
    void flush() override {}
    [[nodiscard]] std::vector<LogEntry> entries(std::string_view = {}) const override { return {}; }
    [[nodiscard]] std::vector<uint64_t> received() const {
        std::scoped_lock const lock{_mtx};
        return _received;
    }

private:
    mutable std::mutex _mtx;
    std::vector<uint64_t> _received;
};
}  // namespace

TEST_CASE("SessionLog::checkpoint: concurrent checkpoints forward in strictly nondecreasing append order",
          "[action_log][journal]") {
    morph::model::detail::ActionDispatcher dispatcher;  // no actions registered -> nothing coalesces
    auto session = std::make_shared<SessionLog>();

    constexpr int kEntries = 2000;
    for (int i = 0; i < kEntries; ++i) {
        session->append(makeEntry("M", "e", "A"));  // seq runs 1..kEntries in append order
    }

    OrderRecordingSink sink;
    std::atomic<bool> go{false};
    auto worker = [&] {
        while (!go.load()) { /* spin until both threads are ready */
        }
        session->checkpoint(sink, dispatcher);
    };
    std::thread t1{worker};
    std::thread t2{worker};
    go.store(true);
    t1.join();
    t2.join();

    auto got = sink.received();
    REQUIRE(got.size() == static_cast<std::size_t>(kEntries));  // every entry forwarded exactly once
    REQUIRE(std::is_sorted(got.begin(), got.end()));            // and in strictly append order
}

// ── setActionLog / defaultActionLog / ScopedActionLog ───────────────────────
//
// morph_tests runs every TEST_CASE in one process, and the default action log
// is process-wide global state — every test here uses ScopedActionLog so its
// override never leaks into a test that runs after it, regardless of order.

TEST_CASE("ScopedActionLog: installs and restores the default action log", "[action_log][default]") {
    REQUIRE(morph::journal::defaultActionLog() == nullptr);
    auto log = std::make_shared<InMemoryActionLog>();
    {
        morph::journal::ScopedActionLog guard{log};
        REQUIRE(morph::journal::defaultActionLog() == log);
    }
    REQUIRE(morph::journal::defaultActionLog() == nullptr);
}

TEST_CASE("ScopedActionLog: nested scopes restore in the correct order", "[action_log][default]") {
    auto outer = std::make_shared<InMemoryActionLog>();
    auto inner = std::make_shared<InMemoryActionLog>();
    morph::journal::ScopedActionLog outerGuard{outer};
    REQUIRE(morph::journal::defaultActionLog() == outer);
    {
        morph::journal::ScopedActionLog innerGuard{inner};
        REQUIRE(morph::journal::defaultActionLog() == inner);
    }
    REQUIRE(morph::journal::defaultActionLog() == outer);
}

TEST_CASE("ModelFactory::create: auto-attaches the default action log when one is installed",
         "[action_log][default]") {
    auto log = std::make_shared<InMemoryActionLog>();
    morph::journal::ScopedActionLog guard{log};

    auto holder = morph::model::detail::ModelFactory::create<ALModel>();
    REQUIRE(holder->hasActionLog());

    morph::model::detail::ActionDispatcher dispatcher;
    dispatcher.registerAction<ALModel, ALDeposit>("AL_Model", "AL_Deposit");
    auto depositJson = morph::model::ActionTraits<ALDeposit>::toJson(ALDeposit{.amount = 12});
    REQUIRE(dispatcher.dispatch("AL_Model", "AL_Deposit", *holder, depositJson) == "12");

    auto entries = log->entries();
    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].entityKey.empty());  // auto-attach uses an empty entityKey
    REQUIRE(entries[0].actionType == "AL_Deposit");
}

TEST_CASE("ModelFactory::create: does not attach a log when no default is installed", "[action_log][default]") {
    REQUIRE(morph::journal::defaultActionLog() == nullptr);
    auto holder = morph::model::detail::ModelFactory::create<ALModel>();
    REQUIRE_FALSE(holder->hasActionLog());
}

TEST_CASE("IModelHolder::attachActionLog: an explicit call overrides the auto-attached default",
         "[action_log][default]") {
    auto defaultLog = std::make_shared<InMemoryActionLog>();
    morph::journal::ScopedActionLog guard{defaultLog};

    auto holder = morph::model::detail::ModelFactory::create<ALModel>();  // auto-attaches defaultLog
    auto specificLog = std::make_shared<InMemoryActionLog>();
    holder->attachActionLog(specificLog, "acct-override");  // explicit call wins

    morph::model::detail::ActionDispatcher dispatcher;
    dispatcher.registerAction<ALModel, ALDeposit>("AL_Model", "AL_Deposit");
    auto depositJson = morph::model::ActionTraits<ALDeposit>::toJson(ALDeposit{.amount = 3});
    dispatcher.dispatch("AL_Model", "AL_Deposit", *holder, depositJson);

    REQUIRE(defaultLog->entries().empty());
    auto entries = specificLog->entries();
    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].entityKey == "acct-override");
}

TEST_CASE("ModelFactory::create: auto-attach also reaches server-created holders (ModelRegistryFactory)",
         "[action_log][default]") {
    auto log = std::make_shared<InMemoryActionLog>();
    morph::journal::ScopedActionLog guard{log};

    // ModelRegistryFactory::registerModel<Model> is exactly what RemoteServer
    // uses to construct instances for every remote/simulated-remote client —
    // it routes through the same ModelFactory::create<Model>(), so the global
    // default reaches server-side instances with no contextKey/LogProvider
    // wiring needed.
    morph::model::detail::ModelRegistryFactory registry;
    morph::model::detail::ActionDispatcher dispatcher;
    registry.registerModel<ALModel>("AL_Model");
    dispatcher.registerAction<ALModel, ALDeposit>("AL_Model", "AL_Deposit");

    auto holder = registry.create("AL_Model");
    REQUIRE(holder->hasActionLog());
    auto depositJson = morph::model::ActionTraits<ALDeposit>::toJson(ALDeposit{.amount = 7});
    dispatcher.dispatch("AL_Model", "AL_Deposit", *holder, depositJson);

    REQUIRE(log->entries().size() == 1);
}

// ── LogEntry::causalParentId ─────────────────────────────────────────────────

TEST_CASE("LogEntry::causalParentId defaults to empty and round-trips through toJson/fromJson",
          "[action_log][causal]") {
    LogEntry entry = makeEntry("AL_Model", "acct-1", "AL_Deposit");
    REQUIRE(entry.causalParentId.empty());  // sentinel for "no parent", mirroring idempotencyKey's empty default

    entry.causalParentId = "cause-123";
    const auto json = morph::journal::toJson(entry);
    const auto decoded = morph::journal::fromJson(json);
    REQUIRE(decoded.causalParentId == "cause-123");
}

TEST_CASE("LogEntry::causalParentId is additive: a legacy line missing the key decodes with the empty default",
          "[action_log][causal]") {
    // A pre-existing on-disk line written before causalParentId existed has no
    // such key -- fromJson's leniency (error_on_unknown_keys = false plus every
    // absent key falling back to its member default) must still decode it, the
    // same guarantee `v`/`idempotencyKey` already document.
    const std::string legacyLine =
        R"({"seq":1,"modelType":"AL_Model","entityKey":"","actionType":"AL_Deposit","payload":"{}","result":"7",)"
        R"("outcome":"Succeeded","error":"","principal":"","timestampMs":123})";
    const auto decoded = morph::journal::fromJson(legacyLine);
    REQUIRE(decoded.causalParentId.empty());
}

// ── morph::journal::isReplaying() ────────────────────────────────────────────

TEST_CASE("journal::isReplaying: false outside of replay()", "[action_log][journal][replay-mode]") {
    REQUIRE_FALSE(morph::journal::isReplaying());
}

TEST_CASE("journal::isReplaying: true for every dispatch inside replay(), false again afterward",
          "[action_log][journal][replay-mode]") {
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    registry.registerModel<ALModel>("AL_Model");
    dispatcher.registerAction<ALModel, ALDeposit>("AL_Model", "AL_Deposit");

    // ALModel::execute doesn't itself observe isReplaying() -- this test only
    // confirms replay() doesn't leave the flag stuck on afterward. The next
    // test case (RMModel) confirms the flag is actually true from inside a
    // replayed Model::execute.
    REQUIRE_FALSE(morph::journal::isReplaying());

    std::vector<LogEntry> entries{
        makeEntry("AL_Model", "", "AL_Deposit", morph::model::ActionTraits<ALDeposit>::toJson(ALDeposit{.amount = 10})),
    };
    auto holder = morph::journal::replay("AL_Model", entries, registry, dispatcher);
    REQUIRE(holder->into<ALModel>().balance == 10);

    REQUIRE_FALSE(morph::journal::isReplaying());  // flag is scoped to replay()'s own call, not left set
}

TEST_CASE("journal::isReplaying: observable as true from inside a replayed Model::execute",
          "[action_log][journal][replay-mode]") {
    // A model that reads morph::journal::isReplaying() from inside execute()
    // (the shape Phase 6's rules engine will use to suppress rule evaluation)
    // must see true while replay() is dispatching, and false for an ordinary
    // (non-replayed) call.
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    registry.registerModel<RMModel>("RM_Model");
    dispatcher.registerAction<RMModel, ALDeposit>("RM_Model", "AL_Deposit");

    // Ordinary (non-replayed) dispatch: isReplaying() must read false.
    {
        auto holder = registry.create("RM_Model");
        auto depositJson = morph::model::ActionTraits<ALDeposit>::toJson(ALDeposit{.amount = 3});
        dispatcher.dispatch("RM_Model", "AL_Deposit", *holder, depositJson);
        REQUIRE_FALSE(holder->into<RMModel>().sawReplayingDuringExecute);
    }

    // Replayed dispatch: isReplaying() must read true from inside execute().
    {
        std::vector<LogEntry> entries{makeEntry("RM_Model", "", "AL_Deposit",
                                                morph::model::ActionTraits<ALDeposit>::toJson(ALDeposit{.amount = 3}))};
        auto holder = morph::journal::replay("RM_Model", entries, registry, dispatcher);
        REQUIRE(holder->into<RMModel>().sawReplayingDuringExecute);
    }
}

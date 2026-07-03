// SPDX-License-Identifier: Apache-2.0
//
// Coverage for the ordered action log (issue #3, phase 1): action_log.hpp,
// journal.hpp, and the touched lines in model.hpp/registry.hpp/bridge.hpp —
// Loggable default-on, ActionLogPolicy::coalesce, IModelHolder::attachActionLog/
// recordIfAttached, the two real `Model::execute()` sites (ActionDispatcher's
// runner and Bridge::executeVia's localOp), SessionLog checkpoint/undo, and
// journal::replay.

#include <morph/action_log.hpp>
#include <morph/backend.hpp>
#include <morph/bridge.hpp>
#include <morph/executor.hpp>
#include <morph/journal.hpp>
#include <morph/model.hpp>
#include <morph/registry.hpp>
#include <morph/remote.hpp>
#include <morph/session.hpp>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>

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

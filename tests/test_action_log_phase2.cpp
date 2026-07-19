// SPDX-License-Identifier: Apache-2.0
//
// Coverage for phase 2 of the ordered action log (issue #3): LogEntry JSON
// round-trip, FileActionLog, and the wire::Envelope::contextKey +
// RemoteServer::LogProvider mechanism that closes phase 1's "remote identity"
// gap. (Phase 3, a Kafka-shaped sink, was dropped for now.)

#include <morph/journal/action_log.hpp>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/journal/file_action_log.hpp>
#include <morph/journal/journal.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "test_support.hpp"

using SyncExec = morph::testing::InlineExecutor;
using morph::journal::FileActionLog;
using morph::journal::IActionLog;
using morph::journal::InMemoryActionLog;
using morph::journal::LogEntry;

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

/// RAII temp-file path: unique per test, removed on scope exit even on failure.
struct TempFile {
    std::filesystem::path path;
    explicit TempFile(std::string_view name)
        : path{std::filesystem::temp_directory_path() /
               (std::string{"morph_test_"} + std::string{name} + "_" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".ndjson")} {
        std::filesystem::remove(path);
    }
    ~TempFile() { std::filesystem::remove(path); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
};
}  // namespace

// ── Phase 2 test model (separate from test_action_log.cpp's ALModel to avoid
// global-registry collisions across translation units) ─────────────────────

struct P2Deposit {
    int amount = 0;
};
struct P2GetBalance {};
struct P2Save {};  // signals "commit now" — the app decides what that means, not the framework

struct P2Model {
    int balance = 0;
    int execute(const P2Deposit& a) {
        balance += a.amount;
        return balance;
    }
    int execute(const P2GetBalance& /*a*/) { return balance; }
    int execute(const P2Save& /*a*/) { return balance; }
};

BRIDGE_REGISTER_MODEL(P2Model, "P2_Model")
BRIDGE_REGISTER_ACTION(P2Model, P2Deposit, "P2_Deposit")
BRIDGE_REGISTER_ACTION(P2Model, P2GetBalance, "P2_GetBalance", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(P2Model, P2Save, "P2_Save", ::morph::model::Loggable::No)

// ── LogEntry JSON round-trip ─────────────────────────────────────────────────

TEST_CASE("journal::toJson/fromJson: round-trips every field", "[action_log][phase2][json]") {
    auto entry = makeEntry("P2_Model", "acct-1", "P2_Deposit", "{\"amount\":5}", "5");
    entry.seq = 7;
    entry.principal = "alice";
    entry.timestampMs = 123456789;

    auto json = morph::journal::toJson(entry);
    auto decoded = morph::journal::fromJson(json);

    REQUIRE(decoded.seq == 7);
    REQUIRE(decoded.modelType == "P2_Model");
    REQUIRE(decoded.entityKey == "acct-1");
    REQUIRE(decoded.actionType == "P2_Deposit");
    REQUIRE(decoded.payload == "{\"amount\":5}");
    REQUIRE(decoded.result == "5");
    REQUIRE(decoded.principal == "alice");
    REQUIRE(decoded.timestampMs == 123456789);
}

TEST_CASE("journal::fromJson: throws SerializationError on malformed input", "[action_log][phase2][json]") {
    REQUIRE_THROWS_AS(morph::journal::fromJson("not json"), morph::journal::SerializationError);
}

// ── FileActionLog ────────────────────────────────────────────────────────────

TEST_CASE("FileActionLog: append+flush persists entries, entries() reads them back", "[action_log][phase2][file]") {
    TempFile tmp{"file_basic"};
    {
        FileActionLog log{tmp.path};
        log.append(makeEntry("P2_Model", "acct-1", "P2_Deposit", "{}", "10"));
        log.append(makeEntry("P2_Model", "acct-2", "P2_Deposit", "{}", "20"));
        log.flush();

        auto all = log.entries();
        REQUIRE(all.size() == 2);
        REQUIRE(all[0].entityKey == "acct-1");
        REQUIRE(all[0].seq == 1);
        REQUIRE(all[1].entityKey == "acct-2");
        REQUIRE(all[1].seq == 2);

        REQUIRE(log.entries("acct-1").size() == 1);
        REQUIRE(log.entries("no-such").empty());
    }

    // Survives the FileActionLog object being destroyed and a fresh one opened
    // over the same path — this is what makes it "durable" rather than in-memory.
    FileActionLog reopened{tmp.path};
    REQUIRE(reopened.entries().size() == 2);
}

TEST_CASE("FileActionLog: throws if the path cannot be opened", "[action_log][phase2][file]") {
    REQUIRE_THROWS_AS(FileActionLog(std::filesystem::path{"/no/such/directory/at/all/log.ndjson"}),
                      std::runtime_error);
}

TEST_CASE("FileActionLog: entries() skips blank lines", "[action_log][phase2][file]") {
    TempFile tmp{"file_blank_line"};
    {
        FileActionLog log{tmp.path};
        log.append(makeEntry("P2_Model", "acct-1", "P2_Deposit", "{}", "1"));
        log.flush();
    }
    // A blank line can't be produced by FileActionLog itself (every write ends
    // in exactly one '\n'), but a hand-edited or externally-appended file could
    // have one — entries() must skip it rather than fail decoding it as JSON.
    {
        std::ofstream raw{tmp.path, std::ios::app};
        raw << "\n";
    }
    {
        FileActionLog log{tmp.path};
        log.append(makeEntry("P2_Model", "acct-2", "P2_Deposit", "{}", "2"));
        log.flush();

        auto all = log.entries();
        REQUIRE(all.size() == 2);
        REQUIRE(all[0].entityKey == "acct-1");
        REQUIRE(all[1].entityKey == "acct-2");
    }
}

TEST_CASE("FileActionLog: entries() tolerates a malformed trailing line (crash-truncated write)",
          "[action_log][phase2][file]") {
    TempFile tmp{"file_truncated_tail"};
    {
        FileActionLog log{tmp.path};
        log.append(makeEntry("P2_Model", "acct-1", "P2_Deposit", "{}", "1"));
        log.flush();
    }
    // Simulate a crash between fwrite() and the next flush: a syntactically
    // invalid, non-empty final line with no closing brace.
    {
        std::ofstream raw{tmp.path, std::ios::app};
        raw << R"({"seq":2,"entityKey":"acct-2","actionType":"P2_Deposit)";
    }

    FileActionLog log{tmp.path};
    auto all = log.entries();
    REQUIRE(all.size() == 1);
    REQUIRE(all[0].entityKey == "acct-1");
}

TEST_CASE("FileActionLog: entries() rethrows on a malformed line that is NOT the last (genuine corruption)",
          "[action_log][phase2][file]") {
    TempFile tmp{"file_corrupt_mid"};
    {
        FileActionLog log{tmp.path};
        log.append(makeEntry("P2_Model", "acct-1", "P2_Deposit", "{}", "1"));
        log.flush();
    }
    {
        std::ofstream raw{tmp.path, std::ios::app};
        raw << "not json at all\n";
        raw << morph::journal::toJson(makeEntry("P2_Model", "acct-2", "P2_Deposit", "{}", "2")) << "\n";
    }

    FileActionLog log{tmp.path};
    REQUIRE_THROWS_AS(log.entries(), morph::journal::SerializationError);
}

// ── Save action end-to-end: SessionLog + FileActionLog, the pattern the design
// doc asked for ("wire sessionLog.checkpoint(sink) into a real Save action's
// completion handler") ──────────────────────────────────────────────────────

TEST_CASE("Save action end-to-end: intermediate actions stay in-memory, Save checkpoints to a real file, "
         "replay from disk reproduces state",
         "[action_log][phase2][integration]") {
    TempFile tmp{"save_e2e"};
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};

    auto sessionLog = std::make_shared<morph::journal::SessionLog>();
    auto fileLog = std::make_shared<FileActionLog>(tmp.path);

    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = "P2_Model";
    binding->modelFactory = [sessionLog] {
        auto holder = morph::model::detail::ModelFactory::create<P2Model>();
        holder->attachActionLog(sessionLog, "acct-save-1");
        return holder;
    };
    morph::bridge::BridgeHandler<P2Model> handler{bridge, &cbExec, binding};

    for (int amount : {10, 20, 30}) {
        std::atomic<bool> done{false};
        handler.execute(P2Deposit{.amount = amount})
            .then([&](int) { done.store(true); })
            .onError([](const std::exception_ptr&) {});
        REQUIRE(morph::testing::waitUntil([&] { return done.load(); }));
    }
    REQUIRE(sessionLog->entries().size() == 3);
    REQUIRE(fileLog->entries().empty());  // nothing durable yet — no checkpoint has run

    // The app decides what "Save" means; the framework never guesses. Here,
    // the Save action's own completion is where checkpointing happens.
    std::atomic<bool> saved{false};
    handler.execute(P2Save{})
        .then([&](int) {
            sessionLog->checkpoint(*fileLog);
            saved.store(true);
        })
        .onError([](const std::exception_ptr&) {});
    REQUIRE(morph::testing::waitUntil([&] { return saved.load(); }));

    auto onDisk = fileLog->entries();
    REQUIRE(onDisk.size() == 3);  // three Deposits, all distinct (coalesce==false default)
    for (const auto& entry : onDisk) {
        REQUIRE(entry.entityKey == "acct-save-1");
        REQUIRE(entry.actionType == "P2_Deposit");
    }

    // Reconstructing state purely from what's on disk reproduces the live model.
    morph::model::detail::ModelRegistryFactory registry;
    morph::model::detail::ActionDispatcher dispatcher;
    registry.registerModel<P2Model>("P2_Model");
    dispatcher.registerAction<P2Model, P2Deposit>("P2_Model", "P2_Deposit");
    auto reconstructed = morph::journal::replay("P2_Model", onDisk, registry, dispatcher);
    REQUIRE(reconstructed->into<P2Model>().balance == 60);
}

// ── wire::Envelope::contextKey ───────────────────────────────────────────────

TEST_CASE("wire::makeRegister: contextKey round-trips through encode/decode", "[action_log][phase2][wire]") {
    auto env = morph::wire::makeRegister("P2_Model", "acct-42");
    REQUIRE(env.contextKey == "acct-42");
    auto decoded = morph::wire::decode(morph::wire::encode(env));
    REQUIRE(decoded.contextKey == "acct-42");
    REQUIRE(decoded.typeId == "P2_Model");
}

TEST_CASE("wire::makeRegister: contextKey defaults to empty", "[action_log][phase2][wire]") {
    auto env = morph::wire::makeRegister("P2_Model");
    REQUIRE(env.contextKey.empty());
}

// ── RemoteServer::setLogProvider — closes phase 1's remote-identity gap ─────

TEST_CASE("RemoteServer::setLogProvider: attaches a log to the server-created holder", "[action_log][phase2][remote]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::model::detail::ModelRegistryFactory registry;
    morph::model::detail::ActionDispatcher dispatcher;
    registry.registerModel<P2Model>("P2_Model");
    dispatcher.registerAction<P2Model, P2Deposit>("P2_Model", "P2_Deposit");

    auto server = std::make_shared<morph::backend::RemoteServer>(pool, dispatcher, registry);

    std::vector<std::string> requestedFor;
    auto log = std::make_shared<InMemoryActionLog>();
    server->setLogProvider([&](std::string_view modelType, std::string_view contextKey) {
        requestedFor.emplace_back(std::string{modelType} + ":" + std::string{contextKey});
        return log;
    });

    auto regReply =
        morph::wire::decode(server->handleInline(morph::wire::encode(morph::wire::makeRegister("P2_Model", "acct-9"))));
    REQUIRE(regReply.kind == "ok");
    REQUIRE(requestedFor == std::vector<std::string>{"P2_Model:acct-9"});

    morph::wire::Envelope exec;
    exec.kind = "execute";
    exec.modelId = regReply.modelId;
    exec.modelType = "P2_Model";
    exec.actionType = "P2_Deposit";
    exec.body = morph::model::ActionTraits<P2Deposit>::toJson(P2Deposit{.amount = 15});
    morph::testing::WaitReply waiter;
    server->handle(morph::wire::encode(exec), std::ref(waiter));
    REQUIRE(waiter.await());
    REQUIRE(waiter.env.kind == "ok");

    auto entries = log->entries();
    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].entityKey == "acct-9");
    REQUIRE(entries[0].actionType == "P2_Deposit");
}

TEST_CASE("RemoteServer::setLogProvider: not consulted when contextKey is empty", "[action_log][phase2][remote]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::model::detail::ModelRegistryFactory registry;
    morph::model::detail::ActionDispatcher dispatcher;
    registry.registerModel<P2Model>("P2_Model");

    auto server = std::make_shared<morph::backend::RemoteServer>(pool, dispatcher, registry);
    bool providerCalled = false;
    server->setLogProvider([&](std::string_view, std::string_view) {
        providerCalled = true;
        return std::make_shared<InMemoryActionLog>();
    });

    auto reply = morph::wire::decode(server->handleInline(morph::wire::encode(morph::wire::makeRegister("P2_Model"))));
    REQUIRE(reply.kind == "ok");
    REQUIRE_FALSE(providerCalled);
}

TEST_CASE("RemoteServer::setLogProvider: a provider returning nullptr attaches no log", "[action_log][phase2][remote]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::model::detail::ModelRegistryFactory registry;
    morph::model::detail::ActionDispatcher dispatcher;
    registry.registerModel<P2Model>("P2_Model");
    dispatcher.registerAction<P2Model, P2Deposit>("P2_Model", "P2_Deposit");

    auto server = std::make_shared<morph::backend::RemoteServer>(pool, dispatcher, registry);
    server->setLogProvider([](std::string_view, std::string_view) { return nullptr; });

    auto regReply =
        morph::wire::decode(server->handleInline(morph::wire::encode(morph::wire::makeRegister("P2_Model", "acct-x"))));
    REQUIRE(regReply.kind == "ok");

    morph::wire::Envelope exec;
    exec.kind = "execute";
    exec.modelId = regReply.modelId;
    exec.modelType = "P2_Model";
    exec.actionType = "P2_Deposit";
    exec.body = morph::model::ActionTraits<P2Deposit>::toJson(P2Deposit{.amount = 1});
    morph::testing::WaitReply waiter;
    server->handle(morph::wire::encode(exec), std::ref(waiter));
    REQUIRE(waiter.await());
    REQUIRE(waiter.env.kind == "ok");  // executes fine even though no log got attached
}

TEST_CASE("RemoteServer::setLogProvider: nullptr provider removes a previously installed one",
         "[action_log][phase2][remote]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::model::detail::ModelRegistryFactory registry;
    morph::model::detail::ActionDispatcher dispatcher;
    registry.registerModel<P2Model>("P2_Model");
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, dispatcher, registry);

    bool called = false;
    server->setLogProvider([&](std::string_view, std::string_view) {
        called = true;
        return nullptr;
    });
    server->setLogProvider(nullptr);

    auto reply =
        morph::wire::decode(server->handleInline(morph::wire::encode(morph::wire::makeRegister("P2_Model", "acct-y"))));
    REQUIRE(reply.kind == "ok");
    REQUIRE_FALSE(called);
}

// ── End-to-end: Bridge + SimulatedRemoteBackend + contextKey + LogProvider ──
//
// This is the scenario phase 1 explicitly could not support: the client sets
// HandlerBinding::contextKey, SimulatedRemoteBackend carries it across the
// wire, and the server's LogProvider attaches a real log to the instance it
// creates — recording happens server-side, with a real per-account identity,
// for a genuinely remote-shaped topology.

TEST_CASE("End-to-end: HandlerBinding::contextKey reaches the server's LogProvider via SimulatedRemoteBackend",
         "[action_log][phase2][remote]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    morph::model::detail::ModelRegistryFactory registry;
    morph::model::detail::ActionDispatcher dispatcher;
    registry.registerModel<P2Model>("P2_Model");
    dispatcher.registerAction<P2Model, P2Deposit>("P2_Model", "P2_Deposit");

    auto server = std::make_shared<morph::backend::RemoteServer>(pool, dispatcher, registry);
    auto log = std::make_shared<InMemoryActionLog>();
    server->setLogProvider([&](std::string_view, std::string_view) { return log; });

    morph::bridge::Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server)};

    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = "P2_Model";
    binding->contextKey = "acct-remote-1";
    binding->modelFactory = [] { return morph::model::detail::ModelFactory::create<P2Model>(); };
    morph::bridge::BridgeHandler<P2Model> handler{bridge, &cbExec, binding};

    std::atomic<int> result{-1};
    handler.execute(P2Deposit{.amount = 30}).then([&](int v) { result.store(v); }).onError([](const std::exception_ptr&) {
    });
    REQUIRE(morph::testing::waitUntil([&] { return result.load() != -1; }));
    REQUIRE(result.load() == 30);

    auto entries = log->entries();
    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].entityKey == "acct-remote-1");
    REQUIRE(entries[0].actionType == "P2_Deposit");
}

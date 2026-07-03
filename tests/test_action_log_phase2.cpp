// SPDX-License-Identifier: Apache-2.0
//
// Coverage for phase 2/3 of the ordered action log (issue #3): LogEntry JSON
// round-trip, FileActionLog, the wire::Envelope::contextKey + RemoteServer::
// LogProvider mechanism that closes phase 1's "remote identity" gap, and the
// fake-broker-backed KafkaActionLog (phase 3, per the chosen "interface + fake
// broker" approach — no librdkafka or live cluster involved).

#include <morph/action_log.hpp>
#include <morph/backend.hpp>
#include <morph/bridge.hpp>
#include <morph/executor.hpp>
#include <morph/file_action_log.hpp>
#include <morph/journal.hpp>
#include <morph/kafka_action_log.hpp>
#include <morph/registry.hpp>
#include <morph/remote.hpp>
#include <morph/wire.hpp>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "test_support.hpp"

using SyncExec = morph::testing::InlineExecutor;
using morph::journal::FileActionLog;
using morph::journal::IActionLog;
using morph::journal::InMemoryActionLog;
using morph::journal::LogEntry;
namespace kafka = morph::journal::kafka;

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

// Registered only against local (non-global) ActionDispatcher instances in the
// Kafka tests below, purely to populate the coalesce map — dispatch() is never
// actually called on them. Declared here (rather than down by those tests) so
// P2Model can have real execute() overloads for them.
struct KafkaCoalesceAction {
    std::string name;
};
struct KafkaEventAction {
    int amount = 0;
};

struct P2Model {
    int balance = 0;
    int execute(const P2Deposit& a) {
        balance += a.amount;
        return balance;
    }
    int execute(const P2GetBalance& /*a*/) { return balance; }
    int execute(const P2Save& /*a*/) { return balance; }
    std::string execute(const KafkaCoalesceAction& a) { return a.name; }
    int execute(const KafkaEventAction& a) { return a.amount; }
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

// ── Kafka: fake broker + KafkaActionLog ─────────────────────────────────────
//
// KafkaCoalesceAction/KafkaEventAction are declared earlier, alongside P2Model.

// Manual ActionTraits (matching test_dispatch_di.cpp's style) — these actions
// are only ever registered on a local ActionDispatcher to populate its
// coalesce map; dispatcher.dispatch() is never actually called on them, but
// registerAction<>() still compiles the full runner lambda, which references
// ActionTraits<Action>, so a real specialisation is required regardless.
template <>
struct morph::model::ActionTraits<KafkaCoalesceAction> {
    using Result = std::string;
    static constexpr std::string_view typeId() { return "KafkaCoalesceAction"; }
    static std::string toJson(const KafkaCoalesceAction& a) { return R"({"name":")" + a.name + "\"}"; }
    static KafkaCoalesceAction fromJson(std::string_view) { return {}; }
    static std::string resultToJson(const std::string& r) { return r; }
    static std::string resultFromJson(std::string_view s) { return std::string{s}; }
};
template <>
struct morph::model::ActionTraits<KafkaEventAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "KafkaEventAction"; }
    static std::string toJson(const KafkaEventAction& a) { return R"({"amount":)" + std::to_string(a.amount) + "}"; }
    static KafkaEventAction fromJson(std::string_view) { return {}; }
    static std::string resultToJson(const int& r) { return std::to_string(r); }
    static int resultFromJson(std::string_view s) { return std::stoi(std::string{s}); }
};

template <>
struct morph::model::ActionLogPolicy<KafkaCoalesceAction> {
    static constexpr bool coalesce = true;
};

TEST_CASE("KafkaActionLog: coalesce==true actions share one key, compactedView keeps only the latest",
         "[action_log][phase3][kafka]") {
    morph::model::detail::ActionDispatcher dispatcher;
    dispatcher.registerAction<P2Model, KafkaCoalesceAction>("P2_Model", "KafkaCoalesceAction");

    kafka::FakeProducer producer;
    kafka::KafkaActionLog kafkaLog{producer, "action-log-topic", dispatcher};

    kafkaLog.append(makeEntry("P2_Model", "acct-1", "KafkaCoalesceAction", "", "bob"));
    kafkaLog.append(makeEntry("P2_Model", "acct-1", "KafkaCoalesceAction", "", "bobby"));
    kafkaLog.flush();

    auto raw = producer.raw("action-log-topic");
    REQUIRE(raw.size() == 2);
    REQUIRE(raw[0].key == raw[1].key);  // same coalescing key both times

    auto compacted = producer.compactedView("action-log-topic");
    REQUIRE(compacted.size() == 1);
    REQUIRE(morph::journal::fromJson(compacted[0].value).result == "bobby");  // latest wins
    REQUIRE(producer.flushCount() == 1);
}

TEST_CASE("KafkaActionLog: coalesce==false actions each get a unique key, compactedView keeps all",
         "[action_log][phase3][kafka]") {
    morph::model::detail::ActionDispatcher dispatcher;
    dispatcher.registerAction<P2Model, KafkaEventAction>("P2_Model", "KafkaEventAction");

    kafka::FakeProducer producer;
    kafka::KafkaActionLog kafkaLog{producer, "action-log-topic", dispatcher};

    kafkaLog.append(makeEntry("P2_Model", "acct-1", "KafkaEventAction", "", "10"));
    kafkaLog.append(makeEntry("P2_Model", "acct-1", "KafkaEventAction", "", "20"));

    auto raw = producer.raw("action-log-topic");
    REQUIRE(raw.size() == 2);
    REQUIRE(raw[0].key != raw[1].key);  // distinct keys — real events, never merged

    auto compacted = producer.compactedView("action-log-topic");
    REQUIRE(compacted.size() == 2);  // compaction is a no-op for this policy
}

TEST_CASE("KafkaActionLog: unregistered (modelType, actionType) defaults to coalesce==false",
         "[action_log][phase3][kafka]") {
    morph::model::detail::ActionDispatcher dispatcher;  // nothing registered
    kafka::FakeProducer producer;
    kafka::KafkaActionLog kafkaLog{producer, "t", dispatcher};

    kafkaLog.append(makeEntry("Unknown", "e", "Unknown"));
    kafkaLog.append(makeEntry("Unknown", "e", "Unknown"));
    REQUIRE(producer.compactedView("t").size() == 2);
}

TEST_CASE("KafkaActionLog::entries: not supported, throws", "[action_log][phase3][kafka]") {
    morph::model::detail::ActionDispatcher dispatcher;
    kafka::FakeProducer producer;
    kafka::KafkaActionLog kafkaLog{producer, "t", dispatcher};
    REQUIRE_THROWS_AS(kafkaLog.entries(), std::logic_error);
}

TEST_CASE("kafka::FakeProducer: raw() on an unknown topic returns empty", "[action_log][phase3][kafka]") {
    kafka::FakeProducer producer;
    REQUIRE(producer.raw("nope").empty());
    REQUIRE(producer.compactedView("nope").empty());
}

// SPDX-License-Identifier: Apache-2.0
//
// Coverage for phase 2 of the ordered action log (issue #3): LogEntry JSON
// round-trip, FileActionLog, and the wire::Envelope::contextKey +
// RemoteServer::LogProvider mechanism that closes phase 1's "remote identity"
// gap. (Phase 3, a Kafka-shaped sink, was dropped for now.)

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/file_io_ops.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/journal/file_action_log.hpp>
#include <morph/journal/journal.hpp>
#include <string>
#include <vector>

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
    entry.idempotencyKey = "idem-42";

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
    REQUIRE(decoded.idempotencyKey == "idem-42");
}

TEST_CASE("LogEntry::idempotencyKey: defaults to empty", "[action_log][phase2][json]") {
    LogEntry fresh{};
    REQUIRE(fresh.idempotencyKey.empty());
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

    // FileActionLog's constructor now rebuilds its idempotencyKey dedup set by
    // scanning entries() at open time (Task 3, transactional-outbox plan), so a
    // pre-existing interior corruption throws from construction itself, not just
    // from a later explicit entries() call.
    REQUIRE_THROWS_AS(FileActionLog(tmp.path), morph::journal::SerializationError);
}

// ── Save action end-to-end: SessionLog + FileActionLog, the pattern the design
// doc asked for ("wire sessionLog.checkpoint(sink) into a real Save action's
// completion handler") ──────────────────────────────────────────────────────

TEST_CASE(
    "Save action end-to-end: intermediate actions stay in-memory, Save checkpoints to a real file, "
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

TEST_CASE("RemoteServer::setLogProvider: attaches a log to the server-created holder",
          "[action_log][phase2][remote]") {
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

    auto regReply = morph::wire::decode(
        server->handleInline(morph::wire::encode(morph::wire::makeRegister("P2_Model", "acct-9"))));
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

TEST_CASE("RemoteServer::setLogProvider: a provider returning nullptr attaches no log",
          "[action_log][phase2][remote]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::model::detail::ModelRegistryFactory registry;
    morph::model::detail::ActionDispatcher dispatcher;
    registry.registerModel<P2Model>("P2_Model");
    dispatcher.registerAction<P2Model, P2Deposit>("P2_Model", "P2_Deposit");

    auto server = std::make_shared<morph::backend::RemoteServer>(pool, dispatcher, registry);
    server->setLogProvider([](std::string_view, std::string_view) { return nullptr; });

    auto regReply = morph::wire::decode(
        server->handleInline(morph::wire::encode(morph::wire::makeRegister("P2_Model", "acct-x"))));
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

    auto reply = morph::wire::decode(
        server->handleInline(morph::wire::encode(morph::wire::makeRegister("P2_Model", "acct-y"))));
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
    handler.execute(P2Deposit{.amount = 30})
        .then([&](int v) { result.store(v); })
        .onError([](const std::exception_ptr&) {});
    REQUIRE(morph::testing::waitUntil([&] { return result.load() != -1; }));
    REQUIRE(result.load() == 30);

    auto entries = log->entries();
    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].entityKey == "acct-remote-1");
    REQUIRE(entries[0].actionType == "P2_Deposit");
}

// ── A torn trailing record must be repaired, not merely tolerated ────────────
// entries() skips a malformed trailing line, but the file was opened "a", so
// the next append() began writing at the exact byte the truncated JSON stopped
// at with no separating newline. The two merged into one line that swallowed
// the new entry; a further append pushed that merged line out of trailing
// position, at which point entries() threw -- and since the constructor calls
// entries(), the journal became permanently unopenable.

TEST_CASE("FileActionLog: a torn trailing record is truncated on open", "[action_log][phase2][file]") {
    TempFile const tmp{"file_torn_repair"};
    {
        FileActionLog log{tmp.path};
        log.append(makeEntry("P2_Model", "acct-1", "P2_Deposit", "{}", "10"));
        log.flush();
    }
    // Simulate a crash between append()'s fwrite and the next flush: a partial
    // record with no terminating newline.
    {
        std::ofstream out{tmp.path, std::ios::app | std::ios::binary};
        out << R"({"seq":2,"modelType":"P2_Model","entityKe)";
    }

    {
        FileActionLog log{tmp.path};
        REQUIRE(log.entries().size() == 1);
        log.append(makeEntry("P2_Model", "acct-2", "P2_Deposit", "{}", "20"));
        log.flush();
        // The new entry is readable, not fused onto the torn remainder.
        auto all = log.entries();
        REQUIRE(all.size() == 2);
        CHECK(all.at(0).entityKey == "acct-1");
        CHECK(all.at(1).entityKey == "acct-2");
    }
}

TEST_CASE("FileActionLog: a torn trailing record does not make the log unopenable", "[action_log][phase2][file]") {
    TempFile const tmp{"file_torn_unopenable"};
    {
        FileActionLog log{tmp.path};
        log.append(makeEntry("P2_Model", "acct-1", "P2_Deposit", "{}", "10"));
        log.flush();
    }
    {
        std::ofstream out{tmp.path, std::ios::app | std::ios::binary};
        out << R"({"seq":2,"modelType":"P2_Mod)";
    }

    // The second append is what used to be fatal: it pushed the merged line
    // into interior position. Reopen between appends, as a restarting process
    // would, and keep going well past that point.
    for (int restart = 0; restart < 3; ++restart) {
        FileActionLog log{tmp.path};
        REQUIRE_NOTHROW(log.append(makeEntry("P2_Model", "acct-n", "P2_Deposit", "{}", "1")));
        REQUIRE_NOTHROW(log.flush());
    }
    FileActionLog reopened{tmp.path};
    REQUIRE(reopened.entries().size() == 4);
}

TEST_CASE("FileActionLog: interior corruption is still reported, not silently truncated",
          "[action_log][phase2][file]") {
    // The repair only removes bytes after the final newline -- never a complete
    // record. Genuine mid-file corruption must keep throwing.
    TempFile const tmp{"file_interior_corrupt"};
    {
        FileActionLog log{tmp.path};
        log.append(makeEntry("P2_Model", "acct-1", "P2_Deposit", "{}", "10"));
        log.append(makeEntry("P2_Model", "acct-2", "P2_Deposit", "{}", "20"));
        log.flush();
    }
    {
        // Insert a complete-but-malformed line between two good ones.
        std::ifstream input{tmp.path};
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(input, line)) {
            lines.push_back(line);
        }
        input.close();
        REQUIRE(lines.size() == 2);
        std::ofstream out{tmp.path, std::ios::trunc};
        out << lines.at(0) << "\n" << R"({"seq":9,"broken)" << "\n" << lines.at(1) << "\n";
    }
    REQUIRE_THROWS(FileActionLog{tmp.path});
}

TEST_CASE("FileActionLog: append and flush on a log whose rotate() left it closed throw clearly",
          "[action_log][phase2][file]") {
    // rotate() deliberately leaves the handle null rather than dangling when the
    // reopen fails; every entry point must say so instead of dereferencing it.
    TempFile const tmp{"file_rotate_closed"};
    FileActionLog log{tmp.path};
    log.append(makeEntry("P2_Model", "acct-1", "P2_Deposit", "{}", "10"));
    // Rotating into a directory that does not exist fails the rename, but the
    // active path is still reopenable, so the log stays usable.
    REQUIRE_THROWS(log.rotate(std::filesystem::path{"/no/such/directory/at/all/sealed.ndjson"}));
    REQUIRE_NOTHROW(log.append(makeEntry("P2_Model", "acct-2", "P2_Deposit", "{}", "20")));
    REQUIRE_NOTHROW(log.flush());
    REQUIRE(log.entries().size() == 2);
}

// ── repairTornTail(): the pre-existing-but-empty file arm ───────────────────
// repairTornTail()'s early-out is `errorCode || size == 0`. Every other test
// in this file either opens a path that doesn't exist yet (errorCode set) or
// a path that already holds complete records (size != 0); no test exercises
// the "file already exists at this path, but is exactly zero bytes" arm --
// e.g. a host that pre-touches the path, or a previous run that created the
// file but crashed before the first append()'s fwrite. That arm must be a
// no-op, not a crash or a spurious truncation warning.

TEST_CASE("FileActionLog: opening a pre-existing, zero-byte file is a no-op repair",
          "[action_log][phase2][file]") {
    TempFile const tmp{"file_preexisting_empty"};
    {
        // Create the file with zero bytes, without going through FileActionLog.
        std::ofstream touch{tmp.path};
    }
    REQUIRE(std::filesystem::exists(tmp.path));
    REQUIRE(std::filesystem::file_size(tmp.path) == 0);

    FileActionLog log{tmp.path};
    REQUIRE(log.entries().empty());

    // The log stays fully usable afterward.
    log.append(makeEntry("P2_Model", "acct-1", "P2_Deposit", "{}", "10"));
    log.flush();
    REQUIRE(log.entries().size() == 1);
}

// ── rotate(): the idempotencyKey-promotion loop with a non-empty set ───────
// rotate() inlines flush()'s durability steps and then promotes every key
// buffered since the last successful flush into `_seenIdempotencyKeys`
// (mirroring flush()'s own loop, covered elsewhere via plain flush()). No
// existing rotate() test appends an entry with a non-empty idempotencyKey
// first, so that loop's body -- and the dedup state it produces -- is never
// actually exercised by rotate(), only by flush(). Confirm the promoted key
// is recognized as a duplicate on both sides of a rotation: in the freshly
// reopened active file, and after a simulated restart against the sealed file.

TEST_CASE("FileActionLog::rotate: promotes unflushed idempotencyKeys into durable dedup state",
          "[action_log][phase2][file]") {
    TempFile const active{"file_rotate_idem_active"};
    TempFile const sealed{"file_rotate_idem_sealed"};

    auto rowEntry = makeEntry("P2_Model", "acct-1", "P2_Deposit", "{}", "10");
    rowEntry.idempotencyKey = "row-1";

    FileActionLog log{active.path};
    log.append(rowEntry);
    // Not flushed yet: the key lives only in _unflushedIdempotencyKeys going
    // into rotate(), so rotate() -- not flush() -- is what must promote it.
    log.rotate(sealed.path);

    // A re-relay of the same row after the rotation is still recognized as a
    // duplicate: the key was promoted to _seenIdempotencyKeys, not dropped.
    log.append(rowEntry);
    log.flush();
    REQUIRE(log.entries().empty());  // the duplicate never reached the active file

    FileActionLog sealedReader{sealed.path};
    auto sealedEntries = sealedReader.entries();
    REQUIRE(sealedEntries.size() == 1);
    REQUIRE(sealedEntries[0].idempotencyKey == "row-1");
}

// ── FileIoOps fault injection (LASTRADA-Software/morph#97) ─────────────────
//
// Every branch below only runs when a real OS-level file-I/O call fails
// partway through an otherwise-successful operation -- previously
// unreachable from a portable unit test (see each site's prior in-code
// comment, now removed since these tests close them for real). FileIoOps
// (morph/core/file_io_ops.hpp) makes each call injectable; every test here
// overrides exactly one member and leaves the rest at their real defaults,
// so the surrounding I/O still touches the real filesystem normally.

TEST_CASE("FileActionLog::append: a short fwrite() throws and does not record the idempotencyKey as unflushed",
          "[action_log][phase2][file][fault-injection]") {
    TempFile const tmp{"file_fault_append_short_write"};
    morph::core::FileIoOps ioOps;
    ioOps.fwrite = [](const void*, std::size_t size, std::FILE*) { return size - 1; };  // always short by one byte

    FileActionLog log{tmp.path, ioOps};
    auto entry = makeEntry("P2_Model", "acct-1", "P2_Deposit", "{}", "10");
    entry.idempotencyKey = "row-1";
    REQUIRE_THROWS_AS(log.append(entry), std::runtime_error);

    // The failed write must not have left the key recorded as unflushed --
    // a retry of the same row must not be silently deduplicated away.
    morph::core::FileIoOps realOps;  // the retry itself must actually succeed
    FileActionLog log2{tmp.path, realOps};
    log2.append(entry);
    log2.flush();
    REQUIRE(log2.entries().size() == 1);
}

TEST_CASE("FileActionLog::flush: a failing fflush() throws and forgets the unflushed idempotencyKeys",
          "[action_log][phase2][file][fault-injection]") {
    TempFile const tmp{"file_fault_flush_fflush"};
    // `log` stores its own copy of ioOps by value (FileIoOps's whole point is
    // to be a plain, copyable strategy) -- mutating this local `ioOps` after
    // construction has no effect on what `log` already captured. A shared
    // `shouldFail` flag the lambda itself reads is what lets one FileIoOps
    // instance flip from failing to succeeding mid-test.
    auto shouldFail = std::make_shared<bool>(true);
    morph::core::FileIoOps ioOps;
    ioOps.fflush = [shouldFail](std::FILE* file) { return *shouldFail ? -1 : std::fflush(file); };

    FileActionLog log{tmp.path, ioOps};
    auto entry = makeEntry("P2_Model", "acct-1", "P2_Deposit", "{}", "10");
    entry.idempotencyKey = "row-1";
    log.append(entry);
    REQUIRE_THROWS_AS(log.flush(), std::runtime_error);

    // Forgotten, not durably deduplicated: a retry must actually re-append.
    *shouldFail = false;
    log.append(entry);
    log.flush();
    REQUIRE(log.entries().size() == 2);
}

TEST_CASE("FileActionLog::flush: a failing fsync() throws and forgets the unflushed idempotencyKeys",
          "[action_log][phase2][file][fault-injection]") {
    TempFile const tmp{"file_fault_flush_fsync"};
    auto shouldFail = std::make_shared<bool>(true);
    morph::core::FileIoOps ioOps;
    morph::core::FileIoOps const realOps;  // captures the real default fsync callback to fall back to
    ioOps.fsync = [shouldFail, realOps](std::FILE* file) { return *shouldFail ? -1 : realOps.fsync(file); };

    FileActionLog log{tmp.path, ioOps};
    auto entry = makeEntry("P2_Model", "acct-1", "P2_Deposit", "{}", "10");
    entry.idempotencyKey = "row-1";
    log.append(entry);
    REQUIRE_THROWS_AS(log.flush(), std::runtime_error);

    *shouldFail = false;
    log.append(entry);
    log.flush();
    REQUIRE(log.entries().size() == 2);
}

TEST_CASE("FileActionLog::rotate: a failing pre-rotation fflush() throws before anything is closed or renamed",
          "[action_log][phase2][file][fault-injection]") {
    TempFile const active{"file_fault_rotate_fflush_active"};
    TempFile const sealed{"file_fault_rotate_fflush_sealed"};
    auto shouldFail = std::make_shared<bool>(true);
    morph::core::FileIoOps ioOps;
    ioOps.fflush = [shouldFail](std::FILE* file) { return *shouldFail ? -1 : std::fflush(file); };

    FileActionLog log{active.path, ioOps};
    log.append(makeEntry("P2_Model", "acct-1", "P2_Deposit", "{}", "10"));
    REQUIRE_THROWS_AS(log.rotate(sealed.path), std::runtime_error);

    // Nothing was closed or renamed: the active file is exactly as it was,
    // and no sealed file was ever created.
    REQUIRE_FALSE(std::filesystem::exists(sealed.path));
    *shouldFail = false;
    log.flush();
    REQUIRE(log.entries().size() == 1);
}

TEST_CASE("FileActionLog::rotate: a failing pre-rotation fsync() throws before anything is closed or renamed",
          "[action_log][phase2][file][fault-injection]") {
    TempFile const active{"file_fault_rotate_fsync_active"};
    TempFile const sealed{"file_fault_rotate_fsync_sealed"};
    auto shouldFail = std::make_shared<bool>(true);
    morph::core::FileIoOps ioOps;
    morph::core::FileIoOps const realOps;
    ioOps.fsync = [shouldFail, realOps](std::FILE* file) { return *shouldFail ? -1 : realOps.fsync(file); };

    FileActionLog log{active.path, ioOps};
    log.append(makeEntry("P2_Model", "acct-1", "P2_Deposit", "{}", "10"));
    REQUIRE_THROWS_AS(log.rotate(sealed.path), std::runtime_error);

    REQUIRE_FALSE(std::filesystem::exists(sealed.path));
    *shouldFail = false;
    log.flush();
    REQUIRE(log.entries().size() == 1);
}

TEST_CASE(
    "FileActionLog::rotate: a failing reopen after a successful rename leaves the log closed, "
    "requireOpen()'s throwing arm reachable, and the destructor's null check load-bearing",
    "[action_log][phase2][file][fault-injection]") {
    // The one scenario morph#97 called out as needing the *most* real-world
    // contortion to reach without this seam: fopen() failing on the reopen
    // right after the rename to sealedPath already succeeded. With FileIoOps,
    // this is just "let the constructor's own fopen() through for real, then
    // fail every fopen() call after it" -- a call counter, since rotator's
    // own construction needs a real, valid handle before rotate() ever runs.
    // A single FileActionLog instance for the whole test: a second instance
    // on the same path would hold its own competing file handle open, which
    // is exactly the kind of extra concurrency this test does not need.
    TempFile const active{"file_fault_rotate_reopen_active"};
    TempFile const sealed{"file_fault_rotate_reopen_sealed"};

    auto callCount = std::make_shared<int>(0);
    morph::core::FileIoOps ioOps;
    morph::core::FileIoOps const realOps;
    ioOps.fopen = [callCount, realOps](const std::string& path, const char* mode) -> std::FILE* {
        return (*callCount)++ == 0 ? realOps.fopen(path, mode) : nullptr;
    };
    FileActionLog rotator{active.path, ioOps};
    rotator.append(makeEntry("P2_Model", "acct-1", "P2_Deposit", "{}", "10"));
    rotator.flush();

    REQUIRE_THROWS_AS(rotator.rotate(sealed.path), std::runtime_error);
    // The rename itself succeeded (fflush/fsync were untouched -- only fopen
    // fails), so the sealed file now holds every entry that was on disk.
    REQUIRE(std::filesystem::exists(sealed.path));

    // The log is left with no open file: append()/flush()/a further rotate()
    // must all throw via requireOpen(), not dereference a null handle.
    REQUIRE_THROWS_AS(rotator.append(makeEntry("P2_Model", "acct-1", "P2_Deposit", "{}", "20")), std::runtime_error);
    REQUIRE_THROWS_AS(rotator.flush(), std::runtime_error);
    REQUIRE_THROWS_AS(rotator.rotate(sealed.path), std::runtime_error);

    // Destruction with _file == nullptr must be safe (the null check in the
    // destructor is load-bearing, not defensive noise) -- rotator's own
    // scope exit below exercises exactly that.
}

TEST_CASE("FileActionLog: a torn trailing record whose path becomes unreadable is left untouched",
          "[action_log][phase2][file][fault-injection]") {
    TempFile const tmp{"file_fault_repair_unreadable"};
    {
        std::ofstream out{tmp.path, std::ios::binary};
        out << R"({"seq":1,"modelType":"P2_Model","entityKey":"acct-1","actionType":"P2_Deposit","payload":"{}","result":"10","principal":"","timestampMs":0})"
            << "\n";
        out << R"({"seq":2,"modelType":"P2_Model")";  // torn trailing record, no newline
    }
    auto const sizeBefore = std::filesystem::file_size(tmp.path);

    morph::core::FileIoOps ioOps;
    ioOps.canOpenForRead = [](const std::filesystem::path&) { return false; };
    FileActionLog log{tmp.path, ioOps};  // repairTornTail() must skip the truncation

    REQUIRE(std::filesystem::file_size(tmp.path) == sizeBefore);
}

TEST_CASE("FileActionLog: a torn trailing record whose resize_file() fails is logged, not silently swallowed",
          "[action_log][phase2][file][fault-injection]") {
    TempFile const tmp{"file_fault_repair_resize_fails"};
    {
        std::ofstream out{tmp.path, std::ios::binary};
        out << R"({"seq":1,"modelType":"P2_Model","entityKey":"acct-1","actionType":"P2_Deposit","payload":"{}","result":"10","principal":"","timestampMs":0})"
            << "\n";
        out << R"({"seq":2,"modelType":"P2_Model")";  // torn trailing record, no newline
    }
    auto const sizeBefore = std::filesystem::file_size(tmp.path);

    morph::core::FileIoOps ioOps;
    ioOps.resizeFile = [](const std::filesystem::path&, std::uintmax_t, std::error_code& errorCode) {
        errorCode = std::make_error_code(std::errc::permission_denied);
    };
    FileActionLog log{tmp.path, ioOps};  // repairTornTail() logs a warning and returns, does not throw

    // The failed truncation left the file exactly as it was -- not repaired,
    // but not corrupted further either.
    REQUIRE(std::filesystem::file_size(tmp.path) == sizeBefore);
}

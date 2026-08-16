// SPDX-License-Identifier: Apache-2.0
//
// Coverage for the transactional outbox (todo.md B2, docs/planned/outbox.md):
// LogEntry::idempotencyKey dedup in InMemoryActionLog/FileActionLog,
// IModelHolder::setOutboxManaged/isOutboxManaged suppressing recordIfAttached,
// and journal::OutboxRelay.

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/logger.hpp>
#include <morph/core/model.hpp>
#include <morph/core/registry.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/journal/file_action_log.hpp>
#include <morph/journal/journal.hpp>
#include <morph/journal/outbox.hpp>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "test_support.hpp"

using SyncExec = morph::testing::InlineExecutor;
using morph::journal::FileActionLog;
using morph::journal::IActionLog;
using morph::journal::InMemoryActionLog;
using morph::journal::LogEntry;
using morph::journal::OutboxRelay;
using morph::journal::OutboxRelayResult;

namespace {

LogEntry makeEntry(std::string modelType, std::string entityKey, std::string actionType,
                   std::string idempotencyKey = {}) {
    return LogEntry{
        .seq = 0,
        .modelType = std::move(modelType),
        .entityKey = std::move(entityKey),
        .actionType = std::move(actionType),
        .payload = {},
        .result = {},
        .principal = {},
        .timestampMs = 0,
        .idempotencyKey = std::move(idempotencyKey),
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
    TempFile(TempFile&&) = delete;
    TempFile& operator=(TempFile&&) = delete;
};

}  // namespace

// ── Test model, registered via the macros (global registry/dispatcher) ─────────

struct OBDeposit {
    int amount = 0;
};
struct OBGetBalance {};

struct OBModel {
    int balance = 0;
    int execute(const OBDeposit& a) {
        balance += a.amount;
        return balance;
    }
    int execute(const OBGetBalance& /*a*/) { return balance; }
};

BRIDGE_REGISTER_MODEL(OBModel, "OB_Model")
BRIDGE_REGISTER_ACTION(OBModel, OBDeposit, "OB_Deposit")
BRIDGE_REGISTER_ACTION(OBModel, OBGetBalance, "OB_GetBalance", ::morph::model::Loggable::No)

// ── InMemoryActionLog: idempotencyKey dedup ─────────────────────────────────

TEST_CASE("InMemoryActionLog::append: dedups a repeated non-empty idempotencyKey", "[outbox][action_log]") {
    InMemoryActionLog log;
    log.append(makeEntry("OB_Model", "acct-1", "OB_Deposit", "row-1"));
    log.append(makeEntry("OB_Model", "acct-1", "OB_Deposit", "row-1"));  // simulated re-relay

    auto entries = log.entries();
    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].idempotencyKey == "row-1");
}

TEST_CASE("InMemoryActionLog::append: empty idempotencyKey never dedups", "[outbox][action_log]") {
    InMemoryActionLog log;
    log.append(makeEntry("OB_Model", "acct-1", "OB_Deposit"));  // idempotencyKey == ""
    log.append(makeEntry("OB_Model", "acct-1", "OB_Deposit"));

    REQUIRE(log.entries().size() == 2);  // backward compatible: no key means no dedup
}

// ── FileActionLog: idempotencyKey dedup ─────────────────────────────────────

TEST_CASE("FileActionLog::append: dedups a repeated non-empty idempotencyKey within one process",
          "[outbox][file_action_log]") {
    TempFile tmp{"outbox_dedup"};
    FileActionLog log{tmp.path};
    log.append(makeEntry("OB_Model", "acct-1", "OB_Deposit", "row-1"));
    log.append(makeEntry("OB_Model", "acct-1", "OB_Deposit", "row-1"));
    log.flush();

    REQUIRE(log.entries().size() == 1);
}

TEST_CASE("FileActionLog: idempotencyKey dedup survives reopening the same file (simulated restart)",
          "[outbox][file_action_log]") {
    TempFile tmp{"outbox_restart"};
    {
        FileActionLog log{tmp.path};
        log.append(makeEntry("OB_Model", "acct-1", "OB_Deposit", "row-1"));
        log.flush();
    }  // "process" ends here — the FileActionLog and its fd are gone

    FileActionLog reopened{tmp.path};  // "restart": rebuilds the seen-key set from disk
    reopened.append(makeEntry("OB_Model", "acct-1", "OB_Deposit", "row-1"));  // re-relay after crash
    reopened.flush();

    REQUIRE(reopened.entries().size() == 1);
}

// ── IModelHolder::setOutboxManaged / isOutboxManaged ────────────────────────

TEST_CASE("IModelHolder::setOutboxManaged: suppresses recordIfAttached while hasActionLog stays true",
          "[outbox][holder]") {
    auto holder = morph::model::detail::ModelFactory::create<OBModel>();
    auto log = std::make_shared<InMemoryActionLog>();
    holder->attachActionLog(log, "acct-1");
    holder->setOutboxManaged(true);

    REQUIRE(holder->hasActionLog());
    REQUIRE(holder->isOutboxManaged());

    holder->recordIfAttached(makeEntry("OB_Model", "", "OB_Deposit"));

    REQUIRE(log->entries().empty());
}

TEST_CASE("IModelHolder::setOutboxManaged: defaults to false, ordinary recording is unaffected", "[outbox][holder]") {
    auto holder = morph::model::detail::ModelFactory::create<OBModel>();
    auto log = std::make_shared<InMemoryActionLog>();
    holder->attachActionLog(log, "acct-1");

    REQUIRE_FALSE(holder->isOutboxManaged());
    holder->recordIfAttached(makeEntry("OB_Model", "", "OB_Deposit"));

    REQUIRE(log->entries().size() == 1);
}

TEST_CASE("ActionDispatcher: outbox-managed holder does not auto-append despite an attached log",
          "[outbox][dispatch]") {
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    registry.registerModel<OBModel>("OB_Model");
    dispatcher.registerAction<OBModel, OBDeposit>("OB_Model", "OB_Deposit");

    auto holder = registry.create("OB_Model");
    auto log = std::make_shared<InMemoryActionLog>();
    holder->attachActionLog(log, "acct-1");
    holder->setOutboxManaged(true);

    auto depositJson = morph::model::ActionTraits<OBDeposit>::toJson(OBDeposit{.amount = 10});
    REQUIRE(dispatcher.dispatch("OB_Model", "OB_Deposit", *holder, depositJson) == "10");

    REQUIRE(log->entries().empty());  // suppressed — the model is expected to log itself
}

TEST_CASE("Bridge/LocalBackend: outbox-managed holder does not auto-append despite an attached log",
          "[outbox][bridge]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};

    auto log = std::make_shared<InMemoryActionLog>();
    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = "OB_Model";
    binding->modelFactory = [log] {
        auto holder = morph::model::detail::ModelFactory::create<OBModel>();
        holder->attachActionLog(log, "acct-1");
        holder->setOutboxManaged(true);
        return holder;
    };
    morph::bridge::BridgeHandler<OBModel> handler{bridge, &cbExec, binding};

    std::atomic<int> depositResult{-1};
    handler.execute(OBDeposit{.amount = 20})
        .then([&](int v) { depositResult.store(v); })
        .onError([](const std::exception_ptr&) {});
    REQUIRE(morph::testing::waitUntil([&] { return depositResult.load() != -1; }));
    REQUIRE(depositResult.load() == 20);

    REQUIRE(log->entries().empty());
}

// ── journal::OutboxRelay ─────────────────────────────────────────────────────

TEST_CASE("OutboxRelay::relay(): moves drained rows to sink and marks them relayed", "[outbox][relay]") {
    std::vector<LogEntry> pending{
        makeEntry("OB_Model", "acct-1", "OB_Deposit", "row-1"),
        makeEntry("OB_Model", "acct-1", "OB_Deposit", "row-2"),
    };
    int markCalls = 0;
    auto sink = std::make_shared<InMemoryActionLog>();

    OutboxRelay relay;
    relay.drainOutbox = [&] { return pending; };
    relay.markRelayed = [&](std::span<const LogEntry> rows) {
        ++markCalls;
        REQUIRE(rows.size() == 2);
        for (const auto& row : rows) {
            std::erase_if(pending, [&](const LogEntry& e) { return e.idempotencyKey == row.idempotencyKey; });
        }
    };
    relay.sink = sink;

    auto result = relay.relay();

    REQUIRE(result.relayed == 2);
    REQUIRE(markCalls == 1);
    REQUIRE(pending.empty());
    REQUIRE(sink->entries().size() == 2);
}

TEST_CASE("OutboxRelay::relay(): no-op when drainOutbox returns nothing", "[outbox][relay]") {
    bool markCalled = false;
    auto sink = std::make_shared<InMemoryActionLog>();

    OutboxRelay relay;
    relay.drainOutbox = [] { return std::vector<LogEntry>{}; };
    relay.markRelayed = [&](std::span<const LogEntry>) { markCalled = true; };
    relay.sink = sink;

    auto result = relay.relay();

    REQUIRE(result.relayed == 0);
    REQUIRE_FALSE(markCalled);
    REQUIRE(sink->entries().empty());
}

TEST_CASE("OutboxRelay::relay(): re-relay after a simulated crash between append and markRelayed dedups",
          "[outbox][relay]") {
    // drainOutbox always returns the same fixed row, modeling an outbox table
    // whose row never actually got marked relayed (the crash the spec's testing
    // section describes: append succeeded, the mark-relayed commit did not).
    std::vector<LogEntry> fixedRows{makeEntry("OB_Model", "acct-1", "OB_Deposit", "row-1")};
    int markCalls = 0;
    auto sink = std::make_shared<InMemoryActionLog>();

    OutboxRelay relay;
    relay.drainOutbox = [&] { return fixedRows; };
    relay.markRelayed = [&](std::span<const LogEntry> rows) {
        ++markCalls;
        REQUIRE(rows.size() == 1);
    };
    relay.sink = sink;

    auto first = relay.relay();
    auto second = relay.relay();  // simulated retry after the "crash"

    REQUIRE(first.relayed == 1);
    REQUIRE(second.relayed == 1);
    REQUIRE(markCalls == 2);
    REQUIRE(sink->entries().size() == 1);  // dedup collapsed the retry
}

TEST_CASE("OutboxRelay::relay(): a null dependency is logged, not rejected, at call time", "[outbox][relay]") {
    std::vector<std::string> logged;
    morph::log::ScopedLoggerOverride guard{
        [&](morph::log::LogLevel, std::string_view msg) { logged.emplace_back(msg); },
        morph::log::LogLevel::debug,
    };

    auto sink = std::make_shared<InMemoryActionLog>();
    OutboxRelay relay;
    relay.drainOutbox = [] { return std::vector<LogEntry>{makeEntry("OB_Model", "acct-1", "OB_Deposit", "row-1")}; };
    relay.sink = sink;
    // relay.markRelayed left null on purpose.

    REQUIRE_THROWS_AS(relay.relay(), std::bad_function_call);

    bool sawWarning = std::any_of(logged.begin(), logged.end(), [](const std::string& line) {
        return line.find("markRelayed") != std::string::npos;
    });
    REQUIRE(sawWarning);
    REQUIRE(sink->entries().size() == 1);  // the append + flush happened before markRelayed threw
}

TEST_CASE("OutboxRelay + journal::replay: relayed entries reconstruct state matching direct execution",
          "[outbox][relay][journal]") {
    morph::model::detail::ModelRegistryFactory registry;
    morph::model::detail::ActionDispatcher dispatcher;
    registry.registerModel<OBModel>("OB_Model");
    dispatcher.registerAction<OBModel, OBDeposit>("OB_Model", "OB_Deposit");

    std::vector<LogEntry> pending{
        makeEntry("OB_Model", "acct-1", "OB_Deposit", "row-1"),
        makeEntry("OB_Model", "acct-1", "OB_Deposit", "row-2"),
    };
    pending[0].payload = morph::model::ActionTraits<OBDeposit>::toJson(OBDeposit{.amount = 10});
    pending[1].payload = morph::model::ActionTraits<OBDeposit>::toJson(OBDeposit{.amount = 5});

    auto sink = std::make_shared<InMemoryActionLog>();
    OutboxRelay relay;
    relay.drainOutbox = [&] { return pending; };
    relay.markRelayed = [&](std::span<const LogEntry> rows) {
        for (const auto& row : rows) {
            std::erase_if(pending, [&](const LogEntry& e) { return e.idempotencyKey == row.idempotencyKey; });
        }
    };
    relay.sink = sink;

    auto result = relay.relay();
    REQUIRE(result.relayed == 2);
    REQUIRE(pending.empty());

    auto reconstructed = morph::journal::replay("OB_Model", sink->entries(), registry, dispatcher);
    REQUIRE(reconstructed->into<OBModel>().balance == 15);
}

TEST_CASE("OutboxRelay::relay(): a null drainOutbox is logged, not rejected, at call time", "[outbox][relay]") {
    std::vector<std::string> logged;
    morph::log::ScopedLoggerOverride guard{
        [&](morph::log::LogLevel, std::string_view msg) { logged.emplace_back(msg); },
        morph::log::LogLevel::debug,
    };

    auto sink = std::make_shared<InMemoryActionLog>();
    OutboxRelay relay;
    // relay.drainOutbox left null on purpose.
    relay.markRelayed = [](std::span<const LogEntry>) {};
    relay.sink = sink;

    REQUIRE_THROWS_AS(relay.relay(), std::bad_function_call);

    bool sawWarning = std::any_of(logged.begin(), logged.end(), [](const std::string& line) {
        return line.find("drainOutbox") != std::string::npos;
    });
    REQUIRE(sawWarning);
    REQUIRE(sink->entries().empty());  // drainOutbox() threw before anything reached the sink
}

TEST_CASE("OutboxRelay::relay(): a null sink is logged, not rejected, at call time", "[outbox][relay]") {
    // Unlike the null-drainOutbox/markRelayed cases above, relay() must not
    // actually dereference the null sink (sink->append() on a null
    // shared_ptr<IActionLog> is a null-pointer virtual dispatch -- real UB,
    // not a catchable exception -- see the comment below). Pairing the null
    // sink with an empty drainOutbox() keeps relay() on its early-return path
    // (`if (rows.empty()) return {};`), which runs *after*
    // logIfAnyDepNull() but *before* any sink use -- so the null-sink warning
    // fires without ever calling through the null pointer.
    std::vector<std::string> logged;
    morph::log::ScopedLoggerOverride guard{
        [&](morph::log::LogLevel, std::string_view msg) { logged.emplace_back(msg); },
        morph::log::LogLevel::debug,
    };

    OutboxRelay relay;
    relay.drainOutbox = [] { return std::vector<LogEntry>{}; };
    relay.markRelayed = [](std::span<const LogEntry>) {};
    // relay.sink left null on purpose.

    auto result = relay.relay();
    REQUIRE(result.relayed == 0);

    bool sawWarning = std::any_of(logged.begin(), logged.end(), [](const std::string& line) {
        return line.find("null sink") != std::string::npos;
    });
    REQUIRE(sawWarning);
}

// A null `sink` reaching relay()'s *sink-using* path (i.e. with drainOutbox()
// returning rows) is deliberately NOT exercised here: unlike
// `drainOutbox`/`markRelayed` (null std::function -> catchable
// std::bad_function_call), `sink` is a std::shared_ptr<IActionLog>, so
// `sink->append(row)` on a null sink is a null-pointer virtual dispatch --
// real UB that crashes the process (confirmed: SIGSEGV under this build), not
// a C++ exception a REQUIRE_THROWS_AS could observe. This is the documented
// contract (see outbox.hpp's class doc and docs/spec/journal/journal.md:
// "invoking a null member still throws (std::bad_function_call) or crashes
// as usual"), not an oversight -- forcing it into a portable, deterministic
// unit test would need a signal/guard-page harness like
// test_bridge_lifetime.cpp's POSIX-only hasSubscribers() case, which is far
// more machinery than this one branch warrants and still would not run on
// Windows/MSVC, where this suite also builds. The logIfAnyDepNull() call's
// null-sink warning line itself is now covered by the empty-drainOutbox test
// above -- only the subsequent crash on a non-empty drain is left
// unexercised. Tracked as LASTRADA-Software/morph#95.

TEST_CASE("OutboxRelay + FileActionLog: re-relay after a simulated process restart dedups via the sink",
          "[outbox][relay][file_action_log]") {
    TempFile tmp{"outbox_relay_restart"};
    LogEntry row = makeEntry("OB_Model", "acct-1", "OB_Deposit", "row-1");
    row.payload = morph::model::ActionTraits<OBDeposit>::toJson(OBDeposit{.amount = 7});

    {
        auto sink = std::make_shared<FileActionLog>(tmp.path);
        OutboxRelay relay;
        relay.drainOutbox = [&] { return std::vector<LogEntry>{row}; };
        relay.markRelayed = [](std::span<const LogEntry>) {};  // simulate: mark-relayed commit never happened
        relay.sink = sink;

        auto result = relay.relay();
        REQUIRE(result.relayed == 1);
    }  // "process" ends; the FileActionLog and its fd are gone

    {
        auto sink = std::make_shared<FileActionLog>(tmp.path);  // "restart": rebuilds dedup set from disk
        OutboxRelay relay;
        relay.drainOutbox = [&] { return std::vector<LogEntry>{row}; };  // outbox table still shows it unrelayed
        bool marked = false;
        relay.markRelayed = [&](std::span<const LogEntry>) { marked = true; };
        relay.sink = sink;

        auto result = relay.relay();
        REQUIRE(result.relayed == 1);
        REQUIRE(marked);
        REQUIRE(sink->entries().size() == 1);  // dedup: still exactly one entry on disk
    }
}

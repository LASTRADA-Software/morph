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

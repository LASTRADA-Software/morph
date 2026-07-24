// SPDX-License-Identifier: Apache-2.0
//
// Concept: the journal (morph::journal) — a durable, append-only record of
// every action executed against a model instance.
//
// Three small demonstrations:
//   1. Attaching an IActionLog to a model instance and watching a successful
//      execute get recorded automatically — no per-action code needed.
//   2. The idempotency-key dedup an IActionLog sink applies: appending the
//      same LogEntry::idempotencyKey twice only ever stores it once. This is
//      what makes a sink safe to use as an at-least-once relay target.
//   3. The transactional-outbox pattern: a model that commits its own outbox
//      row in its own transaction can call setOutboxManaged(true) to suppress
//      the framework's automatic per-execute append, then a separate
//      journal::OutboxRelay moves the committed rows into a durable
//      IActionLog on its own schedule.
//
// Full design reference: docs/spec/journal/journal.md.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/model.hpp>
#include <morph/core/registry.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/journal/outbox.hpp>
#include <span>
#include <string>
#include <vector>

using morph::journal::InMemoryActionLog;
using morph::journal::LogEntry;
using morph::journal::OutboxRelay;

// Model/action types need external (file-scope) linkage — Glaze's reflection
// mangles a name for the type and needs it to be visible outside this TU's
// anonymous namespace, even though nothing outside this file ever uses them.
// "JournalDemo" is this file's unique type-id prefix so it can never collide
// with another example file or with tests/test_*.cpp's own registrations.

struct JournalDemoDeposit {
    int amount = 0;
};

struct JournalDemoModel {
    int balance = 0;
    int execute(const JournalDemoDeposit& action) {
        balance += action.amount;
        return balance;
    }
};

BRIDGE_REGISTER_MODEL(JournalDemoModel, "JournalDemo_Model")
BRIDGE_REGISTER_ACTION(JournalDemoModel, JournalDemoDeposit, "JournalDemo_Deposit")

// ── 1. Attach a log, dispatch an action, see it recorded ───────────────────
//
// Reach for this when you need an audit trail of "what happened to this
// instance" — attachActionLog() is the one call that turns it on; every
// dispatch through ActionDispatcher (the same path RemoteServer uses) then
// records itself with zero further wiring.

TEST_CASE("journal: attaching an InMemoryActionLog records a successful execute", "[concepts][journal]") {
    auto holder = morph::model::detail::ModelFactory::create<JournalDemoModel>();
    auto log = std::make_shared<InMemoryActionLog>();

    // "acct-1" becomes LogEntry::entityKey on every entry recorded for this
    // instance — the stable identity you'd filter entries()/replay() by.
    holder->attachActionLog(log, "acct-1");
    REQUIRE(holder->hasActionLog());

    auto depositJson = morph::model::ActionTraits<JournalDemoDeposit>::toJson(JournalDemoDeposit{.amount = 50});
    auto resultJson = morph::model::detail::ActionDispatcher::instance().dispatch(
        "JournalDemo_Model", "JournalDemo_Deposit", *holder, depositJson);
    REQUIRE(resultJson == "50");

    auto entries = log->entries("acct-1");
    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].modelType == "JournalDemo_Model");
    REQUIRE(entries[0].actionType == "JournalDemo_Deposit");
    REQUIRE(entries[0].payload == depositJson);
    REQUIRE(entries[0].result == "50");
}

// ── 2. Idempotency-key dedup ────────────────────────────────────────────────
//
// Reach for this when the same logical write might be retried (a crash
// between committing a row and marking it "sent", a network retry, …): give
// each logical operation a stable idempotencyKey and let the sink collapse
// duplicates for you instead of hand-rolling a dedup table.

TEST_CASE("journal: appending the same idempotencyKey twice stores only one entry", "[concepts][journal]") {
    InMemoryActionLog log;

    LogEntry entry;
    entry.modelType = "JournalDemo_Model";
    entry.entityKey = "acct-1";
    entry.actionType = "JournalDemo_Deposit";
    entry.idempotencyKey = "outbox-row-1";

    log.append(entry);
    log.append(entry);  // simulates a re-relay of the same logical row after a crash

    REQUIRE(log.entries().size() == 1);
}

// ── 3. Transactional outbox: setOutboxManaged + OutboxRelay ────────────────
//
// Reach for this when a model has its own transactional store (SQL, a file)
// and wants its state mutation and its journal entry to commit atomically,
// instead of the framework's default two-independent-writes behavior (mutate,
// then auto-append — which can leave the store and the log diverged after a
// crash). The model writes its own outbox row in its own transaction, then
// calls setOutboxManaged(true) so recordIfAttached becomes a no-op for it; a
// separate OutboxRelay drains that outbox on whatever schedule the host
// chooses and forwards rows to a durable IActionLog.

TEST_CASE("journal: setOutboxManaged suppresses auto-append; OutboxRelay delivers the row instead",
          "[concepts][journal][outbox]") {
    auto holder = morph::model::detail::ModelFactory::create<JournalDemoModel>();
    auto log = std::make_shared<InMemoryActionLog>();
    holder->attachActionLog(log, "acct-1");
    holder->setOutboxManaged(true);  // this instance logs itself; the framework must not double-log it

    auto depositJson = morph::model::ActionTraits<JournalDemoDeposit>::toJson(JournalDemoDeposit{.amount = 20});
    morph::model::detail::ActionDispatcher::instance().dispatch("JournalDemo_Model", "JournalDemo_Deposit", *holder,
                                                                depositJson);

    // hasActionLog() still reports the attached log — only the auto-append is
    // suppressed, so the model can still decide to log itself.
    REQUIRE(holder->hasActionLog());
    REQUIRE(log->entries().empty());

    // Stand in for "the model's own outbox table": one committed-but-unrelayed
    // row, shaped like a LogEntry, with a stable idempotencyKey.
    std::vector<LogEntry> outboxTable{LogEntry{
        .seq = 0,
        .modelType = "JournalDemo_Model",
        .entityKey = "acct-1",
        .actionType = "JournalDemo_Deposit",
        .payload = depositJson,
        .result = "20",
        .principal = {},  // the authenticated caller identity, if any (session::Context::principal); unused here
        .timestampMs = 0,
        .idempotencyKey = "acct-1-op-1",
    }};

    OutboxRelay relay;
    relay.drainOutbox = [&] { return outboxTable; };
    relay.markRelayed = [&](std::span<const LogEntry> rows) {
        for (const auto& row : rows) {
            std::erase_if(outboxTable, [&](const LogEntry& e) { return e.idempotencyKey == row.idempotencyKey; });
        }
    };
    relay.sink = log;

    auto result = relay.relay();

    REQUIRE(result.relayed == 1);
    REQUIRE(outboxTable.empty());         // the outbox table's row is now marked relayed
    REQUIRE(log->entries().size() == 1);  // ...and durably present in the real sink
    REQUIRE(log->entries()[0].idempotencyKey == "acct-1-op-1");
}

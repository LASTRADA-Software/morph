// SPDX-License-Identifier: Apache-2.0
//
// The scenarios SYNC-BENCHMARK.md describes, as executable claims.
//
// The document states morph's answer to concurrent edits: server arrival
// order, with one action as the unit of conflict. These cases hold that
// answer to account -- particularly the part that is easy to assert in prose
// and easy to leave unimplemented in code, that a stale edit is *rejected*
// rather than merged or silently applied.

#include "ledger/core/errors.hpp"
#include "ledger/db/ledger_entity.hpp"
#include "ledger/models/ledger_model.hpp"
#include "ledger/models/rule_model.hpp"
#include "testkit/db_fixture.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <morph/session/session.hpp>

#include <string>

namespace {

[[nodiscard]] morph::session::Context contextFor(std::string principal) {
    morph::session::Context ctx;
    ctx.principal = std::move(principal);
    return ctx;
}

/// @brief Installs @p principal for this scope. `_ctx` is a member declared
///        before `_scope` because `ScopedContext` holds a reference to it.
class ScopedPrincipal {
  public:
    explicit ScopedPrincipal(std::string principal) : _ctx{contextFor(std::move(principal))}, _scope{_ctx} {}

  private:
    morph::session::Context _ctx;
    morph::session::detail::ScopedContext _scope;
};

[[nodiscard]] ledger::LedgerId seedLedger() {
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord row;
    row.name = Lightweight::SqlAnsiString<128>{"Personal"};
    mapper.Create(row);
    return ledger::LedgerId{static_cast<std::int64_t>(row.id.Value())};
}

}  // namespace

TEST_CASE("Scenario B: a stale base-version edit is rejected outright, never merged",
          "[ledger][sync-benchmark]") {
    // Two clients read the same rule at version 1. Client 1 commits, taking it
    // to version 2. Client 2's edit still carries base version 1.
    morph::ladder::testkit::DbFixture fixture;
    const auto ledgerId = seedLedger();
    const ScopedPrincipal alice{"alice"};

    ledger::RuleModel model;
    const auto ruleId = model.execute(ledger::CreateRule{.ledgerId = ledgerId,
                                                          .trigger = ledger::RuleTrigger::DescriptionContains,
                                                          .matchText = "COFFEE",
                                                          .action = ledger::RuleAction::SetCategory,
                                                          .actionValue = "7"});
    REQUIRE(ruleId.hasValue());

    // Both clients read version 1.
    const std::int32_t baseVersionBothClientsRead = 1;

    // Client 1 commits first: server arrival order decides, and it arrived.
    const auto afterFirst = model.execute(ledger::UpdateRule{
        .ruleId = ruleId,
        .matchText = "ESPRESSO",
        .actionValue = "8",
        .expectedVersion = baseVersionBothClientsRead});
    CHECK(afterFirst.version == 2);
    CHECK(afterFirst.matchText == "ESPRESSO");

    // Client 2's edit was composed against version 1, which no longer exists.
    // Rejected outright -- not merged with client 1's edit, and not applied
    // over the top of it.
    CHECK_THROWS_AS(model.execute(ledger::UpdateRule{.ruleId = ruleId,
                                                      .matchText = "FLAT WHITE",
                                                      .actionValue = "9",
                                                      .expectedVersion = baseVersionBothClientsRead}),
                    ledger::VersionConflict);

    // And the rejection left client 1's edit intact. A silent overwrite would
    // read "FLAT WHITE" here; a merge would read some combination. Neither is
    // what this framework does.
    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<ledger::db::RuleRecord>()
                    .Where(::Lightweight::FieldNameOf<&ledger::db::RuleRecord::id>, "=", *ruleId)
                    .All();
    REQUIRE(rows.size() == 1);
    CHECK(std::string{rows.front().matchText.Value().ToStringView()} == "ESPRESSO");
    CHECK(std::string{rows.front().actionValue.Value().ToStringView()} == "8");
    CHECK(rows.front().version.Value() == 2);
}

TEST_CASE("A re-read after a conflict succeeds: rejection is recoverable, not terminal",
          "[ledger][sync-benchmark]") {
    // The other half of Scenario B's claim. Rejecting a stale edit is only
    // reasonable if the client can then re-read and reapply -- otherwise
    // "rejected outright" would mean the user's work is simply lost, which is
    // a worse outcome than the merge the document argues against.
    morph::ladder::testkit::DbFixture fixture;
    const auto ledgerId = seedLedger();
    const ScopedPrincipal alice{"alice"};

    ledger::RuleModel model;
    const auto ruleId = model.execute(ledger::CreateRule{.ledgerId = ledgerId,
                                                          .trigger = ledger::RuleTrigger::DescriptionContains,
                                                          .matchText = "COFFEE",
                                                          .action = ledger::RuleAction::SetCategory,
                                                          .actionValue = "7"});
    model.execute(ledger::UpdateRule{
        .ruleId = ruleId, .matchText = "ESPRESSO", .actionValue = "8", .expectedVersion = 1});

    CHECK_THROWS_AS(model.execute(ledger::UpdateRule{
                        .ruleId = ruleId, .matchText = "FLAT WHITE", .actionValue = "9", .expectedVersion = 1}),
                    ledger::VersionConflict);

    // Re-read, then reapply against what is actually there now.
    const auto reapplied = model.execute(ledger::UpdateRule{
        .ruleId = ruleId, .matchText = "FLAT WHITE", .actionValue = "9", .expectedVersion = 2});
    CHECK(reapplied.matchText == "FLAT WHITE");
    CHECK(reapplied.version == 3);
}

TEST_CASE("An update without an expected version still applies unconditionally",
          "[ledger][sync-benchmark]") {
    // The opt-in half. Every caller predating optimistic concurrency passes no
    // expectedVersion and must keep working -- otherwise adding the mechanism
    // would silently turn existing unconditional updates into conflicts.
    morph::ladder::testkit::DbFixture fixture;
    const auto ledgerId = seedLedger();
    const ScopedPrincipal alice{"alice"};

    ledger::RuleModel model;
    const auto ruleId = model.execute(ledger::CreateRule{.ledgerId = ledgerId,
                                                          .trigger = ledger::RuleTrigger::DescriptionContains,
                                                          .matchText = "COFFEE",
                                                          .action = ledger::RuleAction::SetCategory,
                                                          .actionValue = "7"});
    model.execute(ledger::UpdateRule{.ruleId = ruleId, .matchText = "ESPRESSO", .actionValue = "8"});

    // Version is now 2, but this edit names no base version at all.
    const auto unconditional =
        model.execute(ledger::UpdateRule{.ruleId = ruleId, .matchText = "CORTADO", .actionValue = "9"});
    CHECK(unconditional.matchText == "CORTADO");
    CHECK(unconditional.version == 3);
}

TEST_CASE("Scenario A, in the form this rung can express: two clients reversing one transaction",
          "[ledger][sync-benchmark]") {
    // SYNC-BENCHMARK.md's Scenario A asks for two clients editing different
    // fields of a single transaction. This rung has no such action, and that
    // is deliberate rather than unfinished: design spec §6 makes a posted
    // journal entry an audit record, corrected by a *new* compensating entry
    // rather than edited in place ("undo must itself be a new, visible entry,
    // not an erasure of the original one"). There is no UpdateTransaction to
    // race, so Scenario A cannot be run as written.
    //
    // The nearest question the rung *can* answer, and the one a user actually
    // hits: two clients both reverse the same transaction while disconnected,
    // and both queued actions arrive.
    morph::ladder::testkit::DbFixture fixture;
    const auto ledgerId = seedLedger();
    const ScopedPrincipal alice{"alice"};

    ledger::LedgerModel model;
    const auto checking = model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                                            .name = "Checking",
                                                            .kind = ledger::AccountKind::Asset,
                                                            .currency = ledger::Currency::USD});
    const auto groceries = model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                                             .name = "Groceries",
                                                             .kind = ledger::AccountKind::Expense,
                                                             .currency = ledger::Currency::USD});

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    const auto amount = [](std::int64_t cents) {
        return morph::math::Rational{Numerator{cents}, Denominator{1}, DecimalPlaces{2}};
    };

    model.execute(ledger::StoreTransaction{
        .ledgerId = ledgerId,
        .description = "Weekly shop",
        .date = morph::time::Timestamp::now(),
        .legs = {ledger::TransactionLeg{.accountId = checking.id, .amount = amount(-5000)},
                 ledger::TransactionLeg{.accountId = groceries.id, .amount = amount(5000)}}});

    Lightweight::DataMapper mapper;
    auto journals = mapper.Query<ledger::db::TransactionJournalRecord>()
                        .Where(::Lightweight::FieldNameOf<&ledger::db::TransactionJournalRecord::ledger>,
                               "=", *ledgerId)
                        .All();
    REQUIRE(journals.size() == 1);
    const auto journalId = ledger::JournalId{static_cast<std::int64_t>(journals.front().id.Value())};

    // Client 1's queued reversal arrives first and applies: the shop is undone.
    model.execute(ledger::UndoTransaction{.ledgerId = ledgerId, .journalId = journalId});
    {
        const auto afterFirst = model.execute(ledger::GetLedger{.ledgerId = ledgerId});
        for (const auto& account: afterFirst.accounts) {
            INFO("after first reversal: " << account.name);
            CHECK(account.balance.numerator == 0);
        }
    }

    // Client 2's queued reversal names the same journal and arrives second.
    // Note what a zero-sum check cannot tell you here: a compensating entry is
    // itself balanced, so the ledger still sums to zero whether this is
    // rejected or applied a second time. Only the per-account balances
    // distinguish the two.
    CHECK_THROWS_AS(model.execute(ledger::UndoTransaction{.ledgerId = ledgerId, .journalId = journalId}),
                    ledger::AlreadyReversed);

    // Rejected outright, not applied and not quietly swallowed: the ledger
    // still holds exactly two journals, the shop and its one reversal.
    const auto journalsAfter = mapper.Query<ledger::db::TransactionJournalRecord>()
                                   .Where(::Lightweight::FieldNameOf<&ledger::db::TransactionJournalRecord::ledger>,
                                          "=", *ledgerId)
                                   .All();
    CHECK(journalsAfter.size() == 2);

    const auto afterSecond = model.execute(ledger::GetLedger{.ledgerId = ledgerId});
    morph::math::Rational total = morph::math::Rational::zero(DecimalPlaces{2});
    for (const auto& account: afterSecond.accounts) {
        total = total + account.balance;
    }
    CHECK(total.numerator == 0);

    for (const auto& account: afterSecond.accounts) {
        INFO("after second reversal: " << account.name << " = " << account.balance.numerator);
        // The user reversed one transaction, once, from two devices. Their
        // Checking account must not end up +50.00 -- money that never existed
        // -- because a queued action replayed.
        CHECK(account.balance.numerator == 0);
    }

    // The guard is per-journal, not a blanket ban on reversing a reversal:
    // deliberately undoing the correction is still a legitimate action.
    const auto reversalId =
        ledger::JournalId{static_cast<std::int64_t>(journalsAfter.back().id.Value())};
    CHECK_NOTHROW(model.execute(ledger::UndoTransaction{.ledgerId = ledgerId, .journalId = reversalId}));
}

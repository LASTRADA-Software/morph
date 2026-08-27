// SPDX-License-Identifier: Apache-2.0
//
// Coverage for this rung's journaled outcomes (`morph::journal::Outcome`,
// `morph/journal/action_log.hpp`).
//
// `LedgerModel::logAction`/`RuleModel::logAction`/`BudgetModel::logAction`
// stamp every entry `Outcome::Succeeded` unconditionally -- they run only at
// the end of a successful `execute()`. A caught domain exception
// (`ZeroSumViolation`, `AlreadyReversed`, `VersionConflict`,
// `EmptyPrincipalError`, and friends) previously left `execute()` before
// reaching that call, so the refused attempt left **no journal entry at
// all** -- in the rung whose stated purpose is a full audit trail. lims's
// `SelfJournal::recordFailure` (`include/lims/core/self_journal.hpp`)
// demonstrates the correct pattern: a rejected attempt is itself
// audit-worthy, so it gets its own entry with `Outcome::Failed` and the
// rejecting exception's text in `error`.
//
// These cases pin that a refused mutating action now leaves exactly that
// entry, across all three of this rung's hand-rolled journaling models.

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/journal/action_log.hpp>
#include <morph/session/session.hpp>
#include <string>

#include "ledger/core/errors.hpp"
#include "ledger/core/money.hpp"
#include "ledger/db/ledger_entity.hpp"
#include "ledger/models/budget_model.hpp"
#include "ledger/models/ledger_model.hpp"
#include "ledger/models/rule_model.hpp"
#include "testkit/db_fixture.hpp"

namespace {

/// @brief A `Context` carrying only @p principal -- see
///        `test_ledger_model.cpp`'s own identical `contextFor` for why this
///        is not a designated initializer.
[[nodiscard]] morph::session::Context contextFor(std::string principal) {
    morph::session::Context ctx;
    ctx.principal = std::move(principal);
    return ctx;
}

class ScopedPrincipal {
public:
    explicit ScopedPrincipal(std::string principal) : _ctx{contextFor(std::move(principal))}, _scope{_ctx} {}

private:
    morph::session::Context _ctx;
    morph::session::detail::ScopedContext _scope;
};

}  // namespace

TEST_CASE("A ZeroSumViolation leaves a Failed journal entry, not no entry at all", "[ledger][journal][outcome]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::LedgerModel model;
    const ScopedPrincipal principal{"alice"};
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                      .name = "Checking",
                                      .kind = ledger::AccountKind::Asset,
                                      .currency = ledger::Currency::USD});
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                      .name = "Groceries",
                                      .kind = ledger::AccountKind::Expense,
                                      .currency = ledger::Currency::USD});
    auto ledgerState = model.execute(ledger::GetLedger{.ledgerId = ledgerId});

    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    model.attachActionLog(log, std::to_string(*ledgerId));

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    REQUIRE_THROWS_AS(
        model.execute(ledger::StoreTransaction{
            .ledgerId = ledgerId,
            .description = "Bad txn",
            .date = morph::time::Timestamp::now(),
            .legs = {ledger::TransactionLeg{
                         .accountId = ledgerState.accounts[0].id,
                         .amount = morph::math::Rational{Numerator{-5000}, Denominator{1}, DecimalPlaces{2}}},
                     ledger::TransactionLeg{
                         .accountId = ledgerState.accounts[1].id,
                         .amount = morph::math::Rational{Numerator{4000}, Denominator{1}, DecimalPlaces{2}}}}}),
        ledger::ZeroSumViolation);

    auto entries = log->entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().outcome == morph::journal::Outcome::Failed);
    CHECK_FALSE(entries.front().error.empty());
    CHECK(entries.front().result.empty());
    CHECK(entries.front().actionType == std::string{morph::model::ActionTraits<ledger::StoreTransaction>::typeId()});
}

TEST_CASE("An AlreadyReversed refusal leaves a Failed journal entry", "[ledger][journal][outcome]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::LedgerModel model;
    const ScopedPrincipal principal{"alice"};
    const auto checkingId = model
                                .execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                                             .name = "Checking",
                                                             .kind = ledger::AccountKind::Asset,
                                                             .currency = ledger::Currency::USD})
                                .id;
    const auto groceriesId = model
                                 .execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                                              .name = "Groceries",
                                                              .kind = ledger::AccountKind::Expense,
                                                              .currency = ledger::Currency::USD})
                                 .id;

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    model.execute(ledger::StoreTransaction{
        .ledgerId = ledgerId,
        .description = "Weekly shop",
        .date = morph::time::Timestamp::now(),
        .legs = {ledger::TransactionLeg{
                     .accountId = checkingId,
                     .amount = morph::math::Rational{Numerator{-5000}, Denominator{1}, DecimalPlaces{2}}},
                 ledger::TransactionLeg{
                     .accountId = groceriesId,
                     .amount = morph::math::Rational{Numerator{5000}, Denominator{1}, DecimalPlaces{2}}}}});

    auto journalRows = mapper.Query<ledger::db::TransactionJournalRecord>()
                           .Where(::Lightweight::FieldNameOf<&ledger::db::TransactionJournalRecord::description>, "=",
                                  Lightweight::SqlAnsiString<256>{"Weekly shop"})
                           .All();
    REQUIRE(journalRows.size() == 1);
    const auto journalId = ledger::JournalId{static_cast<std::int64_t>(journalRows.front().id.Value())};

    // Attach the log only now, so it holds nothing from the setup above.
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    model.attachActionLog(log, std::to_string(*ledgerId));

    model.execute(ledger::UndoTransaction{.ledgerId = ledgerId, .journalId = journalId});
    REQUIRE(log->entries().size() == 1);
    CHECK(log->entries().front().outcome == morph::journal::Outcome::Succeeded);

    // The second reversal of the same journal is refused.
    REQUIRE_THROWS_AS(model.execute(ledger::UndoTransaction{.ledgerId = ledgerId, .journalId = journalId}),
                      ledger::AlreadyReversed);

    auto entries = log->entries();
    REQUIRE(entries.size() == 2);
    CHECK(entries.back().outcome == morph::journal::Outcome::Failed);
    CHECK_FALSE(entries.back().error.empty());
}

TEST_CASE("A VersionConflict on UpdateRule leaves a Failed journal entry (RuleModel)", "[ledger][journal][outcome]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::RuleModel model;
    const ScopedPrincipal principal{"alice"};
    const auto ruleId = model.execute(ledger::CreateRule{.ledgerId = ledgerId,
                                                         .trigger = ledger::RuleTrigger::DescriptionContains,
                                                         .matchText = "coffee",
                                                         .action = ledger::RuleAction::SetCategory,
                                                         .actionValue = "Dining"});

    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    model.attachActionLog(log, std::to_string(*ledgerId));

    // expectedVersion 99 never matches the freshly created rule's version (1).
    REQUIRE_THROWS_AS(model.execute(ledger::UpdateRule{
                          .ruleId = ruleId, .matchText = "tea", .actionValue = "Dining", .expectedVersion = 99}),
                      ledger::VersionConflict);

    auto entries = log->entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().outcome == morph::journal::Outcome::Failed);
    CHECK_FALSE(entries.front().error.empty());
}

TEST_CASE("A NotFound refusal on CreateCategory leaves a Failed journal entry (BudgetModel)",
          "[ledger][journal][outcome]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::BudgetModel model;
    const ScopedPrincipal principal{"alice"};
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    model.attachActionLog(log, std::to_string(*ledgerId));

    // A ledgerId that names no row.
    REQUIRE_THROWS_AS(model.execute(ledger::CreateCategory{.ledgerId = ledger::LedgerId{999999}, .name = "Rent"}),
                      ledger::NotFound);

    auto entries = log->entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().outcome == morph::journal::Outcome::Failed);
    CHECK_FALSE(entries.front().error.empty());
}

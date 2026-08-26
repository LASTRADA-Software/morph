// SPDX-License-Identifier: Apache-2.0
#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/session/session.hpp>

#include "ledger/core/errors.hpp"
#include "ledger/db/ledger_entity.hpp"
#include "ledger/models/budget_model.hpp"
#include "ledger/models/ledger_model.hpp"
#include "testkit/db_fixture.hpp"

namespace {

/// @brief A `Context` carrying only @p principal. See
///        `bookmarks::tests::test_bookmark_model.cpp`'s own `contextFor` for
///        why this is not a designated initializer (`-Wmissing-designated-
///        field-initializers` under `-Weverything`).
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

TEST_CASE("GetBudgetReport sums matching legs in-model, exactly", "[ledger][budget]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::LedgerModel ledgerModel;
    const ScopedPrincipal principal{"alice"};
    ledgerModel.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                            .name = "Checking",
                                            .kind = ledger::AccountKind::Asset,
                                            .currency = ledger::Currency::USD});
    ledgerModel.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                            .name = "Groceries",
                                            .kind = ledger::AccountKind::Expense,
                                            .currency = ledger::Currency::USD});
    auto ledgerState = ledgerModel.execute(ledger::GetLedger{.ledgerId = ledgerId});
    auto checkingId = ledgerState.accounts[0].id;
    auto groceriesId = ledgerState.accounts[1].id;

    ledger::BudgetModel budgetModel;
    auto categoryId = budgetModel.execute(ledger::CreateCategory{.ledgerId = ledgerId, .name = "Food"});
    budgetModel.execute(ledger::LinkAccountToCategory{.accountId = groceriesId, .categoryId = categoryId});
    auto budgetId = budgetModel.execute(
        ledger::CreateBudget{.ledgerId = ledgerId, .name = "Monthly groceries", .categoryId = categoryId});
    budgetModel.execute(ledger::SetBudgetLimit{
        .budgetId = budgetId,
        .month = "2026-01",
        .limit = morph::math::Rational{morph::math::Numerator{20000}, morph::math::Denominator{1},
                                       morph::math::DecimalPlaces{2}},
        .currency = ledger::Currency::USD});

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    // Two StoreTransaction calls against Groceries, both dated in
    // January 2026 -- -30.00 and -45.50, summing to -75.50 spent.
    //
    // An explicit January 2026 instant, not morph::time::Timestamp::now():
    // the real system/test clock at implementation time reads 2026-08-19,
    // which would not land in the "2026-01" month this test's
    // GetBudgetReport call queries. StoreTransaction's `date` field is
    // client-supplied (design spec §1), so constructing it explicitly here
    // is a test-only concern, not a violation of the morph::ladder::now()
    // injectable-clock convention (which binds server-stamped fields only).
    const auto januaryInstant = morph::time::Timestamp{
        morph::time::DateTime{std::chrono::year{2026}, std::chrono::month{1}, std::chrono::day{15},
                              std::chrono::hours{12}, std::chrono::minutes{0}, std::chrono::seconds{0}}};

    ledgerModel.execute(ledger::StoreTransaction{
        .ledgerId = ledgerId,
        .description = "Groceries 1",
        .date = januaryInstant,
        .legs = {ledger::TransactionLeg{
                     .accountId = checkingId,
                     .amount = morph::math::Rational{Numerator{-3000}, Denominator{1}, DecimalPlaces{2}}},
                 ledger::TransactionLeg{
                     .accountId = groceriesId,
                     .amount = morph::math::Rational{Numerator{3000}, Denominator{1}, DecimalPlaces{2}}}}});
    ledgerModel.execute(ledger::StoreTransaction{
        .ledgerId = ledgerId,
        .description = "Groceries 2",
        .date = januaryInstant,
        .legs = {ledger::TransactionLeg{
                     .accountId = checkingId,
                     .amount = morph::math::Rational{Numerator{-4550}, Denominator{1}, DecimalPlaces{2}}},
                 ledger::TransactionLeg{
                     .accountId = groceriesId,
                     .amount = morph::math::Rational{Numerator{4550}, Denominator{1}, DecimalPlaces{2}}}}});

    // A third transaction dated outside the query month (February 2026,
    // same Groceries account) -- proves the date-range filter actually
    // excludes out-of-month legs rather than the test passing merely
    // because no out-of-month transaction exists to wrongly include.
    const auto februaryInstant = morph::time::Timestamp{
        morph::time::DateTime{std::chrono::year{2026}, std::chrono::month{2}, std::chrono::day{15},
                              std::chrono::hours{12}, std::chrono::minutes{0}, std::chrono::seconds{0}}};
    ledgerModel.execute(ledger::StoreTransaction{
        .ledgerId = ledgerId,
        .description = "Groceries out-of-month",
        .date = februaryInstant,
        .legs = {ledger::TransactionLeg{
                     .accountId = checkingId,
                     .amount = morph::math::Rational{Numerator{-9999}, Denominator{1}, DecimalPlaces{2}}},
                 ledger::TransactionLeg{
                     .accountId = groceriesId,
                     .amount = morph::math::Rational{Numerator{9999}, Denominator{1}, DecimalPlaces{2}}}}});

    auto report = budgetModel.execute(ledger::GetBudgetReport{.budgetId = budgetId, .month = "2026-01"});
    CHECK(report.spent.numerator == 7550);
    CHECK(report.limit.numerator == 20000);
}

TEST_CASE("GetBudgetReport rejects a malformed month rather than silently misparsing it", "[ledger][budget]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::BudgetModel budgetModel;
    const ScopedPrincipal principal{"alice"};
    auto categoryId = budgetModel.execute(ledger::CreateCategory{.ledgerId = ledgerId, .name = "Food"});
    auto budgetId = budgetModel.execute(
        ledger::CreateBudget{.ledgerId = ledgerId, .name = "Monthly groceries", .categoryId = categoryId});

    // "2026-13" has a well-formed length and all-digit positions but names
    // no real calendar month -- must be rejected by validate() rather than
    // silently producing a ~255-day range via unchecked month arithmetic.
    CHECK_THROWS_AS(budgetModel.execute(ledger::GetBudgetReport{.budgetId = budgetId, .month = "2026-13"}),
                    ledger::ValidationError);
    CHECK_THROWS_AS(budgetModel.execute(ledger::SetBudgetLimit{
                        .budgetId = budgetId,
                        .month = "2026-13",
                        .limit = morph::math::Rational{morph::math::Numerator{20000}, morph::math::Denominator{1},
                                                       morph::math::DecimalPlaces{2}},
                        .currency = ledger::Currency::USD}),
                    ledger::ValidationError);
}

TEST_CASE("CreateCategory refuses an empty principal", "[ledger][budget][security]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::BudgetModel model;
    ScopedPrincipal empty{""};
    CHECK_THROWS_AS(model.execute(ledger::CreateCategory{.ledgerId = ledgerId, .name = "Food"}),
                    ledger::EmptyPrincipalError);
}

TEST_CASE("CreateCategory records a LogEntry once a log is attached, and is a no-op without one",
          "[ledger][budget][journal]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::BudgetModel model;
    const ScopedPrincipal principal{"alice"};

    // No log attached: succeeds, no crash, nothing recorded anywhere to
    // check against -- this half of the test exists to prove the no-op
    // path doesn't throw or misbehave when _log is null.
    model.execute(ledger::CreateCategory{.ledgerId = ledgerId, .name = "Food"});

    // Attach a log, then repeat -- this call must be recorded.
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    model.attachActionLog(log, std::to_string(*ledgerId));
    model.execute(ledger::CreateCategory{.ledgerId = ledgerId, .name = "Rent"});

    auto entries = log->entries();
    REQUIRE(entries.size() == 1);  // only the second call was journaled -- the first ran before attachActionLog
    CHECK(entries[0].actionType == "CreateCategory");
    CHECK(entries[0].outcome == morph::journal::Outcome::Succeeded);
    CHECK(entries[0].entityKey == std::to_string(*ledgerId));
}

TEST_CASE("SetBudgetLimit restates a limit onto its currency's scale, and refuses what it cannot",
          "[ledger][budget]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    const ScopedPrincipal principal{"alice"};
    ledger::BudgetModel budgetModel;
    auto categoryId = budgetModel.execute(ledger::CreateCategory{.ledgerId = ledgerId, .name = "Food"});
    auto budgetId = budgetModel.execute(
        ledger::CreateBudget{.ledgerId = ledgerId, .name = "Monthly groceries", .categoryId = categoryId});

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;

    // $200 written at dp 0 is 200 dollars, which is 20000 cents once it is on
    // USD's own scale. A limit is compared against a sum of legs, and the
    // legs are all on the account currency's scale, so the limit must be too.
    budgetModel.execute(
        ledger::SetBudgetLimit{.budgetId = budgetId,
                               .month = "2026-01",
                               .limit = morph::math::Rational{Numerator{200}, Denominator{1}, DecimalPlaces{0}},
                               .currency = ledger::Currency::USD});
    auto report = budgetModel.execute(ledger::GetBudgetReport{.budgetId = budgetId, .month = "2026-01"});
    CHECK(report.limit.numerator == 20000);
    CHECK(report.limit.decimalPlaces == DecimalPlaces{2});

    // Sub-cent precision is rejected, not rounded.
    CHECK_THROWS_AS(budgetModel.execute(ledger::SetBudgetLimit{
                        .budgetId = budgetId,
                        .month = "2026-02",
                        .limit = morph::math::Rational{Numerator{20001}, Denominator{1}, DecimalPlaces{3}},
                        .currency = ledger::Currency::USD}),
                    ledger::ValidationError);
}

TEST_CASE("GetBudgetReport reports a zero-decimal currency's total on its own scale", "[ledger][budget]") {
    // Without this, `spent` was seeded at a hardcoded dp 2 and a JPY budget
    // reported its total tagged as if yen had cents.
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Tokyo trip";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    const ScopedPrincipal principal{"alice"};
    ledger::BudgetModel budgetModel;
    auto categoryId = budgetModel.execute(ledger::CreateCategory{.ledgerId = ledgerId, .name = "Food"});
    auto budgetId =
        budgetModel.execute(ledger::CreateBudget{.ledgerId = ledgerId, .name = "Ramen", .categoryId = categoryId});
    budgetModel.execute(ledger::SetBudgetLimit{
        .budgetId = budgetId,
        .month = "2026-01",
        .limit = morph::math::Rational{morph::math::Numerator{30000}, morph::math::Denominator{1},
                                       morph::math::DecimalPlaces{0}},
        .currency = ledger::Currency::JPY});

    auto report = budgetModel.execute(ledger::GetBudgetReport{.budgetId = budgetId, .month = "2026-01"});
    CHECK(report.currency == ledger::Currency::JPY);
    CHECK(report.limit.numerator == 30000);
    CHECK(report.limit.decimalPlaces == morph::math::DecimalPlaces{0});
    CHECK(report.spent.numerator == 0);
    CHECK(report.spent.decimalPlaces == morph::math::DecimalPlaces{0});
}

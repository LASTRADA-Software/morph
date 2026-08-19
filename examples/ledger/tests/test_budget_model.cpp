// SPDX-License-Identifier: Apache-2.0
#include "ledger/db/ledger_entity.hpp"
#include "ledger/models/budget_model.hpp"
#include "ledger/models/ledger_model.hpp"
#include "testkit/db_fixture.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("GetBudgetReport sums matching legs in-model, exactly", "[ledger][budget]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::LedgerModel ledgerModel;
    ledgerModel.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "Checking",
                                             .kind = ledger::AccountKind::Asset, .currency = ledger::Currency::USD});
    ledgerModel.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "Groceries",
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
        .budgetId = budgetId, .month = "2026-01",
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
    const auto januaryInstant = morph::time::Timestamp{morph::time::DateTime{
        std::chrono::year{2026}, std::chrono::month{1}, std::chrono::day{15}, std::chrono::hours{12},
        std::chrono::minutes{0}, std::chrono::seconds{0}}};

    ledgerModel.execute(ledger::StoreTransaction{
        .ledgerId = ledgerId,
        .description = "Groceries 1",
        .date = januaryInstant,
        .legs = {ledger::TransactionLeg{.accountId = checkingId,
                                         .amount = morph::math::Rational{Numerator{-3000}, Denominator{1},
                                                                          DecimalPlaces{2}}},
                 ledger::TransactionLeg{.accountId = groceriesId,
                                        .amount = morph::math::Rational{Numerator{3000}, Denominator{1},
                                                                         DecimalPlaces{2}}}}});
    ledgerModel.execute(ledger::StoreTransaction{
        .ledgerId = ledgerId,
        .description = "Groceries 2",
        .date = januaryInstant,
        .legs = {ledger::TransactionLeg{.accountId = checkingId,
                                         .amount = morph::math::Rational{Numerator{-4550}, Denominator{1},
                                                                          DecimalPlaces{2}}},
                 ledger::TransactionLeg{.accountId = groceriesId,
                                        .amount = morph::math::Rational{Numerator{4550}, Denominator{1},
                                                                         DecimalPlaces{2}}}}});

    auto report = budgetModel.execute(ledger::GetBudgetReport{.budgetId = budgetId, .month = "2026-01"});
    CHECK(report.spent.numerator == 7550);
    CHECK(report.limit.numerator == 20000);
}

// SPDX-License-Identifier: Apache-2.0
//
// BudgetPresenter's own suite. Budget arithmetic (limit-vs-spent aggregation,
// category linkage) is covered at the model level in test_budget_model.cpp;
// this file proves the presenter wires each action to the right signal and
// relays a refusal rather than throwing.

#include "budget_presenter.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

#include <catch2/catch_test_macros.hpp>

#include <morph/session/session.hpp>

#include <Lightweight/DataMapper/DataMapper.hpp>

#include <ledger/db/ledger_entity.hpp>

#include <memory>
#include <string>

namespace {

using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;

[[nodiscard]] std::unique_ptr<BackendRig> makeAuthedRig(std::string principal) {
    auto rig = std::make_unique<BackendRig>(Mode::Local, 1);
    morph::session::Context ctx;
    ctx.principal = std::move(principal);
    rig->bridge(0).setDefaultSession(ctx);
    return rig;
}

[[nodiscard]] ledger::LedgerId seedLedger(std::string name) {
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord row;
    row.name = Lightweight::SqlAnsiString<128>{std::move(name)};
    mapper.Create(row);
    return ledger::LedgerId{static_cast<std::int64_t>(row.id.Value())};
}

}  // namespace

TEST_CASE("BudgetPresenter emits budgetCreated after a successful CreateBudget", "[ledger][gui]") {
    DbFixture fixture;
    const auto ledgerId = seedLedger("Personal");
    auto rig = makeAuthedRig("alice");
    ledger::gui::BudgetPresenter presenter{rig->bridge(0), rig->executor()};

    ledger::CategoryId categoryId;
    bool categoryDone = false;
    bool failed = false;
    QObject::connect(&presenter, &ledger::gui::BudgetPresenter::categoryCreated,
                     [&](ledger::CategoryId id) {
                         categoryId = id;
                         categoryDone = true;
                     });
    QObject::connect(&presenter, &ledger::gui::BudgetPresenter::failed, [&] { failed = true; });

    presenter.createCategory(ledgerId, "Groceries");
    REQUIRE(pumpUntil([&] { return categoryDone || failed; }));
    REQUIRE_FALSE(failed);
    REQUIRE(categoryId.hasValue());

    ledger::BudgetId budgetId;
    bool budgetDone = false;
    QObject::connect(&presenter, &ledger::gui::BudgetPresenter::budgetCreated, [&](ledger::BudgetId id) {
        budgetId = id;
        budgetDone = true;
    });

    presenter.createBudget(ledgerId, "Monthly groceries", categoryId);
    REQUIRE(pumpUntil([&] { return budgetDone || failed; }));
    CHECK_FALSE(failed);
    CHECK(budgetId.hasValue());
}

TEST_CASE("BudgetPresenter reports a month's limit and spend exactly", "[ledger][gui]") {
    // The report carries `Rational` limit and spent, never floats -- design
    // spec §7 again. A budget with a limit and no spending must report the
    // limit it was given and a spend of zero, not an approximation of either.
    DbFixture fixture;
    const auto ledgerId = seedLedger("Personal");
    auto rig = makeAuthedRig("alice");
    ledger::gui::BudgetPresenter presenter{rig->bridge(0), rig->executor()};

    ledger::CategoryId categoryId;
    bool step = false;
    bool failed = false;
    QObject::connect(&presenter, &ledger::gui::BudgetPresenter::failed, [&] { failed = true; });
    QObject::connect(&presenter, &ledger::gui::BudgetPresenter::categoryCreated,
                     [&](ledger::CategoryId id) {
                         categoryId = id;
                         step = true;
                     });
    presenter.createCategory(ledgerId, "Groceries");
    REQUIRE(pumpUntil([&] { return step || failed; }));
    REQUIRE_FALSE(failed);

    ledger::BudgetId budgetId;
    step = false;
    QObject::connect(&presenter, &ledger::gui::BudgetPresenter::budgetCreated, [&](ledger::BudgetId id) {
        budgetId = id;
        step = true;
    });
    presenter.createBudget(ledgerId, "Monthly groceries", categoryId);
    REQUIRE(pumpUntil([&] { return step || failed; }));
    REQUIRE_FALSE(failed);

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    const auto limit =
        morph::math::Rational{Numerator{30000}, Denominator{1}, DecimalPlaces{2}};  // 300.00

    step = false;
    QObject::connect(&presenter, &ledger::gui::BudgetPresenter::limitSet, [&] { step = true; });
    presenter.setBudgetLimit(budgetId, "2026-01", limit, ledger::Currency::USD);
    REQUIRE(pumpUntil([&] { return step || failed; }));
    REQUIRE_FALSE(failed);

    ledger::GetBudgetReportResult report;
    step = false;
    QObject::connect(&presenter, &ledger::gui::BudgetPresenter::reportReady,
                     [&](ledger::GetBudgetReportResult result) {
                         report = std::move(result);
                         step = true;
                     });
    presenter.getBudgetReport(budgetId, "2026-01");
    REQUIRE(pumpUntil([&] { return step || failed; }));
    REQUIRE_FALSE(failed);

    CHECK(report.limit.numerator == 30000);
    CHECK(report.limit.denominator == 1);
    CHECK(report.spent.numerator == 0);
    CHECK(report.currency == ledger::Currency::USD);
}

TEST_CASE("BudgetPresenter relays a refusal as failed", "[ledger][gui]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    ledger::gui::BudgetPresenter presenter{rig->bridge(0), rig->executor()};

    bool failed = false;
    bool ready = false;
    QObject::connect(&presenter, &ledger::gui::BudgetPresenter::failed, [&] { failed = true; });
    QObject::connect(&presenter, &ledger::gui::BudgetPresenter::reportReady, [&] { ready = true; });

    // A budget id that was never created: the model refuses, and the
    // presenter's job is to surface that rather than interpret it.
    presenter.getBudgetReport(ledger::BudgetId{999999}, "2026-01");
    REQUIRE(pumpUntil([&] { return failed || ready; }));
    CHECK_FALSE(ready);
    CHECK(failed);
}

// SPDX-License-Identifier: Apache-2.0
//
// BudgetQmlBridge's own suite: the QML-facing translation layer. Budget
// arithmetic lives in test_budget_model.cpp and the presenter's wiring in
// test_budget_presenter.cpp; this file proves the bridge publishes what QML
// binds to, exactly.

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ledger/db/ledger_entity.hpp>
#include <memory>
#include <morph/session/session.hpp>
#include <string>

#include "budget_qml_bridge.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

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

TEST_CASE("BudgetQmlBridge publishes a report as exact triples, not a rounded number", "[ledger][gui][bridge]") {
    // Design spec §7 at the QML boundary: a view receives limit and spent as
    // exact numerator/denominator/decimalPlaces and formats them itself, so
    // no float exists anywhere on the path.
    DbFixture fixture;
    const auto ledgerId = seedLedger("Personal");
    auto rig = makeAuthedRig("alice");
    ledger::gui::BudgetQmlBridge bridge{rig->bridge(0), rig->executor()};
    bridge.openLedger(QString::number(*ledgerId));

    bool categoryDone = false;
    QObject::connect(&bridge, &ledger::gui::BudgetQmlBridge::categoryCreated, [&] { categoryDone = true; });
    bridge.createCategory("Groceries");
    REQUIRE(pumpUntil([&] { return categoryDone; }));
    REQUIRE_FALSE(bridge.lastCategoryId().isEmpty());

    bool budgetDone = false;
    QObject::connect(&bridge, &ledger::gui::BudgetQmlBridge::budgetCreated, [&] { budgetDone = true; });
    bridge.createBudget("Monthly groceries", bridge.lastCategoryId());
    REQUIRE(pumpUntil([&] { return budgetDone; }));
    REQUIRE_FALSE(bridge.lastBudgetId().isEmpty());

    bool limitDone = false;
    QObject::connect(&bridge, &ledger::gui::BudgetQmlBridge::limitSet, [&] { limitDone = true; });
    bridge.setBudgetLimit(bridge.lastBudgetId(), "2026-01", 30000, "USD");  // 300.00
    REQUIRE(pumpUntil([&] { return limitDone; }));

    bool reportDone = false;
    QObject::connect(&bridge, &ledger::gui::BudgetQmlBridge::reportChanged, [&] { reportDone = true; });
    bridge.getBudgetReport(bridge.lastBudgetId(), "2026-01");
    REQUIRE(pumpUntil([&] { return reportDone; }));

    const auto report = bridge.report();
    CHECK(report.value("limitNumerator").toLongLong() == 30000);
    CHECK(report.value("limitDenominator").toLongLong() == 1);
    CHECK(report.value("limitDecimalPlaces").toLongLong() == 2);
    CHECK(report.value("spentNumerator").toLongLong() == 0);
    CHECK(report.value("currency").toString() == "USD");
    CHECK(bridge.lastError().isEmpty());
}

TEST_CASE("BudgetQmlBridge surfaces a refusal on lastError", "[ledger][gui][bridge]") {
    DbFixture fixture;
    const auto ledgerId = seedLedger("Personal");
    auto rig = makeAuthedRig("alice");
    ledger::gui::BudgetQmlBridge bridge{rig->bridge(0), rig->executor()};
    bridge.openLedger(QString::number(*ledgerId));

    bridge.getBudgetReport("999999", "2026-01");
    REQUIRE(pumpUntil([&] { return !bridge.lastError().isEmpty(); }));
    CHECK(bridge.report().isEmpty());
}

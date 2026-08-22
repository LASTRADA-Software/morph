// SPDX-License-Identifier: Apache-2.0
//
// ReportPresenter + ReportQmlBridge, end to end through the real submit->poll
// pair. ReportJobPoller's own state machine is covered in isolation by
// test_report_job_poller.cpp; this file proves the whole path works against a
// real model, and -- crucially -- that Task 17's local-month boundary is
// still visible by the time a report body reaches QML.

#include "report_qml_bridge.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

#include <catch2/catch_test_macros.hpp>

#include <morph/session/session.hpp>

#include <Lightweight/DataMapper/DataMapper.hpp>

#include <ledger/db/ledger_entity.hpp>
#include <ledger/models/ledger_model.hpp>

#include <chrono>
#include <memory>
#include <string>

namespace {

using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;
using namespace std::chrono_literals;

/// @brief Installs @p principal for this scope.
///
///        `_ctx` is a member, declared before `_scope`, because
///        `ScopedContext` holds a *reference* to the context it installs: a
///        temporary would die at the end of the constructor and leave the
///        installed principal dangling, which surfaces as "mutating action
///        dispatched with an empty principal" rather than as a crash. Same
///        shape as test_ledger_reports.cpp's own helper, and the same hazard
///        as morph#137 -- a reference outliving what it refers to.
class ScopedPrincipal {
  public:
    explicit ScopedPrincipal(std::string principal) : _ctx{makeContext(std::move(principal))}, _scope{_ctx} {}

  private:
    [[nodiscard]] static morph::session::Context makeContext(std::string principal) {
        morph::session::Context ctx;
        ctx.principal = std::move(principal);
        return ctx;
    }

    morph::session::Context _ctx;
    morph::session::detail::ScopedContext _scope;
};

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

TEST_CASE("ReportQmlBridge drives submit->poll to done and publishes exact lines",
          "[ledger][gui][bridge][report]") {
    DbFixture fixture;
    const auto ledgerId = seedLedger("Personal");

    // Seed two accounts and one transaction booked at 2026-02-01T04:30Z --
    // 23:30 local on January 31st at UTC-5, so January's statement must count
    // it and February's must not (design spec §9, Task 17).
    {
        const ScopedPrincipal alice{"alice"};
        ledger::LedgerModel model;
        model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                          .name = "Checking",
                                          .kind = ledger::AccountKind::Asset,
                                          .currency = ledger::Currency::USD});
        model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                          .name = "Groceries",
                                          .kind = ledger::AccountKind::Expense,
                                          .currency = ledger::Currency::USD});
        const auto state = model.execute(ledger::GetLedger{.ledgerId = ledgerId});
        const auto instant = morph::time::DateTime::fromIso8601("2026-02-01T04:30:00Z");
        REQUIRE(instant.has_value());
        using morph::math::DecimalPlaces;
        using morph::math::Denominator;
        using morph::math::Numerator;
        model.execute(ledger::StoreTransaction{
            .ledgerId = ledgerId,
            .description = "late on the 31st, local",
            .date = morph::time::Timestamp{*instant},
            .legs = {ledger::TransactionLeg{
                         .accountId = state.accounts[0].id,
                         .amount = morph::math::Rational{Numerator{-5000}, Denominator{1},
                                                         DecimalPlaces{2}}},
                     ledger::TransactionLeg{
                         .accountId = state.accounts[1].id,
                         .amount = morph::math::Rational{Numerator{5000}, Denominator{1},
                                                         DecimalPlaces{2}}}}});
    }

    auto rig = makeAuthedRig("alice");
    ledger::gui::ReportQmlBridge bridge{rig->bridge(0), rig->executor()};
    bridge.openLedger(QString::number(*ledgerId));
    CHECK(bridge.status() == "idle");

    bridge.requestMonthlyStatement(2026, /*month=*/1, /*UTC-5*/ -300);
    REQUIRE(pumpUntil([&] { return bridge.status() == "done" || bridge.status() == "failed"; }, 15s));
    REQUIRE(bridge.status() == "done");
    REQUIRE(bridge.lastError().isEmpty());

    REQUIRE(bridge.lines().size() == 1);
    const auto line = bridge.lines().front().toMap();
    CHECK(line.value("currency").toString() == "USD");
    // Zero-sum per currency holds whatever the period; the count is what
    // makes the month boundary observable, all the way out to QML.
    CHECK(line.value("numerator").toLongLong() == 0);
    CHECK(line.value("transactionCount").toLongLong() == 1);

    // March contains neither transaction: the same path must report zero,
    // proving the filter survives to the QML layer rather than the view
    // simply always seeing whatever the ledger holds.
    bridge.requestMonthlyStatement(2026, /*month=*/3, -300);
    REQUIRE(pumpUntil(
        [&] {
            return bridge.status() == "done" && !bridge.lines().isEmpty() &&
                   bridge.lines().front().toMap().value("transactionCount").toLongLong() == 0;
        },
        15s));
    CHECK(bridge.lines().front().toMap().value("transactionCount").toLongLong() == 0);
}

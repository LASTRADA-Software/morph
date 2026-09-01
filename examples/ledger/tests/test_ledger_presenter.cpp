// SPDX-License-Identifier: Apache-2.0
//
// LedgerPresenter's own suite. Domain rules (per-currency zero-sum,
// exactly-once, undo semantics) already have a dedicated suite at the model
// level in test_ledger_model.cpp; this file only proves the presenter wires
// each action to the right signal and neither crashes nor hangs -- the same
// split every other rung's presenter suite makes.

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ledger/db/ledger_entity.hpp>
#include <memory>
#include <morph/session/session.hpp>
#include <string>

#include "ledger_presenter.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

namespace {

using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;

/// @brief Builds a rig whose one bridge already carries a valid session for
///        @p principal -- `LedgerModel` refuses an empty principal (Task 11),
///        so every action here needs one.
/// @param principal The identity to install.
/// @return The rig, owning the bridge and executor the presenter takes.
[[nodiscard]] std::unique_ptr<BackendRig> makeAuthedRig(std::string principal) {
    auto rig = std::make_unique<BackendRig>(Mode::Local, 1);
    morph::session::Context ctx;
    ctx.principal = std::move(principal);
    rig->bridge(0).setDefaultSession(ctx);
    return rig;
}

/// @brief Creates one ledger row directly, bypassing the action surface.
///        `CreateLedger` exists since morph#361 and is what a real client
///        uses; these presenter cases seed the row instead so a presenter
///        failure cannot be a `CreateLedger` failure wearing a disguise (same
///        shape test_ledger_reports.cpp uses).
/// @return The new ledger's id.
[[nodiscard]] ledger::LedgerId seedLedger(std::string name) {
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord row;
    row.name = Lightweight::SqlAnsiString<128>{std::move(name)};
    mapper.Create(row);
    return ledger::LedgerId{static_cast<std::int64_t>(row.id.Value())};
}

}  // namespace

TEST_CASE("LedgerPresenter emits ledgerListed after a successful GetLedger", "[ledger][gui]") {
    DbFixture fixture;
    const auto ledgerId = seedLedger("Personal");
    auto rig = makeAuthedRig("alice");
    ledger::gui::LedgerPresenter presenter{rig->bridge(0), rig->executor()};

    bool listed = false;
    bool failed = false;
    QObject::connect(&presenter, &ledger::gui::LedgerPresenter::ledgerListed, [&] { listed = true; });
    QObject::connect(&presenter, &ledger::gui::LedgerPresenter::failed, [&] { failed = true; });

    presenter.refreshLedger(ledgerId);
    REQUIRE(pumpUntil([&] { return listed || failed; }));
    CHECK_FALSE(failed);
    CHECK(listed);
}

TEST_CASE("LedgerPresenter emits accountOpened carrying the new account", "[ledger][gui]") {
    DbFixture fixture;
    const auto ledgerId = seedLedger("Personal");
    auto rig = makeAuthedRig("alice");
    ledger::gui::LedgerPresenter presenter{rig->bridge(0), rig->executor()};

    ledger::AccountInfo opened;
    bool got = false;
    bool failed = false;
    QObject::connect(&presenter, &ledger::gui::LedgerPresenter::accountOpened, [&](ledger::AccountInfo account) {
        opened = std::move(account);
        got = true;
    });
    QObject::connect(&presenter, &ledger::gui::LedgerPresenter::failed, [&] { failed = true; });

    presenter.openAccount(ledgerId, "Checking", ledger::AccountKind::Asset, ledger::Currency::USD);
    REQUIRE(pumpUntil([&] { return got || failed; }));
    REQUIRE_FALSE(failed);
    CHECK(opened.name == "Checking");
    CHECK(opened.currency == ledger::Currency::USD);
}

TEST_CASE("LedgerPresenter relays a model refusal as failed rather than throwing", "[ledger][gui]") {
    // An unbalanced transaction is refused by the model's per-currency
    // zero-sum rule (design spec §1). The presenter's job is to surface that
    // as `failed`, not to know the rule -- "translates and routes, never
    // decides".
    DbFixture fixture;
    const auto ledgerId = seedLedger("Personal");
    auto rig = makeAuthedRig("alice");
    ledger::gui::LedgerPresenter presenter{rig->bridge(0), rig->executor()};

    ledger::AccountInfo checking;
    bool opened = false;
    QObject::connect(&presenter, &ledger::gui::LedgerPresenter::accountOpened, [&](ledger::AccountInfo account) {
        checking = std::move(account);
        opened = true;
    });
    presenter.openAccount(ledgerId, "Checking", ledger::AccountKind::Asset, ledger::Currency::USD);
    REQUIRE(pumpUntil([&] { return opened; }));

    QString message;
    bool failed = false;
    bool stored = false;
    QObject::connect(&presenter, &ledger::gui::LedgerPresenter::failed, [&](QString msg) {
        message = std::move(msg);
        failed = true;
    });
    QObject::connect(&presenter, &ledger::gui::LedgerPresenter::transactionStored, [&] { stored = true; });

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    presenter.storeTransaction(
        ledgerId, "unbalanced", morph::time::Timestamp::now(),
        {ledger::TransactionLeg{.accountId = checking.id,
                                .amount = morph::math::Rational{Numerator{-5000}, Denominator{1}, DecimalPlaces{2}}}},
        ledger::ImportOpId{});

    REQUIRE(pumpUntil([&] { return failed || stored; }));
    CHECK_FALSE(stored);
    CHECK(failed);
    CHECK_FALSE(message.isEmpty());
}

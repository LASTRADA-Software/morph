// SPDX-License-Identifier: Apache-2.0
//
// LedgerQmlBridge's own suite: the QML-facing translation layer. The
// presenter's own wiring is covered in test_ledger_presenter.cpp and the
// domain rules in test_ledger_model.cpp; this file proves the bridge
// publishes what QML binds to, and that money survives the boundary exactly.

#include "ledger_qml_bridge.hpp"
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

TEST_CASE("LedgerQmlBridge publishes accounts as QML-ready maps", "[ledger][gui][bridge]") {
    DbFixture fixture;
    const auto ledgerId = seedLedger("Personal");
    auto rig = makeAuthedRig("alice");
    ledger::gui::LedgerQmlBridge bridge{rig->bridge(0), rig->executor()};

    int changes = 0;
    QObject::connect(&bridge, &ledger::gui::LedgerQmlBridge::accountsChanged, [&] { ++changes; });

    bridge.openLedger(QString::number(*ledgerId));
    REQUIRE(pumpUntil([&] { return changes > 0; }));
    CHECK(bridge.accounts().isEmpty());  // a fresh ledger has no accounts yet

    bridge.openAccount("Checking", "asset", "USD");
    REQUIRE(pumpUntil([&] { return !bridge.accounts().isEmpty(); }));

    const auto account = bridge.accounts().front().toMap();
    CHECK(account.value("name").toString() == "Checking");
    CHECK(account.value("kind").toString() == "asset");
    CHECK(account.value("currency").toString() == "USD");
    CHECK(account.value("id").toLongLong() > 0);
}

TEST_CASE("LedgerQmlBridge carries balances exactly, never as a float", "[ledger][gui][bridge]") {
    // Design spec §7's no-float rule at the QML boundary. A balance crosses as
    // its exact Rational triple, so a view can render it without the model
    // ever having produced a double to round.
    DbFixture fixture;
    const auto ledgerId = seedLedger("Personal");
    auto rig = makeAuthedRig("alice");
    ledger::gui::LedgerQmlBridge bridge{rig->bridge(0), rig->executor()};

    bridge.openLedger(QString::number(*ledgerId));
    REQUIRE(pumpUntil([&] { return bridge.lastError().isEmpty(); }));
    bridge.openAccount("Checking", "asset", "USD");
    REQUIRE(pumpUntil([&] { return bridge.accounts().size() == 1; }));
    bridge.openAccount("Groceries", "expense", "USD");
    REQUIRE(pumpUntil([&] { return bridge.accounts().size() == 2; }));

    const auto idOf = [&](const QString& name) -> QString {
        for (const auto& entry : bridge.accounts()) {
            const auto map = entry.toMap();
            if (map.value("name").toString() == name) {
                return map.value("id").toString();
            }
        }
        return {};
    };

    // 50.00 in minor units -- an integer in, an exact Rational out.
    bridge.storeTransaction(idOf("Checking"), idOf("Groceries"), 5000, "Weekly shop");
    REQUIRE(pumpUntil([&] {
        for (const auto& entry : bridge.accounts()) {
            if (entry.toMap().value("balanceNumerator").toLongLong() != 0) {
                return true;
            }
        }
        return false;
    }));

    for (const auto& entry : bridge.accounts()) {
        const auto map = entry.toMap();
        const auto numerator = map.value("balanceNumerator").toLongLong();
        CHECK(map.value("balanceDenominator").toLongLong() == 1);
        CHECK(map.value("balanceDecimalPlaces").toLongLong() == 2);
        if (map.value("name").toString() == "Checking") {
            CHECK(numerator == -5000);
        } else {
            CHECK(numerator == 5000);
        }
    }
    CHECK(bridge.lastError().isEmpty());
}

TEST_CASE("LedgerQmlBridge surfaces a model refusal on lastError", "[ledger][gui][bridge]") {
    DbFixture fixture;
    const auto ledgerId = seedLedger("Personal");
    auto rig = makeAuthedRig("alice");
    ledger::gui::LedgerQmlBridge bridge{rig->bridge(0), rig->executor()};

    bridge.openLedger(QString::number(*ledgerId));
    REQUIRE(pumpUntil([&] { return bridge.lastError().isEmpty(); }));

    // Both legs naming an account that does not exist: the model refuses, and
    // the bridge's job is to make that visible to a view rather than swallow
    // it.
    bridge.storeTransaction("999999", "999998", 100, "nowhere");
    REQUIRE(pumpUntil([&] { return !bridge.lastError().isEmpty(); }));
    CHECK_FALSE(bridge.lastError().isEmpty());
}

// SPDX-License-Identifier: Apache-2.0
//
// RulePresenter + RuleQmlBridge. Rule matching and versioning semantics are
// covered at the model level in test_rule_model.cpp; these cases prove the
// transport and QML layers carry each result -- including the version, which
// is the field that makes a rule edit's non-retroactivity visible.

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ledger/db/ledger_entity.hpp>
#include <memory>
#include <morph/session/session.hpp>
#include <string>

#include "rule_qml_bridge.hpp"
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

TEST_CASE("RulePresenter emits ruleCreated and ruleUpdated with the bumped version", "[ledger][gui]") {
    DbFixture fixture;
    const auto ledgerId = seedLedger("Personal");
    auto rig = makeAuthedRig("alice");
    ledger::gui::RulePresenter presenter{rig->bridge(0), rig->executor()};

    ledger::RuleId ruleId;
    bool created = false;
    bool failed = false;
    QObject::connect(&presenter, &ledger::gui::RulePresenter::failed, [&] { failed = true; });
    QObject::connect(&presenter, &ledger::gui::RulePresenter::ruleCreated, [&](ledger::RuleId id) {
        ruleId = id;
        created = true;
    });

    presenter.createRule(ledgerId, ledger::RuleTrigger::DescriptionContains, "COFFEE", ledger::RuleAction::SetCategory,
                         "7");
    REQUIRE(pumpUntil([&] { return created || failed; }));
    REQUIRE_FALSE(failed);
    REQUIRE(ruleId.hasValue());

    ledger::RuleInfo updated;
    bool gotUpdate = false;
    QObject::connect(&presenter, &ledger::gui::RulePresenter::ruleUpdated, [&](ledger::RuleInfo rule) {
        updated = std::move(rule);
        gotUpdate = true;
    });

    presenter.updateRule(ruleId, "ESPRESSO", "9");
    REQUIRE(pumpUntil([&] { return gotUpdate || failed; }));
    REQUIRE_FALSE(failed);
    CHECK(updated.matchText == "ESPRESSO");
    CHECK(updated.actionValue == "9");
    // The version is the point: an edit bumps it so already-categorised
    // transactions, stamped with the old one, are visibly stale rather than
    // silently recategorised.
    CHECK(updated.version > 0);
}

TEST_CASE("RuleQmlBridge publishes a rule's version to QML", "[ledger][gui][bridge]") {
    DbFixture fixture;
    const auto ledgerId = seedLedger("Personal");
    auto rig = makeAuthedRig("alice");
    ledger::gui::RuleQmlBridge bridge{rig->bridge(0), rig->executor()};
    bridge.openLedger(QString::number(*ledgerId));

    bool created = false;
    QObject::connect(&bridge, &ledger::gui::RuleQmlBridge::ruleCreated, [&] { created = true; });
    bridge.createRule("COFFEE", "7");
    REQUIRE(pumpUntil([&] { return created || !bridge.lastError().isEmpty(); }));
    REQUIRE(bridge.lastError().isEmpty());
    const auto ruleId = bridge.lastRule().value("id").toString();
    REQUIRE_FALSE(ruleId.isEmpty());

    bool updated = false;
    QObject::connect(&bridge, &ledger::gui::RuleQmlBridge::ruleUpdated, [&] { updated = true; });
    bridge.updateRule(ruleId, "ESPRESSO", "9");
    REQUIRE(pumpUntil([&] { return updated || !bridge.lastError().isEmpty(); }));
    REQUIRE(bridge.lastError().isEmpty());

    const auto rule = bridge.lastRule();
    CHECK(rule.value("matchText").toString() == "ESPRESSO");
    CHECK(rule.value("actionValue").toString() == "9");
    CHECK(rule.value("version").toLongLong() > 0);
}

TEST_CASE("RuleQmlBridge surfaces a refusal on lastError", "[ledger][gui][bridge]") {
    DbFixture fixture;
    const auto ledgerId = seedLedger("Personal");
    auto rig = makeAuthedRig("alice");
    ledger::gui::RuleQmlBridge bridge{rig->bridge(0), rig->executor()};
    bridge.openLedger(QString::number(*ledgerId));

    // A rule id that was never created.
    bridge.updateRule("999999", "ANY", "1");
    REQUIRE(pumpUntil([&] { return !bridge.lastError().isEmpty(); }));
    CHECK_FALSE(bridge.lastError().isEmpty());
}

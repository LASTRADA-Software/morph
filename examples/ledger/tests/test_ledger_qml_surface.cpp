// SPDX-License-Identifier: Apache-2.0
//
// The QML-visible surface of all four ledger bridges, audited against
// `gui/qml/` itself.
//
// Rung 3 shipped four `QObject` bridges and five QML files and never checked
// that they agree: `test_ledger_qml_bridge.cpp` and its three siblings prove
// what each bridge *does* — that money survives the boundary, that a rule
// round-trips — with the bridge driven from C++, where every name is a
// compile-time symbol. Nothing in this rung read the QML. QML binds by
// string, so until now a renamed invokable, a `Connections` handler for a
// signal that no longer exists, or a property read that resolves to
// `undefined` was not a compile error, not a test failure, and not a QML
// warning — the `LedgerView` pane simply stayed empty.
//
// This file is that check, and it is not hand-written: it points
// `morph::ladder::testkit::QmlSurfaceAudit`
// (`examples/common/testkit/qml_surface.hpp`) at the rung's own QML and lets
// those files be the expectation, in both directions. See that header for
// exactly what the audit does and does not cover.
//
// The alias mapping below is the one per-rung fact the audit cannot derive,
// and it is deliberately doubled: `gui/main.cpp` supplies each bridge under
// its own `setInitialProperties` key, and `Main.qml` then hands each one to a
// sub-view whose property is called plain `bridge`. Both names reach the same
// instance, so both are bound to it and the audit pools their coverage.

#include <QString>
#include <QStringList>
#include <catch2/catch_test_macros.hpp>
#include <initializer_list>
#include <utility>

#include "budget_qml_bridge.hpp"
#include "ledger_qml_bridge.hpp"
#include "report_qml_bridge.hpp"
#include "rule_qml_bridge.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/qml_surface.hpp"

namespace {

using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::QmlSurfaceAudit;

}  // namespace

TEST_CASE("Every ledger bridge exposes exactly the surface gui/qml binds, and nothing more",
          "[ledger][gui][qml-surface]") {
    // No DbFixture and no session: the audit reads metaobjects and text, never
    // dispatches an action, so a bare rig is all four constructors need.
    BackendRig rig{Mode::Local, 1};
    ledger::gui::LedgerQmlBridge ledgerBridge{rig.bridge(0), rig.executor()};
    ledger::gui::BudgetQmlBridge budgetBridge{rig.bridge(0), rig.executor()};
    ledger::gui::RuleQmlBridge ruleBridge{rig.bridge(0), rig.executor()};
    ledger::gui::ReportQmlBridge reportBridge{rig.bridge(0), rig.executor()};

    QmlSurfaceAudit audit{QStringLiteral(MORPH_LADDER_SOURCE_ROOT "/examples/ledger/gui/qml")};
    audit.bind(QStringLiteral("ledgerBridge"), ledgerBridge);
    audit.bindIn(QStringLiteral("LedgerView.qml"), QStringLiteral("bridge"), ledgerBridge);
    audit.bind(QStringLiteral("budgetBridge"), budgetBridge);
    audit.bindIn(QStringLiteral("BudgetView.qml"), QStringLiteral("bridge"), budgetBridge);
    audit.bind(QStringLiteral("ruleBridge"), ruleBridge);
    audit.bindIn(QStringLiteral("RulesView.qml"), QStringLiteral("bridge"), ruleBridge);
    audit.bind(QStringLiteral("reportBridge"), reportBridge);
    audit.bindIn(QStringLiteral("ReportView.qml"), QStringLiteral("bridge"), reportBridge);

    // ── The pre-existing backlog, recorded rather than swallowed (#239) ──
    // The first run of this audit reported 15 members these four bridges
    // publish that no file under gui/qml/ binds. None of them is a broken
    // screen — the other direction was clean, so no QML file binds a name its
    // bridge lacks — but each is either dead surface or a missing control, and
    // deciding which is per-member work this file does not do. They are listed
    // here so the guard goes live now and catches the *next* drift in either
    // direction, with the backlog itemised instead of hidden behind a lowered
    // bar. The list is checked in both directions too: an exemption for a
    // member that has since been deleted, or one QML has since bound, fails
    // this test (testkit/qml_surface.hpp). It can only shrink deliberately,
    // and #239 closes when it is empty.
    const QString backlog = QStringLiteral("unbound bridge surface, tracked in morph#239");
    for (const auto& [alias, member] : std::initializer_list<std::pair<const char*, const char*>>{
             {"ledgerBridge", "busy"},
             {"ledgerBridge", "busyChanged"},
             {"ledgerBridge", "refresh"},
             {"ledgerBridge", "undoTransaction"},
             {"budgetBridge", "busy"},
             {"budgetBridge", "busyChanged"},
             {"budgetBridge", "categoryCreated"},
             {"budgetBridge", "budgetCreated"},
             {"budgetBridge", "limitSet"},
             {"budgetBridge", "lastBudgetId"},
             {"budgetBridge", "linkAccount"},
             {"ruleBridge", "busy"},
             {"ruleBridge", "busyChanged"},
             {"ruleBridge", "ruleCreated"},
             {"ruleBridge", "ruleUpdated"},
         }) {
        audit.allowUnbound(QString::fromLatin1(alias), QString::fromLatin1(member), backlog);
    }

    const QStringList findings = audit.run();
    INFO(findings.join(QStringLiteral("\n")).toStdString());
    CHECK(findings.isEmpty());

    // The audit is only as good as the files it found: the five gui/qml ships.
    CHECK(audit.scannedFiles() == QStringList{QStringLiteral("BudgetView.qml"), QStringLiteral("LedgerView.qml"),
                                              QStringLiteral("Main.qml"), QStringLiteral("ReportView.qml"),
                                              QStringLiteral("RulesView.qml")});
}

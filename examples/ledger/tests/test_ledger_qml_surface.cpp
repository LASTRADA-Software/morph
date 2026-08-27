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

    // ── The former backlog, worked off member by member (#239) ───────────
    // The first run of this audit reported 15 members these four bridges
    // publish that no file under gui/qml/ bound: `busy`/`busyChanged` on all
    // three of `LedgerQmlBridge`/`BudgetQmlBridge`/`RuleQmlBridge`,
    // `ledgerBridge`'s `refresh`/`undoTransaction`, `budgetBridge`'s
    // `categoryCreated`/`budgetCreated`/`limitSet`/`lastBudgetId`/
    // `linkAccount`, and `ruleBridge`'s `ruleCreated`/`ruleUpdated`. Every one
    // was live, meaningful surface backed by real presenter state (`busy()`
    // forwards to `_presenter.busy()` on all three; the others are real
    // create/link/undo gestures with a model and a presenter behind them) —
    // none was dead scaffolding, so all 15 were bound rather than deleted:
    //
    //   * `busy` now gates a `BusyIndicator` in `LedgerView.qml`,
    //     `BudgetView.qml` and `RulesView.qml`. Its `busyChanged` NOTIFY needs
    //     no exemption of its own: the audit treats a property's NOTIFY as
    //     covered by reading the property (testkit/qml_surface.cpp's signal
    //     sweep), and that is exactly what these three views now do.
    //   * `ledgerBridge.refresh` is a "Refresh" button; `undoTransaction` is a
    //     journal-id field plus an "Undo" button, both in `LedgerView.qml`.
    //   * `budgetBridge.linkAccount` is an account-id/category-id pair plus a
    //     button in `BudgetView.qml`; `lastBudgetId` fills the same
    //     chain-without-a-round-trip label next to "Create budget" that
    //     `lastCategoryId` already had next to "Create category".
    //   * `categoryCreated`/`budgetCreated`/`limitSet` and `ruleCreated`/
    //     `ruleUpdated` carry no payload of their own — the state they
    //     describe is already bound (`lastCategoryId`/`lastBudgetId`/
    //     `lastRule`) — so each gets a `Connections` handler that writes a
    //     status line, the same shape bank's `Main.qml` uses for
    //     `txns.posted`/`payees.paid` (#303): a transient confirmation is the
    //     only way a fire-and-forget signal becomes visible at all.
    //
    // The list is checked in both directions: an exemption for a member that
    // has since been deleted, or one QML has since bound, fails this test
    // (testkit/qml_surface.hpp). It can only shrink deliberately, and with
    // every one of the 15 now bound, there is nothing left to exempt.

    const QStringList findings = audit.run();
    INFO(findings.join(QStringLiteral("\n")).toStdString());
    CHECK(findings.isEmpty());

    // The audit is only as good as the files it found: the five gui/qml ships.
    CHECK(audit.scannedFiles() == QStringList{QStringLiteral("BudgetView.qml"), QStringLiteral("LedgerView.qml"),
                                              QStringLiteral("Main.qml"), QStringLiteral("ReportView.qml"),
                                              QStringLiteral("RulesView.qml")});
}

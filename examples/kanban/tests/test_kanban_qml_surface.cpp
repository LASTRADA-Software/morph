// SPDX-License-Identifier: Apache-2.0
//
// The QML-visible surface of both kanban bridges, audited against `gui/qml/`
// itself.
//
// kanban already guarded this by hand, in two files: `indexOfMethod`/
// `indexOfSignal` lists naming what must exist, plus an exact
// `propertyCount() - propertyOffset()` assertion. Those catch a *deletion* but
// not the drift QML actually suffers -- a count stays satisfied when one
// invokable is renamed and another added, and neither check ever reads a `.qml`
// file, so a `Connections` handler for a signal that no longer exists remains
// invisible. QML binds by string: such a mistake is not a compile error, not a
// test failure, and not a QML warning. The pane simply stays empty.
//
// This file points `morph::ladder::testkit::QmlSurfaceAudit`
// (`examples/common/testkit/qml_surface.hpp`) at the rung's own QML and lets
// those files be the expectation, in both directions.
//
// ── The conditional surface ──────────────────────────────────────────────────
//
// `BoardBridge`'s QML surface depends on a compile-time switch:
// `MORPH_BUILD_OFFLINE_SQLITE` adds `queueDepth` and `deadLetterCount`. The
// hand-written guard has to branch on that (`test_board_qml_bridge.cpp` asserts
// 7 properties or 5), because a count cannot describe a surface that changes
// shape.
//
// The audit needs no such branch, and that is the point of a per-name check
// rather than a count: it reads the metaobject that was actually built, so in a
// configure without the switch the two properties simply are not there.
//
// What *did* need work is the other direction. `BoardView.qml` reads
// `deadLetterCount` unconditionally-looking but guards it with
// `!== undefined`, precisely so the dead-letter banner stays hidden in an OFF
// build. The audit used to report that as "reads a property the bridge has no
// such property" -- backwards, since the only way to satisfy it would have been
// to delete the guard that makes the binding safe. A read compared against
// `undefined` is now understood as a *probe* rather than a use
// (QmlScanResult::optionalProbes), so this rung needs no exemption for it.

#include <QString>
#include <QStringList>
#include <catch2/catch_test_macros.hpp>

#include "board_qml_bridge.hpp"
#include "project_admin_qml_bridge.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/qml_surface.hpp"

namespace {

using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::QmlSurfaceAudit;

}  // namespace

TEST_CASE("Every kanban bridge exposes exactly the surface gui/qml binds, and nothing more",
          "[kanban][gui][qml-surface]") {
    // No fixture and no session: the audit reads metaobjects and text, never
    // dispatches an action, so a bare rig is all both constructors need.
    BackendRig rig{Mode::Local, 1};
    kanban::gui::BoardBridge boardBridge{rig.bridge(0), rig.executor()};
    kanban::gui::ProjectAdminBridge projectAdminBridge{rig.bridge(0), rig.executor()};

    QmlSurfaceAudit audit{QStringLiteral(MORPH_LADDER_SOURCE_ROOT "/examples/kanban/gui/qml")};
    // gui/main.cpp supplies both under these keys, and every sub-view names its
    // property the same, so one bind per bridge covers every file that reads it.
    audit.bind(QStringLiteral("boardBridge"), boardBridge);
    audit.bind(QStringLiteral("projectAdminBridge"), projectAdminBridge);

    // ── The pre-existing backlog, recorded rather than swallowed ──────────
    // The first run of this audit reported nine members BoardBridge publishes
    // that no file under gui/qml/ binds. The other direction is clean, so no
    // screen is broken; each is either dead surface or a missing control, and
    // deciding which is per-member work this file does not do. They are listed
    // here so the guard goes live now and catches the *next* drift in either
    // direction, with the backlog itemised instead of hidden behind a lowered
    // bar.
    //
    // The list is checked in both directions too: an exemption for a member
    // that has since been deleted, or one QML has since bound, fails this test
    // (testkit/qml_surface.hpp). It can only shrink deliberately.
    //
    // `syncStatusChanged` is in the list under protest: it is the NOTIFY signal
    // behind `deadLetterCount`, which BoardView.qml *does* bind, so the signal
    // is doing its job -- a property binding consumes a NOTIFY signal without
    // an explicit Connections handler, and the audit does not model that.
    // Teaching it to would change what ledger's list means too (morph#239 has
    // the same shape), so it is recorded here and argued in morph#291 rather
    // than fixed in passing.
    //
    // Same shape as ledger's (morph#239) and lims' (morph#287).
    const QString backlog = QStringLiteral("unbound bridge surface, tracked in morph#291");
    for (const auto* member : {"bound", "ruleCreated", "ruleDeleted", "attachmentUploaded", "attachmentDownloaded",
                               "syncStatusChanged", "getRules", "refresh", "setAttachmentServerUrl"}) {
        audit.allowUnbound(QStringLiteral("boardBridge"), QString::fromLatin1(member), backlog);
    }

    const QStringList findings = audit.run();
    INFO(findings.join(QStringLiteral("\n")).toStdString());
    CHECK(findings.isEmpty());

    // The audit is only as good as the files it found: the seven gui/qml ships.
    CHECK(audit.scannedFiles() == QStringList{QStringLiteral("BoardView.qml"), QStringLiteral("LoginView.qml"),
                                              QStringLiteral("Main.qml"), QStringLiteral("MembersView.qml"),
                                              QStringLiteral("ProjectListView.qml"), QStringLiteral("RulesView.qml"),
                                              QStringLiteral("TaskDetailPopup.qml")});
}

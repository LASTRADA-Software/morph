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
// The audit needs no branch to *find* the surface -- it reads the metaobject
// that was actually built, so in a configure without the switch the two
// properties simply are not there. Both properties are bound in the configure
// where they exist: BoardView.qml reads `deadLetterCount` for the dead-letter
// banner and `queueDepth` for the pending-sync indicator beside it, so neither
// needs an exemption.
//
// This was found by CI, not locally. An earlier draft of this file asserted the
// two configures produce identical findings; they do not, and the check behind
// that claim had a grep that silently dropped half the audit's output. The
// per-configure verification below is what should have been done first.
//
// What *also* needed work is the other direction. `BoardView.qml` reads
// `deadLetterCount` and `queueDepth` unconditionally-looking but guards each
// with `!== undefined`, precisely so their banners stay hidden in an OFF
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

    // ── What is exempt, and why each one is permanent ────────────────────
    // The first run of this audit reported nine members BoardBridge publishes
    // that no file under gui/qml/ binds, recorded as a backlog under
    // morph#291. That backlog is now worked off, and nothing here is "tracked,
    // decide later": every one of the nine was dispositioned by binding or
    // deleting the member, and the two that remain are exempt for a structural
    // reason that will not change.
    //
    // What went, and where it went:
    //   * `getRules`/`ruleCreated`/`ruleDeleted` -- these were not dead
    //     surface, they were the automation-rules pane never being populated
    //     at all (morph#304 §A2). BoardView.qml now fetches on `rulesPopup`'s
    //     `onOpened` and re-fetches on each mutation, so the audit resolves
    //     all three against real binding sites.
    //   * `attachmentUploaded`/`attachmentDownloaded` -- a missing control.
    //     TaskDetailPopup.qml now reports both outcomes; the download half in
    //     particular had no user-visible effect whatsoever, since it writes
    //     its bytes to a path outside the app.
    //   * `bound` -- genuinely dead, and deleted rather than exempted. See
    //     board_qml_bridge.hpp's own note where the signal used to be.
    //   * `queueDepth` -- genuinely unbound, and bound rather than deleted
    //     (morph#308). BoardView.qml now reads it for a pending-sync
    //     indicator beside the dead-letter banner, so the audit resolves it
    //     against a real binding site too.
    //
    // The list is checked in both directions: an exemption for a member that
    // has since been deleted, or one QML has since bound, fails this test
    // (testkit/qml_surface.hpp). It can only shrink deliberately.
    //
    // Both survivors are C++ call sites. The audit scans `.qml` and nothing
    // else, so a member whose only caller is C++ sweeps as unreferenced no
    // matter how load-bearing it is -- that is a limitation of the instrument,
    // not a finding about the bridge, and no amount of later work will clear
    // these two.
    const QString cppOnly = QStringLiteral(
        "called from C++, not QML -- the audit scans .qml only, so a C++-only caller cannot satisfy it");
    // gui/main.cpp:152, on the composed desktop client, before any QML runs.
    audit.allowUnbound(QStringLiteral("boardBridge"), QStringLiteral("setAttachmentServerUrl"), cppOnly);
    // board_qml_bridge.cpp:533 -- BoardBridge::onEventApplied(), the
    // EventPoller's own applied-event handler, which resyncs `board` after
    // every polled event. Deliberately *not* cited as the ReconnectCoordinator
    // replay dependency further down the same file (:645): that call sits
    // inside `#ifdef MORPH_BUILD_OFFLINE_SQLITE`, so citing it would make this
    // reason false in precisely the OFF configure the guard below exists to
    // check. :533 is outside every guard and holds in both.
    audit.allowUnbound(QStringLiteral("boardBridge"), QStringLiteral("refresh"), cppOnly);
#ifndef MORPH_BUILD_OFFLINE_SQLITE
    // `syncStatusChanged` is declared unconditionally (board_qml_bridge.hpp's
    // signals block) while *both* properties naming it as their NOTIFY --
    // `queueDepth` and `deadLetterCount` -- live inside
    // `#ifdef MORPH_BUILD_OFFLINE_SQLITE`. That asymmetry is the whole story:
    //
    //   * ON:  `deadLetterCount` and `queueDepth` both exist, and
    //          BoardView.qml reads each for real (the `!== undefined` guard
    //          on each banner's `visible` is the probe; the `visible ? ... :
    //          ""` ternary on each `text` is the use). The audit treats a
    //          property's NOTIFY signal as covered by reading the property --
    //          testkit/qml_surface.cpp's signal sweep does model this -- so
    //          the signal needs no exemption here, and claiming one would
    //          misdescribe a guard that is already doing its job.
    //   * OFF: neither property is compiled in, so no property read can cover
    //          the signal, and it sweeps as a signal nothing handles.
    //
    // Hence `#ifndef`: this exemption applies only to the configure where
    // both properties -- and the NOTIFY they share -- are compiled out.
    // Worth stating plainly because the exemption list's original comment
    // asserted the opposite -- that the audit "does not model" NOTIFY coverage
    // by property read. It does, and has since the audit landed; the entry was
    // unnecessary in the ON configure all along.
    //
    // Nothing could have told us, either, and that part is not a kanban quirk:
    // a redundant NOTIFY exemption is invisible to the audit in *both* sweep
    // directions. The signal sweep short-circuits on `exempt(name)` before it
    // ever reaches the NOTIFY-coverage check, so an exempt-but-already-covered
    // signal is skipped rather than reported. The staleness sweep, which does
    // catch an exemption QML has since bound, only looks for the member being
    // read/handled/called *directly* -- never for it being covered
    // transitively through the property whose NOTIFY it is. So this entry sat
    // here doing nothing in the ON configure, and the comment justifying it
    // survived long enough to be written up as a design question. That blind
    // spot lives in `QmlSurfaceAudit` and affects every rung's exemption list;
    // it is filed separately, not worked around here.
    //
    // Dropping the entry in the configure where it is redundant is what makes
    // the redundancy checkable at all: with no exemption, the ON configure now
    // has to pass on genuine NOTIFY coverage or fail loudly.
    audit.allowUnbound(QStringLiteral("boardBridge"), QStringLiteral("syncStatusChanged"),
                       QStringLiteral("NOTIFY signal of queueDepth/deadLetterCount, neither of which is compiled in "
                                      "without MORPH_BUILD_OFFLINE_SQLITE"));
#endif

    const QStringList findings = audit.run();
    INFO(findings.join(QStringLiteral("\n")).toStdString());
    CHECK(findings.isEmpty());

    // The audit is only as good as the files it found: the seven gui/qml ships.
    CHECK(audit.scannedFiles() == QStringList{QStringLiteral("BoardView.qml"), QStringLiteral("LoginView.qml"),
                                              QStringLiteral("Main.qml"), QStringLiteral("MembersView.qml"),
                                              QStringLiteral("ProjectListView.qml"), QStringLiteral("RulesView.qml"),
                                              QStringLiteral("TaskDetailPopup.qml")});
}

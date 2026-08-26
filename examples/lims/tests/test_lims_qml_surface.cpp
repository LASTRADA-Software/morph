// SPDX-License-Identifier: Apache-2.0
//
// The QML-visible surface of both lims bridges, audited against `gui/qml/`
// itself.
//
// `test_lims_qml_bridges.cpp` proves what the bridges *do*, driven from C++
// where every name is a compile-time symbol, and guards their size with a loop
// over `methodCount()`. A count is not a name: it stays satisfied when one
// invokable is renamed and another added, which is exactly the drift QML cannot
// report. QML binds by string, so a renamed invokable, a `Connections` handler
// for a signal that no longer exists, or a property read resolving to
// `undefined` is not a compile error, not a test failure, and not a QML warning
// -- the pane simply stays empty.
//
// This file points `morph::ladder::testkit::QmlSurfaceAudit`
// (`examples/common/testkit/qml_surface.hpp`) at the rung's own QML and lets
// those files be the expectation, in both directions. See that header for what
// the audit does and does not cover.
//
// Unlike ledger, lims needs no doubled alias: `gui/main.cpp` supplies each
// bridge as `sampleBridge`/`resultBridge`, and `Main.qml` hands each to its
// sub-view under the *same* property name, so one bind per bridge covers every
// file that reads it.

#include <QString>
#include <QStringList>
#include <catch2/catch_test_macros.hpp>
#include <initializer_list>
#include <utility>

#include "result_qml_bridge.hpp"
#include "sample_qml_bridge.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/qml_surface.hpp"

namespace {

using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::QmlSurfaceAudit;

}  // namespace

TEST_CASE("Every lims bridge exposes exactly the surface gui/qml binds, and nothing more",
          "[lims][gui][qml-surface]") {
    // No DbFixture and no session: the audit reads metaobjects and text, never
    // dispatches an action, so a bare rig is all both constructors need.
    BackendRig rig{Mode::Local, 1};
    lims::gui::SampleBridge sampleBridge{rig.bridge(0), rig.executor()};
    lims::gui::ResultBridge resultBridge{rig.bridge(0), rig.executor()};

    QmlSurfaceAudit audit{QStringLiteral(MORPH_LADDER_SOURCE_ROOT "/examples/lims/gui/qml")};
    audit.bind(QStringLiteral("sampleBridge"), sampleBridge);
    audit.bind(QStringLiteral("resultBridge"), resultBridge);

    // ── The pre-existing backlog, recorded rather than swallowed ──────────
    // The first run of this audit reported 15 members these two bridges publish
    // that no file under gui/qml/ binds. The other direction was clean -- no
    // QML file binds a name its bridge lacks -- so no screen is broken; each is
    // either dead surface or a missing control, and deciding which is
    // per-member work this file does not do. They are listed here so the guard
    // goes live now and catches the *next* drift in either direction, with the
    // backlog itemised instead of hidden behind a lowered bar.
    //
    // The list is checked in both directions too: an exemption for a member
    // that has since been deleted, or one QML has since bound, fails this test
    // (testkit/qml_surface.hpp). It can only shrink deliberately.
    //
    // Same shape as ledger's, in test_ledger_qml_surface.cpp (morph#239).
    const QString backlog = QStringLiteral("unbound bridge surface, tracked in morph#287");
    for (const auto& [alias, member] : std::initializer_list<std::pair<const char*, const char*>>{
             {"sampleBridge", "bound"},
             {"sampleBridge", "replyReceived"},
             {"sampleBridge", "registerClient"},
             {"sampleBridge", "registerSample"},
             {"sampleBridge", "rejectSample"},
             {"sampleBridge", "returnForRework"},
             {"resultBridge", "bound"},
             {"resultBridge", "replyReceived"},
             {"resultBridge", "sampleAttached"},
             {"resultBridge", "resultCaptured"},
             {"resultBridge", "resultVerified"},
             {"resultBridge", "conflictResolved"},
             {"resultBridge", "captureQualifier"},
             {"resultBridge", "captureReading"},
             {"resultBridge", "resolveConflict"},
         }) {
        audit.allowUnbound(QString::fromLatin1(alias), QString::fromLatin1(member), backlog);
    }

    const QStringList findings = audit.run();
    INFO(findings.join(QStringLiteral("\n")).toStdString());
    CHECK(findings.isEmpty());

    // The audit is only as good as the files it found: the three gui/qml ships.
    CHECK(audit.scannedFiles() == QStringList{QStringLiteral("Main.qml"), QStringLiteral("ResultEntryView.qml"),
                                              QStringLiteral("SampleView.qml")});
}

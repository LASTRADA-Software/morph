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

    // ── The two exemptions that survive, each with a mechanism ───────────
    // The first run of this audit reported 15 members these two bridges
    // publish that no file under gui/qml/ binds, recorded as one undifferentiated
    // backlog. Thirteen of them are gone: `replyReceived` on each bridge is
    // now handled by the views (see the case below), `resultVerified` is
    // handled by ResultEntryView.qml, and the nine remaining typed invokables
    // and outcome signals were deleted as redundant second dispatch paths --
    // every one of them duplicated an action the schema-driven `submitIfValid`
    // path already carries, from a QML call site that did not exist.
    //
    // The two below are *not* redundant, and the reason is a mechanism rather
    // than a plan. `SamplePresenter::submitIfValid` routes both registration
    // actions to the plain, key-less `_creator` handler, so:
    //
    //  - `registerClient` is the only dispatch that emits `clientRegistered`
    //    and therefore the only thing that ever sets the `clientId` property
    //    SampleView.qml binds; the form path emits `replyReceived` carrying
    //    the same id as JSON and leaves the property at -1.
    //  - `registerSample` is the only dispatch that leaves the *shared*
    //    handler attached to the new sample (`BridgeHandler::execute`'s
    //    `ResultKeyed` branch); through the form path the shared handler stays
    //    unbound, so the follow-up `GetSample` fails with "handler not bound".
    //
    // Deleting either would delete the only working path, so each is exempted
    // until the form path grows the same effect. Both statements are
    // falsifiable by test rather than by opinion: make `submitIfValid`'s
    // `RegisterClient` set `clientId`, or its `RegisterSample` attach the
    // shared handler, and the exemption becomes stale -- which this audit
    // reports as a finding in its own right the moment QML binds the member or
    // the member is deleted.
    const QString registrationPath = QStringLiteral(
        "the only dispatch that emits clientRegistered / attaches the shared handler; "
        "submitIfValid's key-less RegisterClient/RegisterSample path does neither");
    for (const auto& [alias, member] : std::initializer_list<std::pair<const char*, const char*>>{
             {"sampleBridge", "registerClient"},
             {"sampleBridge", "registerSample"},
         }) {
        audit.allowUnbound(QString::fromLatin1(alias), QString::fromLatin1(member), registrationPath);
    }

    const QStringList findings = audit.run();
    INFO(findings.join(QStringLiteral("\n")).toStdString());
    CHECK(findings.isEmpty());

    // The audit is only as good as the files it found: the three gui/qml ships.
    CHECK(audit.scannedFiles() == QStringList{QStringLiteral("Main.qml"), QStringLiteral("ResultEntryView.qml"),
                                              QStringLiteral("SampleView.qml")});
}

TEST_CASE("A refused form submission has a path to the operator", "[lims][gui][qml-surface]") {
    // Every schema-driven form on this rung submits through `submitIfValid`,
    // and `submitIfValid` reports *both* outcomes on one signal:
    // `replyReceived(actionType, ok, payload)`, with the model's own `what()`
    // as the payload when `ok` is false. The `failed`/`lastError` pair the two
    // views bind carries only the *typed* invokables' errors -- the presenters
    // never route a form refusal through it.
    //
    // So `replyReceived` is the only carrier a refused submission has, and a
    // rung whose QML does not handle it shows the operator nothing at all when
    // a reading is over-precise (README decision 7), a capture violates
    // `exactlyOneOf` (decision 6), a four-eyes verification is refused
    // (decision 16), a qualifier or dilution code is unknown (decision 18), or
    // a conflict resolution is rejected. Those refusals are this rung's whole
    // thesis, so "the refusal reached the screen" is asserted here rather than
    // assumed from the fact that the bridge emitted something.
    //
    // Deliberately a separate case from the whole-surface audit above, and
    // deliberately unexempted: that case's exemption list is where a member
    // nothing binds is allowed to live, and an `allowUnbound` entry for
    // `replyReceived` would make the list say this defect is acceptable.
    BackendRig rig{Mode::Local, 1};
    lims::gui::SampleBridge sampleBridge{rig.bridge(0), rig.executor()};
    lims::gui::ResultBridge resultBridge{rig.bridge(0), rig.executor()};

    QmlSurfaceAudit audit{QStringLiteral(MORPH_LADDER_SOURCE_ROOT "/examples/lims/gui/qml")};
    audit.bind(QStringLiteral("sampleBridge"), sampleBridge);
    audit.bind(QStringLiteral("resultBridge"), resultBridge);

    // No exemptions: this case asks one question of the whole surface and then
    // filters the answer down to the one member it is about, so an exemption
    // added elsewhere can never quieten it.
    const QStringList findings = audit.run().filter(QStringLiteral("replyReceived"));
    INFO(findings.join(QStringLiteral("\n")).toStdString());
    CHECK(findings.isEmpty());
}

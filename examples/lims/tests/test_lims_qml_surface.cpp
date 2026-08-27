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

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <catch2/catch_test_macros.hpp>
#include <initializer_list>
#include <morph/forms/forms.hpp>
#include <utility>

#include "lims/dto/sample_dto.hpp"
#include "lims_schemas.hpp"
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

    // ── The exemptions that survive, and the one mechanism behind them ───
    // The first run of this audit reported 15 members these two bridges
    // publish that no file under gui/qml/ binds, recorded as one undifferentiated
    // backlog. Thirteen of them went first: `replyReceived` on each bridge is
    // handled by the views (see the case below), `resultVerified` is handled
    // by ResultEntryView.qml, and the nine remaining typed invokables and
    // outcome signals were deleted as redundant second dispatch paths -- every
    // one of them duplicated an action the schema-driven `submitIfValid` path
    // already carried, from a QML call site that did not exist.
    //
    // The last two, `registerClient` and `registerSample`, survived until
    // `submitIfValid` grew their two effects itself: `RegisterClient`'s reply
    // is decoded to emit `clientRegistered` (the only thing that sets the
    // `clientId` property SampleView.qml binds), and `RegisterSample` runs on
    // the shared handler, so it leaves that handler attached to the new sample
    // the same way the typed call did (`BridgeHandler::execute`'s `ResultKeyed`
    // branch). Neither typed invokable added anything beyond that any more, so
    // both are gone (morph#309, closing morph#287's last two exemptions).

    // `submitIfValid` on both bridges is called from the shipped `MorphForms`
    // renderer's QML (`src/qt/forms/qml/DynamicForm.qml`'s `submit()` and its
    // auto-submit path), not from this rung's. Every form on both views
    // declares `explicitSubmit = true` and is bound with `controller:
    // page.sampleBridge` / `controller: page.resultBridge`, so the renderer's
    // own Submit button is the sole caller — this rung's QML names the
    // *controller*, never the method. The exemption cannot go stale
    // unnoticed: the audit rejects it the moment either bridge loses the
    // member, or some scanned `.qml` file starts binding it again.
    const QString rendererPath = QStringLiteral(
        "called by the shipped MorphForms DynamicForm, whose QML this audit does not scan; this rung's forms "
        "are bound to the controller and submitted by the renderer's own explicit Submit button");
    for (const auto& [alias, member] : std::initializer_list<std::pair<const char*, const char*>>{
             {"sampleBridge", "submitIfValid"},
             {"resultBridge", "submitIfValid"},
         }) {
        audit.allowUnbound(QString::fromLatin1(alias), QString::fromLatin1(member), rendererPath);
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

TEST_CASE("Every form in the shipped schema document opts out of auto-submit", "[lims][gui][qml-surface]") {
    // `SampleView.qml` and `ResultEntryView.qml` bind every one of these forms
    // to a live controller (`sampleBridge`/`resultBridge`), and the shipped
    // renderer's default is to call `submitIfValid` the instant a form is
    // valid (`docs/spec/forms/forms.md`, "Explicit submit mode"). Every one of
    // these six actions has effects -- registering a client or sample, a
    // rework/rejection, a captured reading, a conflict resolution -- so a
    // schema that lost its `x-submitMode` would mean one dispatch per typed
    // character, in the shipped GUI, with nothing else to catch it: the
    // engine-load smoke test loads every root with a null controller, and
    // `QmlSurfaceAudit` reads names, not schemas.
    //
    // The guard reads the generated *schema* rather than each action's
    // `explicitSubmit` declaration, since the declaration is only ever
    // meaningful through what `schemaJson<A>()` emits, which is what
    // `DynamicForm` actually reads. It also sweeps the whole document rather
    // than a written-out list of six, so a seventh form added to
    // `limsSchemasJson()` cannot join the shipped client without answering
    // this question.
    const QJsonDocument schemas = QJsonDocument::fromJson(QByteArray::fromStdString(lims::gui::limsSchemasJson()));
    REQUIRE(schemas.isObject());
    // Held in a named object, not iterated straight off `schemas.object()`:
    // that returns a *copy*, so an iterator taken from the temporary is
    // already dangling by the time the loop body runs.
    const QJsonObject document = schemas.object();
    REQUIRE(document.size() == 6);
    for (auto entry = document.constBegin(); entry != document.constEnd(); ++entry) {
        INFO("action type: " << entry.key().toStdString());
        REQUIRE(entry.value().isObject());
        CHECK(entry.value().toObject().value(QStringLiteral("x-submitMode")).toString() == QStringLiteral("explicit"));
    }
}

TEST_CASE("A read-only action's schema carries no x-submitMode at all", "[lims][gui][qml-surface]") {
    // The other half of the guard above, and the reason it is not vacuous:
    // `x-submitMode` is opt-in, so a check that only ever looked for the key's
    // presence would pass just as happily if `annotateSubmitMode` stamped
    // every schema unconditionally -- at which point it would be measuring
    // nothing. `GetSample` is a pure query: it mutates nothing, carries no
    // fields at all (`lims_schemas.hpp` never renders it as a form), wants the
    // auto-submit default, and declares no `explicitSubmit`, so its schema
    // must not carry the key.
    const QJsonDocument schema =
        QJsonDocument::fromJson(QByteArray::fromStdString(::morph::forms::schemaJson<lims::GetSample>()));
    REQUIRE(schema.isObject());
    CHECK_FALSE(schema.object().contains(QStringLiteral("x-submitMode")));
}

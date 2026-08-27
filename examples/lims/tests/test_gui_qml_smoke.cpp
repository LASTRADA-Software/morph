// SPDX-License-Identifier: Apache-2.0
//
// The one QML test examples/TESTING.md presenter rule 6 asks each rung for:
// "one offscreen engine-load smoke test (engine creates root object, no
// errors) registered in ctest — not Qt Quick Test, and no synthesized-mouse-
// event flows." It loads the *same* Lims/Main.qml the desktop client ships
// (both link the ladder_lims_qml module), with no bridges attached — which is
// why Main.qml's two `*Bridge` properties, and the ones SampleView.qml and
// ResultEntryView.qml declare, all default to null.
//
// What this proves and does not prove, stated here rather than inherited
// silently from the sibling rungs' identical tests.
//
// It proves: every QML file reachable from the roots below parses; the engine
// resolves every *type* they instantiate and every property those types
// declare; and it builds a root object emitting zero QML warnings.
//
// It specifically does NOT prove that `Connections` signal-handler names or
// delegate `modelData.*` property names are correct. Both are resolved
// dynamically against an object this test never supplies: every bridge
// property is null, so Main.qml's `Connections` block has no live target and
// its `onSampleChanged` name is never matched against a real signal; and
// every list model is empty, so no delegate is instantiated and no
// `modelData.valueText` is ever looked up. A handler bound to a signal that
// does not exist passes this test.
//
// That gap is covered from the other side instead: test_result_qml_bridge.cpp
// asserts, against the real `ResultBridge`, that every key the delegates read
// is actually present in the property bags the bridge emits, and that the
// signal names Main.qml connects to exist on the bridge's metaobject.
//
// Main.qml's StackLayout instantiates both tabs eagerly (unlike a StackView,
// which would only build its initial item), so loading Main alone already
// engine-checks SampleView.qml and ResultEntryView.qml. They are still loaded
// standalone below, because a root object is checked more strictly than a
// nested one and because either could later move behind a lazy Loader.
//
// MORPH_LADDER_QML_URI is defined by morph_add_rung() only when the rung's
// QML module was actually built (MORPH_BUILD_FORMS_QML=ON). Without it this
// file is an empty translation unit, so a configure that legitimately has no
// Qt Quick still builds.

#ifdef MORPH_LADDER_QML_URI

#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QQmlApplicationEngine>
#include <QQmlError>
#include <QString>
#include <QVariantMap>
#include <catch2/catch_test_macros.hpp>
#include <string>

#include "lims_schemas.hpp"

namespace {

/// @brief Loads @p typeName from this rung's QML module and returns the first
///        warning the engine emitted, or an empty string.
/// @param typeName Unqualified QML type name within `MORPH_LADDER_QML_URI`.
/// @param created  Set to whether a root object was produced.
/// @return The first warning's text, or an empty string if there was none.
[[nodiscard]] std::string firstWarningLoading(const char* typeName, bool& created) {
    QQmlApplicationEngine engine;

    QString firstWarning;
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, [&firstWarning](const QList<QQmlError>& warnings) {
        if (firstWarning.isEmpty() && !warnings.isEmpty()) {
            firstWarning = warnings.front().toString();
        }
    });

    engine.loadFromModule(MORPH_LADDER_QML_URI, typeName);
    created = !engine.rootObjects().isEmpty();
    return firstWarning.toStdString();
}

}  // namespace

TEST_CASE("lims' QML engine loads Main.qml and creates a root object with no errors", "[lims][gui][qml-smoke]") {
    bool created = false;
    // Reported through the message, not a bare boolean: a QML warning is
    // otherwise a failing assertion with nothing to act on.
    CHECK(firstWarningLoading("Main", created) == std::string{});
    REQUIRE(created);
}

TEST_CASE("lims' sample-lifecycle surface loads standalone with no errors", "[lims][gui][qml-smoke]") {
    bool created = false;
    CHECK(firstWarningLoading("SampleView", created) == std::string{});
    REQUIRE(created);
}

TEST_CASE("lims' result-entry surface loads standalone with no errors", "[lims][gui][qml-smoke]") {
    bool created = false;
    CHECK(firstWarningLoading("ResultEntryView", created) == std::string{});
    REQUIRE(created);
}

TEST_CASE("SampleView's four schema-driven forms render the shipped renderer's own Submit button",
          "[lims][gui][qml-smoke]") {
    // The loads above pass no `schemas` property, so no DynamicForm has a
    // schema to render and this cannot be observed there. Loaded once more
    // with the real document instead, which is the only way to see the
    // consequence of `explicitSubmit = true` in an actual item tree rather
    // than inferring it from the emitted JSON: DynamicForm loads its Submit
    // Button through a `Loader { active: form.explicitSubmitMode }`, so a
    // form whose schema lacked the key would have no such object anywhere
    // beneath it, not merely a hidden one (docs/spec/forms/forms.md,
    // "Explicit submit mode").
    //
    // Still an engine-load test per examples/TESTING.md presenter rule 6 --
    // it queries the object tree, and synthesizes no input events.
    const QJsonDocument document = QJsonDocument::fromJson(QString::fromStdString(lims::gui::limsSchemasJson()).toUtf8());
    REQUIRE(document.isObject());

    QQmlApplicationEngine engine;
    engine.setInitialProperties({{QStringLiteral("schemas"), document.object().toVariantMap()}});
    engine.loadFromModule(MORPH_LADDER_QML_URI, "SampleView");
    REQUIRE_FALSE(engine.rootObjects().isEmpty());

    // Four forms on this view (RegisterClient, RegisterSample, ReturnForRework,
    // RejectSample) -- and `sampleBridge` is still null here, so this counts
    // controls the renderer built, not anything a live controller caused.
    QObject* const root = engine.rootObjects().front();
    CHECK(root->findChildren<QObject*>(QStringLiteral("submitButton")).size() == 4);
}

TEST_CASE("ResultEntryView's two schema-driven forms render the shipped renderer's own Submit button",
          "[lims][gui][qml-smoke]") {
    // Same reasoning as SampleView above.
    const QJsonDocument document = QJsonDocument::fromJson(QString::fromStdString(lims::gui::limsSchemasJson()).toUtf8());
    REQUIRE(document.isObject());

    QQmlApplicationEngine engine;
    engine.setInitialProperties({{QStringLiteral("schemas"), document.object().toVariantMap()}});
    engine.loadFromModule(MORPH_LADDER_QML_URI, "ResultEntryView");
    REQUIRE_FALSE(engine.rootObjects().isEmpty());

    // Two forms on this view (CaptureConcentration, ResolveConflict) -- and
    // `resultBridge` is still null here, so this counts controls the renderer
    // built, not anything a live controller caused.
    QObject* const root = engine.rootObjects().front();
    CHECK(root->findChildren<QObject*>(QStringLiteral("submitButton")).size() == 2);
}

#endif  // MORPH_LADDER_QML_URI

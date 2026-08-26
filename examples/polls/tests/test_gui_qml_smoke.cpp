// SPDX-License-Identifier: Apache-2.0
//
// The one QML test examples/TESTING.md presenter rule 6 asks each rung for:
// "one offscreen engine-load smoke test (engine creates root object, no
// errors) registered in ctest — not Qt Quick Test, and no synthesized-mouse-
// event flows." Mirrors examples/bookmarks/tests/test_gui_qml_smoke.cpp
// (rung 2's Task 18) exactly in shape and in what it does/does not prove —
// see that file's own header comment for the full explanation, restated only
// where this rung's own structure differs below.
//
// This rung ships three QML files (Main, CreatePollView, VoteView), and
// Main.qml's StackView starts on its inline landing screen: nothing pushes
// CreatePollView or VoteView without a live `pollBridge` (both require a
// non-null controller to do anything, and Main's own "Create a new poll"
// button is additionally gated on `pollBridge !== null`). So, exactly as
// rung 2's BookmarkListView needed its own standalone load, both are loaded
// here as root objects in their own right — every controller property
// defaults to null, exactly as when the desktop client has not finished
// connecting yet (and exactly what tests/test_poll_qml_bridges.cpp's own
// suite proves *with* a live controller, at the adapter layer rather than
// through the QML engine).
//
// MORPH_LADDER_QML_URI is defined by morph_add_rung() only when the rung's QML
// module was actually built (MORPH_BUILD_FORMS_QML=ON). Without it this file
// is an empty translation unit, so a configure that legitimately has no Qt
// Quick still builds.

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

#include "poll_schemas.hpp"

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

TEST_CASE("polls' QML engine loads Main.qml and creates a root object with no errors", "[polls][gui][qml-smoke]") {
    bool created = false;
    // Reported through the message, not a bare boolean: a QML warning is
    // otherwise a failing assertion with nothing to act on.
    CHECK(firstWarningLoading("Main", created) == std::string{});
    REQUIRE(created);
}

TEST_CASE("polls' create-poll screen loads standalone with no errors", "[polls][gui][qml-smoke]") {
    // Main.qml's StackView never reaches CreatePollView without a live
    // pollBridge and a click on the (also pollBridge-gated) "Create a new
    // poll" button — see this file's header comment.
    bool created = false;
    CHECK(firstWarningLoading("CreatePollView", created) == std::string{});
    REQUIRE(created);
}

TEST_CASE("polls' vote screen loads standalone with no errors", "[polls][gui][qml-smoke]") {
    // Same reasoning as CreatePollView above; VoteView's own
    // Component.onCompleted also guards its one side effect (calling
    // pollBridge.openPoll) on pollBridge being non-null, so loading it here
    // with the default null controller triggers no dispatch at all.
    bool created = false;
    CHECK(firstWarningLoading("VoteView", created) == std::string{});
    REQUIRE(created);
}

TEST_CASE("VoteView's three schema-driven forms render the shipped renderer's own Submit button",
          "[polls][gui][qml-smoke]") {
    // The load above passes `schemas: {}`, so no DynamicForm has a schema to
    // render and this cannot be observed there. Loaded once more with the real
    // document instead, which is the only way to see the consequence of
    // `explicitSubmit = true` in an actual item tree rather than inferring it
    // from the emitted JSON: DynamicForm loads its Submit Button through a
    // `Loader { active: form.explicitSubmitMode }`, so a form whose schema
    // lacked the key would have no such object anywhere beneath it, not merely
    // a hidden one (docs/spec/forms/forms.md, "Explicit submit mode").
    //
    // Still an engine-load test per examples/TESTING.md presenter rule 6 --
    // it queries the object tree, and synthesizes no input events.
    const QJsonDocument document =
        QJsonDocument::fromJson(QString::fromStdString(polls::gui::pollSchemasJson()).toUtf8());
    REQUIRE(document.isObject());

    QQmlApplicationEngine engine;
    engine.setInitialProperties({{QStringLiteral("schemas"), document.object().toVariantMap()}});
    engine.loadFromModule(MORPH_LADDER_QML_URI, "VoteView");
    REQUIRE_FALSE(engine.rootObjects().isEmpty());

    // Three forms, three buttons -- and `pollBridge` is still null here, so
    // this counts controls the renderer built, not anything a live controller
    // caused.
    QObject* const root = engine.rootObjects().front();
    CHECK(root->findChildren<QObject*>(QStringLiteral("submitButton")).size() == 3);
}

#endif  // MORPH_LADDER_QML_URI

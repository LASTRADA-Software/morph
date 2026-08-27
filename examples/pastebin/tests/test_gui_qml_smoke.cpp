// SPDX-License-Identifier: Apache-2.0
//
// The one QML test examples/TESTING.md presenter rule 6 asks each rung for:
// "one offscreen engine-load smoke test (engine creates root object, no
// errors) registered in ctest — not Qt Quick Test, and no synthesized-mouse-
// event flows." It loads the *same* Pastebin/Main.qml the desktop client
// ships (both link the ladder_pastebin_qml module), with no controllers
// attached — which is why Main.qml's `formsController`/`pasteController`
// default to null.
//
// MORPH_LADDER_QML_URI is defined by morph_add_rung() only when the rung's QML
// module was actually built (MORPH_BUILD_FORMS_QML=ON — the shipped MorphForms
// renderer Main.qml imports). Without it this file is an empty translation
// unit, so a configure that legitimately has no Qt Quick still builds.
//
// Runs under QT_QPA_PLATFORM=offscreen (already set for the ladder-tests and
// clang-coverage CI legs) against the QGuiApplication testkit_main.cpp owns
// when this rung's test binary is built — Qt Quick cannot instantiate a window
// under a plain QCoreApplication.

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

#include "paste_schemas.hpp"

TEST_CASE("pastebin's QML engine loads Main.qml and creates a root object with no errors",
          "[pastebin][gui][qml-smoke]") {
    QQmlApplicationEngine engine;

    QString firstWarning;
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, [&firstWarning](const QList<QQmlError>& warnings) {
        if (firstWarning.isEmpty() && !warnings.isEmpty()) {
            firstWarning = warnings.front().toString();
        }
    });

    engine.loadFromModule(MORPH_LADDER_QML_URI, "Main");

    // Reported through the message, not a bare boolean: a QML warning is
    // otherwise a failing assertion with nothing to act on.
    CHECK(firstWarning.toStdString() == std::string{});
    REQUIRE_FALSE(engine.rootObjects().isEmpty());
}

TEST_CASE("Main.qml's create form renders the shipped renderer's own Submit button", "[pastebin][gui][qml-smoke]") {
    // The load above passes no `schemas` property, so the create DynamicForm
    // has no schema to render and this cannot be observed there. Loaded once
    // more with the real document instead, which is the only way to see the
    // consequence of `explicitSubmit = true` in an actual item tree rather
    // than inferring it from the emitted JSON: DynamicForm loads its Submit
    // Button through a `Loader { active: form.explicitSubmitMode }`, so a
    // form whose schema lacked the key would have no such object anywhere
    // beneath it, not merely a hidden one (docs/spec/forms/forms.md,
    // "Explicit submit mode").
    //
    // Still an engine-load test per examples/TESTING.md presenter rule 6 --
    // it queries the object tree, and synthesizes no input events.
    const QJsonDocument document =
        QJsonDocument::fromJson(QString::fromStdString(pastebin::gui::pasteSchemasJson()).toUtf8());
    REQUIRE(document.isObject());

    QQmlApplicationEngine engine;
    engine.setInitialProperties({{QStringLiteral("schemas"), document.object().toVariantMap()}});
    engine.loadFromModule(MORPH_LADDER_QML_URI, "Main");
    REQUIRE_FALSE(engine.rootObjects().isEmpty());

    // One form on this screen (CreatePaste) -- and `formsController` is still
    // null here, so this counts controls the renderer built, not anything a
    // live controller caused.
    QObject* const root = engine.rootObjects().front();
    CHECK(root->findChildren<QObject*>(QStringLiteral("submitButton")).size() == 1);
}

#endif  // MORPH_LADDER_QML_URI

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

#include <QList>
#include <QQmlApplicationEngine>
#include <QQmlError>
#include <QString>
#include <catch2/catch_test_macros.hpp>
#include <string>

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

#endif  // MORPH_LADDER_QML_URI

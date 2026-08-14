// SPDX-License-Identifier: Apache-2.0
//
// The one QML test examples/TESTING.md presenter rule 6 asks each rung for:
// "one offscreen engine-load smoke test (engine creates root object, no
// errors) registered in ctest — not Qt Quick Test, and no synthesized-mouse-
// event flows." It loads the *same* Bookmarks/Main.qml the desktop client
// ships (both link the ladder_bookmarks_qml module), with no controllers
// attached — which is why Main.qml's four `*Controller` properties, and the
// ones LoginView.qml/BookmarkListView.qml declare, all default to null.
//
// What this does and does not prove, restated here rather than silently
// inherited from rung 1's identical test (Task 12 of that rung's ledger).
//
// It proves: every QML file reachable from the two roots loaded below parses;
// the engine resolves every *type* they instantiate and every property those
// types declare; and it builds a root object emitting zero QML warnings.
//
// It specifically does NOT prove that `Connections` signal-handler names or
// delegate `modelData.*` property names are correct. Both are resolved
// dynamically, against an object this test never supplies: every controller
// property is null, so no `Connections` block has a live `target` and none of
// its `onXxx` handler names is ever matched against a real signal; and every
// list model is empty, so no delegate is ever instantiated and no
// `modelData.someField` is ever looked up. A handler bound to a signal that
// does not exist, or a delegate reading a property the model never supplies,
// passes this test.
//
// It also proves nothing about behavior against a live backend — with
// `formsController` null there is no schema document, so each DynamicForm
// renders an empty field list, and the bootstrap timer in BookmarkListView
// never runs (it is gated on a non-null controller). The backend-facing half
// is covered by the presenter suites (test_bookmark_presenter.cpp and its two
// siblings) and, for the composed client, by manual end-to-end verification —
// see this rung's README.
//
// One structural consequence, and what is done about it: Main.qml's
// StackView starts on LoginView, so loading Main alone would instantiate
// LoginView but *not* BookmarkListView — nothing can push it here, since
// `loggedIn` comes from a controller that is null. The second case below
// therefore loads BookmarkListView as a root object in its own right, so the
// screen with all five DynamicForms, three list views and four `Connections`
// blocks is genuinely engine-checked rather than merely compiled.
//
// MORPH_LADDER_QML_URI is defined by morph_add_rung() only when the rung's QML
// module was actually built (MORPH_BUILD_FORMS_QML=ON — the shipped MorphForms
// renderer these files import). Without it this file is an empty translation
// unit, so a configure that legitimately has no Qt Quick still builds.
//
// Runs under QT_QPA_PLATFORM=offscreen (already set for the ladder-tests and
// clang-coverage CI legs) against the QGuiApplication testkit_main.cpp owns
// when this rung's test binary is built — Qt Quick cannot instantiate a window
// under a plain QCoreApplication.

#ifdef MORPH_LADDER_QML_URI

#include <catch2/catch_test_macros.hpp>

#include <QList>
#include <QQmlApplicationEngine>
#include <QQmlError>
#include <QString>

#include <string>

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

TEST_CASE("bookmarks' QML engine loads Main.qml and creates a root object with no errors",
          "[bookmarks][gui][qml-smoke]") {
    bool created = false;
    // Reported through the message, not a bare boolean: a QML warning is
    // otherwise a failing assertion with nothing to act on.
    CHECK(firstWarningLoading("Main", created) == std::string{});
    REQUIRE(created);
}

TEST_CASE("bookmarks' post-login screen loads standalone with no errors", "[bookmarks][gui][qml-smoke]") {
    // Main.qml's StackView never reaches BookmarkListView without a live
    // controller, so it is loaded directly here — see this file's header
    // comment. Every controller property defaults to null, exactly as when
    // the desktop client has not finished connecting yet.
    bool created = false;
    CHECK(firstWarningLoading("BookmarkListView", created) == std::string{});
    REQUIRE(created);
}

#endif  // MORPH_LADDER_QML_URI

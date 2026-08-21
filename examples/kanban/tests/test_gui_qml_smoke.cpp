// SPDX-License-Identifier: Apache-2.0
//
// The one QML test examples/TESTING.md presenter rule 6 asks each rung for:
// "one offscreen engine-load smoke test (engine creates root object, no
// errors) registered in ctest — not Qt Quick Test, and no synthesized-mouse-
// event flows." It loads the *same* Kanban/Main.qml the desktop client
// ships (both link the ladder_kanban_qml module), with no bridges attached —
// which is why Main.qml's two `*Bridge` properties, and the ones
// LoginView.qml/ProjectListView.qml/BoardView.qml/MembersView.qml/
// TaskDetailPopup.qml/RulesView.qml declare, all default to null. Mirrors
// examples/bookmarks/tests/test_gui_qml_smoke.cpp's structure exactly (see
// that file's own header comment for the full rationale), substituting the
// module URI and this rung's own screen names.
//
// What this does and does not prove, restated here rather than silently
// inherited from bookmarks' identical test.
//
// It proves: every QML file reachable from the roots loaded below parses;
// the engine resolves every *type* they instantiate and every property those
// types declare; and it builds a root object emitting zero QML warnings.
//
// It specifically does NOT prove that `Connections` signal-handler names or
// delegate `modelData.*` property names are correct. Both are resolved
// dynamically, against an object this test never supplies: every bridge
// property is null, so no `Connections` block has a live `target` and none
// of its `onXxx` handler names is ever matched against a real signal; and
// every list model is empty, so no delegate is ever instantiated and no
// `modelData.someField` is ever looked up. A handler bound to a signal that
// does not exist, or a delegate reading a property the model never
// supplies, passes this test.
//
// It also proves nothing about behavior against a live backend. The
// backend-facing half is covered by the presenter/bridge suites
// (test_project_admin_presenter.cpp, test_board_presenter.cpp,
// test_project_admin_qml_bridge.cpp, test_board_qml_bridge.cpp,
// test_board_concurrent_drag.cpp) and, for the composed client, by manual
// end-to-end verification -- see this rung's README.
//
// One structural consequence, and what is done about it: Main.qml's
// StackView starts on LoginView, so loading Main alone would instantiate
// LoginView but *not* ProjectListView or BoardView -- nothing can push them
// here, since `loggedIn`/`projectOpened` come from a bridge that is null.
// The second and third cases below therefore load ProjectListView and
// BoardView directly as root objects in their own right, so those screens
// are genuinely engine-checked rather than merely compiled. MembersView,
// TaskDetailPopup, and RulesView are all reachable from ProjectListView.qml/
// BoardView.qml (ProjectListView instantiates MembersView directly; BoardView
// instantiates both TaskDetailPopup and RulesView directly, the latter inside
// a Popup opened by its own "Rules" header button), so loading those two
// roots already exercises every one of this rung's seven QML files.
//
// MORPH_LADDER_QML_URI is defined by morph_add_rung() only when the rung's
// QML module was actually built (MORPH_BUILD_FORMS_QML=ON). Without it this
// file is an empty translation unit, so a configure that legitimately has no
// Qt Quick still builds.
//
// Runs under QT_QPA_PLATFORM=offscreen (already set for the ladder-tests and
// clang-coverage CI legs) against the QGuiApplication testkit_main.cpp owns
// when this rung's test binary is built — Qt Quick cannot instantiate a
// window under a plain QCoreApplication.

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

TEST_CASE("kanban's QML engine loads Main.qml and creates a root object with no errors",
          "[kanban][gui][qml-smoke]") {
    bool created = false;
    // Reported through the message, not a bare boolean: a QML warning is
    // otherwise a failing assertion with nothing to act on.
    CHECK(firstWarningLoading("Main", created) == std::string{});
    REQUIRE(created);
}

TEST_CASE("kanban's post-login project list loads standalone with no errors", "[kanban][gui][qml-smoke]") {
    // Main.qml's StackView never reaches ProjectListView without a live
    // bridge, so it is loaded directly here — see this file's header
    // comment. Every bridge property defaults to null, exactly as when the
    // desktop client has not finished connecting yet. This also exercises
    // MembersView.qml, which ProjectListView.qml instantiates directly.
    bool created = false;
    CHECK(firstWarningLoading("ProjectListView", created) == std::string{});
    REQUIRE(created);
}

TEST_CASE("kanban's board view loads standalone with no errors", "[kanban][gui][qml-smoke]") {
    // Reached only after a project is opened in the real app, so it is
    // loaded directly here too. This also exercises TaskDetailPopup.qml and
    // RulesView.qml, both of which BoardView.qml instantiates directly (the
    // latter inside a Popup opened by its own "Rules" header button).
    bool created = false;
    CHECK(firstWarningLoading("BoardView", created) == std::string{});
    REQUIRE(created);
}

#endif  // MORPH_LADDER_QML_URI

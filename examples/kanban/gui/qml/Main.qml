// SPDX-License-Identifier: Apache-2.0
//
// kanban's desktop shell: a StackView holding three screens — LoginView
// until projectAdminBridge says a token is installed, ProjectListView
// afterwards, and BoardView once a project is opened. Mirrors
// examples/bookmarks/gui/qml/Main.qml's own StackView shell exactly (see
// that file's own comments), extended by one extra page since this rung has
// one more level of navigation (project list -> board) than bookmarks' flat
// login -> list shape.
//
// The two bridge properties are supplied by gui/main.cpp through
// QQmlApplicationEngine::setInitialProperties. They default to null so this
// same file also loads with nothing wired up, which is exactly what the
// offscreen engine-load smoke test (tests/test_gui_qml_smoke.cpp) does.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    width: 1280
    height: 860
    visible: true
    title: "kanban — morph application ladder, rung 4"

    property var projectAdminBridge: null
    property var boardBridge: null

    /// The signed-in identity, as the *server* echoed it back — never the
    /// username the user typed (kanban/dto/auth_dto.hpp's trust note).
    property string principal: ""

    Connections {
        target: root.projectAdminBridge

        // Emitted by ProjectAdminBridge only after the returned token is
        // already installed as the bridge's default session, so the screen
        // this pushes may dispatch immediately.
        function onLoggedIn(principal) {
            root.principal = principal
            stack.replace(listPage)
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 6

        RowLayout {
            Layout.fillWidth: true

            Label {
                font.bold: true
                text: "kanban"
            }

            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignRight
                opacity: 0.7
                text: root.principal !== "" ? "signed in as " + root.principal : "not signed in"
            }
        }

        StackView {
            id: stack
            Layout.fillWidth: true
            Layout.fillHeight: true
            initialItem: loginPage
        }
    }

    Component {
        id: loginPage

        LoginView {
            projectAdminBridge: root.projectAdminBridge
        }
    }

    Component {
        id: listPage

        ProjectListView {
            projectAdminBridge: root.projectAdminBridge
            boardBridge: root.boardBridge

            onProjectOpened: (projectId, myRole) => {
                root.boardBridge.setMyRole(myRole)
                root.boardBridge.openBoard(projectId)
                stack.push(boardPage)
            }
        }
    }

    Component {
        id: boardPage

        BoardView {
            boardBridge: root.boardBridge
            projectAdminBridge: root.projectAdminBridge

            onCloseRequested: {
                root.boardBridge.stopPolling()
                stack.pop()
            }
        }
    }
}

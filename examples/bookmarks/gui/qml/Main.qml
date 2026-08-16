// SPDX-License-Identifier: Apache-2.0
//
// bookmarks' desktop shell: a StackView holding exactly two screens, and the
// one navigation rule between them — LoginView until FormsBridge says a token
// is installed, BookmarkListView afterwards. Everything else is in those two
// files; this one owns the window, the parsed schema document, and the
// transition.
//
// The four controller properties are supplied by gui/main.cpp through
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
    title: "bookmarks — morph application ladder, rung 2"

    property var formsController: null
    property var bookmarkController: null
    property var tagController: null
    property var feedController: null

    /// The whole `{actionType: schema}` document, parsed once here rather
    /// than per form: it is a CONSTANT property on the controller, so one
    /// parse is all it can ever need.
    property var schemas: root.formsController ? JSON.parse(root.formsController.schemasJson) : ({})

    /// The signed-in identity, as the *server* echoed it back — never the
    /// username the user typed (bookmarks/dto/auth_dto.hpp's trust note).
    property string principal: ""

    Connections {
        target: root.formsController

        // Emitted by FormsBridge only after the returned token is already
        // installed as the bridge's default session, so the screen this
        // pushes may dispatch immediately.
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
                text: "bookmarks"
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
            formsController: root.formsController
            loginSchema: root.schemas["Login"] || ({})
        }
    }

    Component {
        id: listPage

        BookmarkListView {
            formsController: root.formsController
            bookmarkController: root.bookmarkController
            tagController: root.tagController
            feedController: root.feedController
            schemas: root.schemas
        }
    }
}

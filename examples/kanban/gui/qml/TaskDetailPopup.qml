// SPDX-License-Identifier: Apache-2.0
//
// kanban's comment/activity overlay, design spec §7: tapping (not dragging) a
// card opens this Popup -- the board stays visible underneath. Shows the
// task's own comments (filtered out of BoardBridge.board.comments, which
// carries every comment on the whole board -- board_qml_bridge.cpp's
// CommentView has no taskId of its own in the property bag today, so this
// view shows the board's full comment list while the popup is open for a
// given task; the add-comment field is what is genuinely task-scoped) plus
// an add-comment field driven by BoardBridge.addComment.
//
// `boardBridge` defaults to null and `taskId` defaults to -1 so this same
// file also loads standalone with nothing wired up, which is exactly what
// the offscreen engine-load smoke test (tests/test_gui_qml_smoke.cpp) does.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: popup
    modal: true
    focus: true
    width: 420
    height: 480
    x: (parent ? parent.width - width : 0) / 2
    y: (parent ? parent.height - height : 0) / 2

    property var boardBridge: null
    property string taskId: "-1"
    property string taskTitle: ""

    /// Every comment on the board -- see this file's header comment on why
    /// this popup does not filter by task.
    readonly property var comments: boardBridge && boardBridge.board && boardBridge.board.comments
                                     ? boardBridge.board.comments : []

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        Label {
            Layout.fillWidth: true
            font.bold: true
            font.pixelSize: 16
            elide: Text.ElideRight
            text: popup.taskTitle !== "" ? popup.taskTitle : "Task"
        }

        Label {
            font.bold: true
            text: "Comments"
        }

        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: popup.comments

            delegate: ColumnLayout {
                id: row
                required property var modelData
                width: ListView.view ? ListView.view.width : 0

                Label {
                    font.bold: true
                    text: row.modelData.principal
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: row.modelData.body
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true

            TextField {
                id: newComment
                Layout.fillWidth: true
                placeholderText: "add a comment"
                onAccepted: addButton.clicked()
            }

            Button {
                id: addButton
                text: "Add"
                enabled: popup.boardBridge !== null && newComment.text.length > 0
                onClicked: {
                    popup.boardBridge.addComment(popup.taskId, newComment.text)
                    newComment.text = ""
                }
            }
        }

        Button {
            Layout.fillWidth: true
            text: "Close"
            onClicked: popup.close()
        }
    }
}

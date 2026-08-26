// SPDX-License-Identifier: Apache-2.0
//
// kanban's comment/activity overlay, design spec §7: tapping (not dragging) a
// card opens this Popup -- the board stays visible underneath. Shows only the
// tapped task's own comments, filtered client-side out of
// BoardBridge.board.comments (which carries every comment on the whole
// board) by matching each comment's own taskId against popup.taskId --
// board_qml_bridge.cpp's CommentView property bag carries a taskId
// (kanban::CommentView::taskId, populated in board_model.cpp's buildState
// from CommentRecord::task, the existing BelongsTo to the owning task) --
// plus an add-comment field driven by BoardBridge.addComment.
//
// Task 18 adds the attachment list + an "Attach file" button alongside the
// comment section above: a FileDialog picks a local file, BoardBridge.
// uploadAttachment() reads its bytes, POSTs them to Task 17's
// AttachmentServer, and commits the metadata via AddAttachment -- this file
// only ever binds to boardBridge.attachments and calls uploadAttachment()/
// downloadAttachment(), same "translation, not logic" discipline the comment
// section above already follows. Unlike the comment list, BoardBridge.
// attachments already carries only the requested task's own rows (it is
// populated by an explicit getAttachments(taskId) call, not filtered
// client-side out of a whole-board list the way comments are), so no
// client-side filter is needed here. Both attachment operations also report
// their outcome through `status` below -- the one thing the bridge's own
// re-fetch does not cover.
//
// `boardBridge` defaults to null and `taskId` defaults to -1 so this same
// file also loads standalone with nothing wired up, which is exactly what
// the offscreen engine-load smoke test (tests/test_gui_qml_smoke.cpp) does.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

Popup {
    id: popup
    modal: true
    focus: true
    width: 420
    height: 640
    x: (parent ? parent.width - width : 0) / 2
    y: (parent ? parent.height - height : 0) / 2

    property var boardBridge: null
    property string taskId: "-1"
    property string taskTitle: ""

    /// The last attachment upload/download outcome, rendered at the foot of
    /// this popup. Both operations finish asynchronously and neither is
    /// self-evident from the list above: an upload's new row only appears
    /// after a second round trip (the bridge re-fetches), and a download
    /// writes its bytes to a path *outside* the app, so it has no visible
    /// effect here at all.
    property string status: ""

    /// This task's own comments -- boardBridge.board.comments filtered to
    /// the rows whose own taskId matches popup.taskId (a decimal string, per
    /// BoardView.qml's `taskPopup.taskId = String(task.id)`); every other
    /// task's comments on this same board are excluded.
    readonly property var comments: {
        if (!boardBridge || !boardBridge.board || !boardBridge.board.comments) {
            return []
        }
        const wantedTaskId = Number(popup.taskId)
        return boardBridge.board.comments.filter(function (c) {
            return c.taskId === wantedTaskId
        })
    }

    /// This task's own attachments -- unlike `comments` above, `boardBridge.
    /// attachments` is already scoped to whichever task `getAttachments()`
    /// was last called for (populated below, in `onOpened`), so no
    /// client-side filter is applied here.
    readonly property var attachments: (boardBridge && boardBridge.attachments) ? boardBridge.attachments : []

    /// Refreshes the attachment list every time this popup is shown for a
    /// (possibly different) task -- mirrors how `comments` recomputes
    /// automatically from `boardBridge.board`, except `attachments` has no
    /// board-wide list to filter client-side and needs its own explicit
    /// fetch per task.
    onOpened: {
        if (popup.boardBridge !== null) {
            popup.boardBridge.getAttachments(popup.taskId)
        }
    }

    /// The two attachment operations' completion signals. Deliberately no
    /// getAttachments() call in either handler: BoardBridge re-fetches the
    /// list itself after a successful upload (board_qml_bridge.cpp emits
    /// attachmentUploaded and then calls getAttachments on the same task), so
    /// re-fetching here would only duplicate that round trip. What the bridge
    /// does *not* provide is any confirmation to the user, which is all these
    /// handlers add.
    Connections {
        target: popup.boardBridge

        function onAttachmentUploaded(taskId) {
            popup.status = "attachment uploaded"
        }

        function onAttachmentDownloaded(localFilePath) {
            popup.status = "saved to " + localFilePath
        }
    }

    /// Picks a local file to upload as a new attachment on the open task.
    FileDialog {
        id: attachFileDialog
        fileMode: FileDialog.OpenFile
        onAccepted: {
            popup.boardBridge.uploadAttachment(popup.taskId, selectedFile)
        }
    }

    /// Picks where to save a downloaded attachment's bytes -- set by each
    /// download button's own onClicked below (see `downloadStorageKey`).
    FileDialog {
        id: saveAttachmentDialog
        fileMode: FileDialog.SaveFile
        property string storageKey: ""
        onAccepted: {
            popup.boardBridge.downloadAttachment(storageKey, selectedFile)
        }
    }

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

        Label {
            font.bold: true
            text: "Attachments"
        }

        ListView {
            Layout.fillWidth: true
            Layout.preferredHeight: 120
            clip: true
            model: popup.attachments

            delegate: RowLayout {
                id: attachmentRow
                required property var modelData
                width: ListView.view ? ListView.view.width : 0

                Label {
                    Layout.fillWidth: true
                    elide: Text.ElideMiddle
                    text: attachmentRow.modelData.filename
                }

                Button {
                    text: "Download"
                    onClicked: {
                        saveAttachmentDialog.storageKey = attachmentRow.modelData.storageKey
                        saveAttachmentDialog.open()
                    }
                }
            }
        }

        Button {
            Layout.fillWidth: true
            text: "Attach file..."
            enabled: popup.boardBridge !== null
            onClicked: attachFileDialog.open()
        }

        Label {
            Layout.fillWidth: true
            visible: popup.status !== ""
            wrapMode: Text.Wrap
            opacity: 0.7
            text: popup.status
        }

        Button {
            Layout.fillWidth: true
            text: "Close"
            onClicked: popup.close()
        }
    }
}

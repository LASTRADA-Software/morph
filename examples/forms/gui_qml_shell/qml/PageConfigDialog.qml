// SPDX-License-Identifier: Apache-2.0
//
// Modal dialog for managing the page tree: add folders, add/remove pages,
// rename nodes, toggle visibility. The tree is committed to disk on Save.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

Dialog {
    id: dialog
    title: "Manage Pages"
    modal: true
    standardButtons: Dialog.Save | Dialog.Cancel
    width: 520
    height: 480

    required property var controller
    readonly property var pageModel: controller.pageModel

    onAccepted: controller.saveConfig()
    onRejected: controller.loadConfig()  // revert changes

    // ── Layout ──────────────────────────────────────────────────────────────
    contentItem: ColumnLayout {
        RowLayout {
            Layout.fillWidth: true
            spacing: 4

            Action {
                id: addFolderAction
                text: "Folder"
                icon.name: "folder-new"
                onTriggered: {
                    var sel = treeView.currentIndex
                    var parentIdx = sel.valid && pageModel.isFolder(sel) ? sel : undefined
                    pageModel.addFolder(parentIdx, "New Folder")
                }
            }

            Action {
                id: addPageAction
                text: "Page"
                icon.name: "document-new"
                onTriggered: fileDialog.open()
            }

            Item { Layout.fillWidth: true }

Action {
                id: removeAction
                text: "Remove"
                icon.name: "edit-delete"
                enabled: treeView.currentIndex ? treeView.currentIndex.valid : false
                onTriggered: {
                    if (treeView.currentIndex && treeView.currentIndex.valid)
                        pageModel.removeNode(treeView.currentIndex)
                }
            }
        }

        // Tree view
        TreeView {
            id: treeView
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: pageModel
            clip: true
            selectionMode: TreeView.SelectionMode.SingleSelection

            delegate: TreeViewDelegate {
                id: delegate
                implicitHeight: 32
                indentation: 16

                contentItem: RowLayout {
                    spacing: 6

                    // Visibility toggle
                    CheckBox {
                        id: visCheck
                        checked: model.visible !== false
                        implicitWidth: 18
                        implicitHeight: 18
                        onToggled: {
                            var idx = pageModel.index(delegate.row, 0, treeView.parent(delegate.row))
                            pageModel.setNodeVisible(idx, checked)
                        }
                    }

                    // Name label
                    Label {
                        Layout.fillWidth: true
                        text: model.name ?? ""
                        color: delegate.selected ? "#FFFFFF" : "#222222"
                        font.pixelSize: 13
                        font.weight: model.nodeType === "folder" ? Font.DemiBold : Font.Normal
                        elide: Text.ElideRight
                    }

                    // Type badge
                    Label {
                        text: model.nodeType === "folder" ? "📁" : "📄"
                        font.pixelSize: 12
                        opacity: 0.5
                    }
                }

                background: Rectangle {
                    color: delegate.selected ? "#3A6EA5" : "transparent"
                }

                // Context menu
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: {
                        treeView.forceActiveFocus()
                        var idx = pageModel.index(delegate.row, 0, treeView.parent(delegate.row))
                        treeView.currentIndex = idx
                        contextMenu.popup()
                    }
                }
            }
        }

        Menu {
            id: contextMenu
            MenuItem {
                text: "Rename"
                onTriggered: renameDialog.open()
            }
            MenuItem {
                text: "Remove"
                onTriggered: {
                    if (treeView.currentIndex && treeView.currentIndex.valid)
                        pageModel.removeNode(treeView.currentIndex)
                }
            }
        }

        // Rename dialog
        Dialog {
            id: renameDialog
            title: "Rename"
            modal: true
            standardButtons: Dialog.Ok | Dialog.Cancel
            width: 320

            property string oldName: treeView.currentIndex && treeView.currentIndex.valid
                ? pageModel.data(treeView.currentIndex, 257) : ""  // NameRole

            contentItem: ColumnLayout {
                spacing: 8
                Label { text: "New name:" }
                TextField {
                    id: renameField
                    Layout.fillWidth: true
                    text: renameDialog.oldName
                    onAccepted: renameDialog.accept()
                }
            }

            onAccepted: {
                if (renameField.text.trim() !== "" && treeView.currentIndex.valid)
                    pageModel.renameNode(treeView.currentIndex, renameField.text.trim())
            }
        }
    }

    FileDialog {
        id: fileDialog
        title: "Select QML page"
        nameFilters: ["QML files (*.qml)", "All files (*)"]
        onAccepted: {
            var sel = treeView.currentIndex
            var parentIdx = sel.valid && pageModel.isFolder(sel) ? sel : undefined
            var fileName = selectedFile.toString().split("/").pop()
            if (fileName.endsWith(".qml"))
                fileName = fileName.slice(0, -4)
            pageModel.addPage(parentIdx, fileName, selectedFile.toString())
        }
    }
}
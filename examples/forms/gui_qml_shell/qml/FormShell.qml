// SPDX-License-Identifier: Apache-2.0
//
// Shell layout: tree sidebar on the left, dynamic page content on the right.
// The sidebar lists all visible pages from the PageTreeModel; clicking a leaf
// loads it into the content area. Built-in schema pages are wrapped in
// FormPage, user .qml files are loaded directly via Loader.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import MorphForms

RowLayout {
    id: shell
    spacing: 0

    required property var controller
    readonly property var pageModel: controller.pageModel
    property var schemas: ({})

    // Cache schemas JSON once
    Component.onCompleted: {
        try { schemas = JSON.parse(controller.schemasJson) } catch (e) {}
    }

    // Keep the tree model in sync when config is reloaded
    Connections {
        target: controller.pageModel
        function onTreeChanged() {
            // Trigger content re-evaluation if current selection is invalidated
        }
    }

    // ── Sidebar ─────────────────────────────────────────────────────────────
    Rectangle {
        Layout.preferredWidth: 280
        Layout.fillHeight: true
        color: "#1E1E1E"

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // Header
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 52
                color: "#2A2A2A"

                Label {
                    anchors.left: parent.left
                    anchors.leftMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: "morph shell"
                    color: "#FFFFFF"
                    font.pixelSize: 16
                    font.weight: Font.Bold
                }
            }

            // Toolbar: add page, manage pages
            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 8
                Layout.rightMargin: 8
                Layout.topMargin: 6
                Layout.bottomMargin: 6
                spacing: 4

                Button {
                    text: "+"
                    flat: true
                    implicitWidth: 32
                    implicitHeight: 28
                    onClicked: addPageDialog.open()
                }

                Label {
                    Layout.fillWidth: true
                    text: "Pages"
                    color: "#999999"
                    font.pixelSize: 11
                    font.capitalization: Font.AllUppercase
                }

                Button {
                    text: "⚙"
                    flat: true
                    implicitWidth: 32
                    implicitHeight: 28
                    onClicked: configDialog.open()
                }
            }

            // Tree view
            TreeView {
                id: treeView
                Layout.fillWidth: true
                Layout.fillHeight: true
                model: controller.pageModel
                clip: true
                selectionMode: TreeView.SelectionMode.SingleSelection

                delegate: TreeViewDelegate {
                    id: delegate
                    implicitHeight: 36
                    indentation: 16

                    contentItem: Item {
                        implicitHeight: delegate.implicitHeight

                        Label {
                            anchors.left: parent.left
                            anchors.leftMargin: 4
                            anchors.verticalCenter: parent.verticalCenter
                            text: model.name ?? ""
                            color: delegate.selected ? "#FFFFFF" : "#CCCCCC"
                            font.pixelSize: 13
                            font.weight: delegate.selected ? Font.DemiBold : Font.Normal
                            elide: Text.ElideRight
                        }
                    }

                    background: Rectangle {
                        color: delegate.selected ? "#3A3A3A"
                             : (delegate.row === undefined ? "transparent"
                                : treeView.hoveredRow === delegate.row ? "#2A2A2A"
                                : "transparent")
                    }

                    TapHandler {
                        onTapped: {
                            var idx = model.modelIndex
                            if (!idx.valid)
                                return
                            treeView.currentIndex = idx
                            if (!controller.pageModel.isFolder(idx)) {
                                treeView.forceActiveFocus()
                                contentLoader.loadPage(idx)
                            }
                        }
                    }
                }
            }

            // Footer
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: "#333333"
            }
        }
    }

    // ── Content area ────────────────────────────────────────────────────────
    Rectangle {
        Layout.fillWidth: true
        Layout.fillHeight: true
        color: "#F5F5F5"

        Loader {
            id: contentLoader
            anchors.fill: parent
            asynchronous: true

            property var currentSource: ""
            property var currentActionType: ""

            function loadPage(modelIndex) {
                if (!modelIndex.valid)
                    return
                var src = controller.pageModel.nodeSource(modelIndex)
                if (!src)
                    return

                if (src.startsWith("builtin://")) {
                    var actionType = src.substring("builtin://".length)
                    currentActionType = actionType
                    currentSource = ""

                    var comp = Qt.createComponent("FormPage.qml")
                    if (comp.status === Component.Ready || comp.status === Component.Loading) {
                        sourceComponent = comp
                        if (comp.status === Component.Ready)
                            applyFormPageProps()
                        else
                            comp.statusChanged.connect(applyFormPageProps)
                    }
                } else {
                    currentSource = src
                    currentActionType = ""
                    sourceComponent = undefined
                    source = src
                }
            }

            function applyFormPageProps() {
                if (item) {
                    item.actionType = contentLoader.currentActionType
                    var schema = shell.schemas[contentLoader.currentActionType]
                    item.schema = schema !== undefined ? schema : null
                    item.controller = shell.controller
                }
            }
        }
    }

    // ── Dialogs ─────────────────────────────────────────────────────────────

    FileDialog {
        id: addPageDialog
        title: "Add QML page"
        nameFilters: ["QML files (*.qml)", "All files (*)"]
        onAccepted: {
            var treeIdx = treeView.currentIndex.valid ? treeView.currentIndex : controller.pageModel.index(0, 0)
            var parentIdx = controller.pageModel.isFolder(treeIdx) ? treeIdx : undefined
            var fileName = selectedFile.toString().split("/").pop()
            if (fileName.endsWith(".qml"))
                fileName = fileName.slice(0, -4)
            controller.pageModel.addPage(parentIdx, fileName, selectedFile.toString())
            controller.saveConfig()
        }
    }

    PageConfigDialog {
        id: configDialog
        controller: shell.controller
    }
}
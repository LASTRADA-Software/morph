// SPDX-License-Identifier: Apache-2.0
//
// kanban's board screen, design spec §6: an outer vertical section per
// swimlane (only rendered as distinct sections when the board has more than
// one swimlane -- a single-swimlane board, the common case, renders as a
// flat column row with no swimlane chrome), each containing a horizontal
// row of columns. Each column delegate is a Rectangle with a header (name,
// "{count}" when wipLimit == 0, "{count}/{wipLimit}" otherwise) and a
// vertical ListView of task-card delegates.
//
// Drag-and-drop (§6.2): native Qt Quick Drag attached property + DropArea --
// no custom mouse-position tracking, no synthesized events. This is the one
// file in this rung with real logic (the drop-target/position computation),
// kept inside the laneDelegate component below rather than spread through
// the rest of the view, per the design spec's own instruction.
//
// One Repeater (laneModel below) serves both the multi-swimlane and the
// flat/no-swimlane-chrome case: laneModel is the real swimlanes list when
// there is more than one, or a single synthetic {id, name, showHeader:false}
// entry otherwise (swimlaneId -1 when the board has no swimlane of its own
// yet, matching tasksFor's own "-1 means accept any" convention below).
//
// `boardBridge`/`projectAdminBridge` default to null so this same file also
// loads with nothing wired up, which is exactly what the offscreen
// engine-load smoke test (tests/test_gui_qml_smoke.cpp) does.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: page

    property var boardBridge: null
    property var projectAdminBridge: null

    /// Emitted when the user wants to leave the board -- Main.qml stops
    /// polling and pops back to the project list.
    signal closeRequested()

    property string status: ""
    property bool statusIsError: false

    readonly property var board: boardBridge && boardBridge.board ? boardBridge.board : ({})
    readonly property var columns: page.board.columns ? page.board.columns : []
    readonly property var swimlanes: page.board.swimlanes ? page.board.swimlanes : []
    readonly property var tasks: page.board.tasks ? page.board.tasks : []
    readonly property var activity: boardBridge && boardBridge.activity ? boardBridge.activity : []

    /// One row per rendered swimlane section -- the real list when there is
    /// more than one swimlane, otherwise one synthetic entry with no header
    /// chrome (design spec §6.1's "flat column row with no swimlane chrome").
    readonly property var laneModel: page.swimlanes.length > 1
        ? page.swimlanes.map(function (lane) { return { id: lane.id, name: lane.name, showHeader: true } })
        : [{ id: page.swimlanes.length === 1 ? page.swimlanes[0].id : -1, name: "", showHeader: false }]

    /// Tasks in (columnId, swimlaneId), ordered by position -- feeds both
    /// each column delegate's own task list and the drop computation below.
    /// @param columnId   The column to filter to.
    /// @param swimlaneId The swimlane to filter to, or -1 to accept any
    ///                    (a board with no swimlane of its own yet).
    /// @return The matching tasks, ascending by position.
    function tasksFor(columnId, swimlaneId) {
        const rows = page.tasks.filter(function (t) {
            return t.columnId === columnId && (swimlaneId < 0 || t.swimlaneId === swimlaneId)
        })
        rows.sort(function (a, b) { return a.position - b.position })
        return rows
    }

    function report(message, isError) {
        page.status = message
        page.statusIsError = isError
    }

    function openTaskPopup(task) {
        taskPopup.taskId = String(task.id)
        taskPopup.taskTitle = task.title
        taskPopup.open()
    }

    Connections {
        target: page.boardBridge

        function onTaskMoved(taskId) {
            page.report("", false)
        }

        function onCommentAdded(taskId) {
            page.report("comment added", false)
        }

        function onPollingStopped(message) {
            page.report(message, true)
        }

        function onFailed(message) {
            page.report(message, true)
        }
    }

    TaskDetailPopup {
        id: taskPopup
        boardBridge: page.boardBridge
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 8

        RowLayout {
            Layout.fillWidth: true

            Button {
                text: "< Back"
                onClicked: page.closeRequested()
            }

            Label {
                font.bold: true
                elide: Text.ElideRight
                text: page.board.name ? page.board.name : "Board"
            }

            Item { Layout.fillWidth: true }

            Label {
                opacity: 0.7
                text: page.boardBridge && page.boardBridge.myRole !== "" ? "role: " + page.boardBridge.myRole : ""
            }
        }

        RowLayout {
            Layout.fillWidth: true

            TextField {
                id: newColumnName
                Layout.preferredWidth: 160
                placeholderText: "new column name"
            }

            SpinBox {
                id: newColumnWip
                from: 0
                to: 999
                value: 0
                editable: true
            }

            Button {
                text: "Add column"
                enabled: page.boardBridge !== null && newColumnName.text.length > 0
                onClicked: {
                    page.boardBridge.createColumn(newColumnName.text, newColumnWip.value)
                    newColumnName.text = ""
                    newColumnWip.value = 0
                }
            }

            TextField {
                id: newSwimlaneName
                Layout.preferredWidth: 160
                placeholderText: "new swimlane name"
            }

            Button {
                text: "Add swimlane"
                enabled: page.boardBridge !== null && newSwimlaneName.text.length > 0
                onClicked: {
                    page.boardBridge.createSwimlane(newSwimlaneName.text)
                    newSwimlaneName.text = ""
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: page.status !== ""
            wrapMode: Text.Wrap
            color: page.statusIsError ? "#d33" : palette.text
            text: page.status
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 8

            // ── Board area: one section per swimlane (or one flat row) ──
            Item {
                id: boardRoot
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 8

                    Repeater {
                        model: page.laneModel

                        delegate: ColumnLayout {
                            id: laneSection
                            required property var modelData
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            spacing: 4

                            Label {
                                visible: laneSection.modelData.showHeader
                                font.bold: true
                                text: laneSection.modelData.name
                            }

                            Row {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                spacing: 8

                                Repeater {
                                    model: page.columns

                                    delegate: Rectangle {
                                        id: columnDelegate
                                        required property var modelData
                                        width: 240
                                        height: laneSection.height - (laneSection.modelData.showHeader ? 24 : 0)
                                        border.width: 2
                                        border.color: dropArea.containsDrag
                                                      ? (columnDelegate.atWipLimit ? "#d33" : "steelblue")
                                                      : "transparent"
                                        color: palette.base

                                        readonly property var columnTasks:
                                            page.tasksFor(columnDelegate.modelData.id, laneSection.modelData.id)
                                        readonly property bool atWipLimit:
                                            columnDelegate.modelData.wipLimit > 0
                                            && columnDelegate.columnTasks.length >= columnDelegate.modelData.wipLimit

                                        ColumnLayout {
                                            anchors.fill: parent
                                            anchors.margins: 4
                                            spacing: 4

                                            Label {
                                                Layout.fillWidth: true
                                                font.bold: true
                                                elide: Text.ElideRight
                                                text: columnDelegate.modelData.name + "  ("
                                                      + (columnDelegate.modelData.wipLimit === 0
                                                         ? String(columnDelegate.columnTasks.length)
                                                         : columnDelegate.columnTasks.length + "/"
                                                           + columnDelegate.modelData.wipLimit)
                                                      + ")"
                                            }

                                            TextField {
                                                id: newTaskTitle
                                                Layout.fillWidth: true
                                                placeholderText: "new task"
                                                onAccepted: {
                                                    if (page.boardBridge && text.length > 0) {
                                                        page.boardBridge.createTask(
                                                            String(columnDelegate.modelData.id),
                                                            String(laneSection.modelData.id), text)
                                                        text = ""
                                                    }
                                                }
                                            }

                                            ListView {
                                                id: taskList
                                                Layout.fillWidth: true
                                                Layout.fillHeight: true
                                                clip: true
                                                model: columnDelegate.columnTasks

                                                delegate: Rectangle {
                                                    id: card
                                                    required property var modelData
                                                    width: taskList.width
                                                    height: 48
                                                    radius: 4
                                                    color: palette.alternateBase
                                                    border.width: 1
                                                    border.color: palette.mid

                                                    // Reparented to the
                                                    // board's root Item for
                                                    // the duration of the
                                                    // drag so it visually
                                                    // floats above the
                                                    // columns (§6.2 step 1).
                                                    Drag.active: dragHandler.active
                                                    Drag.dragType: Drag.Internal
                                                    Drag.hotSpot.x: width / 2
                                                    Drag.hotSpot.y: height / 2

                                                    DragHandler {
                                                        id: dragHandler
                                                        target: card

                                                        onActiveChanged: {
                                                            if (active) {
                                                                const inBoard = card.mapToItem(
                                                                    boardRoot, 0, 0)
                                                                card.parent = boardRoot
                                                                card.x = inBoard.x
                                                                card.y = inBoard.y
                                                            } else {
                                                                card.Drag.drop()
                                                                card.parent = taskList.contentItem
                                                            }
                                                        }
                                                    }

                                                    TapHandler {
                                                        onTapped: {
                                                            if (!dragHandler.active) {
                                                                page.openTaskPopup(card.modelData)
                                                            }
                                                        }
                                                    }

                                                    Label {
                                                        anchors.fill: parent
                                                        anchors.margins: 6
                                                        elide: Text.ElideRight
                                                        text: card.modelData.title
                                                    }
                                                }
                                            }
                                        }

                                        DropArea {
                                            id: dropArea
                                            anchors.fill: parent

                                            onDropped: (drop) => {
                                                // §6.2 step 3: destination
                                                // column/swimlane come from
                                                // this DropArea; destination
                                                // position is the nearest
                                                // index within the
                                                // destination list to the
                                                // drop's own y.
                                                const destTasks = columnDelegate.columnTasks
                                                const dropY = drop.y
                                                let position = destTasks.length
                                                for (let i = 0; i < destTasks.length; ++i) {
                                                    if (dropY < (i + 0.5) * 48) {
                                                        position = i
                                                        break
                                                    }
                                                }
                                                if (page.boardBridge && drop.source
                                                    && drop.source.modelData) {
                                                    page.boardBridge.moveTask(
                                                        String(drop.source.modelData.id),
                                                        String(columnDelegate.modelData.id),
                                                        String(laneSection.modelData.id),
                                                        position)
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // ── Activity panel: GetActivity's stream, refreshed on the same
            //    poll tick as the board (design spec §7) ──────────────────
            ColumnLayout {
                Layout.preferredWidth: 280
                Layout.fillHeight: true
                spacing: 4

                Label {
                    font.bold: true
                    text: "Activity"
                }

                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: page.activity

                    delegate: ColumnLayout {
                        id: row
                        required property var modelData
                        width: ListView.view ? ListView.view.width : 0

                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            font.pixelSize: 12
                            text: row.modelData.summary
                        }

                        Label {
                            Layout.fillWidth: true
                            opacity: 0.6
                            font.pixelSize: 10
                            text: row.modelData.principal + " · " + row.modelData.actionType
                        }
                    }
                }
            }
        }
    }
}

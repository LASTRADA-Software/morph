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
// Every form on this screen is schema-driven: `CreateColumn`,
// `CreateSwimlane` and `CreateTask` are rendered from
// `morph::forms::schemaJson<A>()` through the shipped MorphForms DynamicForm,
// each with the renderer's own explicit Submit button
// (examples/IMPLEMENTATION.md rule 2, "schema-driven forms only"). There is no
// hand-written TextField, SpinBox or submit Button left on this screen.
//
// `CreateTask` gets one form instance per column delegate, because its
// `columnId`/`swimlaneId` are context rather than input: a task is created
// into the list the user is typing in. Both are declared `hidden` in the DTO's
// fieldMetadata (kanban/dto/board_dto.hpp) and fed through `setFieldValue`
// from the delegate that owns the form, which is the only way a hidden
// required field is ever engaged.
//
// Drag-and-drop (§6.2): native Qt Quick Drag attached property + DropArea --
// no custom mouse-position tracking, no synthesized events. This is the one
// file in this rung with real logic (the drop-target/position computation),
// kept inside the laneDelegate component below rather than spread through
// the rest of the view, per the design spec's own instruction.
//
// The drag-and-drop board is also this rung's one input that stays hand-built
// under rule 2's justification (a): `MoveTaskPosition` is a gesture, not a
// form somebody fills in. A schema-rendered version of it would be four number
// fields and a Submit button in place of dragging a card -- see the rung
// README's "morph subsystems exercised" section for the written justification
// and for why this is not a forms-subsystem gap.
//
// One Repeater (laneModel below) serves both the multi-swimlane and the
// flat/no-swimlane-chrome case: laneModel is the real swimlanes list when
// there is more than one, or a single synthetic {id, name, showHeader:false}
// entry otherwise (swimlaneId -1 when the board has no swimlane of its own
// yet, matching tasksFor's own "-1 means accept any" convention below).
//
// A "Rules" header button opens RulesView.qml (automation rules, design
// spec §11/Task 14's backend) in a Popup -- same "open on demand, board
// stays visible underneath" mechanism as taskPopup below, just wrapping a
// form-plus-list view instead of a single task's comments. Rules are
// board-scoped (a rule's triggerColumnId picker needs the open board's own
// board.columns), which is why this entry point lives here rather than
// alongside MembersView in ProjectListView.qml. That popup owns the whole
// lifecycle of the rules list: it fetches on open, and the Connections block
// below re-fetches after each mutation, because nothing else does.
//
// `boardBridge`/`projectAdminBridge` default to null so this same file also
// loads with nothing wired up, which is exactly what the offscreen
// engine-load smoke test (tests/test_gui_qml_smoke.cpp) does.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MorphForms

Item {
    id: page

    property var boardBridge: null
    property var projectAdminBridge: null

    /// The bridge's schema document, parsed once. `({})` while unwired, which
    /// is what the offscreen engine-load smoke test loads.
    readonly property var schemas: page.boardBridge === null ? ({}) : JSON.parse(page.boardBridge.schemasJson)

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

        // Automation rules are on-demand state, and neither mutation returns
        // the new listing: BoardPresenter::createRule/deleteRule resolve to a
        // bare ruleCreated/ruleDeleted (board_presenter.cpp) and nothing else
        // re-lists. So the refresh has to happen here -- exactly as
        // ProjectListView.qml's own onProjectCreated re-calls
        // refreshProjects() for the same reason.
        function onRuleCreated() {
            page.report("rule added", false)
            page.boardBridge.getRules()
        }

        function onRuleDeleted() {
            page.report("rule removed", false)
            page.boardBridge.getRules()
        }

        function onPollingStopped(message) {
            page.report(message, true)
        }

        function onFailed(message) {
            page.report(message, true)
        }

        // The schema renderer's own reply channel. A successful board form
        // already refreshes the board (BoardPresenter::submitForm re-emits
        // boardOpened with the rebuilt state), so this only has to report a
        // failure and clear the form that produced it.
        //
        // `createTaskForms` is a list because CreateTask has one form instance
        // per column: nothing in the reply says which of them submitted, so
        // they all reset. That clears a title half-typed in a *different*
        // column, which is the honest cost of one form per column -- and the
        // alternative (a single board-level form with a column picker) is a
        // worse screen, not a better one.
        function onReplyReceived(actionType, ok, payload) {
            if (!ok) {
                page.report(payload, true)
                return
            }
            if (actionType === "CreateColumn") {
                createColumnForm.resetFields()
            } else if (actionType === "CreateSwimlane") {
                createSwimlaneForm.resetFields()
            } else if (actionType === "CreateTask") {
                for (let i = 0; i < page.createTaskForms.length; ++i)
                    page.createTaskForms[i].rebind()
            }
        }
    }

    /// Every live CreateTask form, one per rendered column delegate. Registered
    /// by each delegate on creation and dropped on destruction, so the reply
    /// handler above has something to reset without reaching into the two
    /// nested Repeaters' item trees.
    property var createTaskForms: []

    function registerTaskForm(form) {
        const next = page.createTaskForms.slice()
        next.push(form)
        page.createTaskForms = next
    }

    function unregisterTaskForm(form) {
        page.createTaskForms = page.createTaskForms.filter(function (f) { return f !== form })
    }

    TaskDetailPopup {
        id: taskPopup
        objectName: "taskDetailPopup"
        boardBridge: page.boardBridge
    }

    // Automation-rules management (RulesView.qml): rules are board-scoped
    // (a rule's triggerColumnId picker needs the open board's own
    // board.columns), so this popup lives here rather than in
    // ProjectListView.qml alongside MembersView -- same "open on demand,
    // board stays visible underneath" mechanism as taskPopup above, just
    // sized for a form-plus-list view rather than a single task's comments.
    Popup {
        id: rulesPopup
        modal: true
        focus: true
        width: 520
        height: 420
        x: (parent ? parent.width - width : 0) / 2
        y: (parent ? parent.height - height : 0) / 2

        /// Fetches the rule list every time this popup is shown. `rules` is
        /// on-demand state (BoardBridge.getRules), not part of the polled
        /// `board` snapshot RulesView's sibling bindings read, so without this
        /// the pane renders whatever the last fetch left behind -- which,
        /// before any fetch has ever run, is nothing at all. Mirrors
        /// TaskDetailPopup.qml's identical `onOpened: getAttachments(taskId)`.
        onOpened: {
            if (page.boardBridge !== null) {
                page.boardBridge.getRules()
            }
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 8

            RulesView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                boardBridge: page.boardBridge
            }

            Button {
                Layout.fillWidth: true
                text: "Close"
                onClicked: rulesPopup.close()
            }
        }
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

            Button {
                text: "Rules"
                onClicked: rulesPopup.open()
            }

            Label {
                opacity: 0.7
                text: page.boardBridge && page.boardBridge.myRole !== "" ? "role: " + page.boardBridge.myRole : ""
            }
        }

        // Task 6: dead-letter indicator -- README's DoD names this exact
        // wording ("N changes could not be synced"). Bound directly to
        // boardBridge.deadLetterCount (a Q_PROPERTY backed by
        // syncStatusChanged), so it appears the moment SyncWorker's 5-attempt
        // cap drops a queued move and disappears again if that count is
        // ever reset (e.g. a fresh BoardBridge). Absent from a
        // MORPH_BUILD_OFFLINE_SQLITE=OFF build, where boardBridge simply has
        // no such property and this binding's guard keeps it hidden.
        Label {
            Layout.fillWidth: true
            visible: page.boardBridge !== null && page.boardBridge.deadLetterCount !== undefined
                     && page.boardBridge.deadLetterCount > 0
            wrapMode: Text.Wrap
            color: "#d33"
            font.bold: true
            text: visible ? "%1 changes could not be synced".arg(page.boardBridge.deadLetterCount) : ""
        }

        // Pending-sync indicator, next to the dead-letter banner above: the
        // dead-letter banner reports moves SyncWorker gave up on, but nothing
        // showed moves still waiting to replay. Bound directly to
        // boardBridge.queueDepth (a Q_PROPERTY backed by the same
        // syncStatusChanged signal), so it tracks the offline queue live.
        // Absent from a MORPH_BUILD_OFFLINE_SQLITE=OFF build, where
        // boardBridge has no such property and this binding's guard keeps it
        // hidden.
        Label {
            Layout.fillWidth: true
            visible: page.boardBridge !== null && page.boardBridge.queueDepth !== undefined
                     && page.boardBridge.queueDepth > 0
            wrapMode: Text.Wrap
            opacity: 0.8
            text: visible ? "%1 changes pending sync".arg(page.boardBridge.queueDepth) : ""
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 16

            DynamicForm {
                id: createColumnForm
                objectName: "createColumnForm"
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                actionType: "CreateColumn"
                schema: page.schemas["CreateColumn"] || ({})
                // Bound, and safe to bind: `CreateColumn` declares
                // `explicitSubmit = true` (kanban/dto/board_dto.hpp), so its
                // schema carries `"x-submitMode": "explicit"` and the renderer
                // draws its own Submit button rather than firing the moment
                // `name` is non-empty (docs/spec/forms/forms.md, "Explicit
                // submit mode"). `wipLimit` is in the same DTO's
                // `optionalFields`, so leaving it blank submits no key at all
                // and the model's own default -- 0, "unlimited" -- applies.
                controller: page.boardBridge
            }

            DynamicForm {
                id: createSwimlaneForm
                objectName: "createSwimlaneForm"
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                actionType: "CreateSwimlane"
                schema: page.schemas["CreateSwimlane"] || ({})
                controller: page.boardBridge
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

                                            DynamicForm {
                                                id: createTaskForm
                                                objectName: "createTaskForm_"
                                                            + columnDelegate.modelData.id
                                                Layout.fillWidth: true
                                                actionType: "CreateTask"
                                                schema: page.schemas["CreateTask"] || ({})
                                                controller: page.boardBridge

                                                // The two hidden context
                                                // fields (see this file's
                                                // header comment). Seeded on
                                                // creation and re-seeded after
                                                // every successful submit,
                                                // since resetFields() clears
                                                // hidden fields too.
                                                function bindContext() {
                                                    setFieldValue("columnId",
                                                                  String(columnDelegate.modelData.id))
                                                    setFieldValue("swimlaneId",
                                                                  String(laneSection.modelData.id))
                                                }

                                                function rebind() {
                                                    resetFields()
                                                    bindContext()
                                                }

                                                Component.onCompleted: {
                                                    createTaskForm.bindContext()
                                                    page.registerTaskForm(createTaskForm)
                                                }
                                                Component.onDestruction: page.unregisterTaskForm(createTaskForm)
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

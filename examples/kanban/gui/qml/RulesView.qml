// SPDX-License-Identifier: Apache-2.0
//
// kanban's automation-rules management view -- structurally identical to
// MembersView.qml: a flat ListView over getRules' RuleView{id,
// triggerColumnId, mutationType, mutationValue} rows (BoardBridge.rules),
// each row a column-name label, the mutation description, and a remove
// button calling deleteRule(id). Creating a rule is a schema-driven
// `CreateRule` form (`morph::forms::schemaJson<CreateRule>()` through the
// shipped MorphForms DynamicForm) -- no "watch for other trigger kinds"
// affordance, since RuleTriggerEvent has exactly one member (rule_dto.hpp).
//
// Both of this rung's remaining hand-built forms converted together
// (morph#393): `CreateRule::mutationType` (a `RuleMutationType` enum class)
// now renders as the schema-driven combo box DynamicForm draws for a closed
// `oneOf`-of-`const`s set (morph#386 closed the gap that used to force this
// to a free-text field), and `CreateRule::triggerColumnId` moved from a raw
// `ColumnId` to a `morph::forms::Choice<…, "GetBoardState">` (rule 3's shape
// for a user-chosen foreign key) -- so the trigger-column combo box is now
// server-fetched by DynamicForm itself, via BoardBridge.fetchOptions(), rather
// than reading BoardBridge.board.columns by hand.
//
// `boardBridge` defaults to null so this same file also loads standalone
// with nothing wired up, matching MembersView.qml's identical convention.
// Reached from BoardView.qml's "Rules" header button, which opens this view
// inside a Popup -- see BoardView.qml's own header comment.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MorphForms

ColumnLayout {
    id: page
    spacing: 6

    property var boardBridge: null

    /// The bridge's schema document, parsed once. `({})` while unwired, which
    /// is what the smoke test loads. Mirrors BoardView.qml's identical
    /// property; this view has its own copy rather than threading one down
    /// from the parent, matching MembersView.qml's own standalone-load
    /// convention.
    readonly property var schemas: page.boardBridge === null ? ({}) : JSON.parse(page.boardBridge.schemasJson)

    /// The open board's own columns (BoardBridge.board.columns), used only to
    /// resolve a rule row's triggerColumnId back to a display name below --
    /// the trigger-column *picker* in the add-rule form is fetched by
    /// DynamicForm itself via GetBoardState, not read from here.
    readonly property var columns: page.boardBridge && page.boardBridge.board && page.boardBridge.board.columns
        ? page.boardBridge.board.columns : []

    /// @brief The column name for @p columnId, or the id itself if the
    ///        column is no longer in `columns` (e.g. deleted after the rule
    ///        was created).
    /// @param columnId The rule row's own `triggerColumnId`.
    /// @return The matching column's `name`, or `columnId` as a string.
    function columnName(columnId) {
        for (let i = 0; i < page.columns.length; ++i) {
            if (page.columns[i].id === columnId)
                return page.columns[i].name
        }
        return String(columnId)
    }

    Label {
        font.bold: true
        text: "Automation rules"
    }

    ListView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        model: page.boardBridge ? page.boardBridge.rules : []

        delegate: RowLayout {
            id: row
            required property var modelData
            width: ListView.view ? ListView.view.width : 0

            Label {
                Layout.fillWidth: true
                elide: Text.ElideRight
                text: "when moved to \"" + page.columnName(row.modelData.triggerColumnId) + "\": "
                      + row.modelData.mutationType + " \"" + row.modelData.mutationValue + "\""
            }

            Button {
                text: "Remove"
                onClicked: {
                    if (page.boardBridge)
                        page.boardBridge.deleteRule(String(row.modelData.id))
                }
            }
        }
    }

    DynamicForm {
        id: createRuleForm
        objectName: "createRuleForm"
        Layout.fillWidth: true
        actionType: "CreateRule"
        schema: page.schemas["CreateRule"] || ({})
        controller: page.boardBridge

        // `projectId` is the one hidden context field (see rule_dto.hpp's own
        // comment on it). Seeded on creation and re-seeded after every
        // successful submit, since resetFields() clears hidden fields too --
        // same discipline BoardView.qml's createTaskForm delegate documents
        // for CreateTask's own hidden columnId/swimlaneId. triggerColumnId
        // needs no seeding: it is a Choice, and DynamicForm fetches its own
        // options via controller.fetchOptions("GetBoardState", "{}").
        function bindContext() {
            if (page.boardBridge && page.boardBridge.board)
                setFieldValue("projectId", String(page.boardBridge.board.projectId))
        }

        function rebind() {
            resetFields()
            bindContext()
        }

        Component.onCompleted: bindContext()
    }

    // `page.boardBridge.board` changes (a different board opened, or the same
    // board's state refreshing) while this popup's DynamicForm instance stays
    // alive -- BoardView.qml's rulesPopup is created once and reused, not
    // recreated per board -- so the hidden field needs an explicit re-seed.
    Connections {
        target: page.boardBridge
        enabled: page.boardBridge !== null

        function onBoardChanged() {
            createRuleForm.bindContext()
        }

        function onReplyReceived(actionType, ok, payload) {
            if (actionType === "CreateRule" && ok) {
                createRuleForm.rebind()
            }
        }
    }
}

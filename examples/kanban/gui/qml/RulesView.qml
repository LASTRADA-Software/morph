// SPDX-License-Identifier: Apache-2.0
//
// kanban's automation-rules management view -- structurally identical to
// MembersView.qml (Phase 1): a flat ListView over getRules' RuleView{id,
// triggerColumnId, mutationType, mutationValue} rows (BoardBridge.rules),
// each row a column-name label, the mutation description, and a remove
// button calling deleteRule(id). Creating a rule is a column picker (from
// BoardBridge.board.columns -- reused, not a new property, per this task's
// own brief) + a mutation-type picker (AddTag/RemoveTag) + a tag-name text
// field, calling createRule(triggerColumnId, mutationType, mutationValue)
// directly -- no "watch for other trigger kinds" affordance, since
// RuleTriggerEvent has exactly one member (rule_dto.hpp).
//
// This is the second of the two screens whose inputs stay hand-built under
// examples/IMPLEMENTATION.md rule 2's justification (a), for the same filed
// gap as MembersView.qml (morph#386): `CreateRule::mutationType` is a
// `RuleMutationType` enum class, and the shipped DynamicForm renders the
// closed `oneOf`-of-`const`s schemaJson emits for it as a plain free-text
// field -- measured: TextField(objectName "field_mutationType"),
// ready == true for mutationType = "Explode", and
// {"mutationType":"Explode"} submitted. The trigger-column picker below is
// *not* part of that gap: `triggerColumnId` is a foreign key chosen by a user,
// which rule 3 already says should be a
// `morph::forms::Choice<..., "GetBoardState">` rather than a raw id, and
// GetBoardState is a registered action returning `columns` as its first array
// member. Converting this view is therefore one enum fix plus one DTO field
// type -- neither of which belongs in the change that converted the other five
// forms.
//
// `boardBridge` defaults to null so this same file also loads standalone
// with nothing wired up, matching MembersView.qml's identical convention.
// Reached from BoardView.qml's "Rules" header button, which opens this view
// inside a Popup (board-scoped, since the trigger-column picker needs the
// open board's own board.columns) -- see BoardView.qml's own header comment.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: page
    spacing: 6

    property var boardBridge: null

    readonly property var mutationTypeNames: ["AddTag", "RemoveTag"]

    /// The open board's own columns (BoardBridge.board.columns), reused here
    /// as the trigger-column picker's model rather than adding a new
    /// property just for this view.
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

    RowLayout {
        Layout.fillWidth: true

        ComboBox {
            id: newTriggerColumn
            Layout.preferredWidth: 160
            textRole: "name"
            valueRole: "id"
            model: page.columns
        }

        ComboBox {
            id: newMutationType
            model: page.mutationTypeNames
            currentIndex: 0
        }

        TextField {
            id: newMutationValue
            Layout.fillWidth: true
            placeholderText: "tag name"
        }

        Button {
            text: "Add rule"
            enabled: page.boardBridge !== null && page.columns.length > 0 && newMutationValue.text.length > 0
            onClicked: {
                page.boardBridge.createRule(String(newTriggerColumn.currentValue),
                                             page.mutationTypeNames[newMutationType.currentIndex],
                                             newMutationValue.text)
                newMutationValue.text = ""
            }
        }
    }
}

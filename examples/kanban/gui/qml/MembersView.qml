// SPDX-License-Identifier: Apache-2.0
//
// kanban's member-management view, design spec §8: a flat ListView over
// GetProjectRoles' MemberRole{principal, role} rows (ProjectAdminBridge.roles),
// each row a principal label, a role ComboBox (Viewer/Member/Manager) calling
// setMemberRole(principal, role) on selection change, and a remove button
// calling removeMember(principal). Adding a member is a schema-driven
// `SetMemberRole` form (`morph::forms::schemaJson<SetMemberRole>()` through
// the shipped MorphForms DynamicForm) -- there is no "add member by search,"
// per the minimal-bootstrap scope decision (design spec §1).
//
// The per-row role ComboBox above stays hand-built: it edits one field of an
// existing row in place, on selection change, with no Submit gesture and no
// draft -- not the create-a-new-thing shape DynamicForm renders. Converting
// it would mean a form dialog per row, not a control swap, which is a
// different and worse interaction than the inline picker it would replace.
// This is examples/IMPLEMENTATION.md rule 2's justification (a) on the
// row-editing interaction, the same grounds BoardView.qml's header comment
// gives for why MoveTaskPosition's drag gesture stays hand-built -- not the
// enum-rendering gap morph#386 fixed, which this file no longer has (the "add
// member" row's role picker is now the schema-driven combo box DynamicForm
// draws for `SetMemberRole::role`'s closed set). See morph#393.
//
// `projectAdminBridge` defaults to null and `projectId` defaults to -1 so
// this same file also loads standalone with nothing wired up, which is
// exactly what the offscreen engine-load smoke test
// (tests/test_gui_qml_smoke.cpp) does.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MorphForms

ColumnLayout {
    id: page
    spacing: 6

    property var projectAdminBridge: null
    property int projectId: -1
    property string projectName: ""

    /// The bridge's schema document, parsed once. `({})` while unwired, which
    /// is what the smoke test loads. Mirrors ProjectListView.qml's identical
    /// property; this view has its own copy rather than threading one down
    /// from the parent, matching this file's existing standalone-load
    /// convention (`projectAdminBridge`/`projectId` above).
    readonly property var schemas: page.projectAdminBridge === null
                                   ? ({})
                                   : JSON.parse(page.projectAdminBridge.schemasJson)

    readonly property var roleNames: ["Viewer", "Member", "Manager"]

    Label {
        font.bold: true
        elide: Text.ElideRight
        text: page.projectName !== "" ? "Members of " + page.projectName : "Members"
    }

    ListView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        model: page.projectAdminBridge ? page.projectAdminBridge.roles : []

        delegate: RowLayout {
            id: row
            required property var modelData
            width: ListView.view ? ListView.view.width : 0

            Label {
                Layout.fillWidth: true
                elide: Text.ElideRight
                text: row.modelData.principal
            }

            ComboBox {
                id: roleBox
                model: page.roleNames
                currentIndex: page.roleNames.indexOf(row.modelData.role)

                onActivated: (index) => {
                    if (page.projectAdminBridge && page.projectId >= 0)
                        page.projectAdminBridge.setMemberRole(page.projectId, row.modelData.principal,
                                                               page.roleNames[index])
                }
            }

            Button {
                text: "Remove"
                onClicked: {
                    if (page.projectAdminBridge && page.projectId >= 0)
                        page.projectAdminBridge.removeMember(page.projectId, row.modelData.principal)
                }
            }
        }
    }

    DynamicForm {
        id: addMemberForm
        objectName: "addMemberForm"
        Layout.fillWidth: true
        actionType: "SetMemberRole"
        schema: page.schemas["SetMemberRole"] || ({})
        controller: page.projectAdminBridge

        // The one hidden context field (see project_dto.hpp's own comment on
        // it). Seeded on creation and re-seeded after every successful
        // submit, since resetFields() clears hidden fields too -- same
        // discipline BoardView.qml's createTaskForm delegate documents for
        // CreateTask's own hidden columnId/swimlaneId.
        function bindContext() {
            setFieldValue("projectId", String(page.projectId))
        }

        function rebind() {
            resetFields()
            bindContext()
        }

        Component.onCompleted: bindContext()
    }

    // `page.projectId` changes while this view's DynamicForm instance stays
    // alive -- ProjectListView.qml keeps one MembersView and swaps which
    // project it shows (`membersProjectId`) rather than recreating the view
    // per selection -- so the hidden field needs an explicit re-seed here,
    // not just the Component.onCompleted above.
    onProjectIdChanged: addMemberForm.bindContext()

    Connections {
        target: page.projectAdminBridge
        enabled: page.projectAdminBridge !== null

        function onReplyReceived(actionType, ok, payload) {
            if (actionType === "SetMemberRole" && ok) {
                addMemberForm.rebind()
            }
        }
    }
}

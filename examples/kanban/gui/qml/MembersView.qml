// SPDX-License-Identifier: Apache-2.0
//
// kanban's member-management view, design spec §8: a flat ListView over
// GetProjectRoles' MemberRole{principal, role} rows (ProjectAdminBridge.roles),
// each row a principal label, a role ComboBox (Viewer/Member/Manager) calling
// setMemberRole(principal, role) on selection change, and a remove button
// calling removeMember(principal). Adding a member is a text field
// (principal) + role picker, calling setMemberRole directly -- there is no
// "add member by search," per the minimal-bootstrap scope decision (design
// spec §1).
//
// `projectAdminBridge` defaults to null and `projectId` defaults to -1 so
// this same file also loads standalone with nothing wired up, which is
// exactly what the offscreen engine-load smoke test
// (tests/test_gui_qml_smoke.cpp) does.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: page
    spacing: 6

    property var projectAdminBridge: null
    property int projectId: -1
    property string projectName: ""

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

    RowLayout {
        Layout.fillWidth: true

        TextField {
            id: newPrincipal
            Layout.fillWidth: true
            placeholderText: "principal to add"
        }

        ComboBox {
            id: newRole
            model: page.roleNames
            currentIndex: 1
        }

        Button {
            text: "Add"
            enabled: page.projectAdminBridge !== null && page.projectId >= 0 && newPrincipal.text.length > 0
            onClicked: {
                page.projectAdminBridge.setMemberRole(page.projectId, newPrincipal.text,
                                                       page.roleNames[newRole.currentIndex])
                newPrincipal.text = ""
            }
        }
    }
}

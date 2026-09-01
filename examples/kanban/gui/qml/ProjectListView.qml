// SPDX-License-Identifier: Apache-2.0
//
// kanban's post-login screen: the caller's own projects (GetMyProjects, via
// ProjectAdminBridge.projects), a "create project" affordance, and a
// per-project "manage members" affordance. Tapping a project row opens its
// board -- see this file's own projectOpened signal, which Main.qml uses to
// push BoardView and to hand BoardBridge the tapped project's own myRole
// (design spec's own integration point: BoardBridge.myRole has no backing
// action, so whatever opens a board must set it from this bridge's own
// {id, name, myRole} rows).
//
// "Create project" is a schema-driven form: `CreateProject` is rendered from
// `morph::forms::schemaJson<CreateProject>()` through the shipped MorphForms
// `DynamicForm`, with the renderer's own explicit Submit button
// (examples/IMPLEMENTATION.md rule 2, "schema-driven forms only"). There is no
// hand-written field and no hand-written create button here any more.
//
// The member pane it sits beside (MembersView.qml) is still hand-built, under
// rule 2's justification (a) — see that file's own header comment and the rung
// README's "morph subsystems exercised" section.
//
// `projectAdminBridge`/`boardBridge` default to null so this same file also
// loads with nothing wired up, which is exactly what the offscreen
// engine-load smoke test (tests/test_gui_qml_smoke.cpp) does — and why
// `schemas` is read defensively below rather than assuming a live bridge.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MorphForms

Item {
    id: page

    property var projectAdminBridge: null
    property var boardBridge: null

    /// Emitted when a project row is tapped -- Main.qml opens that project's
    /// board and pushes BoardView.
    /// @param projectId The tapped project's id, as its plain number string.
    /// @param myRole    The caller's own role on that project ("Viewer"/
    ///                  "Member"/"Manager"), read from this bridge's own
    ///                  {id, name, myRole} row -- see this file's header
    ///                  comment.
    signal projectOpened(string projectId, string myRole)

    property string status: ""
    property bool statusIsError: false

    /// The bridge's schema document, parsed once. `({})` while unwired, which
    /// is what the smoke test loads.
    readonly property var schemas: page.projectAdminBridge === null
                                   ? ({})
                                   : JSON.parse(page.projectAdminBridge.schemasJson)

    /// The project currently shown in the members panel, or -1 (none open).
    property int membersProjectId: -1
    property string membersProjectName: ""

    function report(message, isError) {
        page.status = message
        page.statusIsError = isError
    }

    // The first listing cannot simply be requested once on completion --
    // see BookmarkListView.qml's identical `onBound` note (bookmarks' own
    // rung) for the full rationale: a Remote-mode BridgeHandler's
    // registration is a round trip, and `bound` (backed by Bridge::
    // whenBound()) is this bridge's own settlement signal for it. Local
    // mode's handler is already bound by construction, so this fires
    // synchronously there.
    Connections {
        target: page.projectAdminBridge

        function onBound() {
            page.projectAdminBridge.refreshProjects()
        }

        function onProjectsListed(projects) {
            page.report("", false)
        }

        function onProjectCreated(id, name) {
            page.report("created project \"" + name + "\"", false)
            page.projectAdminBridge.refreshProjects()
        }

        // The schema renderer's own reply channel, which onProjectCreated
        // above does not replace: it fires on failures too (where no project
        // was created and nothing else would say so), and a submitted form
        // has to be cleared or the next create starts with the last name
        // still in the field. Same shape as bookmarks' BookmarkListView.qml.
        function onReplyReceived(actionType, ok, payload) {
            if (actionType !== "CreateProject")
                return
            if (ok)
                createProjectForm.resetFields()
            else
                page.report(payload, true)
        }

        function onRolesListed(roles) {
            page.report("", false)
        }

        function onMemberRoleSet() {
            page.report("role updated", false)
            if (page.membersProjectId >= 0)
                page.projectAdminBridge.listRoles(page.membersProjectId)
        }

        function onMemberRemoved() {
            page.report("member removed", false)
            if (page.membersProjectId >= 0)
                page.projectAdminBridge.listRoles(page.membersProjectId)
        }

        function onFailed(message) {
            page.report(message, true)
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 8

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

            // ── Pane 1: the caller's own projects ───────────────────────
            ColumnLayout {
                Layout.preferredWidth: 420
                Layout.fillHeight: true
                spacing: 6

                Label {
                    font.bold: true
                    text: "Your projects"
                }

                DynamicForm {
                    id: createProjectForm
                    objectName: "createProjectForm"
                    Layout.fillWidth: true
                    actionType: "CreateProject"
                    schema: page.schemas["CreateProject"] || ({})
                    // Bound, and safe to bind: `CreateProject` declares
                    // `explicitSubmit = true` (kanban/dto/project_dto.hpp), so
                    // its schema carries `"x-submitMode": "explicit"` and the
                    // renderer never auto-submits — it draws its own Submit
                    // button gated on the same `ready` state
                    // (docs/spec/forms/forms.md, "Explicit submit mode").
                    controller: page.projectAdminBridge
                }

                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: page.projectAdminBridge ? page.projectAdminBridge.projects : []

                    delegate: ItemDelegate {
                        id: row
                        required property var modelData
                        width: ListView.view ? ListView.view.width : 0

                        contentItem: RowLayout {
                            Label {
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                text: row.modelData.name + "  ·  " + row.modelData.myRole
                            }

                            Button {
                                text: "Members"
                                onClicked: {
                                    page.membersProjectId = row.modelData.id
                                    page.membersProjectName = row.modelData.name
                                    page.projectAdminBridge.listRoles(row.modelData.id)
                                }
                            }
                        }

                        onClicked: page.projectOpened(String(row.modelData.id), row.modelData.myRole)
                    }
                }
            }

            // ── Pane 2: member management for the selected project ─────
            MembersView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: page.membersProjectId >= 0

                projectAdminBridge: page.projectAdminBridge
                projectId: page.membersProjectId
                projectName: page.membersProjectName
            }
        }
    }
}

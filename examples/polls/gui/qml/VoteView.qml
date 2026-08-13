// SPDX-License-Identifier: Apache-2.0
//
// The vote view: OpenPoll (on load) + SubmitVotes/UpdateVotes (hand-rolled —
// OneVote's `votes` array hits the same DynamicForm gap CreatePoll::options
// does, finding 031) + AddComment/FinalizePoll/UndoLastVoteChange (genuinely
// schema-driven, via DynamicForm) + the live, event-driven results display
// wired to Task 15's EventPoller (through PollBridge — see
// poll_qml_bridges.hpp's own doc comment for the wiring).
//
// `pollBridge`/`schemas` default to null/{} so this same file also loads
// standalone with nothing wired up, which is exactly what the offscreen
// engine-load smoke test (tests/test_gui_qml_smoke.cpp) does.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MorphForms

Item {
    id: page

    property var pollBridge: null
    property var schemas: ({})
    property string pollId: ""

    signal backRequested()

    property var state: null  // GetPollStateResult, as PollBridge's toVariantMap renders it
    property string participantName: ""
    property bool hasVoted: false
    property var activityLog: []  // [{id, kind, summary}], newest last

    property string status: ""
    property bool statusIsError: false

    function report(message, isError) {
        page.status = message
        page.statusIsError = isError
    }

    // One entry per currently-known option: {optionId, choice}. Rebuilt
    // whenever `state.options` changes so a newly-opened poll (or a
    // resync after a live event) always has a picker row per option, and a
    // prior selection survives a resync that didn't change the option list.
    property var picks: ({})

    function pickFor(optionId) {
        return page.picks[optionId] || "No"
    }

    function setPick(optionId, choice) {
        const next = Object.assign({}, page.picks)
        next[optionId] = choice
        page.picks = next
    }

    function votesPayload() {
        const out = []
        if (!page.state)
            return out
        for (let i = 0; i < page.state.options.length; ++i) {
            const optionId = page.state.options[i].id
            out.push({ optionId: optionId, choice: page.pickFor(optionId) })
        }
        return out
    }

    Component.onCompleted: {
        if (page.pollBridge && page.pollId !== "")
            page.pollBridge.openPoll(page.pollId)
    }

    Connections {
        target: page.pollBridge

        function onOpened(newState) {
            page.state = newState
            page.hasVoted = false
            page.activityLog = []
            page.report("", false)
        }

        function onStateChanged(newState) {
            page.state = newState
        }

        function onEventReceived(event) {
            // Newest last, capped so a long-lived open poll does not grow
            // this list without bound — the live tallies (state.options)
            // are the source of truth; this is a human-readable log only.
            const next = page.activityLog.concat([event])
            page.activityLog = next.length > 200 ? next.slice(next.length - 200) : next
        }

        function onReplyReceived(actionType, ok, payload) {
            if (!ok) {
                page.report(actionType + ": " + payload, true)
                return
            }
            page.report(actionType + " ok", false)
            if (actionType === "AddComment")
                commentForm.resetFields()
            else if (actionType === "FinalizePoll")
                finalizeForm.resetFields()
            else if (actionType === "UndoLastVoteChange")
                undoForm.resetFields()
            page.pollBridge.refresh()
        }

        function onPollingStopped(message) {
            page.report("live updates stopped: " + message, true)
        }

        function onFailed(message) {
            page.report(message, true)
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
                onClicked: page.backRequested()
            }
            Label {
                Layout.fillWidth: true
                font.bold: true
                elide: Text.ElideRight
                text: page.state ? (page.state.title + (page.state.finalized ? "  (finalized)" : "")) : "opening…"
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

            // ── Pane 1: results + the vote picker ──────────────────────────
            ColumnLayout {
                Layout.preferredWidth: 380
                Layout.fillHeight: true
                spacing: 6

                Label { text: "Your name" }
                TextField {
                    Layout.fillWidth: true
                    placeholderText: "participant name"
                    onTextChanged: page.participantName = text
                }

                Label { font.bold: true; text: "Options" }

                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: page.state ? page.state.options : []

                    delegate: ColumnLayout {
                        id: optionRow
                        required property var modelData
                        width: ListView.view ? ListView.view.width : 0
                        spacing: 2

                        Label {
                            font.bold: true
                            text: optionRow.modelData.label + "  —  yes: " + optionRow.modelData.yesCount
                                  + "  if-need-be: " + optionRow.modelData.ifNeedBeCount
                                  + "  no: " + optionRow.modelData.noCount
                                  + "  (#" + optionRow.modelData.id + ")"
                        }

                        RowLayout {
                            ButtonGroup { id: choiceGroup }

                            RadioButton {
                                text: "Yes"
                                enabled: page.state && !page.state.finalized
                                ButtonGroup.group: choiceGroup
                                checked: page.pickFor(optionRow.modelData.id) === "Yes"
                                onToggled: page.setPick(optionRow.modelData.id, "Yes")
                            }
                            RadioButton {
                                text: "If need be"
                                enabled: page.state && !page.state.finalized
                                ButtonGroup.group: choiceGroup
                                checked: page.pickFor(optionRow.modelData.id) === "IfNeedBe"
                                onToggled: page.setPick(optionRow.modelData.id, "IfNeedBe")
                            }
                            RadioButton {
                                text: "No"
                                enabled: page.state && !page.state.finalized
                                ButtonGroup.group: choiceGroup
                                checked: page.pickFor(optionRow.modelData.id) === "No"
                                onToggled: page.setPick(optionRow.modelData.id, "No")
                            }
                        }
                    }
                }

                Button {
                    Layout.fillWidth: true
                    text: page.hasVoted ? "Update my votes" : "Submit my votes"
                    enabled: page.pollBridge !== null && page.state !== null && !page.state.finalized
                             && page.participantName.trim() !== ""
                    onClicked: {
                        if (page.hasVoted)
                            page.pollBridge.updateVotes(page.participantName, page.votesPayload())
                        else
                            page.pollBridge.submitVotes(page.participantName, page.votesPayload())
                        page.hasVoted = true
                    }
                }

                DynamicForm {
                    id: undoForm
                    Layout.fillWidth: true
                    actionType: "UndoLastVoteChange"
                    schema: page.schemas["UndoLastVoteChange"] || ({})
                    controller: null
                }
                Button {
                    Layout.fillWidth: true
                    text: "Undo my last vote change"
                    enabled: page.pollBridge !== null && undoForm.ready
                    onClicked: page.pollBridge.submitIfValid("UndoLastVoteChange", undoForm.previewLine)
                }
            }

            // ── Pane 2: comments + finalize (admin) ────────────────────────
            ColumnLayout {
                Layout.preferredWidth: 320
                Layout.fillHeight: true
                spacing: 6

                Label { font.bold: true; text: "Comments (" + (page.state ? page.state.comments.length : 0) + ")" }

                ListView {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 160
                    clip: true
                    model: page.state ? page.state.comments : []

                    delegate: Label {
                        required property var modelData
                        width: ListView.view ? ListView.view.width : 0
                        wrapMode: Text.Wrap
                        text: modelData.participantName + ": " + modelData.body
                    }
                }

                DynamicForm {
                    id: commentForm
                    Layout.fillWidth: true
                    actionType: "AddComment"
                    schema: page.schemas["AddComment"] || ({})
                    controller: null
                }
                Button {
                    Layout.fillWidth: true
                    text: "Add comment"
                    enabled: page.pollBridge !== null && commentForm.ready
                    onClicked: page.pollBridge.submitIfValid("AddComment", commentForm.previewLine)
                }

                Label {
                    Layout.topMargin: 12
                    font.bold: true
                    text: "Admin"
                }

                RowLayout {
                    Layout.fillWidth: true
                    TextField {
                        id: adminTokenField
                        Layout.fillWidth: true
                        placeholderText: "admin token"
                        echoMode: TextInput.Password
                    }
                    Button {
                        text: "use"
                        enabled: page.pollBridge !== null && adminTokenField.text !== ""
                        onClicked: page.pollBridge.setAdminToken(adminTokenField.text)
                    }
                }

                DynamicForm {
                    id: finalizeForm
                    Layout.fillWidth: true
                    actionType: "FinalizePoll"
                    schema: page.schemas["FinalizePoll"] || ({})
                    controller: null
                }
                Button {
                    Layout.fillWidth: true
                    text: "Finalize poll"
                    enabled: page.pollBridge !== null && page.state !== null && !page.state.finalized
                             && finalizeForm.ready
                    onClicked: page.pollBridge.submitIfValid("FinalizePoll", finalizeForm.previewLine)
                }
            }

            // ── Pane 3: live activity log (the Zulip-pattern demo) ─────────
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 6

                Label { font.bold: true; text: "Live activity" }

                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    verticalLayoutDirection: ListView.BottomToTop
                    model: page.activityLog

                    delegate: Label {
                        required property var modelData
                        width: ListView.view ? ListView.view.width : 0
                        elide: Text.ElideRight
                        opacity: 0.8
                        text: "#" + modelData.id + "  [" + modelData.kind + "]  " + modelData.summary
                    }
                }
            }
        }
    }
}

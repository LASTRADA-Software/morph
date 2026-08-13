// SPDX-License-Identifier: Apache-2.0
//
// The organizer's create-poll screen. Native-client-only (Main.qml only ever
// pushes this behind its nativeClient gate) — see examples/polls/README.md's
// resolved design decision 6.
//
// CreatePoll::options is a JSON array field DynamicForm has no control for
// (finding 031) — this whole screen is therefore driven by hand, not by a
// DynamicForm at all, exactly like rung 2's BulkEdit workaround: a plain
// title TextField plus a small hand-written option-label list editor (add/
// remove rows), submitted via PollBridge::createPoll(title, optionLabels)
// directly. See poll_schemas.hpp's own doc comment.
//
// `pollBridge` defaults to null so this same file also loads with nothing
// wired up, which is exactly what the offscreen engine-load smoke test
// (tests/test_gui_qml_smoke.cpp) does.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: page

    property var pollBridge: null

    /// Emitted when the organizer chooses to go straight to the freshly
    /// created poll's vote view. Main.qml listens and pushes VoteView.
    signal openRequested(string pollId)

    property string titleText: ""
    property var optionLabels: ["", ""]  // CreatePoll requires 2-20 options
    property var lastResult: null  // {pollId, adminToken, participantToken}
    property string status: ""
    property bool statusIsError: false

    readonly property bool canSubmit: page.pollBridge !== null
                                       && page.titleText.trim() !== ""
                                       && page.optionLabels.length >= 2
                                       && page.optionLabels.every(function (label) { return label.trim() !== "" })

    function addOption() {
        page.optionLabels = page.optionLabels.concat([""])
    }

    function removeOption(index) {
        if (page.optionLabels.length <= 2)
            return
        const next = page.optionLabels.slice()
        next.splice(index, 1)
        page.optionLabels = next
    }

    function setOption(index, text) {
        const next = page.optionLabels.slice()
        next[index] = text
        page.optionLabels = next
    }

    Connections {
        target: page.pollBridge

        function onCreated(result) {
            page.lastResult = result
            page.status = "poll created — copy the admin token before leaving this screen"
            page.statusIsError = false
        }

        function onFailed(message) {
            page.status = message
            page.statusIsError = true
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
                onClicked: page.StackView.view.pop()
            }
            Label {
                Layout.fillWidth: true
                font.bold: true
                text: "Create a poll"
            }
        }

        Label {
            Layout.fillWidth: true
            visible: page.status !== ""
            wrapMode: Text.Wrap
            color: page.statusIsError ? "#d33" : palette.text
            text: page.status
        }

        ColumnLayout {
            Layout.fillWidth: true
            visible: page.lastResult === null
            spacing: 6

            Label { text: "Title" }
            TextField {
                Layout.fillWidth: true
                placeholderText: "e.g. Team offsite"
                onTextChanged: page.titleText = text
            }

            Label { text: "Candidate dates/options (2-20)" }

            Repeater {
                model: page.optionLabels

                delegate: RowLayout {
                    id: row
                    required property string modelData
                    required property int index
                    Layout.fillWidth: true

                    TextField {
                        Layout.fillWidth: true
                        placeholderText: "e.g. 2026-09-01"
                        text: row.modelData
                        onTextChanged: page.setOption(row.index, text)
                    }

                    Button {
                        text: "remove"
                        enabled: page.optionLabels.length > 2
                        onClicked: page.removeOption(row.index)
                    }
                }
            }

            Button {
                text: "+ add option"
                enabled: page.optionLabels.length < 20
                onClicked: page.addOption()
            }

            Button {
                Layout.fillWidth: true
                text: "Create poll"
                enabled: page.canSubmit
                onClicked: page.pollBridge.createPoll(page.titleText, page.optionLabels)
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            visible: page.lastResult !== null
            spacing: 6

            Label {
                Layout.fillWidth: true
                text: "Poll id (share this link's id with participants):"
            }
            TextField {
                Layout.fillWidth: true
                readOnly: true
                selectByMouse: true
                text: page.lastResult ? page.lastResult.pollId : ""
            }

            Label {
                Layout.fillWidth: true
                text: "Admin token (keep this — needed to finalize the poll):"
            }
            TextField {
                Layout.fillWidth: true
                readOnly: true
                selectByMouse: true
                text: page.lastResult ? page.lastResult.adminToken : ""
            }

            Label {
                Layout.fillWidth: true
                text: "Participant token (goes out with the shared link):"
            }
            TextField {
                Layout.fillWidth: true
                readOnly: true
                selectByMouse: true
                text: page.lastResult ? page.lastResult.participantToken : ""
            }

            Button {
                Layout.fillWidth: true
                text: "Open this poll now"
                onClicked: page.openRequested(page.lastResult.pollId)
            }
        }
    }
}

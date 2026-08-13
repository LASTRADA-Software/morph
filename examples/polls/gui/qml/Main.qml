// SPDX-License-Identifier: Apache-2.0
//
// polls' desktop shell: a StackView holding the landing screen (inline,
// below — this rung ships only three QML files per its task brief, so there
// is no separate LandingView.qml) plus the two screens it can push:
// CreatePollView (native-client-only — see nativeClient below) and VoteView.
//
// The controller properties below are supplied by a client's own entry point
// through QQmlApplicationEngine::setInitialProperties. Exactly one such entry
// point exists today: gui_wasm/main_wasm.cpp, the browser client. There is
// deliberately no gui/main.cpp — no task in this rung's plan wrote a native
// desktop entry point, and adding one is named follow-up work in
// examples/polls/README.md ("No gui/main.cpp yet"), not an oversight this
// file works around.
//
// Every property defaults to a value that makes this file load with nothing
// wired up at all, which is exactly what the offscreen engine-load smoke test
// (tests/test_gui_qml_smoke.cpp) relies on.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    width: 1000
    height: 720
    visible: true
    title: "polls — morph application ladder, rung 3"

    property var pollBridge: null

    /// Whether this build may create polls. `CreatePoll` is native-client-only
    /// per this rung's Global Constraints (examples/polls/README.md,
    /// resolved design decision 6: a WASM tab's `assignHandlerPrimary` promote
    /// step has no async path and would abort the page). Defaults to `true`,
    /// the value a native desktop shell would leave alone;
    /// gui_wasm/main_wasm.cpp passes `nativeClient: false` as an initial
    /// property, which hides (not merely disables — see the Button below) the
    /// one UI affordance that reaches CreatePollView.
    property bool nativeClient: true

    /// Set by the WASM client, which parses `?poll=<id>` from the page url
    /// (`gui_wasm/main_wasm.cpp`'s `EM_JS` shim) so a participant following a shared link
    /// lands directly on that poll's vote view instead of the landing page.
    /// Empty (the default) preserves today's behaviour exactly — the
    /// `StackView` below still starts on, and stays on, `landingPage`; every
    /// existing QML smoke test's assertions are unaffected. Passed the same
    /// way as `pollBridge`/`nativeClient` above: a root-object property set
    /// from C++ via `QQmlApplicationEngine::setInitialProperties` right after
    /// the engine is constructed.
    property string initialPollId: ""

    /// The whole `{actionType: schema}` document, parsed once here rather
    /// than per form: it is a CONSTANT property on the controller, so one
    /// parse is all it can ever need.
    property var schemas: root.pollBridge ? JSON.parse(root.pollBridge.schemasJson) : ({})

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 6

        Label {
            font.bold: true
            text: "polls"
        }

        StackView {
            id: stack
            Layout.fillWidth: true
            Layout.fillHeight: true
            initialItem: landingPage

            // Pushes straight to the shared poll named by a WASM client's
            // `?poll=<id>` link, on top of the still-loaded landingPage (so
            // VoteView's own "< Back" button returns somewhere sensible
            // rather than exiting). A no-op — root.initialPollId stays "" —
            // for every client that does not set it, native or WASM.
            Component.onCompleted: {
                if (root.initialPollId !== "")
                    stack.push(votePage, { pollId: root.initialPollId })
            }
        }
    }

    Component {
        id: landingPage

        Item {
            id: landing
            property string joinPollId: ""

            ColumnLayout {
                anchors.centerIn: parent
                width: Math.min(landing.width - 32, 460)
                spacing: 12

                Label {
                    Layout.fillWidth: true
                    font.pixelSize: 18
                    font.bold: true
                    text: "Doodle-style scheduling polls"
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4

                    Label { text: "Open a poll (paste the shared link's id)" }

                    RowLayout {
                        Layout.fillWidth: true

                        TextField {
                            id: pollIdField
                            Layout.fillWidth: true
                            placeholderText: "poll id"
                            onTextChanged: landing.joinPollId = text
                        }

                        Button {
                            text: "Open"
                            enabled: root.pollBridge !== null && landing.joinPollId.trim() !== ""
                            onClicked: stack.push(votePage, { pollId: landing.joinPollId.trim() })
                        }
                    }
                }

                // The one affordance that reaches CreatePollView — absent
                // (not merely disabled) when nativeClient is false, so a WASM
                // build that sets it never even renders a path there. See
                // root.nativeClient's own doc comment.
                Button {
                    Layout.fillWidth: true
                    visible: root.nativeClient
                    text: "Create a new poll (organizer)"
                    enabled: root.pollBridge !== null
                    onClicked: stack.push(createPage)
                }
            }
        }
    }

    Component {
        id: createPage

        CreatePollView {
            pollBridge: root.pollBridge
            onOpenRequested: function (pollId) {
                stack.push(votePage, { pollId: pollId })
            }
        }
    }

    Component {
        id: votePage

        VoteView {
            pollBridge: root.pollBridge
            schemas: root.schemas
            onBackRequested: {
                if (root.pollBridge)
                    root.pollBridge.stopPolling()
                stack.pop()
            }
        }
    }
}

// SPDX-License-Identifier: Apache-2.0
//
// lims' desktop shell: two tabs, one per surface — the sample lifecycle and
// result entry. Deliberately flat rather than a StackView: this rung's two
// surfaces act on the *same* attached sample at the same time (a bench
// operator captures a reading while the office watches the lifecycle move),
// so hiding one behind the other would misrepresent what the models do.
//
// Both bridge properties are supplied by gui/main.cpp through
// QQmlApplicationEngine::setInitialProperties. They default to null so this
// same file also loads with nothing wired up, which is exactly what the
// offscreen engine-load smoke test (tests/test_gui_qml_smoke.cpp) does.
//
// Bindings only (examples/TESTING.md presenter rule 6): every conditional,
// format and validation below is a property read or a direct invokable call.
// Nothing here decides anything — the id shown in the header is the one the
// *model* returned, not one this file tracked.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    width: 1100
    height: 780
    visible: true
    title: "lims — morph application ladder, rung 6"

    property var sampleBridge: null
    property var resultBridge: null

    // The one piece of cross-surface wiring: attaching the lifecycle surface
    // to a sample points the result surface at the same one. Both handlers
    // are AllowShared over a keyed model, so they land on one instance —
    // which is the property this arrangement exists to show.
    Connections {
        target: root.sampleBridge

        function onSampleChanged(sample) {
            if (sample.id !== undefined && sample.id >= 0) {
                root.resultBridge.openSample(sample.id)
                root.resultBridge.refreshResults()
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 6

        RowLayout {
            Layout.fillWidth: true

            Label {
                font.bold: true
                text: "lims"
            }

            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignRight
                opacity: 0.7
                text: root.sampleBridge && root.sampleBridge.sample.id !== undefined
                      ? "sample " + root.sampleBridge.sample.id
                        + " — " + root.sampleBridge.sample.state
                        + " (v" + root.sampleBridge.sample.version + ")"
                      : "no sample attached"
            }
        }

        TabBar {
            id: tabs
            Layout.fillWidth: true

            TabButton { text: "Lifecycle" }
            TabButton { text: "Results" }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tabs.currentIndex

            SampleView {
                sampleBridge: root.sampleBridge
            }

            ResultEntryView {
                resultBridge: root.resultBridge
            }
        }

        Label {
            Layout.fillWidth: true
            color: "red"
            wrapMode: Text.Wrap
            visible: text !== ""
            text: root.sampleBridge ? root.sampleBridge.lastError : ""
        }
    }
}

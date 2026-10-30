// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The steps of a process in order, drawn as numbered circles joined by connectors that share the width. A done step
// is a filled circle with a tick, the current one an accent outline and number, an upcoming one a grey outline and
// number; a connector is in the accent after a done step. Unlabelled, only the circles and connectors are drawn, and
// each step keeps its label as its accessible name.
MorphWidget {
    id: root

    // [{label, state (0 pending, 1 active, 2 done, 3 failed)}].
    property var steps: []
    property int current: -1
    property bool labelled: true

    readonly property real circleSize: Math.round(1.7 * Theme.bodyPixels)
    readonly property string currentLabel: current >= 0 && current < steps.length ? steps[current].label : ""

    // The state a step's accessible name carries after its label; a pending step has none.
    function stateName(index: int): string {
        if (index === root.current)
            return "current"
        switch (root.steps[index].state) {
        case 2:
            return "done"
        case 3:
            return "failed"
        default:
            return ""
        }
    }

    defaultName: currentLabel
    defaultRole: Accessible.List
    implicitWidth: layout.implicitWidth
    implicitHeight: layout.implicitHeight

    RowLayout {
        id: layout
        anchors.fill: parent
        spacing: 0

        // Steps at the even positions, connectors between them at the odd ones.
        Repeater {
            model: Math.max(0, 2 * root.steps.length - 1)

            Item {
                id: cell
                objectName: isConnector ? "gap" : "step"
                required property int index
                readonly property bool isConnector: index % 2 === 1
                readonly property int stepIndex: Math.floor(index / 2)
                readonly property var step: isConnector ? null : root.steps[stepIndex]
                readonly property bool isCurrent: !isConnector && stepIndex === root.current
                readonly property int stepState: step ? step.state : 0
                // An active step that is not the current one is drawn as an upcoming one.
                readonly property bool isDone: stepState === 2
                readonly property bool isFailed: stepState === 3
                readonly property real minimumConnector: 3 * Theme.unitWidth

                Layout.fillWidth: isConnector
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: isConnector ? minimumConnector : implicitWidth
                Layout.minimumWidth: isConnector ? minimumConnector : implicitWidth
                implicitWidth: isConnector ? minimumConnector : stepRow.implicitWidth
                implicitHeight: isConnector ? 2 : stepRow.implicitHeight

                Accessible.role: isConnector ? Accessible.NoRole : Accessible.ListItem
                Accessible.ignored: isConnector
                Accessible.name: {
                    if (isConnector || !step)
                        return ""
                    const state = root.stateName(stepIndex)
                    return state === "" ? step.label : step.label + ", " + state
                }

                Rectangle {
                    objectName: "connector"
                    visible: cell.isConnector
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    // The connector follows the step before it.
                    readonly property bool followsDone: cell.isConnector && root.steps[cell.stepIndex].state === 2
                    height: 2
                    color: followsDone ? root.scheme.accent : root.scheme.controlBorder
                }

                RowLayout {
                    id: stepRow
                    visible: !cell.isConnector
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.spaceOf(2)

                    Rectangle {
                        objectName: "circle"
                        implicitWidth: root.circleSize
                        implicitHeight: root.circleSize
                        radius: width / 2
                        border.width: 1.5
                        border.color: cell.isFailed ? root.scheme.error : cell.isDone || cell.isCurrent
                                      ? root.scheme.accent : root.scheme.controlBorder
                        color: cell.isFailed ? root.scheme.error : cell.isDone ? root.scheme.accent : "transparent"
                        Accessible.ignored: true

                        Label {
                            objectName: "mark"
                            anchors.centerIn: parent
                            textFormat: Text.PlainText
                            font: Theme.smallStrongFont
                            text: cell.isFailed ? "✗" : cell.isDone ? "✓" : String(cell.stepIndex + 1)
                            color: cell.isFailed || cell.isDone ? "#ffffff"
                                   : cell.isCurrent ? root.scheme.accent : root.scheme.muted
                            Accessible.ignored: true
                        }
                    }

                    Label {
                        objectName: "label"
                        visible: root.labelled
                        textFormat: Text.PlainText
                        font: Theme.bodyFont
                        text: cell.step ? cell.step.label : ""
                        color: cell.isCurrent ? root.scheme.text : root.scheme.muted
                        Accessible.ignored: true
                    }
                }
            }
        }
    }
}

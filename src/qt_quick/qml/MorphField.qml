// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// The root of text and the six inputs: the field state, with the messages shown under the control in the error
// colour and a stale value dimmed.
MorphWidget {
    id: root

    property bool readOnly: false
    property bool required: false
    property list<string> errors: []
    property bool stale: false
    // The control the kind shows; the field lays it out above its messages.
    property Item control: null
    readonly property real controlWidth: width - (required ? marker.implicitWidth : 0)
    // What assistive technology reads as the field's description: its messages.
    readonly property string accessibleDescription: errors.join("\n")

    focusTarget: control ? control : root
    implicitWidth: (control ? control.implicitWidth : 0) + (required ? marker.implicitWidth : 0)
    implicitHeight: (control ? control.implicitHeight : 0) + (messages.visible ? messages.implicitHeight : 0)
    opacity: stale ? Theme.staleOpacity : 1.0
    Accessible.description: accessibleDescription

    Label {
        id: marker
        anchors.right: parent.right
        visible: root.required
        text: "*"
        color: Theme.error
    }

    Label {
        id: messages
        y: root.control ? root.control.height : 0
        width: root.width
        visible: root.errors.length > 0
        text: root.errors.join("\n")
        color: Theme.error
        wrapMode: Text.Wrap
    }
}

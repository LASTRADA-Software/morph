// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// The root of text and the six inputs: the field state, with the messages shown under the control in the error
// colour and a stale value dimmed. While `labelShown`, the field's `caption` is drawn above the control; otherwise
// the caption is drawn elsewhere (a form's cell), and a restyle reads `caption` knowing that.
MorphWidget {
    id: root

    property bool readOnly: false
    property bool required: false
    property list<string> errors: []
    property bool stale: false
    // The control the kind shows; the field lays it out above its messages.
    property Item control: null
    // The field's label, and whether the field draws it as a visible caption above the control.
    property string caption: ""
    property bool labelShown: false
    readonly property real captionHeight: captionLabel.visible ? captionLabel.implicitHeight : 0
    readonly property real controlWidth: width - (required ? marker.implicitWidth : 0)
    // What assistive technology reads as the field's description: its messages.
    readonly property string accessibleDescription: errors.join("\n")

    focusTarget: control ? control : root
    implicitWidth: Math.max(captionLabel.visible ? captionLabel.implicitWidth : 0,
                            (control ? control.implicitWidth : 0) + (required ? marker.implicitWidth : 0))
    implicitHeight: captionHeight + (control ? control.implicitHeight : 0) +
                    (messages.visible ? messages.implicitHeight : 0)
    opacity: stale ? Theme.staleOpacity : 1.0
    Accessible.description: accessibleDescription

    Label {
        font: Theme.bodyFont
        id: captionLabel
        objectName: "caption"
        width: root.width
        visible: root.labelShown && root.caption !== ""
        text: root.caption
        color: root.scheme.text
        elide: Text.ElideRight
    }

    // The control sits under the caption only while one is drawn, so a kind's own placement stands otherwise.
    Binding {
        target: root.control
        property: "y"
        value: root.captionHeight
        when: root.control !== null && root.captionHeight > 0
    }

    Label {
        font: Theme.bodyFont
        id: marker
        anchors.right: parent.right
        y: root.captionHeight
        visible: root.required
        text: "*"
        color: root.scheme.error
    }

    Label {
        font: Theme.bodyFont
        id: messages
        y: root.captionHeight + (root.control ? root.control.height : 0)
        width: root.width
        visible: root.errors.length > 0
        text: root.errors.join("\n")
        color: root.scheme.error
        wrapMode: Text.Wrap
    }
}

// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// The root of text and the six inputs: the field state, with the messages shown under the control in the error
// colour, then the description, muted or in the tone's colour, and a stale value dimmed. While `labelShown`, the field's `caption` is drawn above the control; otherwise
// the caption is drawn elsewhere (a form's cell), and a restyle reads `caption` knowing that.
MorphWidget {
    id: root

    property bool readOnly: false
    property bool required: false
    property list<string> errors: []
    property bool stale: false
    // A line that describes the field, shown under the messages.
    property string description: ""
    // An evaluation of the value, as the `Tone` index (0 none): it outlines the control and colours the description.
    property int tone: 0
    // The tone the control is outlined in: `Err` while the field has errors, whatever its own tone, so errors and an
    // evaluation share one outline.
    readonly property int shownTone: errors.length > 0 ? 4 : tone
    // The control the kind shows; the field lays it out above its messages.
    property Item control: null
    // The field's label, and whether the field draws it as a visible caption above the control.
    property string caption: ""
    // A hint drawn after the caption and its required mark, in the caption's face and led by a middle dot; only while
    // the caption is drawn.
    property string captionHint: ""
    property bool labelShown: false
    readonly property real captionHeight: captionLabel.visible ? captionLabel.implicitHeight : 0
    // The required mark follows the caption while one is drawn; a kind whose control is text it lays out itself
    // (`markerFollowsText`) has the mark follow the text's last line, at `textEndX`, `textEndY`; otherwise the mark
    // sits beside the control, at the field's right edge.
    property bool markerFollowsText: false
    property real textEndX: 0
    property real textEndY: 0
    readonly property bool markerAfterCaption: captionHeight > 0
    // The room the mark takes on the line it follows: a space step, then the mark.
    readonly property real markerRoom: Theme.spaceOf(1) + marker.implicitWidth
    // The room the hint takes after the caption and its mark: the hint's text and a step before it, at most half the
    // field, so a long hint cannot push the caption out.
    readonly property real hintRoom: hintLabel.visible ? Math.min(hintMetrics.advanceWidth + Theme.spaceOf(1), root.width / 2) : 0
    readonly property real controlWidth: width - (!required || markerAfterCaption ? 0
                                                  : markerFollowsText ? markerRoom : marker.implicitWidth)
    // The unit a kind shows beside its value, announced last, so a listener hears the value with its unit; and the
    // prefix it shows before the value, announced first.
    property string announcedUnit: ""
    property string announcedPrefix: ""
    // What assistive technology reads as the field's description: the prefix, its messages, then its description,
    // then the unit.
    readonly property string accessibleDescription: {
        const lines = errors.concat(description === "" ? [] : [description])
                            .concat(announcedUnit === "" ? [] : [announcedUnit])
        return (announcedPrefix === "" ? lines : [announcedPrefix].concat(lines)).join("\n")
    }

    focusTarget: control ? control : root
    implicitWidth: Math.max(captionLabel.visible ? captionLabel.implicitWidth + (required ? markerRoom : 0)
                                                   + (hintLabel.visible ? hintMetrics.advanceWidth + Theme.spaceOf(1) : 0)
                                                 : 0,
                            (control ? control.implicitWidth : 0) + (!required || markerAfterCaption ? 0
                                                                     : markerFollowsText ? markerRoom
                                                                                         : marker.implicitWidth))
    implicitHeight: captionHeight + (control ? control.implicitHeight : 0) +
                    (messages.visible ? messages.implicitHeight : 0) +
                    (descriptionLabel.visible ? descriptionLabel.implicitHeight : 0)
    opacity: stale ? Theme.staleOpacity : 1.0
    Accessible.description: accessibleDescription

    Label {
        textFormat: Text.PlainText
        font: Theme.captionFont
        id: captionLabel
        objectName: "caption"
        // Elided short of the mark and the hint, so both stay visible after it.
        width: Math.max(0, root.width - (root.required ? root.markerRoom : 0) - root.hintRoom)
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
        textFormat: Text.PlainText
        font: Theme.bodyFont
        id: marker
        objectName: "requiredMark"
        x: root.markerAfterCaption ? captionLabel.contentWidth + Theme.spaceOf(1)
                                   : root.markerFollowsText ? Math.min(root.textEndX + Theme.spaceOf(1),
                                                                       root.width - implicitWidth)
                                                            : root.width - implicitWidth
        y: root.markerAfterCaption ? 0 : root.markerFollowsText ? root.textEndY : root.captionHeight
        visible: root.required
        text: "*"
        color: root.scheme.error
    }

    // The hint's own width: its label elides, so its implicit width follows the room it is given.
    TextMetrics {
        id: hintMetrics
        font: Theme.captionFont
        text: hintLabel.text
    }

    Label {
        textFormat: Text.PlainText
        font: Theme.captionFont
        id: hintLabel
        objectName: "captionHint"
        x: captionLabel.contentWidth + (root.required ? root.markerRoom : 0) + Theme.spaceOf(1)
        width: Math.max(0, root.hintRoom - Theme.spaceOf(1))
        visible: root.captionHint !== "" && captionLabel.visible
        text: "\u00b7 " + root.captionHint
        color: root.scheme.text
        elide: Text.ElideRight
    }

    Label {
        textFormat: Text.PlainText
        font: Theme.bodyFont
        id: messages
        y: root.captionHeight + (root.control ? root.control.height : 0)
        width: root.width
        visible: root.errors.length > 0
        text: root.errors.join("\n")
        color: root.scheme.error
        wrapMode: Text.Wrap
    }

    Label {
        id: descriptionLabel
        objectName: "description"
        textFormat: Text.PlainText
        font: Theme.bodyFont
        y: messages.y + (messages.visible ? messages.height : 0)
        width: root.width
        visible: root.description !== ""
        text: root.description
        color: root.tone !== 0 ? root.toneOf(root.tone) : root.scheme.muted
        wrapMode: Text.Wrap
    }
}

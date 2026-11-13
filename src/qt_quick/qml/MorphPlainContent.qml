// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Templates as T

// A control's content drawn as plain text, in place of the style's own content item: the style's IconLabel takes a
// string that looks like markup for markup, and a control's text is a document's or a value's, shown as the
// characters it holds. The control's icon, when it has one, is drawn before the text; an avatar (initials in a
// filled circle) before the icon, and a caret (a chevron that says the control opens) after the text.
//
// A control with no text whose icon the theme's set does not name is never blank: it draws a built-in chevron for the
// framework's `nav.back` and `nav.forward`, else the framework's glyph for the icon, else the name it is given.
Item {
    id: content

    required property T.AbstractButton control
    property color color: content.control.palette.buttonText
    // Where the icon and the text sit across the content: Qt.AlignHCenter or Qt.AlignLeft.
    property int alignment: Qt.AlignHCenter
    // The name of the control's icon, and the text drawn for an icon the set lacks when there is no glyph either.
    property string iconName: ""
    property string fallbackText: ""
    // Initials drawn in a circle before everything else; empty for none.
    property string avatar: ""
    // The marker after the text: 0 none, 1 a chevron pointing down, 2 an up and a down chevron.
    property int caret: 0

    readonly property bool iconShown: image.status === Image.Ready
    readonly property real iconExtent: iconShown ? image.width + content.control.spacing : 0
    // Whether the control is an icon alone that the theme's set does not draw.
    readonly property bool unnamedIcon: content.iconName !== "" && content.control.text === ""
                                        && content.control.icon.source.toString() === ""
    readonly property bool chevronShown: content.unnamedIcon
                                         && (content.iconName === "nav.back" || content.iconName === "nav.forward")
    readonly property string shownText: {
        if (content.control.text !== "" || !content.unnamedIcon || content.chevronShown)
            return content.control.text
        const glyph = Theme.iconGlyphs[content.iconName]
        return glyph !== undefined ? glyph : content.fallbackText
    }
    readonly property real chevronExtent: chevron.visible ? chevron.width : 0
    // The circle is two lines of text across, so a two-line caption and its avatar stand level.
    readonly property real avatarSize: Math.round(2 * Theme.bodyPixels)
    readonly property real avatarExtent: content.avatar !== "" ? content.avatarSize + Theme.spaceOf(2) : 0
    readonly property real caretExtent: content.caret > 0 ? caretMark.width + Theme.spaceOf(2) : 0
    // Where the content starts across the box.
    readonly property real startX: content.alignment === Qt.AlignLeft
                                   ? 0 : Math.max(0, (content.width - content.implicitWidth) / 2)

    implicitWidth: avatarExtent + iconExtent + chevronExtent + label.implicitWidth + caretExtent
    implicitHeight: Math.max(content.avatar !== "" ? content.avatarSize : 0, iconShown ? image.height : 0,
                             chevron.visible ? chevron.height : 0, label.implicitHeight)

    Image {
        id: image
        objectName: "contentIcon"
        x: content.startX + content.avatarExtent
        anchors.verticalCenter: parent.verticalCenter
        visible: content.iconShown
        source: content.control.icon.source
        sourceSize.width: content.control.icon.width
        sourceSize.height: content.control.icon.height
        width: content.control.icon.width
        height: content.control.icon.height
    }

    MorphChevron {
        id: chevron
        objectName: "navChevron"
        x: content.startX + content.avatarExtent
        anchors.verticalCenter: parent.verticalCenter
        visible: content.chevronShown
        direction: content.iconName === "nav.forward" ? "forward" : "back"
        color: content.color
    }

    Text {
        id: label
        objectName: "contentLabel"
        x: content.startX + content.avatarExtent + content.iconExtent + content.chevronExtent
        width: Math.min(implicitWidth, content.width - content.avatarExtent - content.iconExtent -
                                       content.chevronExtent - content.caretExtent)
        anchors.verticalCenter: parent.verticalCenter
        text: content.shownText
        textFormat: Text.PlainText
        font: content.control.font
        color: content.color
        elide: Text.ElideRight
        visible: text !== ""
    }

    // Declared after the label: a caller that looks for the control's text finds the caption first.
    Rectangle {
        id: avatarCircle
        objectName: "contentAvatar"
        x: content.startX
        anchors.verticalCenter: parent.verticalCenter
        visible: content.avatar !== ""
        width: content.avatarSize
        height: content.avatarSize
        radius: width / 2
        antialiasing: true
        // The caption's colour, tinted: the circle is a wash of what the text is drawn in.
        color: Qt.rgba(content.color.r, content.color.g, content.color.b, 0.16)
        Text {
            objectName: "contentAvatarText"
            anchors.centerIn: parent
            text: content.avatar
            textFormat: Text.PlainText
            font: Theme.fontOf(Math.round(0.8 * Theme.bodyPixels), Theme.strongWeight)
            color: content.color
        }
    }

    MorphCaret {
        id: caretMark
        objectName: "contentCaret"
        x: label.x + label.width + Theme.spaceOf(2)
        anchors.verticalCenter: parent.verticalCenter
        visible: content.caret > 0
        kind: content.caret === 2 ? "updown" : "down"
        color: content.color
    }
}

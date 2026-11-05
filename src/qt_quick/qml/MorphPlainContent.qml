// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Templates as T

// A control's content drawn as plain text, in place of the style's own content item: the style's IconLabel takes a
// string that looks like markup for markup, and a control's text is a document's or a value's, shown as the
// characters it holds. The control's icon, when it has one, is drawn before the text.
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

    implicitWidth: iconExtent + chevronExtent + label.implicitWidth
    implicitHeight: Math.max(iconShown ? image.height : 0, chevron.visible ? chevron.height : 0, label.implicitHeight)

    Image {
        id: image
        objectName: "contentIcon"
        x: content.alignment === Qt.AlignLeft ? 0 : Math.max(0, (content.width - content.implicitWidth) / 2)
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
        x: content.alignment === Qt.AlignLeft ? 0 : Math.max(0, (content.width - content.implicitWidth) / 2)
        anchors.verticalCenter: parent.verticalCenter
        visible: content.chevronShown
        direction: content.iconName === "nav.forward" ? "forward" : "back"
        color: content.color
    }

    Text {
        id: label
        objectName: "contentLabel"
        x: (content.alignment === Qt.AlignLeft ? 0 : Math.max(0, (content.width - content.implicitWidth) / 2)) +
           content.iconExtent + content.chevronExtent
        width: Math.min(implicitWidth, content.width - content.iconExtent - content.chevronExtent)
        anchors.verticalCenter: parent.verticalCenter
        text: content.shownText
        textFormat: Text.PlainText
        font: content.control.font
        color: content.color
        elide: Text.ElideRight
        visible: text !== ""
    }
}

// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Templates as T

// A control's content drawn as plain text, in place of the style's own content item: the style's IconLabel takes a
// string that looks like markup for markup, and a control's text is a document's or a value's, shown as the
// characters it holds. The control's icon, when it has one, is drawn before the text.
Item {
    id: content

    required property T.AbstractButton control
    property color color: content.control.palette.buttonText
    // Where the icon and the text sit across the content: Qt.AlignHCenter or Qt.AlignLeft.
    property int alignment: Qt.AlignHCenter

    readonly property bool iconShown: image.status === Image.Ready
    readonly property real iconExtent: iconShown ? image.width + content.control.spacing : 0

    implicitWidth: iconExtent + label.implicitWidth
    implicitHeight: Math.max(iconShown ? image.height : 0, label.implicitHeight)

    Image {
        id: image
        x: content.alignment === Qt.AlignLeft ? 0 : Math.max(0, (content.width - content.implicitWidth) / 2)
        anchors.verticalCenter: parent.verticalCenter
        visible: content.iconShown
        source: content.control.icon.source
        sourceSize.width: content.control.icon.width
        sourceSize.height: content.control.icon.height
        width: content.control.icon.width
        height: content.control.icon.height
    }

    Text {
        id: label
        x: (content.alignment === Qt.AlignLeft ? 0 : Math.max(0, (content.width - content.implicitWidth) / 2)) +
           content.iconExtent
        width: Math.min(implicitWidth, content.width - content.iconExtent)
        anchors.verticalCenter: parent.verticalCenter
        text: content.control.text
        textFormat: Text.PlainText
        font: content.control.font
        color: content.color
        elide: Text.ElideRight
        visible: text !== ""
    }
}

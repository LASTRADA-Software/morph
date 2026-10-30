// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The header of a section that folds: the title in the strong weight, an optional slot for the node's own header, and
// a chevron at the trailing edge unless the theme draws no marker. The whole row is the toggle, at least a touch
// target tall; the slot sits above it, so what it holds keeps its own clicks. The toggle reports `toggled`; the section decides what that changes.
Item {
    id: root

    property string title: ""
    property bool open: true
    // Whether the row folds anything: a header that does not has no chevron, answers no click, and is as tall as its
    // content.
    property bool foldable: true
    property MorphScheme scheme: Theme.themeScheme
    readonly property Item slot: slotRow
    readonly property Item toggle: toggleButton

    signal toggled

    implicitWidth: row.implicitWidth
    implicitHeight: root.foldable ? Math.max(Theme.targetHeight, row.implicitHeight) : row.implicitHeight

    AbstractButton {
        id: toggleButton
        objectName: "toggle"
        anchors.fill: parent
        enabled: root.foldable
        focusPolicy: root.foldable ? Qt.StrongFocus : Qt.NoFocus
        hoverEnabled: true
        background: Rectangle {
            color: toggleButton.hovered ? root.scheme.hover : "transparent"
            radius: Theme.radiusControl
        }
        Accessible.role: Accessible.Button
        Accessible.name: root.title
        // Qt 6.8's Accessible has no `expandable` or `expanded`, so they are set where they exist rather than
        // declared, which would stop the header loading there.
        Component.onCompleted: {
            const a11y = toggleButton.Accessible
            if ("expanded" in a11y) {
                a11y["expandable"] = true
                a11y["expanded"] = Qt.binding(() => root.open)
            }
        }
        Keys.onReturnPressed: clicked()
        Keys.onEnterPressed: clicked()
        onClicked: root.toggled()
    }

    RowLayout {
        id: row
        anchors.fill: parent
        spacing: Theme.spaceOf(2)

        Label {
            objectName: "title"
            Layout.fillWidth: true
            textFormat: Text.PlainText
            font: Theme.strongFont
            color: root.scheme.text
            elide: Text.ElideRight
            text: root.title
            Accessible.ignored: true
        }

        RowLayout {
            id: slotRow
            spacing: Theme.spaceOf(2)
        }

        // The chevron: the theme's icon when it has one, else a glyph.
        Image {
            objectName: "chevron"
            readonly property string name: root.open ? "nav.collapse" : "nav.expand"
            source: Theme.iconSource(name)
            visible: root.foldable && Theme.collapsibleMarker && source.toString() !== ""
            sourceSize.height: Theme.bodyPixels
            Accessible.ignored: true
        }
        Label {
            objectName: "chevronGlyph"
            visible: root.foldable && Theme.collapsibleMarker &&
                     Theme.iconSource(root.open ? "nav.collapse" : "nav.expand") === ""
            textFormat: Text.PlainText
            font: Theme.bodyFont
            color: root.scheme.muted
            text: root.open ? "▾" : "▸"
            Accessible.ignored: true
        }
    }
}

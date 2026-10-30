// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A framed region with a title and `padding` (a spacing step) around its content. A collapsible panel is an accordion section:
// its header button asks to collapse or expand it, and a collapsed panel hides its content.
MorphWidget {
    id: root

    property string title: ""
    property int padding: 0
    property bool collapsible: false
    property bool collapsed: false
    readonly property Item content: column
    readonly property Item headerArea: header.slot
    // Whether a shown content child stretches down; the content then takes the panel's spare height, else a filler
    // does, which keeps the content under the header instead of centred in the spare height.
    readonly property bool contentStretches: {
        for (const child of column.children) {
            if (child.visible && child.heightKind === 2)
                return true
        }
        return false
    }
    // The scheme the frame and the divider draw with: a panel that names no surface is a card, whatever scheme it
    // inherits.
    readonly property MorphScheme frameScheme: root.surface === "" ? Theme.cardScheme : root.scheme
    readonly property bool headerRowShown: root.title !== "" || root.collapsible || headerArea.children.length > 0

    implicitWidth: frame.implicitWidth
    implicitHeight: frame.implicitHeight

    Rectangle {
        id: frame
        objectName: "frame"
        anchors.fill: parent
        implicitWidth: inner.implicitWidth + 2 * Theme.spaceOf(root.padding) + 2
        implicitHeight: inner.implicitHeight + Theme.spaceOf(root.padding) * (root.headerRowShown && root.collapsible ? 1 : 2) + 2
        // A panel that names no surface fills itself as a card. One that names a surface the theme draws leaves the
        // fill to the widget, which draws that surface behind the frame, so the frame is not drawn twice.
        color: root.surface === "" ? Theme.cardScheme.surface
               : Theme.draws(root.surface) ? "transparent" : root.scheme.surface
        radius: Theme.radiusCard
        border.color: root.frameScheme.border

        // A card's shadow, drawn under a panel that names no surface unless it sits on a card already: a card on a
        // card has no elevation.
        Rectangle {
            objectName: "frameShadow"
            anchors.fill: parent
            anchors.leftMargin: Theme.cardShadowOffsetX - Theme.cardShadowSpread
            anchors.rightMargin: -Theme.cardShadowOffsetX - Theme.cardShadowSpread
            anchors.topMargin: Theme.cardShadowOffsetY - Theme.cardShadowSpread
            anchors.bottomMargin: -Theme.cardShadowOffsetY - Theme.cardShadowSpread
            z: -1
            visible: root.surface === "" && root.inheritedScheme !== Theme.cardScheme
            radius: Theme.radiusCard + Theme.cardShadowBlur / 4
            color: Theme.cardShadowColor
        }

        ColumnLayout {
            id: inner
            anchors.fill: parent
            anchors.leftMargin: Theme.spaceOf(root.padding) + 1
            anchors.rightMargin: Theme.spaceOf(root.padding) + 1
            // A foldable panel's header is flush with the frame's top; its content has the padding above it.
            anchors.topMargin: (root.headerRowShown && root.collapsible ? 0 : Theme.spaceOf(root.padding)) + 1
            anchors.bottomMargin: Theme.spaceOf(root.padding) + 1

            MorphSectionHeader {
                id: header
                visible: root.headerRowShown
                Layout.fillWidth: true
                title: root.title
                open: !root.collapsed
                foldable: root.collapsible
                scheme: root.frameScheme
                onToggled: {
                    root.collapsed = !root.collapsed
                    root.fire("toggle", root.collapsed)
                }
            }

            ColumnLayout {
                id: column
                visible: !root.collapsible || !root.collapsed
                Layout.topMargin: root.headerRowShown && root.collapsible ? Theme.spaceOf(root.padding) : 0
                Layout.fillWidth: true
                Layout.fillHeight: root.contentStretches
            }

            Item {
                Layout.fillHeight: true
                visible: !root.contentStretches
            }
        }
    }
}

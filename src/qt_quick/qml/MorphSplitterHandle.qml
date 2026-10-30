// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// The handle a splitter draws between two panes: the `splitterHandle` part a restyle replaces. The split view makes
// one per gap and the splitter sets `axis` and `scheme` on each; the handle reads `SplitHandle.pressed` and
// `SplitHandle.hovered`. Its implicit extent along the axis is the room the splitter leaves between panes. The stock
// handle is 6 px thick, in the scheme's border colour, and in its control border colour while hovered or pressed; a
// short grip across its middle shows that it can be dragged.
Rectangle {
    id: root

    // 0 vertical (panes top to bottom), 1 horizontal.
    property int axis: 1
    // The colour roles of the splitter the handle is in.
    property MorphScheme scheme: Theme.themeScheme

    implicitWidth: root.axis === 1 ? 6 : 0
    implicitHeight: root.axis === 0 ? 6 : 0
    color: SplitHandle.pressed || (root.enabled && SplitHandle.hovered) ? root.scheme.controlBorder
                                                                         : root.scheme.border

    // 2 px across the axis by at most 28 px along it, centred, with rounded ends.
    Rectangle {
        objectName: "grip"
        anchors.centerIn: parent
        width: root.axis === 1 ? 2 : Math.min(28, root.width / 3)
        height: root.axis === 1 ? Math.min(28, root.height / 3) : 2
        radius: 1
        color: root.scheme.neutralDot
    }
}

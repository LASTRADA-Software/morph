// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// The handle a splitter draws between two panes: the `splitterHandle` part a restyle replaces. The split view makes
// one per gap and the splitter sets `axis` and `scheme` on each; the handle reads `SplitHandle.pressed` and
// `SplitHandle.hovered`. Its implicit extent along the axis is the room the splitter leaves between panes. The stock
// handle is the Basic style's: 6 px thick, in the palette's button colour, midlight while hovered, mid while pressed.
Rectangle {
    id: root

    // 0 vertical (panes top to bottom), 1 horizontal.
    property int axis: 1
    // The colour roles of the splitter the handle is in.
    property MorphScheme scheme: Theme.themeScheme

    implicitWidth: root.axis === 1 ? 6 : 0
    implicitHeight: root.axis === 0 ? 6 : 0
    color: SplitHandle.pressed ? root.palette.mid
                               : (root.enabled && SplitHandle.hovered ? root.palette.midlight : root.palette.button)
}

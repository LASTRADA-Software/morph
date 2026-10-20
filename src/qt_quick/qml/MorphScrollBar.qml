// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// The bar a scroll area attaches on its axis: the `scrollBar` part a restyle replaces. The stock bar is the style's
// own; `scheme` is the colour roles of the scroll area it is in, for a restyle to draw with.
ScrollBar {
    // The colour roles of the scroll area the bar is in.
    property MorphScheme scheme: Theme.themeScheme
}

// SPDX-License-Identifier: Apache-2.0
import QtQuick

// What an input (a text field, a text area, a select) is drawn on: the control height, the control radius, and a
// border that takes the focus colour while the input has focus. A toned input's border is the tone's colour, focused
// or not, so a refused field does not stop looking refused when the user tabs into it, with a 2 px ring of the tone's
// fill outside it; focus shows as the border's width. A read-only input draws on the page's background
// rather than the input fill, so it does not invite typing; it keeps the border and the focus ring, since it still
// takes focus for selection.
Rectangle {
    id: background

    // The scheme of the widget the input belongs to.
    property MorphScheme scheme: Theme.themeScheme
    property bool focused: false
    property bool readOnly: false
    // The `Tone` index the field is outlined in (0 none): its errors, or its evaluation.
    property int tone: 0
    // The screen's focal field: a 2 px accent border at all times and, while focused, a 3 px ring of the accent,
    // faint. A tone wins over the accent, so a refused scan looks refused.
    property bool prominent: false
    readonly property color toneColor: [scheme.controlBorder, scheme.info, scheme.success, scheme.warn, scheme.error][tone]
                                       ?? scheme.controlBorder
    readonly property color toneFill: [scheme.controlSurface, scheme.infoBg, scheme.successBg, scheme.warnBg,
                                       scheme.errorBg][tone] ?? scheme.controlSurface

    // A prominent field keeps the touch height on every window.
    implicitHeight: prominent ? Math.max(Theme.targetHeight, Theme.touchHeight) : Theme.targetHeight
    implicitWidth: Theme.unitWidth * 20
    radius: Theme.radiusControl
    color: readOnly ? Theme.background : scheme.controlSurface
    // Whether the plain outline is stroked in dashes: the surface's `border.style`.
    readonly property bool dashed: scheme.borderDashed && !focused && !prominent && tone === 0
    border.width: focused || prominent ? 2 : dashed ? 0 : 1
    border.color: tone !== 0 ? toneColor : prominent ? scheme.accent : focused ? scheme.focus : scheme.controlBorder

    MorphDashedOutline {
        objectName: "dashedOutline"
        visible: background.dashed
        color: background.scheme.controlBorder
        radius: background.radius
    }

    // The ring: a sibling's outline, since a Rectangle draws nothing outside itself.
    Rectangle {
        objectName: "toneRing"
        z: -1
        anchors.fill: parent
        anchors.margins: background.tone === 0 && background.prominent ? -3 : -2
        visible: background.tone !== 0 || (background.prominent && background.focused)
        radius: background.radius - anchors.margins
        color: background.tone !== 0 ? background.toneFill : Qt.alpha(background.scheme.accent, 0.2)
    }
}

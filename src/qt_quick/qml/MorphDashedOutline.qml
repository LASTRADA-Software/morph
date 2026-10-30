// SPDX-License-Identifier: Apache-2.0
import QtQuick

// A rounded rectangle outlined in dashes, over the item it is a child of. A dash is three line widths long and the
// gap after it two, as a pen's dash pattern counts them. It draws on a Canvas, which strokes at device resolution,
// so the dashes stay crisp on a dense screen.
Canvas {
    id: root

    property color color: "black"
    property real radius: 0
    property real lineWidth: 1

    anchors.fill: parent

    onColorChanged: requestPaint()
    onRadiusChanged: requestPaint()
    onLineWidthChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()

    onPaint: {
        const ctx = getContext("2d")
        ctx.clearRect(0, 0, width, height)
        if (width <= root.lineWidth || height <= root.lineWidth)
            return
        // The stroke is centred on the path, so the path lies half a line width inside the item's edge.
        const inset = root.lineWidth / 2
        const left = inset
        const top = inset
        const right = width - inset
        const bottom = height - inset
        const corner = Math.max(0, Math.min(root.radius - inset, (right - left) / 2, (bottom - top) / 2))
        ctx.beginPath()
        ctx.moveTo(left + corner, top)
        ctx.lineTo(right - corner, top)
        ctx.arc(right - corner, top + corner, corner, -Math.PI / 2, 0)
        ctx.lineTo(right, bottom - corner)
        ctx.arc(right - corner, bottom - corner, corner, 0, Math.PI / 2)
        ctx.lineTo(left + corner, bottom)
        ctx.arc(left + corner, bottom - corner, corner, Math.PI / 2, Math.PI)
        ctx.lineTo(left, top + corner)
        ctx.arc(left + corner, top + corner, corner, Math.PI, 3 * Math.PI / 2)
        ctx.closePath()
        ctx.lineWidth = root.lineWidth
        ctx.strokeStyle = root.color
        ctx.setLineDash([3, 2])
        ctx.stroke()
    }
}

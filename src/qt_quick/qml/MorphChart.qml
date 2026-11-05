// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick

// Points against a numeric axis, with horizontal reference lines, painted on one Canvas from the ChartData the C++
// widget writes as `chart`. Every text arrives formatted; only positions are computed here. The axis labels (the
// reference lines' when three or more label the axis, else the ticks) sit in a gutter at the left, the points' labels
// in a row below the plot, and the legend below that when there are two or more series. A series is told apart by
// its shape as well as its colour, a marked point by a cross no series uses, and a clipped point by an open triangle
// at the edge it lies beyond, so nothing rests on colour alone.
MorphWidget {
    id: root

    // {xMin, xMax, yMin, yMax, yLabel, ticks: [{value, text}], lines: [{value, label, tone, style}],
    //  series: [{label, joined, points: [{x, y, gap, clip, xLabel, tone, note}]}]}; tone -1 is none, clip 0 none,
    //  1 below, 2 above, style 0 solid, 1 dashed, 2 dotted.
    property var chart: ({})
    property string summary: ""

    // A content-sized chart is wide and tall enough to read before any sizing.
    implicitWidth: 40 * Theme.unitWidth
    implicitHeight: 8 * Theme.unitHeight

    defaultName: summary
    defaultRole: Accessible.Chart
    // The summary is read once: as the name when the document names none, else as the description.
    Accessible.description: a11yName !== "" && a11yName !== summary ? summary : ""

    onChartChanged: canvas.requestPaint()
    onSchemeChanged: canvas.requestPaint()

    // A series' colour, in token order.
    function seriesColor(index: int): color {
        switch (index % 4) {
        case 1:
            return root.scheme.chart2
        case 2:
            return root.scheme.chart3
        case 3:
            return root.scheme.chart4
        default:
            return root.scheme.chart1
        }
    }

    Canvas {
        id: canvas
        objectName: "plot"
        anchors.fill: parent

        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()

        // A series' marker, 5 px across: a circle, a square, a triangle or a diamond, in token order.
        function marker(ctx: var, shape: int, x: real, y: real) {
            const r = 2.5
            ctx.beginPath()
            switch (shape % 4) {
            case 1:
                ctx.rect(x - r, y - r, 2 * r, 2 * r)
                break
            case 2:
                ctx.moveTo(x, y - r)
                ctx.lineTo(x + r, y + r)
                ctx.lineTo(x - r, y + r)
                ctx.closePath()
                break
            case 3:
                ctx.moveTo(x, y - r)
                ctx.lineTo(x + r, y)
                ctx.lineTo(x, y + r)
                ctx.lineTo(x - r, y)
                ctx.closePath()
                break
            default:
                ctx.arc(x, y, r, 0, 2 * Math.PI)
                break
            }
            ctx.fill()
        }

        // An open triangle at the plot's edge, pointing out of it: up for a value above the axis, down for below.
        function clipped(ctx: var, x: real, y: real, up: bool) {
            const r = 4
            ctx.beginPath()
            ctx.moveTo(x, up ? y - r : y + r)
            ctx.lineTo(x + r, up ? y + r : y - r)
            ctx.lineTo(x - r, up ? y + r : y - r)
            ctx.closePath()
            ctx.stroke()
        }

        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const data = root.chart
            if (!data || data.series === undefined)
                return
            const unitW = Theme.unitWidth
            const unitH = Theme.unitHeight
            const font = Theme.bodyFont
            // A family Context2D does not know (the platform's "Sans Serif" alias) falls back to the generic one, rather
            // than leaving the canvas on its default font and warning on every paint.
            ctx.font = Math.round(Theme.bodyPixels) + "px \"" + font.family + "\", sans-serif"
            ctx.textBaseline = "middle"

            const byLines = data.lines.length >= 3
            const axisTexts = byLines ? data.lines.map(line => line.label) : data.ticks.map(tick => tick.text)
            let gutter = 0
            for (const text of axisTexts)
                gutter = Math.max(gutter, ctx.measureText(text).width)
            gutter = gutter > 0 ? gutter + unitW : unitW / 2
            let hasXLabels = false
            for (const series of data.series)
                for (const point of series.points)
                    hasXLabels = hasXLabels || point.xLabel !== ""
            const legend = data.series.length >= 2
            const top = (data.yLabel !== "" ? unitH : 0) + unitH / 2
            const bottom = height - (hasXLabels ? unitH : 0) - (legend ? unitH : 0) - unitH / 2
            const left = gutter
            const right = width - unitW
            if (right <= left || bottom <= top)
                return
            const ySpan = data.yMax - data.yMin
            const xSpan = data.xMax - data.xMin
            const py = v => top + (ySpan > 0 ? (data.yMax - v) / ySpan : 0.5) * (bottom - top)
            const px = v => left + (xSpan > 0 ? (v - data.xMin) / xSpan : 0.5) * (right - left)
            // A 1 px line on a pixel row's centre, so it is one row and not a blend of two.
            const crisp = v => Math.round(v - 0.5) + 0.5

            if (data.yLabel !== "") {
                ctx.fillStyle = root.scheme.muted
                ctx.textAlign = "left"
                ctx.fillText(data.yLabel, 0, unitH / 2)
            }

            // The ticks, in the gutter.
            ctx.textAlign = "right"
            if (!byLines) {
                ctx.fillStyle = root.scheme.muted
                for (const tick of data.ticks)
                    ctx.fillText(tick.text, left - unitW / 2, py(tick.value))
            }

            // The reference lines, then their labels: in the gutter when they label the axis, else at their right end.
            ctx.lineWidth = 1
            for (const line of data.lines) {
                const y = crisp(py(line.value))
                ctx.strokeStyle = root.toneOf(line.tone)
                ctx.setLineDash(line.style === 1 ? [4, 3] : line.style === 2 ? [2, 3] : [])
                ctx.beginPath()
                ctx.moveTo(left, y)
                ctx.lineTo(right, y)
                ctx.stroke()
                ctx.fillStyle = root.toneOf(line.tone)
                if (byLines) {
                    ctx.textAlign = "right"
                    ctx.fillText(line.label, left - unitW / 2, y)
                } else {
                    ctx.textAlign = "right"
                    ctx.fillText(line.label, right, y - unitH / 2)
                }
            }
            ctx.setLineDash([])

            // Each series: its line, broken at gaps, then its points.
            for (let index = 0; index < data.series.length; ++index) {
                const series = data.series[index]
                const colour = root.seriesColor(index)
                if (series.joined) {
                    ctx.strokeStyle = colour
                    ctx.lineWidth = 1.5
                    let open = false
                    ctx.beginPath()
                    for (const point of series.points) {
                        if (point.gap) {
                            open = false
                            continue
                        }
                        if (open)
                            ctx.lineTo(px(point.x), py(point.y))
                        else
                            ctx.moveTo(px(point.x), py(point.y))
                        open = true
                    }
                    ctx.stroke()
                }
                ctx.fillStyle = colour
                for (const point of series.points) {
                    if (!point.gap && point.clip === 0 && point.tone < 0)
                        marker(ctx, index, px(point.x), py(point.y))
                }
            }

            // Clipped and marked points over every series, with their notes.
            for (let index = 0; index < data.series.length; ++index) {
                for (const point of data.series[index].points) {
                    if (point.gap)
                        continue
                    const x = px(point.x)
                    const y = py(point.y)
                    const colour = point.tone >= 0 ? root.toneOf(point.tone) : root.seriesColor(index)
                    if (point.clip !== 0) {
                        ctx.strokeStyle = colour
                        ctx.lineWidth = 1.5
                        clipped(ctx, x, y, point.clip === 2)
                    } else if (point.tone >= 0) {
                        ctx.strokeStyle = colour
                        ctx.lineWidth = 2
                        ctx.beginPath()
                        ctx.moveTo(x - 5, y - 5)
                        ctx.lineTo(x + 5, y + 5)
                        ctx.moveTo(x + 5, y - 5)
                        ctx.lineTo(x - 5, y + 5)
                        ctx.stroke()
                    }
                    if (point.note !== "") {
                        ctx.fillStyle = colour
                        const wide = ctx.measureText(point.note).width
                        const flip = x + unitW + wide > width
                        ctx.textAlign = flip ? "right" : "left"
                        ctx.fillText(point.note, flip ? x - unitW : x + unitW, y)
                    }
                }
            }

            // The points' labels below the plot, left to right; one that would overlap the last drawn is skipped, and
            // the newest is always drawn.
            if (hasXLabels) {
                ctx.fillStyle = root.scheme.muted
                ctx.textAlign = "center"
                const labels = []
                for (const series of data.series)
                    for (const point of series.points)
                        if (point.xLabel !== "")
                            labels.push({
                                x: px(point.x),
                                text: point.xLabel,
                                half: ctx.measureText(point.xLabel).width / 2 + unitW / 2
                            })
                labels.sort((a, b) => a.x - b.x)
                const newest = labels[labels.length - 1]
                const y = bottom + unitH
                let reach = -Infinity
                for (let index = 0; index < labels.length - 1; ++index) {
                    const label = labels[index]
                    if (label.x - label.half < reach || label.x + label.half > newest.x - newest.half)
                        continue
                    ctx.fillText(label.text, label.x, y)
                    reach = label.x + label.half
                }
                ctx.fillText(newest.text, newest.x, y)
            }

            // The legend: each series' shape beside its label.
            if (legend) {
                const y = height - unitH / 2
                let x = left
                ctx.textAlign = "left"
                for (let index = 0; index < data.series.length; ++index) {
                    ctx.fillStyle = root.seriesColor(index)
                    marker(ctx, index, x + 3, y)
                    ctx.fillStyle = root.scheme.text
                    ctx.fillText(data.series[index].label, x + 3 + unitW, y)
                    x += 3 + 2 * unitW + ctx.measureText(data.series[index].label).width
                }
            }
        }
    }
}

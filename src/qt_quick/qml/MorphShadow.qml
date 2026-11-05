// SPDX-License-Identifier: Apache-2.0
//
// A soft shadow under its parent: the parent's outline moved by (offsetX, offsetY) and grown by `spread`, blurred as a
// Gaussian of standard deviation `blur / 2` (the CSS `box-shadow` convention). It is drawn as nested rounded
// rectangles, outermost first, so it needs no offscreen texture and no effects module; each layer's alpha is chosen so
// that the layers composited at any point add up to the shadow's alpha times the Gaussian's falloff there. Its cost is
// `layerCount` rectangles, so it is for the few surfaces that float (a dialog, a drawer), not for every card.
pragma ComponentBehavior: Bound
import QtQuick

Item {
    id: root

    // The shadow's colour; its alpha is the strength well inside the outline, where the blur has fully covered.
    property color color: "#14000000"
    property real offsetX: 0
    property real offsetY: 0
    property real blur: 0
    property real spread: 0
    // The corner radius of the outline the shadow follows.
    property real radius: 0
    // How many rectangles draw the falloff: more is smoother.
    property int layerCount: 16

    readonly property real sigma: blur / 2
    // Rectangles drawn: none without a blur, which is a single hard-edged rectangle.
    readonly property int count: sigma > 0 ? Math.max(layerCount, 2) : 1

    // The standard normal's upper-tail quantile: the z with P(Z > z) = @p p (Abramowitz and Stegun 26.2.23, error
    // under 5e-4). Negative for a p above one half.
    function upperQuantile(p: real): real {
        if (p > 0.5)
            return -upperQuantile(1 - p)
        const t = Math.sqrt(-2 * Math.log(p))
        return t - (2.515517 + 0.802853 * t + 0.010328 * t * t) / (1 + 1.432788 * t + 0.189269 * t * t + 0.001308 * t * t * t)
    }

    // The share of the shadow's strength that layers 0..k reach together, 0 for k below the first layer. It grows as
    // the square of the layer's position, not linearly, so the outermost layers are far fainter than a uniform share
    // would make them: a uniform first layer is a visible step of 1/count of the strength at the edge of the whole
    // shadow, where the squared one adds less than one 8-bit level and the shadow fades to nothing.
    function reachedBy(k: int): real {
        return k < 0 ? 0 : Math.pow((k + 1) / count, 2)
    }

    // How far the k-th layer (0 is the outermost) reaches beyond the outline: where the shadow's falloff crosses the
    // middle of the k-th layer's share of its strength, so the layers' steps follow the Gaussian's edge. Negative for
    // a layer inside the outline.
    function extentOf(k: int): real {
        return sigma > 0 ? sigma * upperQuantile((reachedBy(k - 1) + reachedBy(k)) / 2) : 0
    }

    // The alpha the k-th layer is drawn with so that layers 0..k composited with source-over reach the colour's alpha
    // times `reachedBy(k)`: what the layers under it left uncovered is what it adds.
    function alphaOf(k: int): real {
        if (count === 1)
            return color.a
        const reached = color.a * reachedBy(k)
        const below = color.a * reachedBy(k - 1)
        return (reached - below) / (1 - below)
    }

    anchors.fill: parent

    Repeater {
        model: root.count
        delegate: Rectangle {
            id: ring
            required property int index
            readonly property real extent: root.spread + root.extentOf(ring.index)
            x: root.offsetX - ring.extent
            y: root.offsetY - ring.extent
            width: Math.max(0, root.width + 2 * ring.extent)
            height: Math.max(0, root.height + 2 * ring.extent)
            radius: Math.max(0, root.radius + ring.extent)
            color: Qt.rgba(root.color.r, root.color.g, root.color.b, root.alphaOf(ring.index))
        }
    }
}

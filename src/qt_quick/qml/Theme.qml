// SPDX-License-Identifier: Apache-2.0
pragma Singleton
import QtQuick

// The colours, fonts, units and spacing every MorphUi component reads. A unit is the body font's average character
// width horizontally and its line height vertically. The colours, the stale opacity and the spacing are the
// ui-theme/1 defaults; qt_quick::applyTheme sets them from a theme's tokens, and a token the theme lacks keeps its
// default here.
QtObject {
    id: theme

    // The bundle's icon set: icon name to the URL of its SVG; empty while the theme declares none.
    property var icons: ({})
    // Whether the bundle's theme declares an icon set: a node icon it does not name is then reported.
    property bool iconSet: false

    // The URL an icon draws from: the bundle's set's, or empty, for which a component draws today's text or glyph.
    function iconSource(name: string): string {
        const source = name !== "" ? theme.icons[name] : undefined
        return source !== undefined ? source : ""
    }

    // The families `font.family` names, in order of preference; empty draws in the platform font.
    property list<string> fontFamilies: []
    // The platform font: what the sizes derive from when the theme gives none, and the font while it names no family.
    readonly property FontMetrics platformMetrics: FontMetrics {}
    // The theme's body font while it names a family, else the platform font. It is built from the platform metrics'
    // size rather than bound to `bodyFont`, which reads `bodyPixels`, so a unit never depends on what it feeds.
    readonly property FontMetrics metrics: FontMetrics {
        font: theme.fontFamilies.length > 0 ? Qt.font({
            family: theme.fontFamilies[0],
            families: theme.fontFamilies,
            pixelSize: theme.bodyPixels
        }) : theme.platformMetrics.font
    }
    // The families text is drawn in: the theme's, else the platform font's.
    readonly property list<string> families: fontFamilies.length > 0 ? fontFamilies : [platformMetrics.font.family]
    // Body text and controls: the theme's families at the body size, or the platform font untouched.
    readonly property font bodyFont: fontFamilies.length > 0 ? fontOf(bodyPixels, Font.Normal) : platformMetrics.font
    // Emphasised text at the body size: a title, a header, the current step.
    readonly property font strongFont: fontFamilies.length > 0 ? fontOf(bodyPixels, Font.Bold)
                                                               : boldOf(platformMetrics.font)
    // A heading: the families at the heading size, bold.
    readonly property font headingFont: fontOf(headingPixels, Font.Bold)
    // The families `font.mono` names, in order of preference; empty draws in the platform's fixed-pitch font.
    property list<string> monoFontFamilies: []
    // The family of the platform's fixed-pitch font; the backend writes it, since QML cannot ask the font database.
    property string platformMonoFamily: ""
    // The families a `mono` text is drawn in: the theme's, else the platform's fixed-pitch one.
    readonly property list<string> monoFamilies: monoFontFamilies.length > 0
                                                 ? monoFontFamilies
                                                 : platformMonoFamily !== "" ? [platformMonoFamily] : []
    // An identifier: the mono families at the body size; before the backend wrote the platform's family, the
    // generic `monospace` alias.
    readonly property font monoFont: Qt.font({
        family: monoFamilies.length > 0 ? monoFamilies[0] : "monospace",
        families: monoFamilies.length > 0 ? monoFamilies : ["monospace"],
        pixelSize: Math.round(bodyPixels)
    })
    readonly property real unitWidth: metrics.averageCharacterWidth
    readonly property real unitHeight: metrics.height
    readonly property real spacing: unitHeight / 2

    property color text: "#1d1d1f"
    property color muted: "#6e6e73"
    property color error: "#c62828"
    property color success: "#2e7d32"
    property color info: "#1565c0"
    property color warn: "#b26a00"
    property color neutral: "#e5e5ea"
    property color surface: "#ffffff"
    property color border: "#c7c7cc"
    property color selection: "#cfe3ff"
    property color accent: "#1565c0"
    property color focus: "#1565c0"
    // A tone's fill, outline and marker: `color.<ok|warn|err|info>.<bg|border|dot>`.
    property color successBg: "#e8f5e9"
    property color successBorder: "#a5d6a7"
    property color successDot: "#2e7d32"
    property color warnBg: "#fff4e5"
    property color warnBorder: "#ffcc80"
    property color warnDot: "#b26a00"
    property color errorBg: "#fdecea"
    property color errorBorder: "#ef9a9a"
    property color errorDot: "#c62828"
    property color infoBg: "#e3f2fd"
    property color infoBorder: "#90caf9"
    property color infoDot: "#1565c0"
    // A table's header background; transparent draws no fill, matching a table with no theme-set header.
    property color tableHeader: "#00000000"
    // A chart's series colours, in drawing order: `color.chart.1` to `color.chart.4`.
    property color chart1: "#0072b2"
    property color chart2: "#c25400"
    property color chart3: "#00805e"
    property color chart4: "#a8508a"
    property real staleOpacity: 0.55
    // The window behind every surface, the corner radii, the height of an input or a button, and the font sizes, in
    // px. A font size of 0 is derived from the platform font: the body is its size, a heading 1.3 times the body.
    property color background: "#f2f3f7"
    property real radiusControl: 6
    property real radiusCard: 10
    property real controlHeight: 32
    // The least height of a control while `touchTargets` holds: `control.touchHeight`.
    property real touchHeight: 44
    // Whether controls are sized as touch targets: the backend sets it while the window's width class is compact.
    property bool touchTargets: false
    // The height every control is drawn at: `controlHeight`, or at least `touchHeight` while `touchTargets` holds.
    readonly property real targetHeight: touchTargets ? Math.max(controlHeight, touchHeight) : controlHeight
    property real bodySize: 0
    property real headingSize: 0
    readonly property real bodyPixels: bodySize > 0 ? bodySize : platformMetrics.font.pixelSize > 0
                                                                   ? platformMetrics.font.pixelSize
                                                                   : platformMetrics.height * 0.75
    readonly property real headingPixels: headingSize > 0 ? headingSize : bodyPixels * 1.3

    // @p base in bold, at its own size: a pixel size, or else a point size, whichever it was given in.
    function boldOf(base: font): font {
        const spec = {
            family: base.family,
            weight: Font.Bold
        }
        if (base.pixelSize > 0)
            spec.pixelSize = base.pixelSize
        else
            spec.pointSize = base.pointSize
        return Qt.font(spec)
    }

    // The text font at a size and a weight, in the families text is drawn in.
    function fontOf(pixels: real, weight: int): font {
        return Qt.font({
            family: theme.families[0],
            families: theme.families,
            pixelSize: Math.round(pixels),
            weight: weight
        })
    }
    // The card's shadow: `elevation.card`.
    property color cardShadowColor: "#14000000"
    property real cardShadowOffsetX: 0
    property real cardShadowOffsetY: 2
    property real cardShadowBlur: 10
    property real cardShadowSpread: 0

    // The spacing scale in px, one entry per spacing step 0 to 5: the `space.<step>` defaults.
    property list<real> space: [0, 4, 8, 12, 16, 24]

    // A spacing step's distance; a step outside 0 to 5 is the nearest end, as the mount already clamps it.
    function spaceOf(step: int): real {
        return space[Math.max(0, Math.min(space.length - 1, step))]
    }

    // The theme's own scheme, and the built-in surfaces: the page screens sit on, a raised card, the dark navigator
    // column and the top bar. A role a built-in surface does not set follows the theme's. A theme index's surface of
    // the same name replaces one, and any other name gets a scheme of its own (`scheme`).
    readonly property MorphScheme themeScheme: MorphScheme {
        surface: theme.surface
        text: theme.text
        muted: theme.muted
        accent: theme.accent
        focus: theme.focus
        selection: theme.selection
        border: theme.border
        neutral: theme.neutral
        error: theme.error
        success: theme.success
        info: theme.info
        warn: theme.warn
        successBg: theme.successBg
        successBorder: theme.successBorder
        successDot: theme.successDot
        warnBg: theme.warnBg
        warnBorder: theme.warnBorder
        warnDot: theme.warnDot
        errorBg: theme.errorBg
        errorBorder: theme.errorBorder
        errorDot: theme.errorDot
        infoBg: theme.infoBg
        infoBorder: theme.infoBorder
        infoDot: theme.infoDot
        tableHeader: theme.tableHeader
        chart1: theme.chart1
        chart2: theme.chart2
        chart3: theme.chart3
        chart4: theme.chart4
    }
    readonly property MorphScheme pageScheme: MorphScheme {
        surface: theme.background
        text: theme.text
        muted: theme.muted
        accent: theme.accent
        focus: theme.focus
        selection: theme.selection
        border: theme.border
        neutral: theme.neutral
        error: theme.error
        success: theme.success
        info: theme.info
        warn: theme.warn
        successBg: theme.successBg
        successBorder: theme.successBorder
        successDot: theme.successDot
        warnBg: theme.warnBg
        warnBorder: theme.warnBorder
        warnDot: theme.warnDot
        errorBg: theme.errorBg
        errorBorder: theme.errorBorder
        errorDot: theme.errorDot
        infoBg: theme.infoBg
        infoBorder: theme.infoBorder
        infoDot: theme.infoDot
        tableHeader: theme.tableHeader
        chart1: theme.chart1
        chart2: theme.chart2
        chart3: theme.chart3
        chart4: theme.chart4
    }
    readonly property MorphScheme cardScheme: MorphScheme {
        surface: theme.surface
        text: theme.text
        muted: theme.muted
        accent: theme.accent
        focus: theme.focus
        selection: theme.selection
        border: theme.border
        neutral: theme.neutral
        error: theme.error
        success: theme.success
        info: theme.info
        warn: theme.warn
        successBg: theme.successBg
        successBorder: theme.successBorder
        successDot: theme.successDot
        warnBg: theme.warnBg
        warnBorder: theme.warnBorder
        warnDot: theme.warnDot
        errorBg: theme.errorBg
        errorBorder: theme.errorBorder
        errorDot: theme.errorDot
        infoBg: theme.infoBg
        infoBorder: theme.infoBorder
        infoDot: theme.infoDot
        tableHeader: theme.tableHeader
        chart1: theme.chart1
        chart2: theme.chart2
        chart3: theme.chart3
        chart4: theme.chart4
    }
    readonly property MorphScheme sidebarScheme: MorphScheme {
        surface: "#1f2430"
        text: "#c9ccd6"
        muted: "#7d8396"
        accent: "#4c8dff"
        focus: theme.focus
        selection: "#2f3646"
        border: "#2f3646"
        neutral: theme.neutral
        error: theme.error
        success: theme.success
        info: theme.info
        warn: theme.warn
        successBg: theme.successBg
        successBorder: theme.successBorder
        successDot: theme.successDot
        warnBg: theme.warnBg
        warnBorder: theme.warnBorder
        warnDot: theme.warnDot
        errorBg: theme.errorBg
        errorBorder: theme.errorBorder
        errorDot: theme.errorDot
        infoBg: theme.infoBg
        infoBorder: theme.infoBorder
        infoDot: theme.infoDot
        tableHeader: theme.tableHeader
        chart1: theme.chart1
        chart2: theme.chart2
        chart3: theme.chart3
        chart4: theme.chart4
    }
    readonly property MorphScheme toolbarScheme: MorphScheme {
        surface: theme.surface
        text: theme.text
        muted: theme.muted
        accent: theme.accent
        focus: theme.focus
        selection: theme.selection
        border: "#e3e5ea"
        neutral: theme.neutral
        error: theme.error
        success: theme.success
        info: theme.info
        warn: theme.warn
        successBg: theme.successBg
        successBorder: theme.successBorder
        successDot: theme.successDot
        warnBg: theme.warnBg
        warnBorder: theme.warnBorder
        warnDot: theme.warnDot
        errorBg: theme.errorBg
        errorBorder: theme.errorBorder
        errorDot: theme.errorDot
        infoBg: theme.infoBg
        infoBorder: theme.infoBorder
        infoDot: theme.infoDot
        tableHeader: theme.tableHeader
        chart1: theme.chart1
        chart2: theme.chart2
        chart3: theme.chart3
        chart4: theme.chart4
    }
    property var extraSchemes: ({})
    readonly property Component schemeComponent: Component {
        MorphScheme {}
    }

    // The scheme of a surface name: a built-in one, one a theme declared, or, for the empty name, the theme's own.
    function scheme(name: string): MorphScheme {
        switch (name) {
        case "":
            return themeScheme
        case "page":
            return pageScheme
        case "card":
            return cardScheme
        case "sidebar":
            return sidebarScheme
        case "toolbar":
            return toolbarScheme
        default:
            if (!(name in extraSchemes))
                extraSchemes[name] = schemeComponent.createObject(theme) as MorphScheme
            return extraSchemes[name] as MorphScheme
        }
    }

    // The same, for applyTheme: a function C++ calls returns its value as a QVariant only when it declares no types.
    function schemeObject(name) {
        return scheme(name)
    }

    // Whether a surface name is one the theme draws a background for.
    function draws(name: string): bool {
        return name === "page" || name === "card" || name === "sidebar" || name === "toolbar" || (name in extraSchemes)
    }

    // A text role's colour: Normal, Muted, Heading, Error, Success.
    function textColor(role: int): color {
        switch (role) {
        case 1:
            return muted
        case 3:
            return error
        case 4:
            return success
        default:
            return text
        }
    }

    // A tone's colour: Neutral, Info, Ok, Warn, Err.
    function tone(index: int): color {
        switch (index) {
        case 1:
            return info
        case 2:
            return success
        case 3:
            return warn
        case 4:
            return error
        default:
            return muted
        }
    }
}

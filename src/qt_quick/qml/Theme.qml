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
    // The text glyph of each framework icon name that has one, the glyphs the terminal draws: icon name to glyph.
    property var iconGlyphs: ({})
    // Whether the bundle's theme declares an icon set: a node icon it does not name is then reported.
    property bool iconSet: false

    // The bundle's icon set as SVG text: icon name to text; empty while the theme declares none.
    property var iconSvgs: ({})
    // Icons already drawn in a colour, by name and colour.
    property var tintedIcons: ({})
    onIconSvgsChanged: theme.tintedIcons = ({})

    // The URL an icon draws from: the bundle's set's, or empty, for which a component draws today's text or glyph.
    function iconSource(name: string): string {
        const source = name !== "" ? theme.icons[name] : undefined
        return source !== undefined ? source : ""
    }

    // The URL an icon draws from, in the colour of the control that shows it: every `currentColor` of the icon's SVG
    // becomes @p color, since Qt's SVG image has no colour of its own to resolve it against. An icon that never paints
    // `currentColor` is the set's own URL, and one the set lacks is empty.
    function iconSourceIn(name: string, color: color): string {
        const svg = name !== "" ? theme.iconSvgs[name] : undefined
        if (svg === undefined || svg.indexOf("currentColor") < 0)
            return theme.iconSource(name)
        const hex = "#" + [color.r, color.g, color.b].map(channel => {
            const part = Math.round(channel * 255).toString(16)
            return part.length < 2 ? "0" + part : part
        }).join("")
        const key = name + "|" + hex
        if (!(key in theme.tintedIcons))
            theme.tintedIcons[key] = "data:image/svg+xml;base64," +
                    Qt.btoa(Uint8Array.from(unescape(encodeURIComponent(svg.split("currentColor").join(hex))),
                                            ch => ch.charCodeAt(0)).buffer)
        return theme.tintedIcons[key]
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
    // Emphasised text at the body size: a title, a header, the current step. Every strong font draws at
    // `strongWeight`.
    readonly property font strongFont: fontFamilies.length > 0 ? fontOf(bodyPixels, strongWeight)
                                                               : boldOf(platformMetrics.font)
    // A heading: the families at the heading size, strong.
    readonly property font headingFont: fontOf(headingPixels, strongWeight)
    // The small size, regular and strong: a table header (strong), a footnote, a timestamp, a tag.
    readonly property font smallFont: fontOf(smallPixels, Font.Normal)
    readonly property font smallStrongFont: fontOf(smallPixels, strongWeight)
    // An overline: a short label over a group. It is drawn as a section (body size, strong) unless `overlineCaps`, when
    // it is the small size, strong, in capitals, letter-spaced by 0.05 times that size. The font is built whole, with
    // its capitalisation and letter spacing, since a font rebuilt from it keeps only what it is given.
    readonly property font overlineFont: overlineCaps ? Qt.font({
        family: families[0],
        families: families,
        pixelSize: Math.round(smallPixels),
        weight: strongWeight,
        capitalization: Font.AllUppercase,
        letterSpacing: 0.05 * smallPixels
    }) : strongFont
    // A field's caption above its control.
    readonly property font captionFont: fontOf(captionPixels, Font.Normal)
    // A figure that stands out, such as a KPI's number: the display size, strong.
    readonly property font displayFont: fontOf(displayPixels, strongWeight)
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
        pixelSize: Math.round(monoPixels)
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
    // The neutral tone's text, fill, outline and marker: `color.neutral` and its `.bg`, `.border` and `.dot`.
    property color neutral: "#6e6e73"
    property color neutralBg: "#266e6e73"
    property color neutralBorder: "#6e6e73"
    property color neutralDot: "#6e6e73"
    // The fill under a hovered or pressed item, and the unfilled part of a progress bar: `color.hover`, `color.track`.
    property color hover: "#141d1d1f"
    property color track: "#e5e5ea"
    // The dim behind a modal dialog or drawer: `color.scrim`.
    property color scrim: "#4d000000"
    // A control's fill and outline, and the rule between rows: `color.controlSurface`, `color.controlBorder`,
    // `color.divider`.
    property color controlSurface: "#ffffff"
    property color controlBorder: "#c7c7cc"
    property color divider: "#dcdce0"
    // Whether outlines are stroked in dashes: `border.style`.
    property bool borderDashed: false
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
    // A tag's, a chip's and a select option's corners: `radius.badge`.
    property real radiusBadge: 4
    property real controlHeight: 32
    // The least height of a control while `touchTargets` holds: `control.touchHeight`.
    property real touchHeight: 44
    // Whether controls are sized as touch targets: the backend sets it while the window's width class is compact.
    property bool touchTargets: false
    // The height every control is drawn at: `controlHeight`, or at least `touchHeight` while `touchTargets` holds.
    readonly property real targetHeight: touchTargets ? Math.max(controlHeight, touchHeight) : controlHeight
    // A table cell's and a header label's inset: `table.cellPaddingX`, `table.cellPaddingY`.
    property real tableCellPaddingX: 8
    property real tableCellPaddingY: 4
    property real bodySize: 0
    property real headingSize: 0
    // The other sizes, derived from the body size while 0: `font.size.small`, `.caption`, `.mono` and `.display`.
    property real smallSize: 0
    property real captionSize: 0
    property real monoSize: 0
    property real displaySize: 0
    // The weight of every strong font: `font.weight.strong`.
    property int strongWeight: Font.Bold
    // Whether an icon-only push button or menu trigger that gives no shape is drawn square: `shape.iconOnly`.
    property bool iconOnlySquare: false
    // Whether a collapsible section's and a collapsible panel's header draws its chevron: `collapsible.marker`.
    property bool collapsibleMarker: true
    // Whether the `overline` text role is drawn in small capitals: `font.overline.case`.
    property bool overlineCaps: false
    readonly property real bodyPixels: bodySize > 0 ? bodySize : platformMetrics.font.pixelSize > 0
                                                                   ? platformMetrics.font.pixelSize
                                                                   : platformMetrics.height * 0.75
    readonly property real headingPixels: headingSize > 0 ? headingSize : bodyPixels * 1.3
    readonly property real smallPixels: smallSize > 0 ? smallSize : Math.round(bodyPixels * 0.85)
    readonly property real captionPixels: captionSize > 0 ? captionSize : Math.round(bodyPixels * 0.92)
    readonly property real monoPixels: monoSize > 0 ? monoSize : bodyPixels
    readonly property real displayPixels: displaySize > 0 ? displaySize : Math.round(bodyPixels * 1.7)

    // @p base in bold, at its own size: a pixel size, or else a point size, whichever it was given in.
    function boldOf(base: font): font {
        const spec = {
            family: base.family,
            weight: strongWeight
        }
        if (base.pixelSize > 0)
            spec.pixelSize = base.pixelSize
        else
            spec.pointSize = base.pointSize
        return Qt.font(spec)
    }

    // @p base, drawn in @p families, with tabular figures when @p tabular: every digit one width, so numbers line up
    // in a column.
    function figuresOf(base: font, families: list<string>, tabular: bool): font {
        if (!tabular)
            return base
        const spec = {
            family: base.family,
            families: families.length > 0 ? families : [base.family],
            weight: base.weight,
            italic: base.italic,
            capitalization: base.capitalization,
            letterSpacing: base.letterSpacing,
            features: {
                "tnum": 1
            }
        }
        if (base.pixelSize > 0)
            spec.pixelSize = base.pixelSize
        else
            spec.pointSize = base.pointSize
        return Qt.font(spec)
    }

    // The `mono` role's families at a size and a weight: an identifier in a control.
    function monoFontOf(pixels: real, weight: int): font {
        return Qt.font({
            family: monoFamilies.length > 0 ? monoFamilies[0] : "monospace",
            families: monoFamilies.length > 0 ? monoFamilies : ["monospace"],
            pixelSize: Math.round(pixels),
            weight: weight
        })
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

    // A popup's shadow: `elevation.popup`.
    property color popupShadowColor: "#24000000"
    property real popupShadowOffsetX: 0
    property real popupShadowOffsetY: 8
    property real popupShadowBlur: 24
    property real popupShadowSpread: 0

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
        neutralBg: theme.neutralBg
        neutralBorder: theme.neutralBorder
        neutralDot: theme.neutralDot
        hover: theme.hover
        track: theme.track
        controlSurface: theme.controlSurface
        controlBorder: theme.controlBorder
        divider: theme.divider
        borderDashed: theme.borderDashed
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
        neutralBg: theme.neutralBg
        neutralBorder: theme.neutralBorder
        neutralDot: theme.neutralDot
        hover: theme.hover
        track: theme.track
        controlSurface: theme.controlSurface
        controlBorder: theme.controlBorder
        divider: theme.divider
        borderDashed: theme.borderDashed
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
        neutralBg: theme.neutralBg
        neutralBorder: theme.neutralBorder
        neutralDot: theme.neutralDot
        hover: theme.hover
        track: theme.track
        controlSurface: theme.controlSurface
        controlBorder: theme.controlBorder
        divider: theme.divider
        borderDashed: theme.borderDashed
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
        neutral: "#7d8396"
        neutralBg: "#267d8396"
        neutralBorder: "#7d8396"
        neutralDot: "#7d8396"
        hover: "#14ffffff"
        track: "#2f3646"
        controlSurface: "#2a3040"
        controlBorder: "#3a4152"
        divider: "#2f3646"
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
        neutralBg: theme.neutralBg
        neutralBorder: theme.neutralBorder
        neutralDot: theme.neutralDot
        hover: theme.hover
        track: theme.track
        controlSurface: theme.controlSurface
        controlBorder: theme.controlBorder
        divider: theme.divider
        borderDashed: theme.borderDashed
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
    readonly property MorphScheme barScheme: MorphScheme {
        surface: theme.surface
        text: theme.text
        muted: theme.muted
        accent: theme.accent
        focus: theme.focus
        selection: theme.selection
        border: theme.border
        neutral: theme.neutral
        neutralBg: theme.neutralBg
        neutralBorder: theme.neutralBorder
        neutralDot: theme.neutralDot
        hover: theme.hover
        track: theme.track
        controlSurface: theme.controlSurface
        controlBorder: theme.controlBorder
        divider: theme.divider
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
        case "bar":
            return barScheme
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
        return name === "page" || name === "card" || name === "sidebar" || name === "toolbar" || name === "bar" || (name in extraSchemes)
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
            return neutral
        }
    }

    // The mono face's character advance: the width unit of a monospace column, whose characters are each this wide.
    readonly property FontMetrics monoMetrics: FontMetrics {
        font: theme.monoFont
    }
    // `advanceWidth` is a call, which a binding does not follow, so the font is read for the binding to follow it.
    readonly property real monoAdvance: monoMetrics.font.pixelSize > 0 ? monoMetrics.advanceWidth("0") : 0
}

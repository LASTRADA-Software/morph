// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The root of every MorphUi component: what every widget kind shares. The C++ widget writes these properties and
// hears the user through `host`, its relay.
Item {
    id: root

    // The relay user actions are reported through: `host.fire(event, payload)` and `host.ask(question, payload)`.
    property var host: null
    // Sizing per dimension: 0 content, 1 fixed (amount units), 2 stretch (amount is the weight), 3 percent (amount is
    // the share of the width inside the padding of the row or splitter the widget is in; a width only).
    property int widthKind: 0
    property int widthAmount: 0
    property int heightKind: 0
    property int heightAmount: 0
    // The bounds the extent is clamped into, in units; -1 for none.
    // Where the widget sits across its container's axis: its own `alignSelf` (−1 none), else the container's
    // `crossAlign` (0 stretch, 1 start, 2 center, 3 end); `alignAcross` says which way the container places it (0 not
    // at all, 1 horizontally in a column, 2 vertically in a row or grid). The container writes the last two.
    property int alignSelf: -1
    property int crossAlign: 0
    property int alignAcross: 0
    readonly property int placedAlign: alignSelf >= 0 ? alignSelf : crossAlign
    // Whether the keyboard focus is on the accessible proxy of an action drawn as a link in this widget's runs.
    property bool linkFocused: false
    // Whether the widget's sides lie at its container's edges, past the container's horizontal padding: the container
    // gives that padding as `childBleedInset`, and a layout honours the negative margins it becomes.
    property bool bleed: false
    readonly property real bleedInset: bleedInsetOf(root.parent)
    // The `childBleedInset` of a container, 0 for a parent that gives none. The parent is passed as a `var` because
    // only some containers declare the property, and a typed `Item` lookup of it does not lint.
    function bleedInsetOf(holder: var): real {
        const inset = holder ? holder["childBleedInset"] : undefined
        return inset !== undefined ? inset : 0
    }
    // The width this widget's content cannot be narrower than, in pixels; -1 for none. A container binds it to its
    // padding and its content's minimum, so an ancestor sees content that does not fit. A document's `minWidth` wins.
    property real contentMinimumWidth: -1
    // False for a widget whose container places it at a width of its own choosing (a table's cell, at its column's),
    // which must not be pushed wider by its content.
    property bool honoursContentMinimum: true
    // The narrowest the widget can be laid out without losing content, in pixels: a text that wraps can be as narrow as
    // its widest word. -1 for a widget that cannot be narrower than its natural width (a button, a fixed-width input).
    // A wrapping row counts each child by the lesser of its basis and this.
    property real shrinkWidth: -1
    // The width the widget asks for however wide it is laid out, in pixels; -1 when it has none apart from its
    // `implicitWidth`. A widget whose `implicitWidth` follows the width it is given (a table, a text that wraps rich
    // text) states here what it would ask for if it did not, so a table that sizes its columns by their cells'
    // widths does not feed its own result back to itself.
    property real widthFreeNatural: -1
    // The table that sizes its column by this widget when it is a cell, and the column; the table's cell reports
    // through them (`reportCell`) whenever the widget's widths change.
    property var measuredBy: null
    property int measuredIndex: -1
    function reportMeasure() {
        if (root.measuredBy && root.visible)
            root.measuredBy.reportCell(root.measuredIndex, root)
    }
    onShrinkWidthChanged: root.reportMeasure()
    onWidthFreeNaturalChanged: root.reportMeasure()
    property int minWidthUnits: -1
    property int maxWidthUnits: -1
    property int minHeightUnits: -1
    property int maxHeightUnits: -1
    property int columnSpan: 1
    // Whether a content-sized dimension fills the container: the container sets them, from its kind and axis.
    property bool fillsWidth: false
    property bool fillsHeight: false
    // False for a kind whose width is its label (a button, a badge, a checkbox): it keeps that width at the start of
    // the line it fills, as the terminal draws it.
    property bool fillsAcross: true
    // Whether the widget is round: its corners are half its height, and so are its surface's.
    property bool round: false
    // Whether the widget is a square button for a glyph: the theme's corners, at least as wide as tall.
    property bool square: false
    property string tooltip: ""
    // Whether C++ listens for the pointer coming to rest here.
    property bool hoverWatched: false
    property string a11yName: ""
    property int a11yRole: 0
    // What assistive technology reads when the document names no name or role: a kind's own text and role.
    property string defaultName: ""
    property int defaultRole: Accessible.Client
    readonly property string accessibleName: a11yName !== "" ? a11yName : defaultName
    readonly property int accessibleRole: a11yRole !== 0 ? a11yRole : defaultRole
    // The item that is the widget's one accessible node when the root is not: an input's control, which carries
    // the name, the role, the test id and the text or value a driver types into or sets. The control binds
    // `accessibleName` and `accessibleRole` itself, and the root then presents no node of its own.
    property Item accessibleItem: null
    property string surface: ""
    // The radius of a frame the widget draws over its own surface, or -1 when it draws none. The surface's fill
    // takes it, so the fill does not show outside the frame's rounded corners.
    property real frameRadius: -1
    // The corner radius of the surface's background.
    readonly property real surfaceRadius: round ? height / 2 : frameRadius >= 0 ? frameRadius
                                          : surface === "card" ? Theme.radiusCard : 0
    // The colour roles this widget draws with: its own surface's scheme when it has a surface, else the one its
    // container passed down when it adopted it (the theme's at the roots).
    property MorphScheme inheritedScheme: Theme.themeScheme
    readonly property MorphScheme scheme: surface !== "" ? Theme.scheme(surface) : inheritedScheme
    // A drag key as text (`i:<int64>` or `s:<text>`); empty when the widget cannot be dragged.
    property string dragKey: ""
    property bool dropEnabled: false
    property list<string> chords: []
    // The item keyboard focus goes to.
    property Item focusTarget: root

    // A text role's colour in this widget's scheme: Normal, Muted, Heading, Error, Success, Section, Mono, Overline.
    function textColorOf(role: int): color {
        switch (role) {
        case 1:
        case 5:
        case 9:
            return root.scheme.muted
        case 3:
            return root.scheme.error
        case 4:
            return root.scheme.success
        default:
            return root.scheme.text
        }
    }

    // A tone's colour in this widget's scheme: Neutral, Info, Ok, Warn, Err.
    function toneOf(index: int): color {
        switch (index) {
        case 1:
            return root.scheme.info
        case 2:
            return root.scheme.success
        case 3:
            return root.scheme.warn
        case 4:
            return root.scheme.error
        default:
            return root.scheme.neutral
        }
    }

    // A colour as a CSS hex colour, which rich text reads: `#rrggbb`.
    function cssColor(value: color): string {
        const hex = channel => ("0" + Math.round(channel * 255).toString(16)).slice(-2)
        return "#" + hex(value.r) + hex(value.g) + hex(value.b)
    }

    // The rich text of a list of runs, each `{text, strong, mono, tone}` with its text already escaped, in one
    // `pre-wrap` span so line feeds and runs of spaces stay as a plain text keeps them. A strong run is drawn at the
    // theme's strong weight, a mono run in the theme's mono families and a toned run in its tone's colour in this
    // widget's scheme. It reads the theme and the scheme when it is evaluated, so a binding to it follows both.
    function runsHtml(runs: var): string {
        const quoted = Array.from(Theme.monoFamilies).map(family => "'" + family.replace(/\\/g, "\\\\").replace(/'/g, "\\'") + "'")
        const monoFamily = (quoted.length > 0 ? quoted.join(",") : "monospace")
            .replace(/&/g, "&amp;").replace(/"/g, "&quot;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
        let html = "<span style=\"white-space:pre-wrap\">"
        for (const run of runs) {
            let style = ""
            if (run.strong) {
                style += "font-weight:" + Theme.textWeightOf(Theme.strongWeight) + ";"
            }
            if (run.mono) {
                style += "font-family:" + monoFamily + ";"
            }
            if (run.tone !== 0) {
                style += "color:" + root.cssColor(root.toneOf(run.tone)) + ";"
            }
            const markup = style === "" ? run.text : "<span style=\"" + style + "\">" + run.text + "</span>"
            // The banner's action: a link the label's `linkActivated` fires, in the accent and underlined. While the
            // accessible proxy has the focus, its markup is drawn on the focus colour so a keyboard user sees where
            // the focus is. The inner span sets the colour because an anchor takes the style's link colour otherwise.
            const linkStyle = "color:" + root.cssColor(root.linkFocused ? root.scheme.surface : root.scheme.accent)
                + ";text-decoration:underline;"
                + (root.linkFocused ? "background-color:" + root.cssColor(root.scheme.focus) + ";" : "")
            html += run.action
                ? "<a href=\"action\"><span style=\"" + linkStyle + "\">" + run.text + "</span></a>"
                : markup
        }
        return html + "</span>"
    }

    // A tone's fill in this widget's scheme: `color.<tone>.bg`.
    function toneBgOf(index: int): color {
        switch (index) {
        case 1:
            return root.scheme.infoBg
        case 2:
            return root.scheme.successBg
        case 3:
            return root.scheme.warnBg
        case 4:
            return root.scheme.errorBg
        default:
            return root.scheme.neutralBg
        }
    }

    // A tone's outline in this widget's scheme: `color.<tone>.border`.
    function toneBorderOf(index: int): color {
        switch (index) {
        case 1:
            return root.scheme.infoBorder
        case 2:
            return root.scheme.successBorder
        case 3:
            return root.scheme.warnBorder
        case 4:
            return root.scheme.errorBorder
        default:
            return root.scheme.neutralBorder
        }
    }

    // A tone's marker colour in this widget's scheme: `color.<tone>.dot`.
    function toneDotOf(index: int): color {
        switch (index) {
        case 1:
            return root.scheme.infoDot
        case 2:
            return root.scheme.successDot
        case 3:
            return root.scheme.warnDot
        case 4:
            return root.scheme.errorDot
        default:
            return root.scheme.neutralDot
        }
    }

    function fire(event: string, payload: var) {
        if (root.host)
            root.host.fire(event, payload)
    }

    function question(name: string, payload: var): bool {
        return root.host ? root.host.ask(name, payload) : false
    }

    // Runs the chord a key is, when this widget declares it; returns whether it did. A control calls it before its
    // own handling of a key, so a chord declared on the very node takes that key from the control.
    function takeOwnChord(event: var): bool {
        return root.chords.length > 0 && root.question("chord", [event.key, event.modifiers])
    }

    // A content child under an alignment other than stretch keeps its own extent across; a stretch child keeps its
    // fill, and its maximum with the alignment places it.
    // Whether this widget hands the width it has to each widget in it, as a row of actions in a sheet's footer does:
    // the widget's content item holds them, so each takes a share of the row's spare width, whatever its own sizing.
    property bool sharesWidth: false
    readonly property bool takesShare: parent !== null && ((parent.parent as MorphWidget)?.sharesWidth ?? false)
    Layout.fillWidth: takesShare || widthKind === 2 ||
                      (widthKind === 0 && fillsWidth && fillsAcross && !(alignAcross === 1 && placedAlign !== 0))
    Layout.fillHeight: heightKind === 2 || (heightKind === 0 && fillsHeight && !(alignAcross === 2 && placedAlign !== 0))
    Layout.alignment: {
        if (alignAcross === 0 || placedAlign === 0)
            return 0
        if (alignAcross === 1)
            return placedAlign === 1 ? Qt.AlignLeft : placedAlign === 2 ? Qt.AlignHCenter : Qt.AlignRight
        return placedAlign === 1 ? Qt.AlignTop : placedAlign === 2 ? Qt.AlignVCenter : Qt.AlignBottom
    }
    Layout.horizontalStretchFactor: widthKind === 2 ? widthAmount : -1
    Layout.verticalStretchFactor: heightKind === 2 ? heightAmount : -1
    // The width inside a stack's padding, or -1 for a parent that is not a stack. The parameter is untyped because only a
    // stack declares `insetX`.
    function roomOf(stack): real {
        return stack.insetX !== undefined ? stack.width - 2 * stack.insetX : -1
    }
    // A percent width is a share of the stack two levels up (the stack, its layout, this widget), read by its width and
    // padding, never of the layout between them: in a row packed at its natural length the layout's width is its own
    // implicit width, which includes this share.
    readonly property real shareWidth: {
        const stack = parent ? parent.parent : null
        const room = stack ? root.roomOf(stack) : -1
        return widthKind === 3 && room >= 0 ? widthAmount / 100 * room : -1
    }
    // A percent child is not squeezed below its share by a wide sibling: its minimum is the share (held to its maximum).
    // A share of a whole is left to give way, or a row of it beside anything else would grow without end, each width
    // asking for the whole of the one before.
    readonly property real shareFloor: {
        if (widthKind !== 3 || widthAmount >= 100 || shareWidth < 0)
            return -1
        return maxWidthUnits >= 0 ? Math.min(shareWidth, maxWidthUnits * Theme.unitWidth) : shareWidth
    }
    // The least width without a share: the document's `minWidth`, else the content's minimum where it is honoured.
    readonly property real baseMinimumWidth: minWidthUnits >= 0 ? minWidthUnits * Theme.unitWidth
                                                               : honoursContentMinimum ? contentMinimumWidth : -1
    Layout.preferredWidth: widthKind === 1 ? widthAmount * Theme.unitWidth : widthKind === 3 ? shareWidth : -1
    Layout.preferredHeight: heightKind === 1 ? heightAmount * Theme.unitHeight : -1
    Layout.minimumWidth: Math.max(baseMinimumWidth, shareFloor)
    Layout.maximumWidth: maxWidthUnits >= 0 ? maxWidthUnits * Theme.unitWidth : -1
    Layout.minimumHeight: minHeightUnits >= 0 ? minHeightUnits * Theme.unitHeight : -1
    Layout.maximumHeight: maxHeightUnits >= 0 ? maxHeightUnits * Theme.unitHeight : -1
    // A pane of a splitter is held within the bounds the document gives it, a drag included.
    SplitView.minimumWidth: minWidthUnits >= 0 ? minWidthUnits * Theme.unitWidth : 0
    SplitView.maximumWidth: maxWidthUnits >= 0 ? maxWidthUnits * Theme.unitWidth : Infinity
    SplitView.minimumHeight: minHeightUnits >= 0 ? minHeightUnits * Theme.unitHeight : 0
    SplitView.maximumHeight: maxHeightUnits >= 0 ? maxHeightUnits * Theme.unitHeight : Infinity
    Layout.columnSpan: columnSpan
    Layout.leftMargin: bleed ? -bleedInset : 0
    Layout.rightMargin: bleed ? -bleedInset : 0

    Accessible.name: accessibleName
    Accessible.role: accessibleRole
    Accessible.ignored: accessibleItem !== null

    // The tooltip, made the first time the widget has one: the attached ToolTip's content item reads markup, and a
    // tooltip is a document's text.
    property ToolTip tip: null
    // What the tooltip says: the node's own `tooltip`, unless a kind has something to say when it has none.
    property string tooltipText: tooltip
    onTooltipTextChanged: {
        if (root.tooltipText !== "" && root.tip === null)
            root.tip = tipComponent.createObject(root) as ToolTip
    }

    Component {
        id: tipComponent

        ToolTip {
            id: tipPopup
            objectName: "tooltip"
            text: root.tooltipText
            visible: root.tooltipText !== "" && hover.hovered
            delay: 600
            contentItem: Text {
                text: tipPopup.text
                textFormat: Text.PlainText
                font: tipPopup.font
                wrapMode: Text.Wrap
                color: tipPopup.palette.toolTipText
            }
        }
    }

    // What a container's content needs across its width and the width it has inside its padding, both in pixels; a
    // container that checks binds them, and the others leave `needWidth` at -1.
    property real needWidth: -1
    property real haveWidth: 0
    // The kind the report names the container by: `row`, `column`, `panel` or `table`.
    property string overflowKind: ""
    // Whether the container has reported its overflow; it reports once while it is mounted.
    property bool overflowReported: false
    // Whether the content is wider than the container by 2 px or more: layouts round each child's width, so a
    // smaller shortfall is not one anybody sees.
    readonly property bool overflowing: needWidth > 0 && needWidth >= haveWidth + 2

    // The name of the child the report singles out, or empty; a container that can name one declares its own.
    function overflowChild(): string {
        return ""
    }

    // Where the container is, as the report names it: its test id, else the nearest ancestor that has one.
    function whereAmI(): string {
        if (root.objectName !== "")
            return root.overflowKind + " `" + root.objectName + "`"
        for (let up = root.parent as Item; up; up = up.parent) {
            if ((up as MorphWidget) && up.objectName !== "")
                return "a " + root.overflowKind + " inside `" + up.objectName + "`"
        }
        return "a " + root.overflowKind
    }

    // Whether the content is wider than the container, and the container is shown, sized and in a window: a hidden tab
    // page, a folded section, a closed overlay and a start-up before the window has its size are not.
    readonly property bool overflowCheckable: root.overflowing && root.visible && root.width > 0 &&
                                              root.Window.window !== null

    // Reports the overflow once. It is checked again after the layout has settled, since the width and the content's
    // minimum settle over several passes.
    function checkOverflow() {
        if (root.overflowReported || !root.overflowCheckable)
            return
        root.overflowReported = true
        root.fire("overflow", {
            "where": root.whereAmI(),
            "need": Math.ceil(root.needWidth),
            "have": Math.floor(root.haveWidth),
            "child": root.overflowChild(),
            "table": root.overflowKind === "table"
        })
    }

    // The check waits for the layout to settle: a window resize polishes the items after the events already queued, so
    // a check posted at once would read the widths the old size left. The timer is made the first time a container
    // overflows, so a widget that never does carries none.
    property Timer overflowTimer: null
    Component {
        id: overflowTimerComponent

        Timer {
            interval: 60
            onTriggered: root.checkOverflow()
        }
    }

    onOverflowCheckableChanged: {
        if (root.overflowCheckable) {
            if (root.overflowTimer === null)
                root.overflowTimer = overflowTimerComponent.createObject(root) as Timer
            root.overflowTimer.restart()
        } else if (root.overflowTimer !== null) {
            root.overflowTimer.stop()
        }
    }

    // A widget the user cannot see keeps no keyboard focus, so it takes no keys.
    onVisibleChanged: {
        if (!visible && root.focusTarget && root.focusTarget.activeFocus)
            root.focusTarget.focus = false
    }

    // The keys a kind takes itself, after the widget's own chords: answers whether it took the key. A kind that takes
    // keys declares its own `ownKey`, which replaces this one; a widget takes none.
    function ownKey(event: var): bool {
        return false
    }

    Keys.onPressed: event => {
        if (root.takeOwnChord(event) || root.ownKey(event))
            event.accepted = true
    }

    HoverHandler {
        id: hover
        onHoveredChanged: {
            if (hovered && root.hoverWatched)
                root.question("hover", [])
        }
    }

    DragHandler {
        id: dragHandler
        enabled: root.dragKey !== ""
        target: dragProxy
        onActiveChanged: {
            if (!active) {
                dragProxy.Drag.drop()
                dragProxy.x = 0
                dragProxy.y = 0
            }
        }
    }

    // What moves under the pointer while the widget is dragged, carrying the key; the widget itself stays in place.
    component DragSource: Item {
        property string dragKey: ""
    }

    function keyOf(source: var): string {
        const dragged = source as DragSource
        return dragged ? dragged.dragKey : ""
    }

    DragSource {
        id: dragProxy
        dragKey: root.dragKey
        width: 1
        height: 1
        Drag.active: dragHandler.active
        Drag.keys: ["morph-key"]
        Drag.source: dragProxy
    }

    DropArea {
        id: dropArea
        anchors.fill: parent
        enabled: root.dropEnabled
        keys: ["morph-key"]
        onEntered: drag => {
            drag.accepted = root.question("accepts", root.keyOf(drag.source))
        }
        onDropped: drop => {
            const key = root.keyOf(drop.source)
            if (root.question("accepts", key)) {
                drop.accept()
                root.fire("drop", key)
            }
        }
    }

    // A surface's background, under everything the widget shows; a card has the card's radius and a shadow below it.
    Rectangle {
        anchors.fill: parent
        anchors.leftMargin: Theme.cardShadowOffsetX - Theme.cardShadowSpread
        anchors.rightMargin: -Theme.cardShadowOffsetX - Theme.cardShadowSpread
        anchors.topMargin: Theme.cardShadowOffsetY - Theme.cardShadowSpread
        anchors.bottomMargin: -Theme.cardShadowOffsetY - Theme.cardShadowSpread
        z: -2
        visible: root.surface === "card"
        radius: Theme.radiusCard + Theme.cardShadowBlur / 4
        color: Theme.cardShadowColor
    }
    Rectangle {
        anchors.fill: parent
        z: -1
        visible: root.surface !== "" && Theme.draws(root.surface)
        radius: root.surfaceRadius
        color: root.scheme.surface
        border.width: root.surface === "toolbar" || root.surface === "card" ? 1 : 0
        border.color: root.scheme.border
    }

    Rectangle {
        anchors.fill: parent
        z: 100
        visible: dropArea.containsDrag
        color: "transparent"
        border.color: root.scheme.info
        border.width: 2
    }
}

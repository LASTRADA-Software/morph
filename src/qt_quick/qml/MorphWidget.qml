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
    // Sizing per dimension: 0 content, 1 fixed (amount units), 2 stretch (amount is the weight).
    property int widthKind: 0
    property int widthAmount: 0
    property int heightKind: 0
    property int heightAmount: 0
    property int columnSpan: 1
    // Whether a content-sized dimension fills the container: the container sets them, from its kind and axis.
    property bool fillsWidth: false
    property bool fillsHeight: false
    // False for a kind whose width is its label (a button, a badge, a checkbox): it keeps that width at the start of
    // the line it fills, as the terminal draws it.
    property bool fillsAcross: true
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

    // A text role's colour in this widget's scheme: Normal, Muted, Heading, Error, Success.
    function textColorOf(role: int): color {
        switch (role) {
        case 1:
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
            return root.scheme.muted
        }
    }

    function fire(event: string, payload: var) {
        if (root.host)
            root.host.fire(event, payload)
    }

    function question(name: string, payload: var): bool {
        return root.host ? root.host.ask(name, payload) : false
    }

    Layout.fillWidth: widthKind === 2 || (widthKind === 0 && fillsWidth && fillsAcross)
    Layout.fillHeight: heightKind === 2 || (heightKind === 0 && fillsHeight)
    Layout.horizontalStretchFactor: widthKind === 2 ? widthAmount : -1
    Layout.verticalStretchFactor: heightKind === 2 ? heightAmount : -1
    Layout.preferredWidth: widthKind === 1 ? widthAmount * Theme.unitWidth : -1
    Layout.preferredHeight: heightKind === 1 ? heightAmount * Theme.unitHeight : -1
    Layout.columnSpan: columnSpan

    Accessible.name: accessibleName
    Accessible.role: accessibleRole
    Accessible.ignored: accessibleItem !== null

    ToolTip.text: tooltip
    ToolTip.visible: tooltip !== "" && hover.hovered
    ToolTip.delay: 600

    // A widget the user cannot see keeps no keyboard focus, so it takes no keys.
    onVisibleChanged: {
        if (!visible && root.focusTarget && root.focusTarget.activeFocus)
            root.focusTarget.focus = false
    }

    Keys.onPressed: event => {
        if (root.chords.length > 0 && root.question("chord", [event.key, event.modifiers]))
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
        radius: root.surface === "card" ? Theme.radiusCard : 0
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

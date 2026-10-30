// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A modal panel along the start or end edge of the window, with a dialog's rules: Escape or a click outside closes
// it and reports a dismissal; `show` opens and closes it without one.
MorphWidget {
    id: root

    property string title: ""
    // A line under the title, in the head, divided from the body by a rule: the overlay's accessible description.
    property string description: ""
    // The description as runs (`{text, strong, mono, tone}`, each text escaped), which `description` then holds the
    // joined characters of; while there are any, the label shows their markup.
    property var descriptionRuns: []
    // 0 start, 1 end; fixed at creation.
    property int side: 0
    property bool programmatic: false
    readonly property Item content: column
    // Where C++ puts the drawer's footer, under the content: a restyle lays it out where it wants.
    readonly property Item footer: footerArea
    readonly property bool shown: drawer.visible

    function show(open: bool) {
        if (drawer.visible === open)
            return
        root.programmatic = true
        if (open)
            drawer.open()
        else
            drawer.close()
        root.programmatic = false
    }

    function dismiss() {
        if (drawer.visible)
            drawer.close()
    }

    focusTarget: column
    // A Popup takes no `Accessible` attached property, since it is not an Item, so the content column is the
    // overlay's accessible node: a dialog named by its title, described by its description.
    accessibleItem: contentColumn
    defaultName: title
    defaultRole: Accessible.Dialog
    implicitWidth: 0
    implicitHeight: 0
    // root is only a zero-sized placeholder for the Drawer overlay below, which is reparented to the Overlay; without
    // these, MorphWidget's own Layout.preferredWidth/Height (bound from widthKind/heightKind) would make this
    // invisible placeholder claim real space in whatever layout holds the drawer node, independent of the overlay's
    // own on-screen width -- which a document setting `sizing` to get the explicit-width lever below would otherwise
    // trigger silently.
    Layout.preferredWidth: -1
    Layout.fillWidth: false
    Layout.preferredHeight: -1
    Layout.fillHeight: false

    Drawer {
        id: drawer
        edge: root.side === 0 ? Qt.LeftEdge : Qt.RightEdge
        // Interactive, so Escape and a press outside close it; no drag margin, so the user cannot open it by
        // swiping from the edge, which only the document may do.
        interactive: true
        dragMargin: 0
        // Opening and closing are immediate: what the document says is what the window shows, at once.
        enter: Transition {}
        exit: Transition {}
        modal: true
        // The window behind the drawer is dimmed in `color.scrim`.
        Overlay.modal: Rectangle {
            objectName: "scrim"
            color: Theme.scrim
        }
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        // An explicit width (widthKind Fixed) wins outright over the circular inference below, which reports back
        // roughly the width it was just given rather than what its children need. Capped against the window less a
        // margin either way: Math.min only ever shrinks the natural width down, it does not stretch a narrow drawer
        // up to the cap. The drawer's height is always the full window height by design (unlike a dialog, a drawer
        // is a full-height side panel) -- only its width is at issue here.
        width: root.widthKind === 1
            ? Math.min(root.widthAmount * Theme.unitWidth, (Overlay.overlay?.width ?? Infinity) - 4 * Theme.unitWidth)
            : Math.min(Math.max(column.implicitWidth + drawer.leftPadding + drawer.rightPadding, 30 * Theme.unitWidth),
                       (Overlay.overlay?.width ?? Infinity) - 4 * Theme.unitWidth)
        height: Overlay.overlay ? Overlay.overlay.height : 0
        onAboutToHide: {
            if (!root.programmatic)
                root.fire("dismiss", undefined)
        }
        // Drawn as the theme's card whatever surface encloses the node, as its content's colours are (the C++ side
        // hands the content the card's scheme).
        // Each side set itself: the style sets a one-pixel side padding along the edge for its own border line.
        leftPadding: Theme.spaceOf(4)
        rightPadding: Theme.spaceOf(4)
        topPadding: Theme.spaceOf(4)
        bottomPadding: Theme.spaceOf(4)
        background: Rectangle {
            color: Theme.cardScheme.surface
            border.width: 1
            border.color: Theme.cardScheme.border
            // Square: the drawer lies against the window's edge.

            Rectangle {
                anchors.fill: parent
                anchors.leftMargin: Theme.cardShadowOffsetX - Theme.cardShadowSpread
                anchors.rightMargin: -Theme.cardShadowOffsetX - Theme.cardShadowSpread
                anchors.topMargin: Theme.cardShadowOffsetY - Theme.cardShadowSpread
                anchors.bottomMargin: -Theme.cardShadowOffsetY - Theme.cardShadowSpread
                z: -1
                radius: parent.radius + Theme.cardShadowBlur / 4
                color: Theme.cardShadowColor
            }
        }

        contentItem: ColumnLayout {
            id: contentColumn
            Accessible.role: root.accessibleRole
            Accessible.name: root.accessibleName
            Accessible.description: root.description

            Label {
                textFormat: Text.PlainText
                id: titleLabel
                font: Theme.headingFont
                color: Theme.cardScheme.text
                visible: root.title !== ""
                text: root.title
            }

            Label {
                id: descriptionLabel
                objectName: "description"
                textFormat: root.descriptionRuns.length > 0 ? Text.RichText : Text.PlainText
                font: Theme.bodyFont
                color: Theme.cardScheme.muted
                visible: root.description !== ""
                text: root.descriptionRuns.length > 0 ? root.runsHtml(root.descriptionRuns) : root.description
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }

            // Spans the overlay, edge to edge, under the head while the description shows.
            Rectangle {
                id: headRule
                objectName: "headRule"
                visible: root.description !== ""
                color: Theme.cardScheme.border
                implicitHeight: 1
                Layout.fillWidth: true
                Layout.leftMargin: -drawer.leftPadding
                Layout.rightMargin: -drawer.rightPadding
            }

            // Only the content between the title and the footer scrolls, so both stay reachable however tall the
            // content is -- the drawer's own height is already fixed to the window (above), so without this, tall
            // content pushes the footer below the window's bottom edge rather than being itself cut off.
            // Layout.maximumHeight is computed from the title's and footer's own independent heights, not from the
            // scroll area's own size, to avoid the circularity deriving its cap from its own content would create;
            // below the cap, Layout.fillHeight already gives a short drawer just the room it needs. A ScrollView
            // (not a bare Flickable) correctly lets a plain click on an interactive child pass through to it.
            ScrollView {
                id: scroll
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.maximumHeight: Math.max(0, drawer.height - titleLabel.height - footerArea.height -
                               (footerRule.visible ? footerRule.height + contentColumn.spacing : 0) -
                                               contentColumn.spacing * 2 -
                                               (root.description === "" ? 0 : descriptionLabel.height +
                                                                              headRule.height +
                                                                              contentColumn.spacing * 2))
                clip: true

                ColumnLayout {
                    id: column
                    width: scroll.availableWidth
                }
            }

            // A hairline above the footer while it holds an action, the divider the cards use between sections.
            Rectangle {
                id: footerRule
                objectName: "footerRule"
                visible: footerArea.visible
                color: Theme.divider
                implicitHeight: 1
                Layout.fillWidth: true
                Layout.leftMargin: -drawer.leftPadding
                Layout.rightMargin: -drawer.rightPadding
            }

            // Shown while the footer holds something: a layout gives no size to what it hides, so its own
            // implicit size cannot say.
            ColumnLayout {
                id: footerArea
                Layout.fillWidth: true
                visible: {
                    for (let i = 0; i < footerArea.children.length; ++i) {
                        if ((footerArea.children[i] as MorphFooter)?.holding)
                            return true
                    }
                    return false
                }
            }
        }
    }
}

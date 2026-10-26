// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A modal dialog: an overlay holding its content, which traps focus while open. Escape or a click outside closes it
// and reports a dismissal; `show` opens and closes it without one.
MorphWidget {
    id: root

    property string title: ""
    // Set while C++ opens or closes it, so that is not taken for a dismissal.
    property bool programmatic: false
    readonly property Item content: column
    // Where C++ puts the dialog's footer, under the content: a restyle lays it out where it wants.
    readonly property Item footer: footerArea
    readonly property bool shown: popup.visible

    function show(open: bool) {
        if (popup.visible === open)
            return
        root.programmatic = true
        if (open)
            popup.open()
        else
            popup.close()
        root.programmatic = false
    }

    // Closes it as Escape would.
    function dismiss() {
        if (popup.visible)
            popup.close()
    }

    focusTarget: column
    implicitWidth: 0
    implicitHeight: 0
    // root is only a zero-sized placeholder for the Popup overlay below, which is reparented to the Overlay; without
    // these, MorphWidget's own Layout.preferredWidth/Height (bound from widthKind/heightKind) would make this
    // invisible placeholder claim real space in whatever layout holds the dialog node, independent of the overlay's
    // own on-screen width -- which a document setting `sizing` to get the explicit-width lever below would otherwise
    // trigger silently.
    Layout.preferredWidth: -1
    Layout.fillWidth: false
    Layout.preferredHeight: -1
    Layout.fillHeight: false

    Popup {
        id: popup
        anchors.centerIn: Overlay.overlay
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        // An explicit width (widthKind Fixed) wins outright over the Popup's own default sizing. The fallback branch
        // evaluates to `undefined`, not a captured number, so it resets rather than sets the Popup's own `hasWidth`
        // state -- a bound value here, even one equal to the Popup's own implicitWidth, would otherwise silently
        // disable QQuickPopupPositioner's window-edge clamping for every dialog, not just ones using the new lever.
        width: root.widthKind === 1
            ? Math.min(root.widthAmount * Theme.unitWidth, (Overlay.overlay?.width ?? Infinity) - 4 * Theme.unitWidth)
            : undefined
        onAboutToHide: {
            if (!root.programmatic)
                root.fire("dismiss", undefined)
        }
        // Drawn as the theme's card whatever surface encloses the node, as its content's colours are (the C++ side
        // hands the content the card's scheme).
        padding: Theme.spaceOf(4)
        background: Rectangle {
            color: Theme.cardScheme.surface
            border.width: 1
            border.color: Theme.cardScheme.border
            radius: Theme.radiusCard

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
            // A document does not know the window size, so the frontend keeps a wide dialog from running past it;
            // Math.min only ever shrinks, it does not stretch a narrow dialog's own natural width up to the cap.
            // contentItem is the Popup's root, not a layout's child, so its own width is set directly, not via a
            // Layout.* attached property (which only affects a layout-managed child, not the layout item itself).
            // With an explicit width, Qt's own resizeContent() only re-asserts contentItem's width from specific
            // trigger points (not from a plain change of contentItem's own implicit size), so this must track the
            // Popup's own availableWidth directly rather than keep deriving from its own (by then stale) implicit
            // size, or later content changes would silently drift out of sync with the pinned width.
            width: root.widthKind === 1
                ? popup.availableWidth
                : Math.min(implicitWidth, (popup.Overlay.overlay?.width ?? implicitWidth) - 4 * Theme.unitWidth)

            Label {
                textFormat: Text.PlainText
                id: titleLabel
                font: Theme.strongFont
                color: Theme.cardScheme.text
                visible: root.title !== ""
                text: root.title
            }

            // Only the content between the title and the footer scrolls, so both stay reachable however tall the
            // content is. Capped directly against the window, less what the title and footer and the Popup's own
            // margin already take: the one number nothing else needs to derive circularly from the scroll area's
            // own size, since title and footer are both fixed-height, independent of whatever it does. A ScrollView
            // (not a bare Flickable) correctly lets a plain click on an interactive child pass through to it, rather
            // than being claimed by the scroll gesture's own press handling.
            ScrollView {
                id: scroll
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.maximumHeight: Math.max(0, (popup.Overlay.overlay?.height ?? Infinity) - 4 * Theme.unitHeight -
                                               titleLabel.height - footerArea.height - contentColumn.spacing * 2)
                clip: true

                ColumnLayout {
                    id: column
                    width: scroll.availableWidth
                }
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

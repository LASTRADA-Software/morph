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

    Popup {
        id: popup
        anchors.centerIn: Overlay.overlay
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onAboutToHide: {
            if (!root.programmatic)
                root.fire("dismiss", undefined)
        }

        contentItem: ColumnLayout {
            id: contentColumn
            // A document does not know the window size, so the frontend keeps a wide dialog from running past it;
            // Math.min only ever shrinks, it does not stretch a narrow dialog's own natural width up to the cap.
            // contentItem is the Popup's root, not a layout's child, so its own width is set directly, not via a
            // Layout.* attached property (which only affects a layout-managed child, not the layout item itself).
            width: Math.min(implicitWidth, (popup.Overlay.overlay?.width ?? implicitWidth) - 4 * Theme.unitWidth)

            Label {
                id: titleLabel
                font: Theme.strongFont
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

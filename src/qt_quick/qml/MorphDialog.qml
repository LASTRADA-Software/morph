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
            Label {
                font: Theme.strongFont
                visible: root.title !== ""
                text: root.title
            }

            ColumnLayout {
                id: column
                Layout.fillWidth: true
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

// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls

// Where a popup that opens from a control goes: under the control at its start when the popup fits there inside the
// window, else ending at the control's end. The popup's margins then keep it inside the window whatever the sizes,
// and a popup that fits starts flush with a control at the window's edge, a margin in from it.
// A right-to-left layout swaps the two ends.
QtObject {
    // Sets `popup.x`, in its parent's coordinates, from where `trigger` is in the window now. A popup calls it when it
    // is about to show and again when its width changes while it is shown, as its entries lay out; it is never a
    // binding, because a binding reads the trigger's position once, before layout has placed it.
    function place(popup: Popup, trigger: Item) {
        const margin = Theme.spaceOf(1)
        popup.margins = margin
        const window = trigger.Window.window
        if (!window || !popup.parent)
            return
        const length = Math.max(popup.width, popup.implicitWidth)
        const origin = trigger.mapToItem(null, 0, 0).x
        const mirrored = trigger.LayoutMirroring.enabled
        const startsAt = mirrored ? origin + trigger.width - length : origin
        const fits = startsAt >= 0 && startsAt + length <= window.width
        const endsAt = mirrored ? origin : origin + trigger.width - length
        popup.x = (fits ? startsAt : endsAt) - popup.parent.mapToItem(null, 0, 0).x
    }
}

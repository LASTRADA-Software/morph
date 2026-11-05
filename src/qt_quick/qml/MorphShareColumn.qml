// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import "MorphShares.js" as Shares

// The column a container holds its content in. A child that stretches down (`stretch` height, or a spacer) takes a
// share of the height left by the others and the spacing, by weight from zero, as in a stack, not a growth from its
// own natural height. A share is the child's preferred height (`MorphWidget.stretchHeight`), so the layout places
// the shares exactly.
//
// The layout's own implicit height sums those shares, which follow the column's height, so a container that derives
// the column's height from its natural size reads `naturalHeight` instead: the others' heights, the spacing and
// `W * max(n / w)` for the stretching children (`MorphShares.stretchNatural`), which never reads a share.
ColumnLayout {
    id: root

    // Whether a shown child stretches down, or is a spacer, which counts as a stretch of weight 1.
    readonly property bool stretches: {
        for (const child of root.children) {
            if (root.stretchesIn(child))
                return true
        }
        return false
    }
    // The height the column asks of its container.
    readonly property real naturalHeight: {
        if (!root.stretches)
            return root.implicitHeight
        let others = 0
        let shown = 0
        const stretching = []
        for (const object of root.children) {
            const child = object as Item
            if (!child || !child.visible)
                continue
            ++shown
            if (root.stretchesIn(child)) {
                stretching.push({"natural": Math.min(Math.max(child.implicitHeight, root.leastOf(child)),
                                                     root.mostOf(child)),
                                    "weight": root.weightOf(child)})
            } else {
                others += root.heightOf(child)
            }
        }
        return others + Shares.stretchNatural(stretching) + root.spacing * Math.max(0, shown - 1)
    }
    // The height each child has, aligned with the layout's children: -1 for a child that does not stretch, else its
    // share of the column's height.
    readonly property var stretchShares: {
        // A column with no stretching child has no share to give, and reads nothing else, so a long one is not
        // evaluated again whenever a child's size changes.
        if (!root.stretches)
            return []
        const shares = []
        const members = []
        const owners = []
        let used = 0
        let shown = 0
        let index = 0
        for (const object of root.children) {
            const child = object as Item
            shares.push(-1)
            if (child && child.visible) {
                ++shown
                if (root.stretchesIn(child)) {
                    members.push({"weight": root.weightOf(child), "minimum": root.leastOf(child),
                                     "maximum": root.mostOf(child)})
                    owners.push(index)
                } else {
                    used += root.heightOf(child)
                }
            }
            ++index
        }
        const given = Shares.shareByWeight(members, root.height - used - root.spacing * Math.max(0, shown - 1))
        for (let i = 0; i < owners.length; ++i)
            shares[owners[i]] = given[i]
        return shares
    }

    // Stretching children are sized by their share, so the column asks its container for the natural height that
    // keeps every share at its child's natural, not the sum of the shares.
    Layout.preferredHeight: root.stretches ? root.naturalHeight : -1

    // The share of @p child along @p along (0 down the column), or -1 when this column does not size the child that
    // way.
    function shareOf(child: Item, along: int): real {
        if (along !== 0 || !root.stretchesIn(child))
            return -1
        let index = 0
        for (const object of root.children) {
            if (object === child)
                return index < root.stretchShares.length ? root.stretchShares[index] : -1
            ++index
        }
        return -1
    }
    // Whether @p child is shown and stretches down. The parameter is untyped because only a spacer declares
    // `expandsByDefault`.
    function stretchesIn(child: var): bool {
        if (!child || !child.visible)
            return false
        return child.heightKind === 2 || (child.heightKind === 0 && child["expandsByDefault"] === true)
    }
    // A stretching child's weight: its amount, at least 1; a spacer's is 1.
    function weightOf(child: var): real {
        return child.heightKind === 2 ? Math.max(1, child.heightAmount) : 1
    }
    // A child's height bounds; the maximum is unbounded while it has none.
    function leastOf(child: Item): real {
        return Math.max(0, child.Layout.minimumHeight)
    }
    function mostOf(child: Item): real {
        return child.Layout.maximumHeight >= 0 ? child.Layout.maximumHeight : Infinity
    }
    // The height of a child that does not stretch, as the layout sizes it: its fixed height, else its implicit one,
    // within its bounds.
    function heightOf(child: Item): real {
        const widget = child as MorphWidget
        const fixed = widget && widget.heightKind === 1
        const preferred = fixed ? child.Layout.preferredHeight : -1
        return Math.min(Math.max(preferred >= 0 ? preferred : child.implicitHeight, root.leastOf(child)),
                        root.mostOf(child))
    }
}

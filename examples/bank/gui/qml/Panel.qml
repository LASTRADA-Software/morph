// SPDX-License-Identifier: Apache-2.0
import QtQuick

// A rounded "card" surface. Put a Layout (anchored with margins) inside.
Rectangle {
    color: theme.surface
    radius: theme.radius
    border.width: 1
    border.color: theme.border
}

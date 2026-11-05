// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Empty space that takes what its layout leaves over. In a stack it shares that room as a stretch child of weight 1
// and no natural length.
MorphWidget {
    readonly property bool expandsByDefault: true
    Layout.fillWidth: true
    Layout.fillHeight: true
}

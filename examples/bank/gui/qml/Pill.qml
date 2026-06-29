// SPDX-License-Identifier: Apache-2.0
import QtQuick

// A small status badge. `kind`: "good" | "warn" | "bad" | "neutral".
Rectangle {
    id: pill
    property string text: ""
    property string kind: "neutral"

    radius: 11
    implicitHeight: 24
    implicitWidth: label.implicitWidth + 22
    color: kind === "good" ? theme.goodBg
         : kind === "warn" ? theme.warnBg
         : kind === "bad" ? theme.badBg
         : theme.neutralBg

    Text {
        id: label
        anchors.centerIn: parent
        text: pill.text
        font.pixelSize: 12
        font.weight: Font.DemiBold
        color: pill.kind === "good" ? theme.good
             : pill.kind === "warn" ? theme.warn
             : pill.kind === "bad" ? theme.bad
             : theme.inkSoft
    }
}

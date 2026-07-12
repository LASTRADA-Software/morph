// SPDX-License-Identifier: Apache-2.0

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MorphFormsShell

ApplicationWindow {
    id: root
    width: 1160
    height: 760
    visible: true
    title: "morph shell"

    FormShell {
        anchors.fill: parent
        controller: morphController
    }
}
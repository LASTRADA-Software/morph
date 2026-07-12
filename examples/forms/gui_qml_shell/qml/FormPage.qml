// SPDX-License-Identifier: Apache-2.0
//
// Wraps a DynamicForm (from the MorphForms module) as a full-page component
// inside a ScrollView. Used by the shell for built-in schema pages.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MorphForms

ScrollView {
    id: formPage
    clip: true
    contentWidth: availableWidth

    property string actionType: ""
    property var schema: null
    property var controller: null

    ColumnLayout {
        width: formPage.width
        spacing: 0

        Label {
            Layout.fillWidth: true
            Layout.margins: 24
            Layout.bottomMargin: 8
            text: formPage.actionType
            font.pixelSize: 24
            font.weight: Font.Bold
            color: "#222222"
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.leftMargin: 24
            Layout.rightMargin: 24
            Layout.bottomMargin: 12
            height: 1
            color: "#DDDDDD"
        }

        DynamicForm {
            Layout.fillWidth: true
            Layout.leftMargin: 24
            Layout.rightMargin: 24
            Layout.bottomMargin: 24
            actionType: formPage.actionType
            schema: formPage.schema
            controller: formPage.controller
        }
    }
}
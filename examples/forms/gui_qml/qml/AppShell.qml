// SPDX-License-Identifier: Apache-2.0
//
// The app-shell entry point: renders `app-menu` as a sidebar and routes the
// selected entry to its `app-screens` target — a plain form (DynamicForm) or
// a wizard (WizardView). Replaces "every schema on one scroll" as the demo's
// default root; Main.qml is unchanged and still directly loadable.
//
// Switching the menu away from a wizard screen and back recreates its
// WizardView (the Loader's sourceComponent changes), resetting its step
// back to 0 — this reference demo has no cross-screen state store (see
// docs/spec/forms/workflows_navigation.md's Non-goals).

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MorphForms

ApplicationWindow {
    id: root
    width: 640
    height: 780
    visible: true
    title: appDescriptor["app-title"] || "morph app shell"

    property var appDescriptor: JSON.parse(formsController.appSchemaJson)
    property var schemas: JSON.parse(formsController.schemasJson)
    property var wizardSchemas: JSON.parse(formsController.wizardSchemasJson)
    property var menu: appDescriptor["app-menu"] || []
    property var screensById: appDescriptor["app-screens"] || ({})
    property string currentScreen: menu.length > 0 ? menu[0].screen : ""
    property var currentScreenDescriptor: screensById[currentScreen] || ({})

    FormsController {
        id: formsController
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        ListView {
            Layout.preferredWidth: 160
            Layout.fillHeight: true
            model: root.menu
            delegate: ItemDelegate {
                required property var modelData
                width: ListView.view.width
                text: modelData.label
                highlighted: modelData.screen === root.currentScreen
                onClicked: root.currentScreen = modelData.screen
            }
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth

            Loader {
                width: parent ? parent.width : 0
                sourceComponent: root.currentScreenDescriptor.kind === "wizard" ? wizardDelegate
                                 : root.currentScreenDescriptor.kind === "form" ? formDelegate
                                 : placeholderDelegate
            }
        }
    }

    Component {
        id: formDelegate
        DynamicForm {
            width: parent.width
            actionType: root.currentScreenDescriptor.ref || ""
            schema: root.schemas[actionType] || ({})
            controller: formsController
        }
    }

    Component {
        id: wizardDelegate
        WizardView {
            width: parent.width
            wizardId: root.currentScreenDescriptor.ref || ""
            wizardSchema: root.wizardSchemas[wizardId] || ({})
            schemas: root.schemas
            controller: formsController
        }
    }

    Component {
        id: placeholderDelegate
        Label {
            padding: 16
            wrapMode: Text.Wrap
            text: "Screen kind '" + (root.currentScreenDescriptor.kind || "?")
                  + "' is not rendered by this reference demo yet."
        }
    }
}

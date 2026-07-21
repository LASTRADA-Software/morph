// SPDX-License-Identifier: Apache-2.0
//
// SlotRegistry priority-resolution + fallback, and one end-to-end override
// slot (a Slider) that participates in the required-gate/auto-fire exactly
// like a built-in control -- the toolkit's client-side "escape hatch"
// (docs/spec/forms/forms.md, "Theming / component-override registry").

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtTest
import MorphForms

Item {
    width: 480
    height: 480

    Component { id: fieldSlot;  Item {} }
    Component { id: widgetSlot; Item {} }
    Component { id: unitSlot;   Item {} }
    Component { id: typeSlot;   Item {} }

    SlotRegistry { id: emptyRegistry }

    TestCase {
        id: testCase
        name: "SlotRegistryResolution"

        function test_missEverywhereReturnsNull() {
            compare(emptyRegistry.resolve("Probe", "mass", "slider", "kg", "integer"), null)
        }

        function test_byTypeIsTheWeakestMatch() {
            const registry = Qt.createQmlObject('import MorphForms; SlotRegistry {}', testCase, "reg1")
            registry.byType("integer", typeSlot)
            compare(registry.resolve("Probe", "mass", "", "", "integer"), typeSlot)
        }

        function test_byUnitBeatsByType() {
            const registry = Qt.createQmlObject('import MorphForms; SlotRegistry {}', testCase, "reg2")
            registry.byType("integer", typeSlot)
            registry.byUnit("kg", unitSlot)
            compare(registry.resolve("Probe", "mass", "", "kg", "integer"), unitSlot)
        }

        function test_byWidgetBeatsByUnit() {
            const registry = Qt.createQmlObject('import MorphForms; SlotRegistry {}', testCase, "reg3")
            registry.byUnit("kg", unitSlot)
            registry.byWidget("slider", widgetSlot)
            compare(registry.resolve("Probe", "mass", "slider", "kg", "integer"), widgetSlot)
        }

        function test_byFieldIsTheStrongestMatch() {
            const registry = Qt.createQmlObject('import MorphForms; SlotRegistry {}', testCase, "reg4")
            registry.byWidget("slider", widgetSlot)
            registry.byField("Probe", "mass", fieldSlot)
            compare(registry.resolve("Probe", "mass", "slider", "kg", "integer"), fieldSlot)
        }

        function test_byFieldIsScopedToItsOwnAction() {
            const registry = Qt.createQmlObject('import MorphForms; SlotRegistry {}', testCase, "reg5")
            registry.byField("OtherAction", "mass", fieldSlot)
            compare(registry.resolve("Probe", "mass", "", "", ""), null)
        }
    }

    // End-to-end: an x-widget: "slider" field with no registered slot renders
    // the default number control (additive/ignorable key); with a registered
    // byWidget slider, DynamicForm loads the override and it drives the same
    // setFieldValue/revalidate path a built-in control uses.
    property var rangedSchema: ({
        properties: {
            level: { type: "integer", "x-order": 0, "x-widget": "slider", minimum: 0, maximum: 100 }
        },
        required: ["level"]
    })

    Component {
        id: sliderOverride
        Slider {
            property var field
            property var setValue
            from: 0
            to: 100
            onMoved: setValue(String(Math.round(value)))
        }
    }

    SlotRegistry {
        id: sliderRegistry
        Component.onCompleted: byWidget("slider", sliderOverride)
    }

    DynamicForm {
        id: withoutOverride
        actionType: "Probe"
        controller: null
        schema: rangedSchema
    }

    DynamicForm {
        id: withOverride
        actionType: "Probe"
        controller: null
        schema: rangedSchema
        slotRegistry: sliderRegistry
    }

    TestCase {
        name: "SlotRegistryEndToEnd"

        function test_unregisteredWidgetShowsTheDefaultControl() {
            const entry = findChild(withoutOverride, "field_level")
            verify(entry !== null)
            verify(entry.visible)
        }

        function test_registeredWidgetHidesTheDefaultControl() {
            const entry = findChild(withOverride, "field_level")
            verify(entry !== null)
            verify(!entry.visible)
        }

        function test_overrideParticipatesInTheReadinessGate() {
            withOverride.setFieldValue("level", "42")
            verify(withOverride.ready)
            compare(withOverride.previewLine, '{"level":42}')
        }
    }
}

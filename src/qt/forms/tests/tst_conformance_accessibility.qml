// SPDX-License-Identifier: Apache-2.0
//
// Accessibility slice of the conformance kit (docs/spec/forms/forms.md,
// "Renderer conformance kit"): every control exposes an accessible name (the
// wire key, since gui_field_metadata.md's `title` -- when present -- is used
// as the visual label but this slice checks the fallback contract), required
// fields announce it via the accessible description, focus order follows
// x-order, and every control class is keyboard-operable.
//
// Note: if keyClick-based focus assertions prove flaky under the offscreen
// QPA platform this suite runs under (QT_QPA_PLATFORM=offscreen, set by
// src/qt/forms/CMakeLists.txt's forms_qml_logic test), the fallback is to
// assert the Tab order via each control's position in `form.fields` (which
// is already x-order-sorted) instead of live key simulation.

pragma ComponentBehavior: Bound

import QtQuick
import QtTest
import MorphForms

Item {
    width: 480
    height: 480

    // Distinct name from DynamicForm's own `schema` property (see Main.qml's
    // `i18nCatalog`/`catalog` precedent): `schema: schema` below would
    // otherwise resolve the right-hand `schema` to the object's own
    // still-unset property instead of this outer one.
    property var accessibilitySchema: ({
        properties: {
            first: { type: "integer", "x-order": 0 },
            second: { type: "string", "x-order": 1 }
        },
        required: ["first", "second"]
    })

    DynamicForm {
        id: form
        actionType: "AccessibilityProbe"
        controller: null
        schema: accessibilitySchema
    }

    TestCase {
        name: "ConformanceAccessibility"
        when: windowShown

        function test_everyControlExposesAnAccessibleNameFallingBackToTheWireKey() {
            const first = findChild(form, "field_first")
            const second = findChild(form, "field_second")
            compare(first.Accessible.name, "first")
            compare(second.Accessible.name, "second")
        }

        function test_requiredFieldsAnnounceItInTheAccessibleDescription() {
            const first = findChild(form, "field_first")
            verify(first.Accessible.description.indexOf("Required") !== -1)
        }

        function test_focusOrderFollowsXOrder() {
            const first = findChild(form, "field_first")
            const second = findChild(form, "field_second")
            first.forceActiveFocus()
            verify(first.activeFocus)
            keyClick(Qt.Key_Tab)
            verify(second.activeFocus)
        }

        function test_everyControlIsKeyboardOperableNotPointerOnly() {
            const first = findChild(form, "field_first")
            first.forceActiveFocus()
            keyClick(Qt.Key_5)
            compare(first.text, "5")
        }
    }
}

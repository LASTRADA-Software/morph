// SPDX-License-Identifier: Apache-2.0
//
// Covers DynamicForm's client-side evaluation of the schema's x-rules array:
// requiredWhen extends the required set live, greater/exactlyOneOf gate
// submission exactly like the static required array, and visibleWhen /
// readonlyWhen toggle presentation without ever affecting submit-ability.

import QtQuick
import QtTest
import MorphForms

TestCase {
    id: testCase
    name: "DynamicFormRules"
    // visible: true is required for the discount-column visibility
    // assertion below: TestCase (and everything parented under it) defaults
    // to an invisible root in this headless suite, and Item.visible reads
    // the *effective* (ancestor-cascaded) visibility, not just this item's
    // own explicit flag -- without an effectively-visible root, every
    // descendant's `.visible` reads false regardless of its own binding.
    visible: true

    QtObject {
        id: mockController
        signal replyReceived(string actionType, bool ok, string payload)
        signal optionsReceived(string optionsAction, bool ok, string payload)

        property int submitCount: 0

        function submitIfValid(actionType, bodyJson) {
            submitCount += 1
            replyReceived(actionType, true, JSON.stringify({booked: true}))
        }

        function fetchOptions(optionsAction) {
            optionsReceived(optionsAction, true, "[]")
        }
    }

    property var testSchema: ({
        properties: {
            name: { type: "string", "x-order": 0 },
            checkInN: { type: "integer", "x-order": 1 },
            checkOutN: { type: "integer", "x-order": 2 },
            email: { type: "string", "x-order": 3 },
            phone: { type: "string", "x-order": 4 },
            promo: { type: "integer", "x-order": 5 },
            discount: { type: "integer", "x-order": 6 }
        },
        required: ["name"],
        "x-rules": [
            { kind: "greater", fields: ["checkOutN", "checkInN"] },
            { kind: "exactlyOneOf", fields: ["email", "phone"] },
            { kind: "requiredWhen", fields: ["discount"], when: { kind: "engaged", fields: ["promo"] } },
            { kind: "visibleWhen", fields: ["discount"], when: { kind: "engaged", fields: ["promo"] } }
        ]
    })

    Component {
        id: formComponent
        DynamicForm {
            actionType: "CFR_BookRoom"
            schema: testCase.testSchema
            controller: mockController
        }
    }

    function test_discount_column_hidden_until_promo_engaged() {
        var form = createTemporaryObject(formComponent, testCase)
        verify(form !== null)
        var discountColumn = findChild(form, "column_discount")
        verify(discountColumn !== null)
        compare(discountColumn.visible, false)

        findChild(form, "field_promo").text = "5"
        compare(discountColumn.visible, true)
    }

    function test_requiredWhen_extends_the_required_set_live() {
        mockController.submitCount = 0
        var form = createTemporaryObject(formComponent, testCase)
        verify(form !== null)

        findChild(form, "field_name").text = "Alice"
        findChild(form, "field_email").text = "a@b.com"
        compare(form.ready, true)
        compare(mockController.submitCount, 1)

        findChild(form, "field_promo").text = "5"
        // discount is now dynamically required and still empty.
        compare(form.ready, false)
        compare(mockController.submitCount, 1)

        findChild(form, "field_discount").text = "2"
        compare(form.ready, true)
        compare(mockController.submitCount, 2)
    }

    function test_greater_rule_gates_submit() {
        mockController.submitCount = 0
        var form = createTemporaryObject(formComponent, testCase)
        verify(form !== null)

        findChild(form, "field_name").text = "Alice"
        findChild(form, "field_email").text = "a@b.com"
        compare(form.ready, true)

        findChild(form, "field_checkInN").text = "10"
        findChild(form, "field_checkOutN").text = "3"
        compare(form.ready, false)  // checkOutN < checkInN

        findChild(form, "field_checkOutN").text = "20"
        compare(form.ready, true)  // checkOutN > checkInN
    }

    function test_exactlyOneOf_rule_gates_submit() {
        var form = createTemporaryObject(formComponent, testCase)
        verify(form !== null)

        findChild(form, "field_name").text = "Alice"
        compare(form.ready, false)  // neither email nor phone engaged

        findChild(form, "field_email").text = "a@b.com"
        compare(form.ready, true)

        findChild(form, "field_phone").text = "555"
        compare(form.ready, false)  // both engaged now
    }
}

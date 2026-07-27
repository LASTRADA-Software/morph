// SPDX-License-Identifier: Apache-2.0
//
// Test-only double: a "renderer" that ignores x-order (sorts by raw JSON key
// order instead) -- used by tst_conformance_negative.qml to prove the kit's
// field-order assertion fails against a renderer that violates it, and only
// that assertion. Never shipped: not part of the MorphForms QML module.

import QtQuick

QtObject {
    property var schema

    property var fields: {
        const props = schema.properties || {}
        const required = schema.required || []
        // BUG (deliberate): no x-order sort -- raw JSON.parse key order.
        return Object.keys(props).map(function (name) {
            return { name: name, required: required.indexOf(name) !== -1 }
        })
    }
}

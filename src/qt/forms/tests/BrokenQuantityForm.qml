// SPDX-License-Identifier: Apache-2.0
//
// Test-only double: a "renderer" that silently rounds an over-precise
// Quantity entry to the declared decimals instead of rejecting it -- the
// real, conformant behavior (docs/spec/forms/forms.md's exact-payload
// contract, verified for the real renderer in tst_dynamicform.qml's
// test_readinessGateAndComposition). Used by tst_conformance_negative.qml to
// prove the kit's exact-payload assertion fails against a renderer that
// rounds, and only that assertion. Never shipped.

import QtQuick

QtObject {
    property string text
    property int canonDp: 3

    // BUG (deliberate): always "ready" for any syntactically-valid decimal,
    // rounding away extra fractional digits instead of rejecting input more
    // precise than canonDp.
    function ready() {
        return /^-?\d+(\.\d+)?$/.test(text)
    }

    function payload() {
        const value = parseFloat(text)
        const scaled = Math.round(value * Math.pow(10, canonDp))
        return '{"num":' + scaled + ',"den":' + Math.pow(10, canonDp) + ',"dp":' + canonDp + '}'
    }
}

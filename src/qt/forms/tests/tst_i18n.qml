// SPDX-License-Identifier: Apache-2.0
//
// Locale fixtures: I18nCatalog itself, then (added by later tasks in this
// same plan) DynamicForm's i18n resolution, decimal-comma numeric entry, and
// zoned Timestamp entry.

import QtQuick
import QtTest
import MorphForms

Item {
    width: 480
    height: 480

    // Distinct id from DynamicForm's own `catalog` property (see Main.qml's
    // `i18nCatalog`/`catalog` precedent): `catalog: catalog` below would
    // otherwise resolve the right-hand `catalog` to the object's own
    // still-unset property instead of this sibling instance.
    I18nCatalog {
        id: i18nCatalog
        Component.onCompleted: {
            addTranslation("de", "greeting", "Hallo")
            addTranslation("de", "Probe.slot.label", "Steckplatz")
            addTranslation("de", "custom.stem.label", "Übersteuert")
        }
    }

    DynamicForm {
        id: form
        actionType: "Probe"
        controller: null
        catalog: i18nCatalog
        displayLocale: "de"
        schema: ({
            "properties": {
                "slot": { "type": ["integer", "null"], "x-order": 0 },
                "mass": { "$ref": "#/$defs/q", "x-order": 1, "x-decimalPlaces": 3,
                          "ExtUnits": { "unitAscii": "kg", "unitUnicode": "kg" } }
            },
            "$defs": { "q": { "type": ["object", "null"] } },
            "required": ["slot", "mass"]
        })
    }

    DynamicForm {
        id: formNoCatalog
        actionType: "Probe"
        controller: null
        schema: form.schema
    }

    DynamicForm {
        id: formOverride
        actionType: "Probe"
        controller: null
        catalog: i18nCatalog
        displayLocale: "de"
        schema: ({
            "properties": {
                "slot": { "type": ["integer", "null"], "x-order": 0, "x-i18nKey": "custom.stem" }
            },
            "required": ["slot"]
        })
    }

    DynamicForm {
        id: localeForm
        actionType: "Probe"
        controller: null
        displayLocale: "de"  // decimal comma, "." grouping
        schema: ({
            "properties": {
                "mass": { "$ref": "#/$defs/q", "x-order": 0, "x-decimalPlaces": 3,
                          "ExtUnits": { "unitAscii": "kg", "unitUnicode": "kg" } }
            },
            "$defs": { "q": { "type": ["object", "null"] } },
            "required": ["mass"]
        })
    }

    DynamicForm {
        id: zonedForm
        actionType: "Probe"
        controller: null
        displayOffsetMinutes: 120  // a UTC+2 display zone
        schema: ({
            "properties": {
                "when": { "type": ["string", "null"], "format": "date-time", "x-order": 0 }
            },
            "required": ["when"]
        })
    }

    TestCase {
        name: "I18nCatalog"

        function test_lookupHit() {
            compare(i18nCatalog.lookup("de", "greeting"), "Hallo")
        }

        function test_lookupMissReturnsUndefined() {
            compare(i18nCatalog.lookup("de", "unknown-key"), undefined)
            compare(i18nCatalog.lookup("fr", "greeting"), undefined)  // wrong locale
        }

        function test_addTranslationReplacesAnExistingEntry() {
            i18nCatalog.addTranslation("de", "greeting", "Servus")
            compare(i18nCatalog.lookup("de", "greeting"), "Servus")
            i18nCatalog.addTranslation("de", "greeting", "Hallo")  // restore for other tests
        }
    }

    TestCase {
        name: "DynamicFormI18n"

        function test_catalogHitRendersTranslatedLabel() {
            const slot = form.fields[0]
            compare(slot.label, "Steckplatz")
        }

        function test_catalogMissFallsBackToSchemaLiteral() {
            const mass = form.fields[1]
            compare(mass.label, "mass")  // no translation, no `title` yet: raw wire key
        }

        function test_noCatalogRendersExactlyLikeToday() {
            compare(formNoCatalog.fields[0].label, "slot")
        }

        function test_explicitKeyOverridesDerivedKey() {
            compare(formOverride.fields[0].label, "Übersteuert")
        }

        function test_decimalCommaEntryProducesCanonicalPayload() {
            localeForm.setFieldValue("mass", "1.050,25")
            verify(localeForm.ready)
            compare(localeForm.previewLine, '{"mass":{"num":1050250,"den":1000,"dp":3}}')
        }

        // morph#574. The de-DE locale groups with "." and this form's user
        // typed the US decimal form. Stripping the group separator
        // unconditionally -- which this mirror did, byte for byte in step with
        // its C++ twin -- submitted 1.5 as 15: a valid payload, ten times too
        // large, with nothing downstream able to tell. The field is now
        // reported malformed, which is what the user can act on.
        function test_foreignDecimalSeparatorIsRejectedNotAbsorbed() {
            localeForm.setFieldValue("mass", "1.5")
            verify(!localeForm.ready)
            compare(localeForm.previewLine, "")

            localeForm.setFieldValue("mass", "1.50")
            verify(!localeForm.ready)

            // A group separator off a group boundary is malformed wherever it
            // sits, not only at the end.
            localeForm.setFieldValue("mass", "1234.050,25")
            verify(!localeForm.ready)

            // Control: the well-formed entry still goes through, so the
            // rejection above is about placement and not about "." at all.
            localeForm.setFieldValue("mass", "1.050,25")
            verify(localeForm.ready)
            compare(localeForm.previewLine, '{"mass":{"num":1050250,"den":1000,"dp":3}}')
        }

        function test_zonedTimestampRoundTripsToUtc() {
            zonedForm.setFieldValue("when", "2026-07-05T16:30:00")  // 16:30 in UTC+2
            verify(zonedForm.ready)
            compare(zonedForm.previewLine, '{"when":"2026-07-05T14:30:00Z"}')
        }
    }
}

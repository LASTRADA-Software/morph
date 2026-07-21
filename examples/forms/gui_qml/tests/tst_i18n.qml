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
    }
}

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

    I18nCatalog {
        id: catalog
        Component.onCompleted: {
            addTranslation("de", "greeting", "Hallo")
        }
    }

    TestCase {
        name: "I18nCatalog"

        function test_lookupHit() {
            compare(catalog.lookup("de", "greeting"), "Hallo")
        }

        function test_lookupMissReturnsUndefined() {
            compare(catalog.lookup("de", "unknown-key"), undefined)
            compare(catalog.lookup("fr", "greeting"), undefined)  // wrong locale
        }

        function test_addTranslationReplacesAnExistingEntry() {
            catalog.addTranslation("de", "greeting", "Servus")
            compare(catalog.lookup("de", "greeting"), "Servus")
            catalog.addTranslation("de", "greeting", "Hallo")  // restore for other tests
        }
    }
}

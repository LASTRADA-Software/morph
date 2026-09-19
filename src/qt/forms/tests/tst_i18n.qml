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

    // morph#583. eu_ES spells its negative sign U+2212 MINUS SIGN, not the
    // ASCII hyphen -- one of 77 locales in Qt 6.11.2 whose sign is not a bare
    // "-". Its separators are de-DE's, so the only thing under test here is the
    // sign.
    DynamicForm {
        id: signForm
        actionType: "Probe"
        controller: null
        displayLocale: "eu_ES"
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

        // ── morph#583: the negative sign is locale data too ──────────────
        //
        // The premise, measured rather than assumed. If Qt's CLDR data ever
        // stops reporting U+2212 for eu_ES, this fails first and says so,
        // rather than the tests below failing for a reason that looks like a
        // regression in the renderer.
        function test_qtReportsANonAsciiSignForThisLocale() {
            compare(Qt.locale("eu_ES").negativeSign, "\u2212")
            compare(Qt.locale("eu_ES").decimalPoint, ",")
            compare(Qt.locale("eu_ES").groupSeparator, ".")
            compare(Qt.locale("de").negativeSign, "-")  // the control locale, unchanged
        }

        // The defect: the display edge emitted "−5" and the entry edge, which
        // compared one code unit against "-", rejected it. The pair was not
        // inverse for any of the 77 locales.
        function test_localeNegativeSignIsAccepted() {
            signForm.setFieldValue("mass", "\u22125")
            verify(signForm.ready)
            compare(signForm.previewLine, '{"mass":{"num":-5000,"den":1000,"dp":3}}')

            signForm.setFieldValue("mass", "\u22121.050,25")
            verify(signForm.ready)
            compare(signForm.previewLine, '{"mass":{"num":-1050250,"den":1000,"dp":3}}')
        }

        // U+2212 is on no keyboard. Matching only the locale's own spelling
        // would reject the sign the user can actually type, which is a wall
        // with no way round it rather than a fix.
        function test_asciiHyphenStaysAcceptedInANonAsciiSignLocale() {
            signForm.setFieldValue("mass", "-5")
            verify(signForm.ready)
            compare(signForm.previewLine, '{"mass":{"num":-5000,"den":1000,"dp":3}}')
        }

        // A bidi-control-prefixed sign is two or three UTF-16 units, so a
        // one-unit comparison cannot match it at all -- ar_DZ included, whose
        // sign *is* the ASCII hyphen behind a U+200E. Driven through the mirror
        // directly: these locales' own separators are Arabic-Indic, which is a
        // separate gap (morph#591), so the sign is isolated here.
        function test_bidiPrefixedSignIsMatchedAsAWholeString() {
            compare(signForm.normalizeLocaleNumber("\u200E\u22125", ".", "", "\u200E\u2212"), "-5")  // fa_IR
            compare(signForm.normalizeLocaleNumber("\u200E-\u200E5", ".", "", "\u200E-\u200E"), "-5")  // az_IR
            compare(signForm.normalizeLocaleNumber("\u200E-5", ".", "", "\u200E-"), "-5")  // ar_DZ
            compare(signForm.normalizeLocaleNumber("\u061C-5", ".", "", "\u061C-"), "-5")  // ar_EG

            // Controls: rejected with the sign left at its ASCII default, which
            // is exactly what the renderer passed before this change.
            compare(signForm.normalizeLocaleNumber("\u200E\u22125", ".", ""), null)
            compare(signForm.normalizeLocaleNumber("\u200E-5", ".", ""), null)
            compare(signForm.normalizeLocaleNumber("\u22125", ".", ""), null)
        }

        // The C++ edge (tests/test_render_locale_format.cpp, [morph583]) pins
        // the identical table; a divergence between the two is a divergence in
        // what the product accepts.
        function test_displayEdgeEmitsTheLocaleSignAndEntryTakesItBack() {
            const signs = ["\u2212", "\u200E\u2212", "\u200E-", "\u200E-\u200E", "\u061C-"]
            for (let i = 0; i < signs.length; ++i) {
                const display = signForm.formatCanonicalNumber("-1050.25", ",", ".", signs[i])
                compare(display, signs[i] + "1.050,25")
                compare(signForm.normalizeLocaleNumber(display, ",", ".", signs[i]), "-1050.25")
            }
            // A positive never carries the sign, and the ASCII default is
            // unchanged for every existing caller.
            compare(signForm.formatCanonicalNumber("1050.25", ",", ".", "\u2212"), "1.050,25")
            compare(signForm.formatCanonicalNumber("-1050.25", ",", "."), "-1.050,25")
        }

        // Unlike a group separator, no locale is without a negative sign, so an
        // omitted or empty one reads as "-" rather than as absence: formatting
        // -5 to "5" would be a silently wrong value, which is the morph#574
        // failure mode rather than a rejection.
        function test_anEmptySignReadsAsTheAsciiDefault() {
            compare(signForm.formatCanonicalNumber("-5", ".", "", ""), "-5")
            compare(signForm.normalizeLocaleNumber("-5", ".", "", ""), "-5")
            compare(signForm.normalizeLocaleNumber("123", ".", "", ""), "123")
        }

        // The morph#497 rule is about the *output*, so it has to hold for a
        // multi-unit sign exactly as it does for "-".
        function test_aLocaleSignIsStillRejectedOffTheLeadingPosition() {
            compare(signForm.normalizeLocaleNumber("1\u22122", ".", "", "\u2212"), null)
            compare(signForm.normalizeLocaleNumber(",\u22125", ",", ".", "\u2212"), null)
            compare(signForm.normalizeLocaleNumber("\u2212", ".", "", "\u2212"), null)
        }

        function test_zonedTimestampRoundTripsToUtc() {
            zonedForm.setFieldValue("when", "2026-07-05T16:30:00")  // 16:30 in UTC+2
            verify(zonedForm.ready)
            compare(zonedForm.previewLine, '{"when":"2026-07-05T14:30:00Z"}')
        }
    }
}

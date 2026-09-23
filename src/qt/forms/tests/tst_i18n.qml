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

    // eu_ES spells its negative sign U+2212 MINUS SIGN, not the
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

        // The de-DE locale groups with "." and this form's user typed the US
        // decimal form. Stripping the group separator unconditionally would
        // submit 1.5 as 15: a valid payload, ten times too large, with nothing
        // downstream able to tell. The field is reported malformed instead,
        // which is what the user can act on.
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

        // ── the negative sign is locale data too ────────────────────────
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
        // separate matter, so the sign is isolated here.
        function test_bidiPrefixedSignIsMatchedAsAWholeString() {
            compare(signForm.normalizeLocaleNumber("\u200E\u22125", { decimalSeparator: ".", groupSeparator: "", negativeSign: "\u200E\u2212" }), "-5")  // fa_IR
            compare(signForm.normalizeLocaleNumber("\u200E-\u200E5", { decimalSeparator: ".", groupSeparator: "", negativeSign: "\u200E-\u200E" }), "-5")  // az_IR
            compare(signForm.normalizeLocaleNumber("\u200E-5", { decimalSeparator: ".", groupSeparator: "", negativeSign: "\u200E-" }), "-5")  // ar_DZ
            compare(signForm.normalizeLocaleNumber("\u061C-5", { decimalSeparator: ".", groupSeparator: "", negativeSign: "\u061C-" }), "-5")  // ar_EG

            // Controls: rejected with the sign left at its ASCII default, which
            // is exactly what the renderer passed before this change.
            compare(signForm.normalizeLocaleNumber("\u200E\u22125", { decimalSeparator: ".", groupSeparator: "" }), null)
            compare(signForm.normalizeLocaleNumber("\u200E-5", { decimalSeparator: ".", groupSeparator: "" }), null)
            compare(signForm.normalizeLocaleNumber("\u22125", { decimalSeparator: ".", groupSeparator: "" }), null)
        }

        // The C++ edge (tests/test_render_locale_format.cpp, [morph583]) pins
        // the identical table; a divergence between the two is a divergence in
        // what the product accepts.
        function test_displayEdgeEmitsTheLocaleSignAndEntryTakesItBack() {
            const signs = ["\u2212", "\u200E\u2212", "\u200E-", "\u200E-\u200E", "\u061C-"]
            for (let i = 0; i < signs.length; ++i) {
                const display = signForm.formatCanonicalNumber("-1050.25", { decimalSeparator: ",", groupSeparator: ".", negativeSign: signs[i] })
                compare(display, signs[i] + "1.050,25")
                compare(signForm.normalizeLocaleNumber(display, { decimalSeparator: ",", groupSeparator: ".", negativeSign: signs[i] }), "-1050.25")
            }
            // A positive never carries the sign, and the ASCII default is
            // unchanged for every existing caller.
            compare(signForm.formatCanonicalNumber("1050.25", { decimalSeparator: ",", groupSeparator: ".", negativeSign: "\u2212" }), "1.050,25")
            compare(signForm.formatCanonicalNumber("-1050.25", { decimalSeparator: ",", groupSeparator: "." }), "-1.050,25")
        }

        // Unlike a group separator, no locale is without a negative sign, so an
        // omitted or empty one reads as "-" rather than as absence: formatting
        // -5 to "5" would be a silently wrong value rather than a rejection --
        // the same failure mode unconditional group-stripping has.
        function test_anEmptySignReadsAsTheAsciiDefault() {
            compare(signForm.formatCanonicalNumber("-5", { decimalSeparator: ".", groupSeparator: "", negativeSign: "" }), "-5")
            compare(signForm.normalizeLocaleNumber("-5", { decimalSeparator: ".", groupSeparator: "", negativeSign: "" }), "-5")
            compare(signForm.normalizeLocaleNumber("123", { decimalSeparator: ".", groupSeparator: "", negativeSign: "" }), "123")
        }

        // The leading-position rule is about the *output*, so it has to hold
        // for a multi-unit sign exactly as it does for "-".
        function test_aLocaleSignIsStillRejectedOffTheLeadingPosition() {
            compare(signForm.normalizeLocaleNumber("1\u22122", { decimalSeparator: ".", groupSeparator: "", negativeSign: "\u2212" }), null)
            compare(signForm.normalizeLocaleNumber(",\u22125", { decimalSeparator: ",", groupSeparator: ".", negativeSign: "\u2212" }), null)
            compare(signForm.normalizeLocaleNumber("\u2212", { decimalSeparator: ".", groupSeparator: "", negativeSign: "\u2212" }), null)
        }

        // ── a leading positive sign is accepted, and dropped ────────────
        //
        // The premise, measured rather than assumed, and on the same object the
        // renderer forwards from. 54 of the 711 locales Qt 6.11.2 knows spell
        // the positive sign with a bidi control mark before the "+"; unlike the
        // negative side there is no U+2212 analogue, so every non-ASCII
        // spelling is two or three code units and a one-unit comparison could
        // match none of them.
        function test_qtExposesAPositiveSignOnTheLocaleObject() {
            compare(Qt.locale("C").positiveSign, "+")
            compare(Qt.locale("de").positiveSign, "+")
            compare(Qt.locale("ar_EG").positiveSign, "\u061C+")
            compare(Qt.locale("az_IR").positiveSign, "\u200E+\u200E")
        }

        // The defect: a leading "+" fell through to the "any other character is
        // malformed" arm, so an explicitly-positive entry was rejected in every
        // locale. It is now accepted and *dropped* -- canonical text is
        // -?[0-9]+(\.[0-9]+)? and has no "+" in it, so the payload is
        // byte-identical to the one the unsigned entry produces.
        function test_aLeadingPositiveSignIsAcceptedAndDropped() {
            localeForm.setFieldValue("mass", "+1.050,25")
            verify(localeForm.ready)
            compare(localeForm.previewLine, '{"mass":{"num":1050250,"den":1000,"dp":3}}')

            // Byte-identical to the unsigned entry: the sign changes nothing
            // about the value, which is the whole reason dropping it is safe.
            localeForm.setFieldValue("mass", "1.050,25")
            compare(localeForm.previewLine, '{"mass":{"num":1050250,"den":1000,"dp":3}}')
        }

        // The C++ edge (tests/test_render_locale_format.cpp, [morph596]) pins
        // the identical table; a divergence between the two is a divergence in
        // what the product accepts.
        function test_positiveSignIsMatchedAsAWholeString() {
            compare(localeForm.normalizeLocaleNumber("+5", { decimalSeparator: ".", groupSeparator: "" }), "5")
            compare(localeForm.normalizeLocaleNumber("\u061C+5", { decimalSeparator: ".", groupSeparator: "", negativeSign: "-", positiveSign: "\u061C+" }), "5")      // ar_EG
            compare(localeForm.normalizeLocaleNumber("\u200E+5", { decimalSeparator: ".", groupSeparator: "", negativeSign: "-", positiveSign: "\u200E+" }), "5")      // ar_DZ
            compare(localeForm.normalizeLocaleNumber("\u200E+\u200E5", { decimalSeparator: ".", groupSeparator: "", negativeSign: "-", positiveSign: "\u200E+\u200E" }), "5")  // az_IR
            compare(localeForm.normalizeLocaleNumber("\u200F+5", { decimalSeparator: ".", groupSeparator: "", negativeSign: "-", positiveSign: "\u200F+" }), "5")      // ckb_IQ

            // Controls: with positiveSign left at its ASCII default, the
            // bidi-prefixed spellings are still rejected -- which is what makes
            // the parameter, and not the unconditional ASCII acceptance, the
            // thing under test above.
            compare(localeForm.normalizeLocaleNumber("\u061C+5", { decimalSeparator: ".", groupSeparator: "" }), null)
            compare(localeForm.normalizeLocaleNumber("\u200E+\u200E5", { decimalSeparator: ".", groupSeparator: "" }), null)

            // The ASCII "+" stays accepted in a bidi-sign locale, for the same
            // reason the ASCII "-" does: the locale's own spelling is on no
            // keyboard.
            compare(localeForm.normalizeLocaleNumber("+5", { decimalSeparator: ".", groupSeparator: "", negativeSign: "-", positiveSign: "\u061C+" }), "5")
            // An empty positiveSign leaves the ASCII spelling, and must not
            // match at every index.
            compare(localeForm.normalizeLocaleNumber("+5", { decimalSeparator: ".", groupSeparator: "", negativeSign: "-", positiveSign: "" }), "5")
            compare(localeForm.normalizeLocaleNumber("123", { decimalSeparator: ".", groupSeparator: "", negativeSign: "-", positiveSign: "" }), "123")
        }

        // The leading-position rule is about the *output*, so a second sign
        // spelling must not open a second way to inject one.
        function test_aPositiveSignObeysTheLeadingPositionRule() {
            compare(localeForm.normalizeLocaleNumber("1+2", { decimalSeparator: ".", groupSeparator: "" }), null)
            compare(localeForm.normalizeLocaleNumber("+-5", { decimalSeparator: ".", groupSeparator: "" }), null)
            compare(localeForm.normalizeLocaleNumber("-+5", { decimalSeparator: ".", groupSeparator: "" }), null)
            compare(localeForm.normalizeLocaleNumber("++5", { decimalSeparator: ".", groupSeparator: "" }), null)
            compare(localeForm.normalizeLocaleNumber(",+5", { decimalSeparator: ",", groupSeparator: "." }), null)
            compare(localeForm.normalizeLocaleNumber("+", { decimalSeparator: ".", groupSeparator: "" }), null)
        }

        // The deliberate asymmetry, pinned so that "make it symmetric" is a
        // test failure rather than a tidy-up. formatCanonicalNumber takes no
        // positiveSign at all and never emits one -- Qt reports "+" for 657 of
        // 711 locales, so emitting it would turn every positive number in every
        // form from "5" into "+5".
        function test_theDisplayEdgeNeverEmitsAPositiveSign() {
            compare(localeForm.formatCanonicalNumber("5", { decimalSeparator: ".", groupSeparator: "" }), "5")
            compare(localeForm.formatCanonicalNumber("1050.25", { decimalSeparator: ",", groupSeparator: "." }), "1.050,25")
            // So the pair is not inverse across a positive sign: entry accepts
            // a spelling display never produces.
            const canonical = localeForm.normalizeLocaleNumber("+1.050,25", { decimalSeparator: ",", groupSeparator: "." })
            compare(canonical, "1050.25")
            compare(localeForm.formatCanonicalNumber(canonical, { decimalSeparator: ",", groupSeparator: "." }), "1.050,25")
            // ...while the negative side still round-trips exactly.
            compare(localeForm.formatCanonicalNumber("-1050.25", { decimalSeparator: ",", groupSeparator: "." }), "-1.050,25")
        }

        // ── the separators are matched as whole strings too ─────────────
        //
        // The premise, measured rather than assumed -- and it says something
        // different from the two sign premises above, which is the whole point
        // of stating it. Over the same 711 locales Qt 6.11.2 reports through
        // QLocale::matchingLocales, *every* decimalPoint and *every*
        // groupSeparator is exactly one UTF-16 code unit:
        //
        //   decimalPoint   with size() > 1: 0
        //   groupSeparator with size() > 1: 0
        //   negativeSign   with size() > 1: 54   <- the control
        //   positiveSign   with size() > 1: 54   <- the control
        //
        // So no locale reaches this and no user is affected. What it buys is
        // the mirror's own consistency: the *signs* in this function are
        // matched as whole strings, and separators compared one code unit at a
        // time a few lines away would be a mixed idiom with nothing saying why.
        // docs/spec/forms/forms.md, "Both edges, or neither": a divergence
        // between the mirror and include/morph/render/locale_format.hpp is a
        // divergence in what the product accepts, whether or not a locale can
        // currently express it.
        //
        // Qt.locale() cannot enumerate, so the widest spellings the
        // enumeration found are pinned here one by one. If CLDR ever gives one
        // of them a second code unit, this fails first and says so.
        function test_noLocaleSeparatorNeedsMoreThanOneCodeUnit() {
            // One locale per distinct groupSeparator spelling Qt reports:
            // U+002C, U+002E, U+0027, U+00A0, U+060C, U+066C, U+12C8, U+202F,
            // U+2E41 -- plus the two control locales the tests above use.
            const names = ["C", "de", "eu_ES", "en_CH", "ar_EG", "ar_DZ",
                           "nqo_GN", "gez_ET", "ff_BF", "ab_GE", "en_FR"]
            for (let i = 0; i < names.length; ++i) {
                const l = Qt.locale(names[i])
                compare(l.decimalPoint.length, 1, names[i] + " decimalPoint")
                compare(l.groupSeparator.length, 1, names[i] + " groupSeparator")
            }
            // The control, read off the same objects: a length check on locale
            // data is not vacuously 1: the signs really are two and three units
            // in these very locales.
            compare(Qt.locale("ar_EG").negativeSign.length, 2)
            compare(Qt.locale("az_IR").positiveSign.length, 3)
        }

        // The defect, driven directly because nothing else can drive it. A
        // test built on Qt.locale(...) would pass against the unfixed code --
        // every real separator is one code unit, so `ch === groupSeparator`
        // and `text.startsWith(groupSeparator, i)` agree on all 711 -- and
        // would therefore be evidence of nothing. These separators are
        // synthetic for exactly that reason.
        //
        // Before the fix: a multi-unit separator matched no single `ch`, so
        // each of its units fell through to the "any other character is
        // malformed" arm and the whole entry was rejected. The C++ edge, which
        // has always used `rest.starts_with(...)`, accepted it.
        //
        // ── the shared corpus ────────────────────────────────────────────
        // tests/test_render_locale_format.cpp, [morph599], pins the identical
        // rows against the C++ edge (as their UTF-8 spellings). The two lists
        // are meant to be read side by side; a row that disagrees between them
        // is the divergence the rule forbids.
        //
        //   G2 = U+200E U+002E   D2 = U+200E U+002C   two code points
        //   G4 = U+1D16D         D4 = U+1D16E         one code point, two
        //                                             UTF-16 units each
        //
        // G4/D4 are the sharper case: they are single *code points*, so a
        // mirror that iterated code points rather than code units would still
        // fail on them. The bug is about UTF-16 code units.
        function test_aMultiUnitSeparatorIsMatchedAsAWholeString() {
            const G2 = "\u200E."
            const D2 = "\u200E,"
            const G4 = "\uD834\uDD6D"
            const D4 = "\uD834\uDD6E"

            compare(localeForm.normalizeLocaleNumber("1" + G2 + "050" + D2 + "25", { decimalSeparator: D2, groupSeparator: G2 }), "1050.25")
            compare(localeForm.normalizeLocaleNumber("-1" + G2 + "050" + D2 + "25", { decimalSeparator: D2, groupSeparator: G2 }), "-1050.25")
            compare(localeForm.normalizeLocaleNumber("+1" + G2 + "050" + D2 + "25", { decimalSeparator: D2, groupSeparator: G2 }), "1050.25")
            compare(localeForm.normalizeLocaleNumber("1" + G4 + "050" + D4 + "25", { decimalSeparator: D4, groupSeparator: G4 }), "1050.25")
            compare(localeForm.normalizeLocaleNumber("1" + G4 + "050" + G4 + "000", { decimalSeparator: "", groupSeparator: G4 }), "1050000")
            // A locale with a multi-unit decimal separator and no grouping.
            compare(localeForm.normalizeLocaleNumber("5" + D2 + "25", { decimalSeparator: D2, groupSeparator: "" }), "5.25")
        }

        // Whole-string matching must not loosen any of the rules a one-unit
        // comparison enforces. The grouping validation and the
        // leading-position rule are stated over "the separator",
        // so they have to hold when the separator is more than one unit.
        //
        // Stated plainly, because it matters for what this function is worth
        // as evidence: every row here is a rejection, and the *unfixed* code
        // rejected all of them too -- it rejected everything with a multi-unit
        // separator in it. So this function is a guard against the fix
        // over-accepting, not a demonstration of the defect. The two functions
        // either side of it are the ones that fail against the unfixed mirror.
        function test_aMultiUnitSeparatorIsStillValidatedTheSameWay() {
            const G2 = "\u200E."
            const D2 = "\u200E,"

            // The last group of the integer part is short.
            compare(localeForm.normalizeLocaleNumber("1" + G2 + "5", { decimalSeparator: D2, groupSeparator: G2 }), null)
            compare(localeForm.normalizeLocaleNumber("1" + G2 + "2" + G2 + "3" + G2 + "4", { decimalSeparator: D2, groupSeparator: G2 }), null)
            // Grouping belongs to the integer part only.
            compare(localeForm.normalizeLocaleNumber("1" + D2 + "5" + G2 + "000", { decimalSeparator: D2, groupSeparator: G2 }), null)
            // A second decimal separator.
            compare(localeForm.normalizeLocaleNumber("1" + D2 + "0" + D2 + "5", { decimalSeparator: D2, groupSeparator: G2 }), null)
            // A sign after the decimal separator is not leading.
            compare(localeForm.normalizeLocaleNumber(D2 + "-5", { decimalSeparator: D2, groupSeparator: G2 }), null)
            // One string cannot play both roles, multi-unit or not.
            compare(localeForm.normalizeLocaleNumber("1" + G2 + "050", { decimalSeparator: G2, groupSeparator: G2 }), null)
            // Controls, unchanged by this fix and rejected before and after: a
            // lone *prefix* of the separator is not the separator. Whole-string
            // matching must not degrade into "any unit of it will do".
            compare(localeForm.normalizeLocaleNumber("1\u200E050" + D2 + "25", { decimalSeparator: D2, groupSeparator: G2 }), null)
            compare(localeForm.normalizeLocaleNumber("1.050,25", { decimalSeparator: D2, groupSeparator: G2 }), null)
        }

        // The round trip, which is where "both edges, or neither" bites:
        // formatCanonicalNumber emits the separators as whole strings, so an
        // entry edge that did not match them whole would reject text the
        // display edge produced -- for a locale that does not exist yet.
        function test_theDisplayEdgeEmitsAMultiUnitSeparatorAndEntryTakesItBack() {
            const G2 = "\u200E."
            const D2 = "\u200E,"
            const display = localeForm.formatCanonicalNumber("-1050.25", { decimalSeparator: D2, groupSeparator: G2, negativeSign: "\u2212" })
            compare(display, "\u2212" + "1" + G2 + "050" + D2 + "25")
            compare(localeForm.normalizeLocaleNumber(display, { decimalSeparator: D2, groupSeparator: G2, negativeSign: "\u2212" }), "-1050.25")
        }

        // --- the digits are locale data too -------------------------------
        //
        // The same corpus the C++ edge pins in
        // tests/test_render_locale_format.cpp. docs/spec/forms/forms.md,
        // "Both edges, or neither": a row that disagrees between the two lists
        // is a divergence in what the product accepts.
        //
        // Measured on this revision with QLocale::matchingLocales under
        // Qt 6.11.2: 76 of the 711 locales report a zeroDigit other than ASCII
        // "0", across the eleven sets below. Two of them are astral, so one
        // digit is two UTF-16 units here and four UTF-8 bytes there -- the case
        // a code-unit scan gets wrong.

        // The premise, measured rather than assumed, on the same object the
        // renderer forwards from. If Qt stops reporting these, this fails first
        // and says so, rather than the tests below failing for a reason that
        // looks like a regression in the renderer.
        function test_qtExposesAZeroDigitOnTheLocaleObject() {
            compare(Qt.locale("C").zeroDigit, "0")
            compare(Qt.locale("de").zeroDigit, "0")
            compare(Qt.locale("ar_EG").zeroDigit, "\u0660")
            compare(Qt.locale("fa_IR").zeroDigit, "\u06F0")
            // Astral: one code point, two UTF-16 units.
            compare(Qt.locale("ccp_BD").zeroDigit.length, 2)
            compare(Qt.locale("ccp_BD").zeroDigit.codePointAt(0), 0x11136)
            compare(Qt.locale("ff_Adlm_BF").zeroDigit.codePointAt(0), 0x1E950)
        }

        // The defect: the scan compared one code unit against ["0","9"], so a
        // locale whose digits are not ASCII could not enter a number at all --
        // a flat rejection, not a wrong value.
        function test_theLocalesOwnDigitsAreAccepted() {
            compare(localeForm.normalizeLocaleNumber("\u0665", { zeroDigit: "\u0660" }), "5")
            compare(localeForm.normalizeLocaleNumber("\u06F5", { zeroDigit: "\u06F0" }), "5")
            compare(localeForm.normalizeLocaleNumber("\u061C-\u0665",
                                                     { negativeSign: "\u061C-", zeroDigit: "\u0660" }), "-5")
            // Astral digits, one code point in two units each.
            compare(localeForm.normalizeLocaleNumber("\u{1113B}", { zeroDigit: "\u{11136}" }), "5")
            compare(localeForm.normalizeLocaleNumber("\u{1E955}", { zeroDigit: "\u{1E950}" }), "5")
        }

        // The display edge had to move with the entry edge or the pair would no
        // longer be inverse. These are the exact units Qt's own
        // QLocale("ar_BH").toString(-1050.25) produces.
        function test_theDisplayEdgeEmitsTheLocalesDigits() {
            const display = localeForm.formatCanonicalNumber("-1050.25", {
                decimalSeparator: "\u066B", groupSeparator: "\u066C",
                negativeSign: "\u061C-", zeroDigit: "\u0660"
            })
            compare(display, "\u061C-\u0661\u066C\u0660\u0665\u0660\u066B\u0662\u0665")
        }

        // The acceptance test this ticket exists for. The round trip alone does
        // not catch a fix applied to the entry edge only -- entry accepts ASCII
        // digits too, so a display edge still emitting them round-trips fine.
        // The "no ASCII digit in the display text" check is what fails then.
        function test_thePairRoundTripsThroughEveryMeasuredDigitSet() {
            const zeros = ["\u0660", "\u06F0", "\u07C0", "\u0966", "\u09E6", "\u0F20",
                           "\u1040", "\u1C50", "\uABF0", "\u{11136}", "\u{1E950}"]
            const canonicals = ["0", "5", "-5", "1050.25", "-1050.25", "1000000.25",
                                "0.001", "-0.001", "1234567"]
            for (let i = 0; i < zeros.length; ++i) {
                const loc = {
                    decimalSeparator: "\u066B", groupSeparator: "\u066C",
                    negativeSign: "\u061C-", zeroDigit: zeros[i]
                }
                for (let k = 0; k < canonicals.length; ++k) {
                    const display = localeForm.formatCanonicalNumber(canonicals[k], loc)
                    verify(!/[0-9]/.test(display),
                           "display for " + canonicals[k] + " in set " + i + " still has an ASCII digit: " + display)
                    compare(localeForm.normalizeLocaleNumber(display, loc), canonicals[k],
                            "round trip failed for " + canonicals[k] + " in set " + i)
                }
            }
        }

        // The positive-sign precedent, applied to digits: the locale's own digits
        // are on the user's keyboard only if their keyboard has them, so entry
        // accepts a spelling display never produces.
        function test_asciiDigitsStayAcceptedInANativeDigitLocale() {
            compare(localeForm.normalizeLocaleNumber("5", { zeroDigit: "\u0660" }), "5")
            compare(localeForm.normalizeLocaleNumber("-1050.25",
                                                     { negativeSign: "\u061C-", zeroDigit: "\u0660" }), "-1050.25")
            compare(localeForm.normalizeLocaleNumber("5", { zeroDigit: "\u{1E950}" }), "5")
        }

        // A decision, not a consequence, so a test records it: an entry that
        // interleaves the two families is malformed rather than read as 55.
        function test_anEntryMayNotMixDigitFamilies() {
            compare(localeForm.normalizeLocaleNumber("\u06655", { zeroDigit: "\u0660" }), null)
            compare(localeForm.normalizeLocaleNumber("5\u0665", { zeroDigit: "\u0660" }), null)
            compare(localeForm.normalizeLocaleNumber("1\u0660\u06650", { zeroDigit: "\u0660" }), null)
            // Each family on its own is fine, which is what makes the rejection
            // about the mixing rather than about either spelling.
            compare(localeForm.normalizeLocaleNumber("\u0665\u0665", { zeroDigit: "\u0660" }), "55")
            compare(localeForm.normalizeLocaleNumber("55", { zeroDigit: "\u0660" }), "55")
            // In an ASCII-digit locale the two families are the same set, so
            // nothing can mix -- which is why this costs no existing caller.
            compare(localeForm.normalizeLocaleNumber("55", {}), "55")
        }

        // Grouping counts digits, not units. A two-unit astral digit would
        // otherwise reset the run-length counter mid-digit, so a correctly
        // grouped native-digit entry -- what the display edge now emits -- would
        // be rejected and the pair would not round-trip. The rule itself is
        // unchanged: a short group is still malformed.
        function test_groupingIsValidatedInTheLocalesDigitsToo() {
            const arEg = { decimalSeparator: "\u066B", groupSeparator: "\u066C", zeroDigit: "\u0660" }
            compare(localeForm.normalizeLocaleNumber("\u0661\u066C\u0660\u0665\u0660", arEg), "1050")
            compare(localeForm.normalizeLocaleNumber("\u0661\u066C\u0665", arEg), null)  // short group
            const ccp = { decimalSeparator: ".", groupSeparator: ",", zeroDigit: "\u{11136}" }
            compare(localeForm.normalizeLocaleNumber("\u{11137},\u{11136}\u{1113B}\u{11136}", ccp), "1050")
            compare(localeForm.normalizeLocaleNumber("\u{11137},\u{1113B}", ccp), null)  // short group
        }

        // An omitted or empty zeroDigit reads as ASCII "0", the reading an
        // omitted negativeSign gets, so a caller that names no zeroDigit gets
        // the plain ASCII behaviour.
        function test_anAbsentZeroDigitReadsAsAscii() {
            compare(localeForm.normalizeLocaleNumber("55", { zeroDigit: "" }), "55")
            compare(localeForm.formatCanonicalNumber("55", { zeroDigit: "" }), "55")
            compare(localeForm.normalizeLocaleNumber("1.050,25", { decimalSeparator: ",", groupSeparator: "." }), "1050.25")
            compare(localeForm.formatCanonicalNumber("1050.25", { decimalSeparator: ",", groupSeparator: "." }), "1.050,25")
            compare(localeForm.formatCanonicalNumber("-1050.25", { decimalSeparator: ",", groupSeparator: "." }), "-1.050,25")
        }

        function test_zonedTimestampRoundTripsToUtc() {
            zonedForm.setFieldValue("when", "2026-07-05T16:30:00")  // 16:30 in UTC+2
            verify(zonedForm.ready)
            compare(zonedForm.previewLine, '{"when":"2026-07-05T14:30:00Z"}')
        }
    }
}

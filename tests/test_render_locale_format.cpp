// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <morph/render/locale_format.hpp>
#include <morph/util/datetime.hpp>
#include <optional>
#include <string>

using morph::render::formatCanonicalNumber;
using morph::render::normalizeLocaleNumber;

TEST_CASE("render::normalizeLocaleNumber converts de-DE grouped/decimal-comma text", "[render][locale]") {
    CHECK(normalizeLocaleNumber("1.050,25", ",", ".") == "1050.25");
    CHECK(normalizeLocaleNumber("-1.050,25", ",", ".") == "-1050.25");
}

TEST_CASE("render::normalizeLocaleNumber converts fr-FR space-grouped/decimal-comma text", "[render][locale]") {
    CHECK(normalizeLocaleNumber("1 050,25", ",", " ") == "1050.25");
}

TEST_CASE("render::normalizeLocaleNumber is the identity transform for plain '.'-decimal text", "[render][locale]") {
    CHECK(normalizeLocaleNumber("1234.5", ".", "") == "1234.5");
    CHECK(normalizeLocaleNumber("-0.001", ".", "") == "-0.001");
}

TEST_CASE("render::normalizeLocaleNumber rejects malformed input rather than guessing", "[render][locale]") {
    CHECK(normalizeLocaleNumber("12.34.56", ".", "") == std::nullopt);
    CHECK(normalizeLocaleNumber("abc", ".", "") == std::nullopt);
    CHECK(normalizeLocaleNumber("-", ".", "") == std::nullopt);
}

TEST_CASE("render::normalizeLocaleNumber rejects empty input", "[render][locale]") {
    // No characters ever reach the output, so `canonical` stays empty -- the
    // `canonical.empty()` arm of the final malformed check, distinct from the
    // `canonical == "-"` arm the lone-dash case above exercises.
    CHECK(normalizeLocaleNumber("", ".", "") == std::nullopt);
}

TEST_CASE("render::normalizeLocaleNumber rejects a sign that is not in the leading position", "[render][locale]") {
    // A `-` reached after digits have already been emitted is sign injection,
    // not a leading sign -- distinct from the lone-dash case (`sawAnyOutput`
    // is still false there) and from the multi-decimal case (a different
    // malformed reason entirely).
    CHECK(normalizeLocaleNumber("1-2", ".", "") == std::nullopt);
}

TEST_CASE("render::normalizeLocaleNumber supports a locale with no decimal separator", "[render][locale]") {
    // Some locales (e.g. integer-only entry fields) pass an empty
    // decimalSeparator: the decimal-separator match must short-circuit on
    // `decimalSeparator.empty()` rather than call `starts_with` on an empty
    // needle, and grouping must still work standalone.
    CHECK(normalizeLocaleNumber("1.050", "", ".") == "1050");
    CHECK(normalizeLocaleNumber("-1.050", "", ".") == "-1050");
}

TEST_CASE("render::formatCanonicalNumber groups thousands and swaps the decimal separator", "[render][locale]") {
    CHECK(formatCanonicalNumber("1050.25", ",", ".") == "1.050,25");
    CHECK(formatCanonicalNumber("-1050.25", ",", ".") == "-1.050,25");
    CHECK(formatCanonicalNumber("1234.5", ".", "") == "1234.5");
}

TEST_CASE("render::formatCanonicalNumber handles empty input", "[render][locale]") {
    // `canonicalText.empty()` must short-circuit the sign check rather than
    // call `.front()` on an empty view.
    CHECK(formatCanonicalNumber("", ",", ".").empty());
}

TEST_CASE("render locale numeric round-trip: normalize then format reproduces the original", "[render][locale]") {
    auto const canonical = normalizeLocaleNumber("1.050,25", ",", ".");
    REQUIRE(canonical == "1050.25");
    CHECK(formatCanonicalNumber("1050.25", ",", ".") == "1.050,25");
}

TEST_CASE("A zoned DateTime display shift round-trips to the identical canonical instant", "[render][locale]") {
    using morph::time::DateTime;
    auto const utc = DateTime::fromIso8601("2026-07-20T12:00:00Z");
    REQUIRE(utc.has_value());

    // The renderer shows this instant in a UTC+2 display zone by shifting it
    // with DateTime's existing duration-arithmetic operators — no new
    // production code is needed for the zone contract itself.
    auto const displayed = *utc + std::chrono::minutes{120};
    CHECK(displayed.toIso8601() == "2026-07-20T14:00:00.000Z");

    // ...and shifts back by the same offset before it reaches the wire.
    auto const backToUtc = displayed - std::chrono::minutes{120};
    CHECK(backToUtc == *utc);
    CHECK(backToUtc.toIso8601() == "2026-07-20T12:00:00.000Z");
}

// ── Multi-byte separators ────────────────────────────────────────────────────
// A real locale's separator is not always one byte: fr-FR groups with U+202F
// (narrow no-break space, 3 bytes in UTF-8) and several locales use U+00A0
// (2 bytes). Typed as `char`, neither could be expressed at all -- a caller
// could only pass some single byte that never matched, so a perfectly valid
// entry a French user typed normalised to std::nullopt and the control
// reported it malformed.

namespace {
// Spelled as explicit UTF-8 bytes rather than \u universal-character-names:
// MSVC rejects the latter in a narrow literal when the code page cannot
// represent the character (warning C4566, fatal under this project's
// warnings-as-errors policy), and the byte sequence is what the test is
// actually about.
constexpr std::string_view kNarrowNbsp = "\xE2\x80\xAF";  // U+202F, 3 UTF-8 bytes
constexpr std::string_view kNbsp = "\xC2\xA0";            // U+00A0, 2 UTF-8 bytes
}  // namespace

TEST_CASE("render::normalizeLocaleNumber accepts a multi-byte group separator", "[render][locale]") {
    REQUIRE(kNarrowNbsp.size() == 3);
    REQUIRE(kNbsp.size() == 2);

    CHECK(normalizeLocaleNumber(std::string{"1"} + std::string{kNarrowNbsp} + "050,25", ",", kNarrowNbsp) ==
          "1050.25");
    CHECK(normalizeLocaleNumber(std::string{"-1"} + std::string{kNarrowNbsp} + "050,25", ",", kNarrowNbsp) ==
          "-1050.25");
    CHECK(normalizeLocaleNumber(std::string{"1"} + std::string{kNbsp} + "234", ",", kNbsp) == "1234");
}

TEST_CASE("render::normalizeLocaleNumber accepts a multi-byte decimal separator", "[render][locale]") {
    // Not a real locale, but it pins that the decimal branch matches the whole
    // separator too, rather than only its first byte.
    CHECK(normalizeLocaleNumber(std::string{"1"} + std::string{kNbsp} + "5", kNbsp, "") == "1.5");
    // A second one is still malformed.
    CHECK(normalizeLocaleNumber(std::string{"1"} + std::string{kNbsp} + "5" + std::string{kNbsp} + "2", kNbsp, "") ==
          std::nullopt);
}

TEST_CASE("render::normalizeLocaleNumber rejects a stray separator byte", "[render][locale]") {
    // A lone continuation byte of a multi-byte separator is not the separator,
    // and must not be silently stripped.
    CHECK(normalizeLocaleNumber(std::string{"1"} + std::string{kNarrowNbsp.substr(0, 1)} + "050", ",", kNarrowNbsp) ==
          std::nullopt);
}

TEST_CASE("render::formatCanonicalNumber emits a multi-byte group separator", "[render][locale]") {
    CHECK(formatCanonicalNumber("1050.25", ",", kNarrowNbsp) ==
          std::string{"1"} + std::string{kNarrowNbsp} + "050,25");
    CHECK(formatCanonicalNumber("1234567", ",", kNbsp) ==
          std::string{"1"} + std::string{kNbsp} + "234" + std::string{kNbsp} + "567");
}

TEST_CASE("render::locale_format round-trips through a multi-byte separator", "[render][locale]") {
    auto const display = formatCanonicalNumber("1050.25", ",", kNarrowNbsp);
    CHECK(normalizeLocaleNumber(display, ",", kNarrowNbsp) == "1050.25");
}

// ── morph#497: a sign after the decimal separator is not "leading" ──
//
// `sawAnyOutput` was only set at the bottom of the loop, and the
// decimal-separator branch `continue`d past it -- so after a separator the sign
// guard still believed nothing had been emitted and accepted an injected sign.
// The QML mirror (src/qt/forms/qml/DynamicForm.qml, documented as mirroring
// this function) always rejected these, so the two control edges disagreed.
TEST_CASE("normalizeLocaleNumber: a sign after the decimal separator is rejected", "[render][locale][morph497]") {
    // de-DE: comma decimal, dot grouping -- the reported shape.
    REQUIRE_FALSE(morph::render::normalizeLocaleNumber(",-5", ",", ".").has_value());
    // en-US equivalent.
    REQUIRE_FALSE(morph::render::normalizeLocaleNumber(".-5", ".", ",").has_value());
    // With a group separator stripped first, which is the case the guard's own
    // comment is about.
    REQUIRE_FALSE(morph::render::normalizeLocaleNumber("1.,-5", ",", ".").has_value());

    // Control: the guard already worked once a digit had been emitted, and must
    // keep working.
    REQUIRE_FALSE(morph::render::normalizeLocaleNumber("1-2", ".", ",").has_value());
    // Control: a genuinely leading sign still parses.
    REQUIRE(morph::render::normalizeLocaleNumber("-1,5", ",", ".") == "-1.5");
}

TEST_CASE("normalizeLocaleNumber: the loose shapes stay accepted, in step with the QML mirror",
          "[render][locale][morph497]") {
    // Deliberately NOT narrowed to `-?[0-9]+(\.[0-9]+)?`: DynamicForm.qml's
    // normalizeLocaleNumber accepts all three, and tightening one edge alone
    // would put them back out of step. Documented on the function.
    REQUIRE(morph::render::normalizeLocaleNumber(".5", ".", ",") == ".5");
    REQUIRE(morph::render::normalizeLocaleNumber("5.", ".", ",") == "5.");
    REQUIRE(morph::render::normalizeLocaleNumber(".", ".", ",") == ".");
}

// ── morph#574: a group separator is validated, not stripped ──────────────────
//
// Before this, every occurrence of the group separator was dropped
// unconditionally, so a de-DE user typing the US form "1.5" into a price field
// submitted 15 -- a valid-looking number, ten times too large, with no
// diagnostic anywhere. The suite above has 18 cases and not one of them was
// cross-locale: every single-locale case passes with the stripping or with the
// validation, which is exactly the check that would still pass if the feature
// did nothing.

TEST_CASE("normalizeLocaleNumber: the decimal separator of another locale is rejected, not absorbed",
          "[render][locale][morph574]") {
    // THE case. de-DE locale, US-style decimal typed: this returned "15".
    CHECK(normalizeLocaleNumber("1.5", ",", ".") == std::nullopt);
    CHECK(normalizeLocaleNumber("1.50", ",", ".") == std::nullopt);
    CHECK(normalizeLocaleNumber("1.2.3.4", ",", ".") == std::nullopt);

    // The mirror image: en-US locale, EU-style decimal typed. Returned "15".
    CHECK(normalizeLocaleNumber("1,5", ".", ",") == std::nullopt);

    // And with a multi-byte group separator, where the same mistake is a
    // narrow no-break space away from a well-formed entry.
    CHECK(normalizeLocaleNumber(std::string{"1"} + std::string{kNarrowNbsp} + "5", ",", kNarrowNbsp) == std::nullopt);
}

TEST_CASE("normalizeLocaleNumber: equal decimal and group separators are rejected rather than guessed",
          "[render][locale][morph574]") {
    // One string in both roles has no defensible reading, and the old code
    // silently ate the decimal: this returned "15".
    CHECK(normalizeLocaleNumber("1.5", ".", ".") == std::nullopt);
    // Not even the shapes that would be unambiguous if you squinted: the
    // rejection is on the configuration, not on the text.
    CHECK(normalizeLocaleNumber("1.050", ".", ".") == std::nullopt);
    CHECK(normalizeLocaleNumber("1", ".", ".") == std::nullopt);
    // Control: an empty group separator is "this locale does not group", which
    // is a different statement and stays legal.
    CHECK(normalizeLocaleNumber("1.5", ".", "") == "1.5");
}

TEST_CASE("normalizeLocaleNumber: a group separator must sit on a group boundary", "[render][locale][morph574]") {
    // Preceded by one to three digits...
    CHECK(normalizeLocaleNumber("1.050", "", ".") == "1050");
    CHECK(normalizeLocaleNumber("12.050", "", ".") == "12050");
    CHECK(normalizeLocaleNumber("123.050", "", ".") == "123050");
    CHECK(normalizeLocaleNumber("1234.050", "", ".") == std::nullopt);
    CHECK(normalizeLocaleNumber(".050", "", ".") == std::nullopt);

    // ...followed by exactly three, at every group and at the end of the
    // integer part.
    CHECK(normalizeLocaleNumber("1.05", "", ".") == std::nullopt);
    CHECK(normalizeLocaleNumber("1.0500", "", ".") == std::nullopt);
    CHECK(normalizeLocaleNumber("1.050.", "", ".") == std::nullopt);
    CHECK(normalizeLocaleNumber("1.000.00", "", ".") == std::nullopt);
    CHECK(normalizeLocaleNumber("1.000.000", "", ".") == "1000000");

    // ...and never after the decimal separator.
    CHECK(normalizeLocaleNumber("1,050.25", ",", ".") == std::nullopt);
    CHECK(normalizeLocaleNumber("1.000,250.25", ",", ".") == std::nullopt);

    // A grouping locale with no separator in the entry at all: there is no
    // placement to be wrong about, and the digits pass through.
    CHECK(normalizeLocaleNumber("1050", ",", ".") == "1050");

    // A non-digit inside a grouping locale restarts the digit run rather than
    // being counted into it -- and is malformed for the ordinary reason.
    CHECK(normalizeLocaleNumber("1.0x0", "", ".") == std::nullopt);
}

TEST_CASE("normalizeLocaleNumber: every well-formed locale entry still normalises", "[render][locale][morph574]") {
    // The validation must not cost a single legitimate entry -- this is the
    // half of the change that the rejection cases cannot show.
    CHECK(normalizeLocaleNumber("1.050,25", ",", ".") == "1050.25");
    CHECK(normalizeLocaleNumber("-1.050,25", ",", ".") == "-1050.25");
    CHECK(normalizeLocaleNumber("1.000.000,25", ",", ".") == "1000000.25");
    CHECK(normalizeLocaleNumber("1050,25", ",", ".") == "1050.25");   // ungrouped
    CHECK(normalizeLocaleNumber("1,050.25", ".", ",") == "1050.25");  // en-US
    CHECK(normalizeLocaleNumber(std::string{"1"} + std::string{kNarrowNbsp} + "050,25", ",", kNarrowNbsp) ==
          "1050.25");  // fr-FR

    // A grouped entry round-trips through the display direction unchanged.
    auto const canonical = normalizeLocaleNumber("1.000.000,25", ",", ".");
    REQUIRE(canonical.has_value());
    CHECK(formatCanonicalNumber(*canonical, ",", ".") == "1.000.000,25");
}

// ──── morph#583: the negative sign is locale data, and is not always one byte ────
//
// Of the 711 locales Qt 6.11.2 reports through QLocale::matchingLocales, 77
// spell the negative sign as something other than a bare ASCII '-':
//
//   U+002D                 634   e.g. C
//   U+061C U+002D           24   e.g. ar_EG
//   U+200E U+002D            9   e.g. ar_DZ
//   U+200E U+002D U+200E    17   e.g. az_IR
//   U+200E U+2212            2   e.g. fa_IR
//   U+200F U+002D            2   e.g. ckb_IQ
//   U+2212                  23   e.g. eu_ES
//
// Matched as the literal byte '-', none of the 77 round-tripped: the display
// direction emitted a sign the entry direction then rejected.
namespace {
// Five of the sign spellings the table above enumerates, by the locale that
// reports each. Spelled out as `constexpr std::string_view` rather than built
// by concatenating the marks above: a namespace-scope `std::string` allocates
// during static initialisation, which `bugprone-throwing-static-initialization`
// flags, and these need no dynamic initialisation at all.
constexpr std::string_view kSignEuEs = "\xE2\x88\x92";               // eu_ES: U+2212
constexpr std::string_view kSignFaIr = "\xE2\x80\x8E\xE2\x88\x92";   // fa_IR: U+200E U+2212
constexpr std::string_view kSignArDz = "\xE2\x80\x8E-";              // ar_DZ: U+200E U+002D
constexpr std::string_view kSignAzIr = "\xE2\x80\x8E-\xE2\x80\x8E";  // az_IR: U+200E U+002D U+200E
constexpr std::string_view kSignArEg = "\xD8\x9C-";                  // ar_EG: U+061C U+002D

// The entry a user of that locale would have typed. A named helper because
// `kSign... + "5"` no longer compiles once the constants are views, and
// `std::string{sign} + digits` at twenty call sites reads worse than this does.
[[nodiscard]] std::string entry(std::string_view sign, std::string_view digits) {
    return std::string{sign} + std::string{digits};
}
}  // namespace

TEST_CASE("normalizeLocaleNumber: a locale whose sign is U+2212 can be entered", "[render][locale][morph583]") {
    // The spellings really are multi-byte: a `char` parameter could carry none
    // of them, which is the whole reason the sign is a view.
    REQUIRE(kSignEuEs.size() == 3);
    REQUIRE(kSignAzIr.size() == 7);

    // eu_ES: the bare U+2212 of the issue title.
    CHECK(normalizeLocaleNumber(entry(kSignEuEs, "5"), ".", "", kSignEuEs) == "-5");
    CHECK(normalizeLocaleNumber(entry(kSignEuEs, "1050,25"), ",", ".", kSignEuEs) == "-1050.25");

    // The control for the defect: with the sign left at its ASCII default, the
    // same entry is still rejected -- that default is the whole of today's
    // behaviour.
    CHECK(normalizeLocaleNumber(entry(kSignEuEs, "5"), ".", "") == std::nullopt);
}

TEST_CASE("normalizeLocaleNumber: a bidi-prefixed sign is matched as a whole string", "[render][locale][morph583]") {
    // Two and three code points respectively. A one-unit comparison cannot
    // match either, which is why both edges compare the whole string.
    CHECK(normalizeLocaleNumber(entry(kSignFaIr, "5"), ".", "", kSignFaIr) == "-5");  // fa_IR
    CHECK(normalizeLocaleNumber(entry(kSignAzIr, "5"), ".", "", kSignAzIr) == "-5");  // az_IR, 3 code points
    CHECK(normalizeLocaleNumber(entry(kSignArEg, "5"), ".", "", kSignArEg) == "-5");  // ar_EG, U+061C prefix

    // Same controls: rejected today, for each shape.
    CHECK(normalizeLocaleNumber(entry(kSignFaIr, "5"), ".", "") == std::nullopt);
    CHECK(normalizeLocaleNumber(entry(kSignAzIr, "5"), ".", "") == std::nullopt);
}

TEST_CASE("normalizeLocaleNumber: ar_DZ fails today even though its sign is the ASCII hyphen",
          "[render][locale][morph583]") {
    // The case that shows this is not "the U+2212 locales": ar_DZ's sign *is*
    // '-', prefixed by U+200E. The stray prefix byte is what the per-byte scan
    // rejects, so matching the hyphen byte-wise never helped it.
    CHECK(normalizeLocaleNumber(entry(kSignArDz, "5"), ".", "") == std::nullopt);
    CHECK(normalizeLocaleNumber(entry(kSignArDz, "5"), ".", "", kSignArDz) == "-5");
}

TEST_CASE("normalizeLocaleNumber: the ASCII hyphen stays accepted in every locale", "[render][locale][morph583]") {
    // U+2212 and the bidi marks are on no keyboard. Matching only the locale's
    // own spelling would reject the sign the user can actually type and leave
    // them no way to enter a negative number at all.
    CHECK(normalizeLocaleNumber("-5", ".", "", kSignEuEs) == "-5");
    CHECK(normalizeLocaleNumber("-5", ".", "", kSignFaIr) == "-5");
    // And the plain ASCII locale is untouched -- the default parameter means no
    // existing caller changed behaviour.
    CHECK(normalizeLocaleNumber("-5", ".", "") == "-5");
    CHECK(normalizeLocaleNumber("-1.050,25", ",", ".") == "-1050.25");
}

TEST_CASE("normalizeLocaleNumber: a locale sign is still rejected off the leading position",
          "[render][locale][morph583]") {
    // The morph#497 rule is about the *output*, so it has to hold for a
    // multi-byte sign exactly as it does for '-'.
    CHECK(normalizeLocaleNumber("1" + entry(kSignEuEs, "2"), ".", "", kSignEuEs) == std::nullopt);
    CHECK(normalizeLocaleNumber("," + entry(kSignEuEs, "5"), ",", ".", kSignEuEs) == std::nullopt);
    // A sign and nothing else is not a number, whatever its spelling.
    CHECK(normalizeLocaleNumber(kSignEuEs, ".", "", kSignEuEs) == std::nullopt);
    CHECK(normalizeLocaleNumber(kSignAzIr, ".", "", kSignAzIr) == std::nullopt);
}

TEST_CASE("formatCanonicalNumber: the display edge emits the locale's sign", "[render][locale][morph583]") {
    // Before this, the sign was a hardcoded '-' whatever the locale -- so even
    // a caller that knew its locale's sign could not ask for it.
    CHECK(formatCanonicalNumber("-1050.25", ",", ".", kSignEuEs) == entry(kSignEuEs, "1.050,25"));
    CHECK(formatCanonicalNumber("-5", ".", "", kSignFaIr) == entry(kSignFaIr, "5"));
    // A positive is untouched: the sign string is only ever emitted for a
    // negative, so a locale-specific sign cannot leak into a positive display.
    CHECK(formatCanonicalNumber("1050.25", ",", ".", kSignEuEs) == "1.050,25");
    CHECK(formatCanonicalNumber("-1050.25", ",", ".") == "-1.050,25");
}

TEST_CASE("locale_format: the pair is inverse for every measured sign spelling", "[render][locale][morph583]") {
    // The property the issue is actually about: whatever the display edge
    // emits, the entry edge takes back to the identical canonical text.
    for (auto const& sign : {kSignEuEs, kSignFaIr, kSignArDz, kSignAzIr, kSignArEg}) {
        INFO("sign = " << sign);
        auto const display = formatCanonicalNumber("-1050.25", ",", ".", sign);
        CHECK(display == entry(sign, "1.050,25"));
        CHECK(normalizeLocaleNumber(display, ",", ".", sign) == "-1050.25");
    }
}

TEST_CASE("locale_format: an empty negative sign reads as '-', not as 'no sign'", "[render][locale][morph583]") {
    // Unlike a group separator, there is no locale without a negative sign, so
    // empty cannot mean absence. On the display edge it would be a silently
    // wrong value: -5 formatted to "5" is a valid number of the wrong sign,
    // which is the morph#574 failure mode, not a rejection.
    CHECK(formatCanonicalNumber("-5", ".", "", "") == "-5");
    CHECK(normalizeLocaleNumber("-5", ".", "", "") == "-5");
    // And an empty needle must not match at every index: a scan that treated it
    // as a separator would never advance.
    CHECK(normalizeLocaleNumber("123", ".", "", "") == "123");
}

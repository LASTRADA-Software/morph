// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <initializer_list>
#include <morph/render/locale_format.hpp>
#include <morph/util/datetime.hpp>
#include <optional>
#include <string>
#include <string_view>

using morph::render::formatCanonicalNumber;
using morph::render::normalizeLocaleNumber;

TEST_CASE("render::normalizeLocaleNumber converts de-DE grouped/decimal-comma text", "[render][locale]") {
    CHECK(normalizeLocaleNumber("1.050,25", {.decimalSeparator = ",", .groupSeparator = "."}) == "1050.25");
    CHECK(normalizeLocaleNumber("-1.050,25", {.decimalSeparator = ",", .groupSeparator = "."}) == "-1050.25");
}

TEST_CASE("render::normalizeLocaleNumber converts fr-FR space-grouped/decimal-comma text", "[render][locale]") {
    CHECK(normalizeLocaleNumber("1 050,25", {.decimalSeparator = ",", .groupSeparator = " "}) == "1050.25");
}

TEST_CASE("render::normalizeLocaleNumber is the identity transform for plain '.'-decimal text", "[render][locale]") {
    CHECK(normalizeLocaleNumber("1234.5", {.decimalSeparator = ".", .groupSeparator = ""}) == "1234.5");
    CHECK(normalizeLocaleNumber("-0.001", {.decimalSeparator = ".", .groupSeparator = ""}) == "-0.001");
}

TEST_CASE("render::normalizeLocaleNumber rejects malformed input rather than guessing", "[render][locale]") {
    CHECK(normalizeLocaleNumber("12.34.56", {.decimalSeparator = ".", .groupSeparator = ""}) == std::nullopt);
    CHECK(normalizeLocaleNumber("abc", {.decimalSeparator = ".", .groupSeparator = ""}) == std::nullopt);
    CHECK(normalizeLocaleNumber("-", {.decimalSeparator = ".", .groupSeparator = ""}) == std::nullopt);
}

TEST_CASE("render::normalizeLocaleNumber rejects empty input", "[render][locale]") {
    // No characters ever reach the output, so `canonical` stays empty -- the
    // `canonical.empty()` arm of the final malformed check, distinct from the
    // `canonical == "-"` arm the lone-dash case above exercises.
    CHECK(normalizeLocaleNumber("", {.decimalSeparator = ".", .groupSeparator = ""}) == std::nullopt);
}

TEST_CASE("render::normalizeLocaleNumber rejects a sign that is not in the leading position", "[render][locale]") {
    // A `-` reached after digits have already been emitted is sign injection,
    // not a leading sign -- distinct from the lone-dash case (`sawAnyOutput`
    // is still false there) and from the multi-decimal case (a different
    // malformed reason entirely).
    CHECK(normalizeLocaleNumber("1-2", {.decimalSeparator = ".", .groupSeparator = ""}) == std::nullopt);
}

TEST_CASE("render::normalizeLocaleNumber supports a locale with no decimal separator", "[render][locale]") {
    // Some locales (e.g. integer-only entry fields) pass an empty
    // decimalSeparator: the decimal-separator match must short-circuit on
    // `decimalSeparator.empty()` rather than call `starts_with` on an empty
    // needle, and grouping must still work standalone.
    CHECK(normalizeLocaleNumber("1.050", {.decimalSeparator = "", .groupSeparator = "."}) == "1050");
    CHECK(normalizeLocaleNumber("-1.050", {.decimalSeparator = "", .groupSeparator = "."}) == "-1050");
}

TEST_CASE("render::formatCanonicalNumber groups thousands and swaps the decimal separator", "[render][locale]") {
    CHECK(formatCanonicalNumber("1050.25", {.decimalSeparator = ",", .groupSeparator = "."}) == "1.050,25");
    CHECK(formatCanonicalNumber("-1050.25", {.decimalSeparator = ",", .groupSeparator = "."}) == "-1.050,25");
    CHECK(formatCanonicalNumber("1234.5", {.decimalSeparator = ".", .groupSeparator = ""}) == "1234.5");
}

TEST_CASE("render::formatCanonicalNumber handles empty input", "[render][locale]") {
    // `canonicalText.empty()` must short-circuit the sign check rather than
    // call `.front()` on an empty view.
    CHECK(formatCanonicalNumber("", {.decimalSeparator = ",", .groupSeparator = "."}).empty());
}

TEST_CASE("render locale numeric round-trip: normalize then format reproduces the original", "[render][locale]") {
    auto const canonical = normalizeLocaleNumber("1.050,25", {.decimalSeparator = ",", .groupSeparator = "."});
    REQUIRE(canonical == "1050.25");
    CHECK(formatCanonicalNumber("1050.25", {.decimalSeparator = ",", .groupSeparator = "."}) == "1.050,25");
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

    CHECK(normalizeLocaleNumber(std::string{"1"} + std::string{kNarrowNbsp} + "050,25",
                                {.decimalSeparator = ",", .groupSeparator = kNarrowNbsp}) == "1050.25");
    CHECK(normalizeLocaleNumber(std::string{"-1"} + std::string{kNarrowNbsp} + "050,25",
                                {.decimalSeparator = ",", .groupSeparator = kNarrowNbsp}) == "-1050.25");
    CHECK(normalizeLocaleNumber(std::string{"1"} + std::string{kNbsp} + "234",
                                {.decimalSeparator = ",", .groupSeparator = kNbsp}) == "1234");
}

TEST_CASE("render::normalizeLocaleNumber accepts a multi-byte decimal separator", "[render][locale]") {
    // Not a real locale, but it pins that the decimal branch matches the whole
    // separator too, rather than only its first byte.
    CHECK(normalizeLocaleNumber(std::string{"1"} + std::string{kNbsp} + "5",
                                {.decimalSeparator = kNbsp, .groupSeparator = ""}) == "1.5");
    // A second one is still malformed.
    CHECK(normalizeLocaleNumber(std::string{"1"} + std::string{kNbsp} + "5" + std::string{kNbsp} + "2",
                                {.decimalSeparator = kNbsp, .groupSeparator = ""}) == std::nullopt);
}

TEST_CASE("render::normalizeLocaleNumber rejects a stray separator byte", "[render][locale]") {
    // A lone continuation byte of a multi-byte separator is not the separator,
    // and must not be silently stripped.
    CHECK(normalizeLocaleNumber(std::string{"1"} + std::string{kNarrowNbsp.substr(0, 1)} + "050",
                                {.decimalSeparator = ",", .groupSeparator = kNarrowNbsp}) == std::nullopt);
}

TEST_CASE("render::formatCanonicalNumber emits a multi-byte group separator", "[render][locale]") {
    CHECK(formatCanonicalNumber("1050.25", {.decimalSeparator = ",", .groupSeparator = kNarrowNbsp}) ==
          std::string{"1"} + std::string{kNarrowNbsp} + "050,25");
    CHECK(formatCanonicalNumber("1234567", {.decimalSeparator = ",", .groupSeparator = kNbsp}) ==
          std::string{"1"} + std::string{kNbsp} + "234" + std::string{kNbsp} + "567");
}

TEST_CASE("render::locale_format round-trips through a multi-byte separator", "[render][locale]") {
    auto const display = formatCanonicalNumber("1050.25", {.decimalSeparator = ",", .groupSeparator = kNarrowNbsp});
    CHECK(normalizeLocaleNumber(display, {.decimalSeparator = ",", .groupSeparator = kNarrowNbsp}) == "1050.25");
}

// ── A sign after the decimal separator is not "leading" ──
//
// `sawAnyOutput` was only set at the bottom of the loop, and the
// decimal-separator branch `continue`d past it -- so after a separator the sign
// guard still believed nothing had been emitted and accepted an injected sign.
// The QML mirror (src/qt/forms/qml/DynamicForm.qml, documented as mirroring
// this function) always rejected these, so the two control edges disagreed.
TEST_CASE("normalizeLocaleNumber: a sign after the decimal separator is rejected", "[render][locale][morph497]") {
    // de-DE: comma decimal, dot grouping -- the reported shape.
    REQUIRE_FALSE(
        morph::render::normalizeLocaleNumber(",-5", {.decimalSeparator = ",", .groupSeparator = "."}).has_value());
    // en-US equivalent.
    REQUIRE_FALSE(
        morph::render::normalizeLocaleNumber(".-5", {.decimalSeparator = ".", .groupSeparator = ","}).has_value());
    // With a group separator stripped first, which is the case the guard's own
    // comment is about.
    REQUIRE_FALSE(
        morph::render::normalizeLocaleNumber("1.,-5", {.decimalSeparator = ",", .groupSeparator = "."}).has_value());

    // Control: the guard already worked once a digit had been emitted, and must
    // keep working.
    REQUIRE_FALSE(
        morph::render::normalizeLocaleNumber("1-2", {.decimalSeparator = ".", .groupSeparator = ","}).has_value());
    // Control: a genuinely leading sign still parses.
    REQUIRE(morph::render::normalizeLocaleNumber("-1,5", {.decimalSeparator = ",", .groupSeparator = "."}) == "-1.5");
}

TEST_CASE("normalizeLocaleNumber: the loose shapes stay accepted, in step with the QML mirror",
          "[render][locale][morph497]") {
    // Deliberately NOT narrowed to `-?[0-9]+(\.[0-9]+)?`: DynamicForm.qml's
    // normalizeLocaleNumber accepts all three, and tightening one edge alone
    // would put them back out of step. Documented on the function.
    REQUIRE(morph::render::normalizeLocaleNumber(".5", {.decimalSeparator = ".", .groupSeparator = ","}) == ".5");
    REQUIRE(morph::render::normalizeLocaleNumber("5.", {.decimalSeparator = ".", .groupSeparator = ","}) == "5.");
    REQUIRE(morph::render::normalizeLocaleNumber(".", {.decimalSeparator = ".", .groupSeparator = ","}) == ".");
}

// ── A group separator is validated, not stripped ─────────────────────────────
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
    CHECK(normalizeLocaleNumber("1.5", {.decimalSeparator = ",", .groupSeparator = "."}) == std::nullopt);
    CHECK(normalizeLocaleNumber("1.50", {.decimalSeparator = ",", .groupSeparator = "."}) == std::nullopt);
    CHECK(normalizeLocaleNumber("1.2.3.4", {.decimalSeparator = ",", .groupSeparator = "."}) == std::nullopt);

    // The mirror image: en-US locale, EU-style decimal typed. Returned "15".
    CHECK(normalizeLocaleNumber("1,5", {.decimalSeparator = ".", .groupSeparator = ","}) == std::nullopt);

    // And with a multi-byte group separator, where the same mistake is a
    // narrow no-break space away from a well-formed entry.
    CHECK(normalizeLocaleNumber(std::string{"1"} + std::string{kNarrowNbsp} + "5",
                                {.decimalSeparator = ",", .groupSeparator = kNarrowNbsp}) == std::nullopt);
}

TEST_CASE("normalizeLocaleNumber: equal decimal and group separators are rejected rather than guessed",
          "[render][locale][morph574]") {
    // One string in both roles has no defensible reading, and the old code
    // silently ate the decimal: this returned "15".
    CHECK(normalizeLocaleNumber("1.5", {.decimalSeparator = ".", .groupSeparator = "."}) == std::nullopt);
    // Not even the shapes that would be unambiguous if you squinted: the
    // rejection is on the configuration, not on the text.
    CHECK(normalizeLocaleNumber("1.050", {.decimalSeparator = ".", .groupSeparator = "."}) == std::nullopt);
    CHECK(normalizeLocaleNumber("1", {.decimalSeparator = ".", .groupSeparator = "."}) == std::nullopt);
    // Control: an empty group separator is "this locale does not group", which
    // is a different statement and stays legal.
    CHECK(normalizeLocaleNumber("1.5", {.decimalSeparator = ".", .groupSeparator = ""}) == "1.5");
}

TEST_CASE("normalizeLocaleNumber: a group separator must sit on a group boundary", "[render][locale][morph574]") {
    // Preceded by one to three digits...
    CHECK(normalizeLocaleNumber("1.050", {.decimalSeparator = "", .groupSeparator = "."}) == "1050");
    CHECK(normalizeLocaleNumber("12.050", {.decimalSeparator = "", .groupSeparator = "."}) == "12050");
    CHECK(normalizeLocaleNumber("123.050", {.decimalSeparator = "", .groupSeparator = "."}) == "123050");
    CHECK(normalizeLocaleNumber("1234.050", {.decimalSeparator = "", .groupSeparator = "."}) == std::nullopt);
    CHECK(normalizeLocaleNumber(".050", {.decimalSeparator = "", .groupSeparator = "."}) == std::nullopt);

    // ...followed by exactly three, at every group and at the end of the
    // integer part.
    CHECK(normalizeLocaleNumber("1.05", {.decimalSeparator = "", .groupSeparator = "."}) == std::nullopt);
    CHECK(normalizeLocaleNumber("1.0500", {.decimalSeparator = "", .groupSeparator = "."}) == std::nullopt);
    CHECK(normalizeLocaleNumber("1.050.", {.decimalSeparator = "", .groupSeparator = "."}) == std::nullopt);
    CHECK(normalizeLocaleNumber("1.000.00", {.decimalSeparator = "", .groupSeparator = "."}) == std::nullopt);
    CHECK(normalizeLocaleNumber("1.000.000", {.decimalSeparator = "", .groupSeparator = "."}) == "1000000");

    // ...and never after the decimal separator.
    CHECK(normalizeLocaleNumber("1,050.25", {.decimalSeparator = ",", .groupSeparator = "."}) == std::nullopt);
    CHECK(normalizeLocaleNumber("1.000,250.25", {.decimalSeparator = ",", .groupSeparator = "."}) == std::nullopt);

    // A grouping locale with no separator in the entry at all: there is no
    // placement to be wrong about, and the digits pass through.
    CHECK(normalizeLocaleNumber("1050", {.decimalSeparator = ",", .groupSeparator = "."}) == "1050");

    // A non-digit inside a grouping locale restarts the digit run rather than
    // being counted into it -- and is malformed for the ordinary reason.
    CHECK(normalizeLocaleNumber("1.0x0", {.decimalSeparator = "", .groupSeparator = "."}) == std::nullopt);
}

TEST_CASE("normalizeLocaleNumber: every well-formed locale entry still normalises", "[render][locale][morph574]") {
    // The validation must not cost a single legitimate entry -- this is the
    // half of the change that the rejection cases cannot show.
    CHECK(normalizeLocaleNumber("1.050,25", {.decimalSeparator = ",", .groupSeparator = "."}) == "1050.25");
    CHECK(normalizeLocaleNumber("-1.050,25", {.decimalSeparator = ",", .groupSeparator = "."}) == "-1050.25");
    CHECK(normalizeLocaleNumber("1.000.000,25", {.decimalSeparator = ",", .groupSeparator = "."}) == "1000000.25");
    CHECK(normalizeLocaleNumber("1050,25", {.decimalSeparator = ",", .groupSeparator = "."}) ==
          "1050.25");  // ungrouped
    CHECK(normalizeLocaleNumber("1,050.25", {.decimalSeparator = ".", .groupSeparator = ","}) == "1050.25");  // en-US
    CHECK(normalizeLocaleNumber(std::string{"1"} + std::string{kNarrowNbsp} + "050,25",
                                {.decimalSeparator = ",", .groupSeparator = kNarrowNbsp}) == "1050.25");  // fr-FR

    // A grouped entry round-trips through the display direction unchanged.
    auto const canonical = normalizeLocaleNumber("1.000.000,25", {.decimalSeparator = ",", .groupSeparator = "."});
    REQUIRE(canonical.has_value());
    CHECK(formatCanonicalNumber(*canonical, {.decimalSeparator = ",", .groupSeparator = "."}) == "1.000.000,25");
}

// ──── The negative sign is locale data, and is not always one byte ────
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
    CHECK(normalizeLocaleNumber(entry(kSignEuEs, "5"),
                                {.decimalSeparator = ".", .groupSeparator = "", .negativeSign = kSignEuEs}) == "-5");
    CHECK(normalizeLocaleNumber(entry(kSignEuEs, "1050,25"),
                                {.decimalSeparator = ",", .groupSeparator = ".", .negativeSign = kSignEuEs}) ==
          "-1050.25");

    // The control for the defect: with the sign left at its ASCII default, the
    // same entry is still rejected -- that default is the whole of today's
    // behaviour.
    CHECK(normalizeLocaleNumber(entry(kSignEuEs, "5"), {.decimalSeparator = ".", .groupSeparator = ""}) ==
          std::nullopt);
}

TEST_CASE("normalizeLocaleNumber: a bidi-prefixed sign is matched as a whole string", "[render][locale][morph583]") {
    // Two and three code points respectively. A one-unit comparison cannot
    // match either, which is why both edges compare the whole string.
    CHECK(normalizeLocaleNumber(entry(kSignFaIr, "5"),
                                {.decimalSeparator = ".", .groupSeparator = "", .negativeSign = kSignFaIr}) ==
          "-5");  // fa_IR
    CHECK(normalizeLocaleNumber(entry(kSignAzIr, "5"),
                                {.decimalSeparator = ".", .groupSeparator = "", .negativeSign = kSignAzIr}) ==
          "-5");  // az_IR, 3 code points
    CHECK(normalizeLocaleNumber(entry(kSignArEg, "5"),
                                {.decimalSeparator = ".", .groupSeparator = "", .negativeSign = kSignArEg}) ==
          "-5");  // ar_EG, U+061C prefix

    // Same controls: rejected today, for each shape.
    CHECK(normalizeLocaleNumber(entry(kSignFaIr, "5"), {.decimalSeparator = ".", .groupSeparator = ""}) ==
          std::nullopt);
    CHECK(normalizeLocaleNumber(entry(kSignAzIr, "5"), {.decimalSeparator = ".", .groupSeparator = ""}) ==
          std::nullopt);
}

TEST_CASE("normalizeLocaleNumber: ar_DZ fails today even though its sign is the ASCII hyphen",
          "[render][locale][morph583]") {
    // The case that shows this is not "the U+2212 locales": ar_DZ's sign *is*
    // '-', prefixed by U+200E. The stray prefix byte is what the per-byte scan
    // rejects, so matching the hyphen byte-wise never helped it.
    CHECK(normalizeLocaleNumber(entry(kSignArDz, "5"), {.decimalSeparator = ".", .groupSeparator = ""}) ==
          std::nullopt);
    CHECK(normalizeLocaleNumber(entry(kSignArDz, "5"),
                                {.decimalSeparator = ".", .groupSeparator = "", .negativeSign = kSignArDz}) == "-5");
}

TEST_CASE("normalizeLocaleNumber: the ASCII hyphen stays accepted in every locale", "[render][locale][morph583]") {
    // U+2212 and the bidi marks are on no keyboard. Matching only the locale's
    // own spelling would reject the sign the user can actually type and leave
    // them no way to enter a negative number at all.
    CHECK(normalizeLocaleNumber("-5", {.decimalSeparator = ".", .groupSeparator = "", .negativeSign = kSignEuEs}) ==
          "-5");
    CHECK(normalizeLocaleNumber("-5", {.decimalSeparator = ".", .groupSeparator = "", .negativeSign = kSignFaIr}) ==
          "-5");
    // And the plain ASCII locale is untouched -- the default parameter means no
    // existing caller changed behaviour.
    CHECK(normalizeLocaleNumber("-5", {.decimalSeparator = ".", .groupSeparator = ""}) == "-5");
    CHECK(normalizeLocaleNumber("-1.050,25", {.decimalSeparator = ",", .groupSeparator = "."}) == "-1050.25");
}

TEST_CASE("normalizeLocaleNumber: a locale sign is still rejected off the leading position",
          "[render][locale][morph583]") {
    // The leading-position rule is about the *output*, so it has to hold for a
    // multi-byte sign exactly as it does for '-'.
    CHECK(normalizeLocaleNumber("1" + entry(kSignEuEs, "2"),
                                {.decimalSeparator = ".", .groupSeparator = "", .negativeSign = kSignEuEs}) ==
          std::nullopt);
    CHECK(normalizeLocaleNumber("," + entry(kSignEuEs, "5"),
                                {.decimalSeparator = ",", .groupSeparator = ".", .negativeSign = kSignEuEs}) ==
          std::nullopt);
    // A sign and nothing else is not a number, whatever its spelling.
    CHECK(normalizeLocaleNumber(
              kSignEuEs, {.decimalSeparator = ".", .groupSeparator = "", .negativeSign = kSignEuEs}) == std::nullopt);
    CHECK(normalizeLocaleNumber(
              kSignAzIr, {.decimalSeparator = ".", .groupSeparator = "", .negativeSign = kSignAzIr}) == std::nullopt);
}

TEST_CASE("formatCanonicalNumber: the display edge emits the locale's sign", "[render][locale][morph583]") {
    // Before this, the sign was a hardcoded '-' whatever the locale -- so even
    // a caller that knew its locale's sign could not ask for it.
    CHECK(formatCanonicalNumber("-1050.25",
                                {.decimalSeparator = ",", .groupSeparator = ".", .negativeSign = kSignEuEs}) ==
          entry(kSignEuEs, "1.050,25"));
    CHECK(formatCanonicalNumber("-5", {.decimalSeparator = ".", .groupSeparator = "", .negativeSign = kSignFaIr}) ==
          entry(kSignFaIr, "5"));
    // A positive is untouched: the sign string is only ever emitted for a
    // negative, so a locale-specific sign cannot leak into a positive display.
    CHECK(formatCanonicalNumber(
              "1050.25", {.decimalSeparator = ",", .groupSeparator = ".", .negativeSign = kSignEuEs}) == "1.050,25");
    CHECK(formatCanonicalNumber("-1050.25", {.decimalSeparator = ",", .groupSeparator = "."}) == "-1.050,25");
}

TEST_CASE("locale_format: the pair is inverse for every measured sign spelling", "[render][locale][morph583]") {
    // The property the issue is actually about: whatever the display edge
    // emits, the entry edge takes back to the identical canonical text.
    for (auto const& sign : {kSignEuEs, kSignFaIr, kSignArDz, kSignAzIr, kSignArEg}) {
        INFO("sign = " << sign);
        auto const display =
            formatCanonicalNumber("-1050.25", {.decimalSeparator = ",", .groupSeparator = ".", .negativeSign = sign});
        CHECK(display == entry(sign, "1.050,25"));
        CHECK(normalizeLocaleNumber(display, {.decimalSeparator = ",", .groupSeparator = ".", .negativeSign = sign}) ==
              "-1050.25");
    }
}

TEST_CASE("locale_format: an empty negative sign reads as '-', not as 'no sign'", "[render][locale][morph583]") {
    // Unlike a group separator, there is no locale without a negative sign, so
    // empty cannot mean absence. On the display edge it would be a silently
    // wrong value: -5 formatted to "5" is a valid number of the wrong sign,
    // which is silent corruption, not a rejection.
    CHECK(formatCanonicalNumber("-5", {.decimalSeparator = ".", .groupSeparator = "", .negativeSign = ""}) == "-5");
    CHECK(normalizeLocaleNumber("-5", {.decimalSeparator = ".", .groupSeparator = "", .negativeSign = ""}) == "-5");
    // And an empty needle must not match at every index: a scan that treated it
    // as a separator would never advance.
    CHECK(normalizeLocaleNumber("123", {.decimalSeparator = ".", .groupSeparator = "", .negativeSign = ""}) == "123");
}

// ──── A leading positive sign is accepted, and dropped ───────────────────────
//
// `normalizeLocaleNumber` had no notion of a positive sign at all: a leading
// '+' fell through to the "any other character is malformed" arm, so an
// explicitly-positive entry was rejected in every locale, "C" included.
// Measured on be64026a:
//
//   normalize("+5", dec=".", grp="")  -> NULLOPT
//   normalize("5",  dec=".", grp="")  -> "5"     (control)
//   normalize("-5", dec=".", grp="")  -> "-5"    (control)
//
// Of the 711 locales Qt 6.11.2 reports through QLocale::matchingLocales, 54
// spell QLocale::positiveSign as more than one code point:
//
//   U+002B                 657   e.g. C
//   U+061C U+002B           24   e.g. ar_EG
//   U+200E U+002B           11   e.g. ar_DZ
//   U+200E U+002B U+200E    17   e.g. az_IR
//   U+200F U+002B            2   e.g. ckb_IQ
//
// Unlike the negative side there is no U+2212 analogue, so every non-ASCII
// spelling here is multi-code-point: whole-string matching is the only thing
// that can match any of them.
namespace {
constexpr std::string_view kPlusArEg = "\xD8\x9C+";                  // ar_EG: U+061C U+002B
constexpr std::string_view kPlusArDz = "\xE2\x80\x8E+";              // ar_DZ: U+200E U+002B
constexpr std::string_view kPlusAzIr = "\xE2\x80\x8E+\xE2\x80\x8E";  // az_IR: U+200E U+002B U+200E
constexpr std::string_view kPlusCkbIq = "\xE2\x80\x8F+";             // ckb_IQ: U+200F U+002B
}  // namespace

TEST_CASE("normalizeLocaleNumber: a bare ASCII '+' is accepted and dropped", "[render][locale][morph596]") {
    // THE case. This was std::nullopt before, in every locale.
    CHECK(normalizeLocaleNumber("+5", {.decimalSeparator = ".", .groupSeparator = ""}) == "5");
    CHECK(normalizeLocaleNumber("+1.050,25", {.decimalSeparator = ",", .groupSeparator = "."}) == "1050.25");
    CHECK(normalizeLocaleNumber("+0.001", {.decimalSeparator = ".", .groupSeparator = ""}) == "0.001");

    // Dropped, not carried: canonical text is `-?[0-9]+(\.[0-9]+)?` and has no
    // '+' in it. "+5" must be "5", never "+5" -- a "+"-prefixed result would
    // fail the renderer's own `/^-?\d+(\.\d+)?$/` gate and every exact digit
    // routine downstream.
    auto const plus = normalizeLocaleNumber("+5", {.decimalSeparator = ".", .groupSeparator = ""});
    REQUIRE(plus.has_value());
    CHECK(*plus == "5");
    CHECK(!plus->contains('+'));
    CHECK(plus == normalizeLocaleNumber("5", {.decimalSeparator = ".", .groupSeparator = ""}));
}

TEST_CASE("normalizeLocaleNumber: a locale's multi-code-point positive sign is matched whole",
          "[render][locale][morph596]") {
    // Two and three code points. A one-byte comparison could match none of
    // them, which is why the parameter is a view matched with starts_with.
    REQUIRE(kPlusArEg.size() == 3);
    REQUIRE(kPlusAzIr.size() == 7);

    CHECK(normalizeLocaleNumber(
              entry(kPlusArEg, "5"),
              {.decimalSeparator = ".", .groupSeparator = "", .negativeSign = "-", .positiveSign = kPlusArEg}) == "5");
    CHECK(normalizeLocaleNumber(
              entry(kPlusArDz, "5"),
              {.decimalSeparator = ".", .groupSeparator = "", .negativeSign = "-", .positiveSign = kPlusArDz}) == "5");
    CHECK(normalizeLocaleNumber(
              entry(kPlusAzIr, "5"),
              {.decimalSeparator = ".", .groupSeparator = "", .negativeSign = "-", .positiveSign = kPlusAzIr}) == "5");
    CHECK(normalizeLocaleNumber(
              entry(kPlusCkbIq, "5"),
              {.decimalSeparator = ".", .groupSeparator = "", .negativeSign = "-", .positiveSign = kPlusCkbIq}) ==
          "5");
    CHECK(normalizeLocaleNumber(
              entry(kPlusArEg, "1.050,25"),
              {.decimalSeparator = ",", .groupSeparator = ".", .negativeSign = "-", .positiveSign = kPlusArEg}) ==
          "1050.25");

    // Controls: with positiveSign left at its ASCII default, the bidi-prefixed
    // spellings are still rejected -- the prefix byte is not a digit, and the
    // bare '+' match cannot reach past it. This is what makes the parameter,
    // rather than the unconditional ASCII acceptance above, the thing under
    // test in this case.
    CHECK(normalizeLocaleNumber(entry(kPlusArEg, "5"), {.decimalSeparator = ".", .groupSeparator = ""}) ==
          std::nullopt);
    CHECK(normalizeLocaleNumber(entry(kPlusAzIr, "5"), {.decimalSeparator = ".", .groupSeparator = ""}) ==
          std::nullopt);
}

TEST_CASE("normalizeLocaleNumber: the ASCII '+' stays accepted in a bidi-sign locale", "[render][locale][morph596]") {
    // The same rule the negative sign follows: the locale's own spelling is on
    // no keyboard, so
    // matching only it would reject the sign the user can actually type.
    CHECK(normalizeLocaleNumber(
              "+5", {.decimalSeparator = ".", .groupSeparator = "", .negativeSign = "-", .positiveSign = kPlusArEg}) ==
          "5");
    CHECK(normalizeLocaleNumber(
              "+5", {.decimalSeparator = ".", .groupSeparator = "", .negativeSign = "-", .positiveSign = kPlusAzIr}) ==
          "5");
    // An empty positiveSign leaves the ASCII spelling as the only one, rather
    // than disabling the sign -- and must not match at every index, which would
    // stall the scan.
    CHECK(normalizeLocaleNumber(
              "+5", {.decimalSeparator = ".", .groupSeparator = "", .negativeSign = "-", .positiveSign = ""}) == "5");
    CHECK(normalizeLocaleNumber(
              "123", {.decimalSeparator = ".", .groupSeparator = "", .negativeSign = "-", .positiveSign = ""}) ==
          "123");
}

TEST_CASE("normalizeLocaleNumber: a positive sign obeys the same leading-position rule",
          "[render][locale][morph596]") {
    // The leading-position rule is about the *output*, so accepting a new sign
    // spelling must not open a new way to inject one.
    CHECK(normalizeLocaleNumber("1+2", {.decimalSeparator = ".", .groupSeparator = ""}) == std::nullopt);
    CHECK(normalizeLocaleNumber("+-5", {.decimalSeparator = ".", .groupSeparator = ""}) == std::nullopt);
    CHECK(normalizeLocaleNumber("-+5", {.decimalSeparator = ".", .groupSeparator = ""}) == std::nullopt);
    CHECK(normalizeLocaleNumber("++5", {.decimalSeparator = ".", .groupSeparator = ""}) == std::nullopt);
    CHECK(normalizeLocaleNumber(",+5", {.decimalSeparator = ",", .groupSeparator = "."}) ==
          std::nullopt);  // straight after the decimal point
    CHECK(normalizeLocaleNumber(
              "1" + entry(kPlusArEg, "2"),
              {.decimalSeparator = ".", .groupSeparator = "", .negativeSign = "-", .positiveSign = kPlusArEg}) ==
          std::nullopt);
    // A sign and nothing else is not a number, whatever its spelling -- and for
    // the positive sign this is the `canonical.empty()` arm rather than the
    // `canonical == "-"` one, because nothing is emitted at all.
    CHECK(normalizeLocaleNumber("+", {.decimalSeparator = ".", .groupSeparator = ""}) == std::nullopt);
    CHECK(normalizeLocaleNumber(
              kPlusArEg,
              {.decimalSeparator = ".", .groupSeparator = "", .negativeSign = "-", .positiveSign = kPlusArEg}) ==
          std::nullopt);
}

TEST_CASE("formatCanonicalNumber: the display edge never emits a positive sign", "[render][locale][morph596]") {
    // The deliberate asymmetry, pinned so that "make it symmetric" is a test
    // failure rather than a tidy-up. positiveSign is '+' in 657 of 711 locales,
    // so emitting it would turn every positive number in every form into "+5".
    CHECK(formatCanonicalNumber("5", {.decimalSeparator = ".", .groupSeparator = ""}) == "5");
    CHECK(formatCanonicalNumber("1050.25", {.decimalSeparator = ",", .groupSeparator = "."}) == "1.050,25");
    CHECK(formatCanonicalNumber(
              "1050.25", {.decimalSeparator = ",", .groupSeparator = ".", .negativeSign = kSignEuEs}) == "1.050,25");
    // The negative direction is untouched by any of this.
    CHECK(formatCanonicalNumber("-1050.25",
                                {.decimalSeparator = ",", .groupSeparator = ".", .negativeSign = kSignEuEs}) ==
          entry(kSignEuEs, "1.050,25"));
}

TEST_CASE("locale_format: the pair is not inverse across a positive sign, by design", "[render][locale][morph596]") {
    // Entry accepts a spelling display never produces. Recorded as a test so
    // the relationship is pinned rather than assumed: normalising a
    // "+"-prefixed entry and formatting the result back gives the *unsigned*
    // display, the same text a user who omitted the sign would see.
    auto const canonical = normalizeLocaleNumber("+1.050,25", {.decimalSeparator = ",", .groupSeparator = "."});
    REQUIRE(canonical == "1050.25");
    CHECK(formatCanonicalNumber(*canonical, {.decimalSeparator = ",", .groupSeparator = "."}) == "1.050,25");
    CHECK(formatCanonicalNumber(*canonical, {.decimalSeparator = ",", .groupSeparator = "."}) != "+1.050,25");
    // ...while the negative side still round-trips exactly, unchanged.
    auto const negative = normalizeLocaleNumber("-1.050,25", {.decimalSeparator = ",", .groupSeparator = "."});
    REQUIRE(negative == "-1050.25");
    CHECK(formatCanonicalNumber(*negative, {.decimalSeparator = ",", .groupSeparator = "."}) == "-1.050,25");
}

TEST_CASE("normalizeLocaleNumber: the new parameter costs no existing behaviour", "[render][locale][morph596]") {
    // Four- and three-argument callers are unchanged: positiveSign is
    // defaulted, and nothing that was accepted or rejected before has moved.
    CHECK(normalizeLocaleNumber("1.050,25", {.decimalSeparator = ",", .groupSeparator = "."}) == "1050.25");
    CHECK(normalizeLocaleNumber("-1.050,25", {.decimalSeparator = ",", .groupSeparator = "."}) == "-1050.25");
    CHECK(normalizeLocaleNumber("1.5", {.decimalSeparator = ",", .groupSeparator = "."}) ==
          std::nullopt);  // grouping validation still holds
    CHECK(normalizeLocaleNumber("abc", {.decimalSeparator = ".", .groupSeparator = ""}) == std::nullopt);
    CHECK(normalizeLocaleNumber("", {.decimalSeparator = ".", .groupSeparator = ""}) == std::nullopt);
    CHECK(normalizeLocaleNumber(entry(kSignEuEs, "5"),
                                {.decimalSeparator = ".", .groupSeparator = "", .negativeSign = kSignEuEs}) ==
          "-5");  // the whole-string sign match still holds
}

// ──── The QML mirror's separators, cross-checked here ────────────────────────
//
// This block adds no C++ behaviour. `normalizeLocaleNumber` has matched both
// separators as whole strings since it was written -- "accepts a multi-byte
// group separator" above already pins that. The defect this guards against is
// in the *QML mirror* (`src/qt/forms/qml/DynamicForm.qml`), which can compare one UTF-16
// code unit (`ch === groupSeparator`) while this side used
// `rest.starts_with`. docs/spec/forms/forms.md, "Both edges, or neither": a
// divergence between the two is a divergence in what the product accepts, so
// the fix is worth only as much as the evidence that the edges now agree.
//
// What that evidence is, stated plainly, because its shape is unusual:
//
//   * No locale reaches this. Measured on this revision with
//     QLocale::matchingLocales under Qt 6.11.2, over all 711 locales:
//
//       decimalPoint   with size() > 1 (UTF-16 units): 0
//       groupSeparator with size() > 1 (UTF-16 units): 0
//       negativeSign   with size() > 1:                54   <- the control
//       positiveSign   with size() > 1:                54   <- the control
//
//     The two sign counts are the control: this is not a measurement that
//     returns 0 for any locale field you point it at. All nine distinct
//     groupSeparator spellings Qt reports (U+0027, U+002C, U+002E, U+00A0,
//     U+060C, U+066C, U+12C8, U+202F, U+2E41) and all three decimalPoint
//     spellings (U+002C, U+002E, U+066B) are single code units. So this is a
//     consistency fix, and nobody is affected today.
//
//   * Which is exactly why the corpus below is synthetic. A test driven by a
//     real locale could not tell the fixed mirror from the broken one -- with
//     a one-unit separator `ch === sep` and `startsWith(sep, i)` agree on all
//     711 -- and would be a check that passes whatever the code does.
//
// The identical rows are pinned against the mirror in
// src/qt/forms/tests/tst_i18n.qml -- its two `test_aMultiUnitSeparator...`
// functions and `test_theDisplayEdgeEmitsAMultiUnitSeparatorAndEntryTakesItBack`.
// The two
// lists are meant to be read side by side; a row that disagrees between them is
// the divergence the rule forbids. UTF-16 units there, UTF-8 bytes here -- the
// same separators, spelled for each edge's string type.
namespace {
// Two code points: a bidi mark before the separator, the shape 54 locales
// really give the signs. Spelled as explicit UTF-8 bytes for the same MSVC
// C4566 reason as kNarrowNbsp above, and split across two literals so the
// trailing '.'/',' cannot be read as a continuation of the \x escape.
constexpr std::string_view kGroup2 =
    "\xE2\x80\x8E"
    ".";  // U+200E U+002E
constexpr std::string_view kDecimal2 =
    "\xE2\x80\x8E"
    ",";  // U+200E U+002C
// One code point each, but two UTF-16 units (a surrogate pair) -- the sharper
// case for the mirror: a scan that iterated code points rather than code units
// would still have failed on these.
constexpr std::string_view kGroup4 = "\xF0\x9D\x85\xAD";    // U+1D16D
constexpr std::string_view kDecimal4 = "\xF0\x9D\x85\xAE";  // U+1D16E

/// @brief Concatenates the parts of one corpus row into an entry string.
///
/// A named helper for the same reason `entry` above is one: the constants are
/// views, so `kGroup2 + "050"` does not compile, and `std::string{...} + ...`
/// at forty call sites reads worse than this does.
[[nodiscard]] std::string joined(std::initializer_list<std::string_view> parts) {
    std::string out;
    for (auto const part : parts) {
        out += part;
    }
    return out;
}
}  // namespace

TEST_CASE("locale_format: the multi-unit separator corpus the QML mirror now shares", "[render][locale][morph599]") {
    // The premise of the spelling: these really are multi-unit on both edges.
    REQUIRE(kGroup2.size() == 4);  // 3 UTF-8 bytes + 1; 2 UTF-16 units
    REQUIRE(kDecimal2.size() == 4);
    REQUIRE(kGroup4.size() == 4);  // 4 UTF-8 bytes; 2 UTF-16 units
    REQUIRE(kDecimal4.size() == 4);

    CHECK(normalizeLocaleNumber(joined({"1", kGroup2, "050", kDecimal2, "25"}),
                                {.decimalSeparator = kDecimal2, .groupSeparator = kGroup2}) == "1050.25");
    CHECK(normalizeLocaleNumber(joined({"-1", kGroup2, "050", kDecimal2, "25"}),
                                {.decimalSeparator = kDecimal2, .groupSeparator = kGroup2}) == "-1050.25");
    CHECK(normalizeLocaleNumber(joined({"+1", kGroup2, "050", kDecimal2, "25"}),
                                {.decimalSeparator = kDecimal2, .groupSeparator = kGroup2}) == "1050.25");
    CHECK(normalizeLocaleNumber(joined({"1", kGroup4, "050", kDecimal4, "25"}),
                                {.decimalSeparator = kDecimal4, .groupSeparator = kGroup4}) == "1050.25");
    CHECK(normalizeLocaleNumber(joined({"1", kGroup4, "050", kGroup4, "000"}),
                                {.decimalSeparator = "", .groupSeparator = kGroup4}) == "1050000");
    CHECK(normalizeLocaleNumber(joined({"5", kDecimal2, "25"}),
                                {.decimalSeparator = kDecimal2, .groupSeparator = ""}) == "5.25");
}

TEST_CASE("locale_format: a multi-unit separator is validated exactly as a one-unit one is",
          "[render][locale][morph599]") {
    // The grouping validation and the leading-position rule are
    // stated over "the separator", so they have to hold when it is longer than
    // one unit -- on both edges. Same rows as the mirror's
    // `test_aMultiUnitSeparatorIsStillValidatedTheSameWay`.
    CHECK(normalizeLocaleNumber(joined({"1", kGroup2, "5"}),
                                {.decimalSeparator = kDecimal2, .groupSeparator = kGroup2}) == std::nullopt);
    CHECK(normalizeLocaleNumber(joined({"1", kGroup2, "2", kGroup2, "3", kGroup2, "4"}),
                                {.decimalSeparator = kDecimal2, .groupSeparator = kGroup2}) == std::nullopt);
    CHECK(normalizeLocaleNumber(joined({"1", kDecimal2, "5", kGroup2, "000"}),
                                {.decimalSeparator = kDecimal2, .groupSeparator = kGroup2}) == std::nullopt);
    CHECK(normalizeLocaleNumber(joined({"1", kDecimal2, "0", kDecimal2, "5"}),
                                {.decimalSeparator = kDecimal2, .groupSeparator = kGroup2}) == std::nullopt);
    CHECK(normalizeLocaleNumber(joined({kDecimal2, "-5"}),
                                {.decimalSeparator = kDecimal2, .groupSeparator = kGroup2}) == std::nullopt);
    CHECK(normalizeLocaleNumber(joined({"1", kGroup2, "050"}),
                                {.decimalSeparator = kGroup2, .groupSeparator = kGroup2}) == std::nullopt);
    // A lone prefix of the separator is not the separator: whole-string
    // matching must not degrade into "any part of it will do".
    CHECK(normalizeLocaleNumber(joined({"1", "\xE2\x80\x8E", "050", kDecimal2, "25"}),
                                {.decimalSeparator = kDecimal2, .groupSeparator = kGroup2}) == std::nullopt);
    CHECK(normalizeLocaleNumber("1.050,25", {.decimalSeparator = kDecimal2, .groupSeparator = kGroup2}) ==
          std::nullopt);
}

TEST_CASE("locale_format: the pair round-trips through a multi-unit separator", "[render][locale][morph599]") {
    // Where "both edges, or neither" bites. `formatCanonicalNumber` has always
    // emitted the separators whole on both sides, so with a multi-unit
    // separator the mirror's display edge produced text its own entry edge then
    // rejected -- the two-edge disagreement, for a locale that does not exist yet.
    // This side round-tripped throughout; that is what made the two disagree.
    auto const display = formatCanonicalNumber(
        "-1050.25", {.decimalSeparator = kDecimal2, .groupSeparator = kGroup2, .negativeSign = kSignEuEs});
    CHECK(display == joined({kSignEuEs, "1", kGroup2, "050", kDecimal2, "25"}));
    CHECK(normalizeLocaleNumber(
              display, {.decimalSeparator = kDecimal2, .groupSeparator = kGroup2, .negativeSign = kSignEuEs}) ==
          "-1050.25");
}

// ---- The digits are locale data too ----------------------------------------
//
// Measured on this revision with QLocale::matchingLocales under Qt 6.11.2, over
// all 711 locales: 76 report a zeroDigit other than ASCII '0', across eleven
// distinct digit sets.
//
//   U+0030   604 locales   e.g. C           <- the control
//   U+0660    26 locales   e.g. ar_BH
//   U+06F0    19 locales   e.g. fa_IR
//   U+1E950   12 locales   e.g. ff_Adlm_BF  <- astral
//   U+0966     8 locales   e.g. bgc_IN
//   U+09E6     4 locales   e.g. as_IN
//   U+11136    2 locales   e.g. ccp_BD      <- astral
//   U+07C0     1 locale    nqo_GN
//   U+0F20     1 locale    dz_BT
//   U+1040     1 locale    my_MM
//   U+1C50     1 locale    sat_IN
//   U+ABF0     1 locale    mni_IN
//
// The issue recorded 24 as the figure that could be defended, from *sign* data
// rather than digit data, and said so. 76 is the measured one. Two of the sets
// are outside the BMP, which is why the scan decodes a code point instead of
// widening a byte comparison: a digit there is four UTF-8 bytes.
//
// The same corpus is pinned against the QML mirror in
// src/qt/forms/tests/tst_i18n.qml ([morph591] there). docs/spec/forms/forms.md,
// "Both edges, or neither".
namespace {
struct DigitSet {
    std::string_view name;
    std::string_view zero;
    std::string_view five;
};

// One representative per distinct set, spelled as explicit UTF-8 bytes with the
// code point in the comment -- the house style for this file, and the reason
// the QML mirror spells them as escapes too: a digit that renders as itself is
// still unreadable when a reviewer does not read that script.
constexpr std::array<DigitSet, 11> kDigitSets = {{
    {.name = "ar_BH", .zero = "\xD9\xA0", .five = "\xD9\xA5"},                    // U+0660, U+0665
    {.name = "fa_IR", .zero = "\xDB\xB0", .five = "\xDB\xB5"},                    // U+06F0, U+06F5
    {.name = "nqo_GN", .zero = "\xDF\x80", .five = "\xDF\x85"},                   // U+07C0, U+07C5
    {.name = "bgc_IN", .zero = "\xE0\xA5\xA6", .five = "\xE0\xA5\xAB"},           // U+0966, U+096B
    {.name = "as_IN", .zero = "\xE0\xA7\xA6", .five = "\xE0\xA7\xAB"},            // U+09E6, U+09EB
    {.name = "dz_BT", .zero = "\xE0\xBC\xA0", .five = "\xE0\xBC\xA5"},            // U+0F20, U+0F25
    {.name = "my_MM", .zero = "\xE1\x81\x80", .five = "\xE1\x81\x85"},            // U+1040, U+1045
    {.name = "sat_IN", .zero = "\xE1\xB1\x90", .five = "\xE1\xB1\x95"},           // U+1C50, U+1C55
    {.name = "mni_IN", .zero = "\xEA\xAF\xB0", .five = "\xEA\xAF\xB5"},           // U+ABF0, U+ABF5
    {.name = "ccp_BD", .zero = "\xF0\x91\x84\xB6", .five = "\xF0\x91\x84\xBB"},   // U+11136, U+1113B
    {.name = "ff_Adlm", .zero = "\xF0\x9E\xA5\x90", .five = "\xF0\x9E\xA5\x95"},  // U+1E950, U+1E955
}};

// ar_EG/ar_BH's full set of locale facts, the one Qt reports.
constexpr std::string_view kArDecimal = "\xD9\xAB";    // U+066B
constexpr std::string_view kArGroup = "\xD9\xAC";      // U+066C
constexpr std::string_view kArZero = "\xD9\xA0";       // U+0660
constexpr std::string_view kArNegative = "\xD8\x9C-";  // U+061C U+002D
}  // namespace

TEST_CASE("formatCanonicalNumber: the display edge emits the locale's digits", "[render][locale][morph591]") {
    // The exact bytes QLocale("ar_BH").toString(-1050.25) produces under
    // Qt 6.11.2, measured rather than derived:
    //   U+061C U+002D U+0661 U+066C U+0660 U+0665 U+0660 U+066B U+0662 U+0665
    // Without a digit base this edge emits "\u061c-1\u066c050\u066b25" -- the
    // locale's sign and separators around ASCII digits, which is what makes the
    // pair self-consistent and the defect invisible from either side alone.
    CHECK(formatCanonicalNumber("-1050.25", {.decimalSeparator = kArDecimal,
                                             .groupSeparator = kArGroup,
                                             .negativeSign = kArNegative,
                                             .zeroDigit = kArZero}) ==
          "\xD8\x9C-\xD9\xA1\xD9\xAC\xD9\xA0\xD9\xA5\xD9\xA0\xD9\xAB\xD9\xA2\xD9\xA5");
}

TEST_CASE("normalizeLocaleNumber: the locale's own digits are accepted", "[render][locale][morph591]") {
    // The issue's reproduction, in reverse. Each of these was std::nullopt on
    // 0e3b8823: the scan compared one byte against ['0','9'] and the first byte
    // of a two-byte digit failed it.
    CHECK(normalizeLocaleNumber("\xD9\xA5", {.zeroDigit = kArZero}) == "5");
    CHECK(normalizeLocaleNumber("\xDB\xB5", {.zeroDigit = "\xDB\xB0"}) == "5");
    CHECK(normalizeLocaleNumber("\xD8\x9C-\xD9\xA5", {.negativeSign = kArNegative, .zeroDigit = kArZero}) == "-5");
    // Astral: one code point, four UTF-8 bytes.
    CHECK(normalizeLocaleNumber("\xF0\x91\x84\xBB", {.zeroDigit = "\xF0\x91\x84\xB6"}) == "5");
    CHECK(normalizeLocaleNumber("\xF0\x9E\xA5\x95", {.zeroDigit = "\xF0\x9E\xA5\x90"}) == "5");
}

TEST_CASE("locale_format: the pair round-trips through every measured digit set", "[render][locale][morph591]") {
    // The acceptance test this ticket exists for. A fix applied to one edge
    // only leaves the suite green -- teaching entry to accept U+0665 while
    // display keeps emitting '5' still round-trips, because entry accepts ASCII
    // too. So the round trip is asserted *and* the intermediate display text is
    // required to contain no ASCII digit: that second assertion is what fails
    // if formatCanonicalNumber did not move.
    for (auto const& set : kDigitSets) {
        CAPTURE(set.name);
        morph::render::NumericLocale const loc{.decimalSeparator = kArDecimal,
                                               .groupSeparator = kArGroup,
                                               .negativeSign = kArNegative,
                                               .zeroDigit = set.zero};
        for (std::string_view const canonical :
             {"0", "5", "-5", "1050.25", "-1050.25", "1000000.25", "0.001", "-0.001", "1234567"}) {
            CAPTURE(canonical);
            std::string const display = formatCanonicalNumber(canonical, loc);
            CHECK(display.find_first_of("0123456789") == std::string::npos);
            CHECK(normalizeLocaleNumber(display, loc) == std::string{canonical});
        }
    }
}

TEST_CASE("normalizeLocaleNumber: ASCII digits stay accepted in a native-digit locale", "[render][locale][morph591]") {
    // The same rule the positive sign follows, applied to digits: the locale's
    // own digits are
    // on the user's keyboard only if their keyboard has them. Entry therefore
    // accepts a spelling display never produces, exactly as it does for the
    // ASCII '+' and '-'.
    CHECK(normalizeLocaleNumber("5", {.zeroDigit = kArZero}) == "5");
    CHECK(normalizeLocaleNumber(
              "-1050.25", {.decimalSeparator = ".", .negativeSign = kArNegative, .zeroDigit = kArZero}) == "-1050.25");
    CHECK(normalizeLocaleNumber("5", {.zeroDigit = "\xF0\x9E\xA5\x90"}) == "5");
}

TEST_CASE("normalizeLocaleNumber: an entry may not mix digit families", "[render][locale][morph591]") {
    // A decision, not a consequence: "\u06655" is rejected rather than read as
    // 55. Neither a keyboard nor a display edge produces an interleaving, and
    // rejecting it matches the existing strictness about a sign anywhere but
    // the leading position. Recorded here because the alternative -- accepting
    // it -- is equally implementable, so only a test says which one morph does.
    CHECK(normalizeLocaleNumber("\xD9\xA5"
                                "5",
                                {.zeroDigit = kArZero}) == std::nullopt);
    CHECK(normalizeLocaleNumber("5\xD9\xA5", {.zeroDigit = kArZero}) == std::nullopt);
    CHECK(normalizeLocaleNumber("1\xD9\xA0\xD9\xA5"
                                "0",
                                {.zeroDigit = kArZero}) == std::nullopt);
    // Each family on its own is fine, which is what makes the rejection about
    // the mixing rather than about either spelling.
    CHECK(normalizeLocaleNumber("\xD9\xA5\xD9\xA5", {.zeroDigit = kArZero}) == "55");
    CHECK(normalizeLocaleNumber("55", {.zeroDigit = kArZero}) == "55");
    // In an ASCII-digit locale the two families are the same set, so nothing
    // can mix and the rule is invisible -- this is why it costs no existing
    // caller anything.
    CHECK(normalizeLocaleNumber("55", {}) == "55");
}

TEST_CASE("normalizeLocaleNumber: grouping is validated in the locale's digits too", "[render][locale][morph591]") {
    // groupingIsWellPlaced counts digits, and used to count bytes. A two-byte
    // digit would have reset the run-length counter on its own continuation
    // byte, so a correctly grouped native-digit entry -- what the display edge
    // now emits -- would have been rejected as badly grouped and the pair would
    // not round-trip. The grouping rule itself is unchanged: a short group is
    // still malformed.
    morph::render::NumericLocale const arEg{
        .decimalSeparator = kArDecimal, .groupSeparator = kArGroup, .zeroDigit = kArZero};
    CHECK(normalizeLocaleNumber("\xD9\xA1\xD9\xAC\xD9\xA0\xD9\xA5\xD9\xA0", arEg) == "1050");
    CHECK(normalizeLocaleNumber("\xD9\xA1\xD9\xAC\xD9\xA5", arEg) == std::nullopt);  // short group
}

TEST_CASE("normalizeLocaleNumber: the digit base does not widen what is well-formed UTF-8",
          "[render][locale][morph591]") {
    // The digit test is a range test on a decoded code point, so the decoder
    // has to be strict or it would widen acceptance by accident. C0 B5 is an
    // overlong encoding of U+0035: read leniently it would be the digit '5' in
    // the *default* locale, where the previous byte-range scan rejected it.
    // Measured: removing the overlong guard changes 13 rows of the
    // before/after sweep over the default locale.
    CHECK(normalizeLocaleNumber("\xC0\xB5", {}) == std::nullopt);      // overlong '5'
    CHECK(normalizeLocaleNumber("\xED\xA0\x80", {}) == std::nullopt);  // a surrogate
    CHECK(normalizeLocaleNumber("\xE2\x80", {}) == std::nullopt);      // truncated
    CHECK(normalizeLocaleNumber("\x80", {}) == std::nullopt);          // stray continuation byte
}

TEST_CASE("locale_format: a defaulted NumericLocale reproduces the previous defaults", "[render][locale][morph591]") {
    // zeroDigit defaults to "0", so every caller that does not name it is
    // byte-identical to the five-positional-parameter version. Asserted rather
    // than assumed, and separately measured: a 1680-case sweep (14 locale
    // configurations x 60 entries x both edges) run against the pre-#591 header
    // and against this one produced identical output.
    CHECK(morph::render::NumericLocale{}.decimalSeparator == ".");
    CHECK(morph::render::NumericLocale{}.groupSeparator.empty());
    CHECK(morph::render::NumericLocale{}.negativeSign == "-");
    CHECK(morph::render::NumericLocale{}.positiveSign == "+");
    CHECK(morph::render::NumericLocale{}.zeroDigit == "0");
    CHECK(normalizeLocaleNumber("1.050,25", {.decimalSeparator = ",", .groupSeparator = "."}) == "1050.25");
    CHECK(formatCanonicalNumber("1050.25", {.decimalSeparator = ",", .groupSeparator = "."}) == "1.050,25");
    CHECK(formatCanonicalNumber("-1050.25", {.decimalSeparator = ",", .groupSeparator = "."}) == "-1.050,25");
}

TEST_CASE("locale_format: an empty or malformed zeroDigit reads as ASCII '0'", "[render][locale][morph591]") {
    // The same reading an empty negativeSign gets, for the same reason: there
    // is no locale without digits, so empty cannot mean absence, and a base of
    // "nothing" would reject every entry the locale can produce.
    CHECK(normalizeLocaleNumber("55", {.zeroDigit = ""}) == "55");
    CHECK(formatCanonicalNumber("55", {.zeroDigit = ""}) == "55");
    CHECK(normalizeLocaleNumber("55", {.zeroDigit = "\xC0"}) == "55");  // not a code point
    CHECK(formatCanonicalNumber("55", {.zeroDigit = "\xC0"}) == "55");
}

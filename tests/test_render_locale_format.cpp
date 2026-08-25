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

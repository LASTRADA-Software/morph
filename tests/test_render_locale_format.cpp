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
    CHECK(normalizeLocaleNumber("1.050,25", ',', '.') == "1050.25");
    CHECK(normalizeLocaleNumber("-1.050,25", ',', '.') == "-1050.25");
}

TEST_CASE("render::normalizeLocaleNumber converts fr-FR space-grouped/decimal-comma text", "[render][locale]") {
    CHECK(normalizeLocaleNumber("1 050,25", ',', ' ') == "1050.25");
}

TEST_CASE("render::normalizeLocaleNumber is the identity transform for plain '.'-decimal text", "[render][locale]") {
    CHECK(normalizeLocaleNumber("1234.5", '.', '\0') == "1234.5");
    CHECK(normalizeLocaleNumber("-0.001", '.', '\0') == "-0.001");
}

TEST_CASE("render::normalizeLocaleNumber rejects malformed input rather than guessing", "[render][locale]") {
    CHECK(normalizeLocaleNumber("12.34.56", '.', '\0') == std::nullopt);
    CHECK(normalizeLocaleNumber("abc", '.', '\0') == std::nullopt);
    CHECK(normalizeLocaleNumber("-", '.', '\0') == std::nullopt);
}

TEST_CASE("render::formatCanonicalNumber groups thousands and swaps the decimal separator", "[render][locale]") {
    CHECK(formatCanonicalNumber("1050.25", ',', '.') == "1.050,25");
    CHECK(formatCanonicalNumber("-1050.25", ',', '.') == "-1.050,25");
    CHECK(formatCanonicalNumber("1234.5", '.', '\0') == "1234.5");
}

TEST_CASE("render locale numeric round-trip: normalize then format reproduces the original", "[render][locale]") {
    auto const canonical = normalizeLocaleNumber("1.050,25", ',', '.');
    REQUIRE(canonical.has_value());
    CHECK(formatCanonicalNumber(*canonical, ',', '.') == "1.050,25");
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

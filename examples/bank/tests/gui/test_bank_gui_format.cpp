// SPDX-License-Identifier: Apache-2.0
//
// `bankgui::fmt::parseMinor` — the one function in the bank GUI that turns
// arbitrary user text into an integer, and therefore the one that has to
// survive arbitrary user text.
//
// Why this file exists at all: `gui/controllers/Format.hpp` is a header under
// `examples/`, and the root `.clang-tidy`'s `HeaderFilterRegex` discarded
// every finding in every such header (morph#664), so no analyser had ever
// reported on it. What it contained was an unbounded `double` → `std::int64_t`
// conversion (morph#663): `QString::toDouble` accepts `1e30`, `inf` and `nan`
// from a QML field that carries no validator, and converting any of those is
// undefined behaviour, not a large number.
//
// The cases below are written against the returned `std::optional`, not
// against the arithmetic, and that is deliberate: on a UBSan build the
// unfixed function *aborts* rather than returning a wrong answer, so a test
// that asserted on the value would report the defect as a crash on one
// configuration and as nothing at all on the others. Asserting that the
// out-of-range inputs are *rejected* fails on both — as `-9223372036854775808
// != nullopt` without a sanitizer, and as an abort with one.
//
// This TU needs Qt6::Core and nothing else (no engine, no platform plugin),
// so it lives in `bank_gui_tests` alongside the QML surface audit rather than
// in the binary that owns a QGuiApplication.

#include <QString>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>

#include "controllers/Format.hpp"

namespace {

using bankgui::fmt::parseMinor;

}  // namespace

TEST_CASE("parseMinor turns well-formed amounts into minor units", "[bank][gui][format]") {
    CHECK(parseMinor(QStringLiteral("12.34")) == 1234);
    CHECK(parseMinor(QStringLiteral("0")) == 0);
    CHECK(parseMinor(QStringLiteral("  7.5  ")) == 750);
    // Rounds to nearest rather than truncating, with a half going away from
    // zero. This input alone does not distinguish `std::llround` from the
    // `+ 0.5` it replaced -- both give 1 -- which is the whole point of the
    // last case in this file.
    CHECK(parseMinor(QStringLiteral("0.005")) == 1);
    // `decimals` comes from the selected currency (JPY has none).
    CHECK(parseMinor(QStringLiteral("1200"), 0) == 1200);
}

TEST_CASE("parseMinor rejects text that is not a non-negative amount", "[bank][gui][format]") {
    CHECK_FALSE(parseMinor(QStringLiteral("")).has_value());
    CHECK_FALSE(parseMinor(QStringLiteral("abc")).has_value());
    CHECK_FALSE(parseMinor(QStringLiteral("-1.00")).has_value());
}

// The morph#663 regression. Each of these returned `-9223372036854775808`
// before the bound existed — via undefined behaviour, and via an abort under
// UBSan — and every call site then fed that through `.value_or(0)` into a
// balance.
TEST_CASE("parseMinor rejects amounts that do not fit in int64 minor units", "[bank][gui][format]") {
    // The value the issue reproduced with: 1e30 major units scale to 1e32.
    CHECK_FALSE(parseMinor(QStringLiteral("1e30")).has_value());
    CHECK_FALSE(parseMinor(QStringLiteral("1e300")).has_value());

    // Not an absurd magnitude: anything above ~9.2e16 major units already
    // overflows once scaled by 100, and nothing in the GUI said so.
    CHECK_FALSE(parseMinor(QStringLiteral("9.3e16")).has_value());

    // `QString::toDouble` accepts both of these, and `nan` passes a `< 0.0`
    // guard because every comparison against a NaN is false.
    CHECK_FALSE(parseMinor(QStringLiteral("inf")).has_value());
    CHECK_FALSE(parseMinor(QStringLiteral("nan")).has_value());

    // The scale is what decides the ceiling, so a value that overflows at two
    // decimals is accepted at none.
    CHECK_FALSE(parseMinor(QStringLiteral("1e17"), 2).has_value());
    CHECK(parseMinor(QStringLiteral("1e17"), 0).has_value());
}

TEST_CASE("parseMinor's ceiling is the int64 range, not an arbitrary cap", "[bank][gui][format]") {
    // 9.2e16 major units scale to 9.2e18 minor, just inside 2^63-1 ≈ 9.223e18,
    // and are accepted exactly. A bound that was conservative by a factor or an
    // order of magnitude would fail here rather than pass quietly.
    // Compared as an `optional`, not dereferenced: `operator==` against a value
    // is false for a disengaged optional, so this asserts both halves at once,
    // and bugprone-unchecked-optional-access cannot see a `REQUIRE` guard
    // through Catch2's macro expansion in any case.
    CHECK(parseMinor(QStringLiteral("92000000000000000")) == 9200000000000000000LL);

    // One order of magnitude further is out, so the accept/reject edge sits
    // between them rather than somewhere arbitrary below.
    CHECK_FALSE(parseMinor(QStringLiteral("920000000000000000")).has_value());
}

// The morph#678 regression. `static_cast<std::int64_t>(x + 0.5)` is not
// "round to nearest": for the double immediately below 0.5, adding 0.5 rounds
// *up* to exactly 1.0 in IEEE-754, and the truncating cast then yields 1 for a
// value that is below half a minor unit.
//
// The witness has to be an input where the two disagree -- `0.005` and `0.004`
// give the same answer either way and would pin nothing. The first case in
// this file keeps `0.005` for exactly that reason: it is the half-way input
// that must still round away from zero, and it does under both.
TEST_CASE("parseMinor rounds a value just below half a minor unit down", "[bank][gui][format]") {
    // "0.004999999999999999" scales to 0.49999999999999994, the largest
    // double below 0.5:
    //
    //     x                = 0.49999999999999994449
    //     x < 0.5          = true
    //     x + 0.5          = 1
    //     (int64)(x + 0.5) = 1      <- what this function returned
    //     std::llround(x)  = 0
    //
    // Not a constructed bit pattern: a decimal string short enough to type
    // into the amount field, through `QString::toDouble`.
    CHECK(parseMinor(QStringLiteral("0.004999999999999999")) == 0);
    CHECK(parseMinor(QStringLiteral("0.0049999999999999994")) == 0);

    // morph#663's bound still comes first. Rounding a value outside the int64
    // range is no better defined than casting one, so an amount that cannot
    // fit has to be rejected before it is rounded, not after.
    CHECK_FALSE(parseMinor(QStringLiteral("1e30")).has_value());
}

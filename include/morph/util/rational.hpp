// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file util/rational.hpp
/// @brief Exact rational arithmetic for morph model/action values.
///
/// `morph::math::Rational` is a small, value-semantic, trivially-copyable
/// struct representing the rational number `numerator/denominator` with
/// `std::int64_t` components. It carries a runtime decimal-precision tag,
/// `decimalPlaces`, as a strong type (`DecimalPlaces`). Arithmetic is
/// *exact* — sums, differences, products, and quotients are reduced to
/// canonical form with no floating-point rounding error. The precision tag
/// affects only decimal scaling (`Rational::fromFloat`) and rounding
/// (`Rational::toDouble`, formatting); it never changes a stored value.
///
/// Adapted from LASTRADA `JPMath/Rational.hpp`, with the `boxed` strong-type
/// dependency replaced by a self-contained `DecimalPlaces` and a Glaze wire
/// codec added so the type round-trips through the morph JSON wire with its
/// invariants restored on read.
///
/// The precision is supplied at construction. There is intentionally no
/// default — every call site states the precision it intends, e.g.
/// `Rational{1, 3, DecimalPlaces{9}}`. Precision is capped at
/// `kMaxDecimalPlaces` (18, the largest `k` for which `10^k` fits in
/// `int64_t`); out-of-range values assert in debug and clamp into
/// `[0, kMaxDecimalPlaces]` in release. Zero decimal places is a legal
/// precision — a whole-number quantity (e.g. a JPY/KRW amount) carries no
/// fractional digit at all.
///
/// @par Cross-precision arithmetic
/// Binary arithmetic propagates `std::max` of the two operands'
/// `decimalPlaces` — the wider precision wins. Comparison (`<=>`, `==`) is
/// purely value-based on the canonical `(numerator, denominator)` pair and
/// ignores `decimalPlaces` entirely.
///
/// @par Invariants
/// Every public operation that produces a `Rational` restores:
///   - `denominator > 0` (strictly positive — never zero, never negative)
///   - `gcd(|numerator|, denominator) == 1`
///   - canonical zero is `0/1`
///   - `0 <= decimalPlaces.value <= kMaxDecimalPlaces`
///
/// All sign lives in the numerator.
///
/// @par Error handling
/// The struct never throws. Operations that may fail (zero divisor,
/// non-finite floating-point input, overflow during decimal scaling) return
/// `std::expected<Rational, RationalError>`. `operator/` and `reciprocal`
/// return `expected`; `operator+`, `operator-`, `operator*` on plain
/// `Rational` pairs cannot fail and return a bare `Rational`.
///
/// @par Mixed-type expressions
/// Whenever an arithmetic expression contains an
/// `std::expected<Rational, RationalError>` sub-expression or a
/// floating-point operand, the whole expression evaluates to
/// `std::expected<Rational, RationalError>`. The float operand is lifted via
/// `Rational::fromFloat` — its precision is taken from the Rational
/// operand's `getDecimalPlaces()`. Errors short-circuit left to right.
///
/// @code{.cpp}
///     using morph::math::DecimalPlaces;
///     using morph::math::Rational;
///     auto const a = Rational{7, 2, DecimalPlaces{9}};
///     auto const b = Rational{2, DecimalPlaces{9}};
///     auto const c = Rational{1, 2, DecimalPlaces{9}};
///     auto result = a / b + c * 3.5;
///     // decltype(result) == std::expected<Rational, RationalError>
/// @endcode
///
/// @par Formatting
/// `std::format` support is split by the supplied spec:
///   - empty spec `"{}"`          -> exact rational form (`"n/d"`, or `"n"` when integer)
///   - non-empty spec `"{:.3f}"`  -> delegated to `std::formatter<double>` on `toDouble()`
///
/// @par Wire format
/// Over the morph JSON wire a `Rational` travels as the object
/// `{"num":617,"den":50,"dp":2}`. Reading goes through the canonicalising
/// constructor, so a non-canonical payload (`1234/100`) or a hostile one
/// (`den == 0`, out-of-range `dp`) always lands as a valid, reduced value.
///
/// @note Negating a `Rational` built from `INT64_MIN` overflows; avoid that
///       extreme value in code (the wire codec clamps it away for untrusted
///       input).
/// @note Comparison is exact over the full int64 range (128-bit cross
///       products). Arithmetic, however, has the usual fixed-width envelope:
///       `+`, `-`, `*` overflow int64 when reduced cross terms exceed ~2^63
///       (e.g. sums over large coprime denominators). Keep operands within
///       the decimal-scaled ranges the precision tags imply.

#include <morph/core/logger.hpp>

#include <glaze/glaze.hpp>

#include <cassert>
#include <cmath>
#include <compare>
#include <concepts>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <numeric>
#include <string>
#include <type_traits>

namespace morph::math {

/// @brief Strong type for a decimal-precision count.
///
/// Prevents the precision from being confused with a numerator or a
/// denominator at a call site: `Rational{1, 3, DecimalPlaces{9}}`.
struct DecimalPlaces {
    /// @brief Raw digit count. Rational's invariant keeps it in [0, kMaxDecimalPlaces].
    std::uint32_t value{};

    constexpr DecimalPlaces() noexcept = default;

    /// @brief Explicit so a bare integer never silently becomes a precision.
    /// @param raw The digit count to box.
    constexpr explicit DecimalPlaces(std::uint32_t raw) noexcept : value{raw} {}

    /// @brief Value ordering/equality on the raw digit count.
    /// @param other Precision to compare against.
    /// @return The ordering of the two raw counts.
    [[nodiscard]] constexpr auto operator<=>(const DecimalPlaces& other) const noexcept = default;
};

/// @brief Strong type for a Rational numerator.
///
/// Prevents numerator/denominator argument swapping at construction sites.
struct Numerator {
    /// @brief Raw signed integer value.
    std::int64_t value{};

    constexpr Numerator() noexcept = default;

    /// @brief Explicit so the type must be spelled out.
    /// @param raw The integer to box as a numerator.
    constexpr explicit Numerator(std::int64_t raw) noexcept : value{raw} {}
};

/// @brief Strong type for a Rational denominator.
///
/// Prevents numerator/denominator argument swapping at construction sites.
struct Denominator {
    /// @brief Raw signed integer value; must never be zero after canonicalisation.
    std::int64_t value{};

    constexpr Denominator() noexcept = default;

    /// @brief Explicit so the type must be spelled out.
    /// @param raw The integer to box as a denominator.
    constexpr explicit Denominator(std::int64_t raw) noexcept : value{raw} {}
};

/// @brief Largest decimal precision the type supports: `10^18` is the
///        greatest power of ten that fits in `std::int64_t`.
inline constexpr std::uint32_t kMaxDecimalPlaces = 18;

/// @brief Error states reachable through `Rational` operations.
enum class RationalError : std::uint8_t {
    DivisionByZero,  ///< Divisor numerator is zero (operator/, reciprocal, From).
    NotFinite,       ///< Floating-point input was NaN or +/-Inf (fromFloat only).
    Overflow,        ///< Result or an intermediate term exceeds int64_t range
                     ///< (`fromFloat`, and the `checked*` arithmetic helpers).
};

namespace detail {

/// @brief Clamps a raw precision into `[0, kMaxDecimalPlaces]` silently — for
///        untrusted wire input.
///
/// The lower bound is `0`, not `1`: zero decimal places is a legal precision
/// (a whole-number quantity, e.g. a JPY/KRW amount with no fractional
/// subunit). `rawDecimalPlaces` is unsigned, so there is no below-zero case
/// to clamp; only the upper bound can ever fire.
[[nodiscard]] constexpr std::uint32_t clampWireDecimalPlaces(std::uint32_t rawDecimalPlaces) noexcept {
    if (rawDecimalPlaces > kMaxDecimalPlaces) {
        return kMaxDecimalPlaces;
    }
    return rawDecimalPlaces;
}

/// @brief Clamps like `clampWireDecimalPlaces` but asserts in debug — for
///        call sites that state a precision in code, where out-of-range is a bug.
[[nodiscard]] constexpr std::uint32_t clampDecimalPlaces(std::uint32_t rawDecimalPlaces) noexcept {
    assert(rawDecimalPlaces <= kMaxDecimalPlaces);
    return clampWireDecimalPlaces(rawDecimalPlaces);
}

/// @brief Whether `lhs + rhs` would overflow `std::int64_t`.
///
/// Asks *before* performing the addition. Detecting overflow by doing it and
/// inspecting the result is undefined behaviour for signed types, so the
/// question has to be answered from the operands alone.
/// @param lhs Left addend.
/// @param rhs Right addend.
/// @return `true` if the sum is not representable.
[[nodiscard]] constexpr bool addOverflows(std::int64_t lhs, std::int64_t rhs) noexcept {
    constexpr auto maxValue = std::numeric_limits<std::int64_t>::max();
    constexpr auto minValue = std::numeric_limits<std::int64_t>::min();
    if (rhs > 0 && lhs > maxValue - rhs) {
        return true;
    }
    return rhs < 0 && lhs < minValue - rhs;
}

/// @brief Whether `lhs - rhs` would overflow `std::int64_t`.
/// @param lhs Minuend.
/// @param rhs Subtrahend.
/// @return `true` if the difference is not representable.
[[nodiscard]] constexpr bool subOverflows(std::int64_t lhs, std::int64_t rhs) noexcept {
    constexpr auto maxValue = std::numeric_limits<std::int64_t>::max();
    constexpr auto minValue = std::numeric_limits<std::int64_t>::min();
    if (rhs < 0 && lhs > maxValue + rhs) {
        return true;
    }
    return rhs > 0 && lhs < minValue + rhs;
}

/// @brief Whether `lhs * rhs` would overflow `std::int64_t`.
///
/// Division-based rather than a wide-product comparison so it stays valid in a
/// constant expression on every supported compiler (MSVC has no `__int128`).
/// @param lhs Left factor.
/// @param rhs Right factor.
/// @return `true` if the product is not representable.
[[nodiscard]] constexpr bool mulOverflows(std::int64_t lhs, std::int64_t rhs) noexcept {
    constexpr auto maxValue = std::numeric_limits<std::int64_t>::max();
    constexpr auto minValue = std::numeric_limits<std::int64_t>::min();
    if (lhs == 0 || rhs == 0) {
        return false;
    }
    // -1 is the one factor whose product can overflow without either operand
    // being large: -1 * INT64_MIN has no positive counterpart.
    if (lhs == -1) {
        return rhs == minValue;
    }
    if (rhs == -1) {
        return lhs == minValue;
    }
    if (lhs > 0) {
        return rhs > 0 ? lhs > maxValue / rhs : rhs < minValue / lhs;
    }
    return rhs > 0 ? lhs < minValue / rhs : lhs < maxValue / rhs;
}

/// @brief A 64x64 -> 128-bit unsigned product, comparable high-word-first.
struct U128 {
    std::uint64_t hi{};
    std::uint64_t lo{};
    [[nodiscard]] constexpr auto operator<=>(const U128&) const noexcept = default;
};

/// @brief Multiplies two u64 exactly into 128 bits (for exact fraction comparison).
[[nodiscard]] constexpr U128 mulU64(std::uint64_t lhs, std::uint64_t rhs) noexcept {
#ifdef __SIZEOF_INT128__
#  if defined(__GNUC__) && !defined(__clang__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wpedantic"
#  endif
    auto const product = static_cast<unsigned __int128>(lhs) * rhs;
#  if defined(__GNUC__) && !defined(__clang__)
#    pragma GCC diagnostic pop
#  endif
    return U128{.hi = static_cast<std::uint64_t>(product >> 64), .lo = static_cast<std::uint64_t>(product)};
#else
    // Portable 32-bit limb multiplication (MSVC has no __int128).
    constexpr std::uint64_t mask = 0xffffffffULL;
    auto const lhsLow = lhs & mask;
    auto const lhsHigh = lhs >> 32;
    auto const rhsLow = rhs & mask;
    auto const rhsHigh = rhs >> 32;
    auto const lowLow = lhsLow * rhsLow;
    auto const lowHigh = lhsLow * rhsHigh;
    auto const highLow = lhsHigh * rhsLow;
    auto const highHigh = lhsHigh * rhsHigh;
    auto const mid = (lowLow >> 32) + (lowHigh & mask) + (highLow & mask);
    return U128{.hi = highHigh + (lowHigh >> 32) + (highLow >> 32) + (mid >> 32),
                .lo = (mid << 32) | (lowLow & mask)};
#endif
}

/// @brief Magnitude of a signed 64-bit value as u64; well-defined for INT64_MIN.
[[nodiscard]] constexpr std::uint64_t absU64(std::int64_t value) noexcept {
    return value < 0 ? 0ULL - static_cast<std::uint64_t>(value) : static_cast<std::uint64_t>(value);
}

}  // namespace detail

/// @brief Exact rational number with a runtime decimal-precision tag.
struct Rational {
    /// @brief Signed numerator. Carries the sign of the rational value.
    std::int64_t numerator{0};

    /// @brief Strictly positive denominator. Never zero, never negative.
    std::int64_t denominator{1};

    /// @brief Decimal-precision tag. Invariant: 0 <= value <= kMaxDecimalPlaces.
    DecimalPlaces decimalPlaces{1};

    /// @brief Default-constructs the canonical zero (0/1) at precision 1.
    constexpr Rational() noexcept = default;

    /// @brief Constructs from a whole integer at the given precision.
    /// @param whole            The integer value; stored as `whole/1`.
    /// @param wantedPrecision  Decimal precision; clamped to [0, kMaxDecimalPlaces].
    constexpr Rational(std::int64_t whole, DecimalPlaces wantedPrecision) noexcept
        : numerator{whole}, decimalPlaces{detail::clampDecimalPlaces(wantedPrecision.value)} {}

        /// @brief Constructs from explicit numerator/denominator, then canonicalises.
    ///
    /// A negative @p wantedDenominator flips the sign of @p wantedNumerator,
    /// the pair is reduced by `gcd`, and a denominator of `0` is clamped to
    /// `1`. Use `from` for explicit zero-denominator detection.
    ///
    /// @param wantedNumerator   Signed numerator.
    /// @param wantedDenominator Denominator. May be negative or zero on input.
    /// @param wantedPrecision   Decimal precision; clamped to [0, kMaxDecimalPlaces].
    constexpr Rational(Numerator wantedNumerator, Denominator wantedDenominator,
                       DecimalPlaces wantedPrecision) noexcept
        : numerator{wantedNumerator.value},
          denominator{wantedDenominator.value},
          decimalPlaces{detail::clampDecimalPlaces(wantedPrecision.value)} {
        canonicalise();
    }

    /// @brief Validating factory.
    /// @param wantedNumerator   Signed numerator.
    /// @param wantedDenominator Denominator; `0` is rejected instead of clamped.
    /// @param wantedPrecision   Decimal precision; clamped to [0, kMaxDecimalPlaces].
    /// @return The canonical rational, or `DivisionByZero` if @p wantedDenominator is 0.
    [[nodiscard]] static constexpr std::expected<Rational, RationalError> from(
        Numerator wantedNumerator, Denominator wantedDenominator, DecimalPlaces wantedPrecision) noexcept {
        if (wantedDenominator.value == 0) {
            return std::unexpected(RationalError::DivisionByZero);
        }
        return Rational{wantedNumerator, wantedDenominator, wantedPrecision};
    }

    /// @brief Converts a `double` to a Rational scaled to @p wantedPrecision.
    /// @param value           Source value.
    /// @param wantedPrecision Decimal precision to scale to.
    /// @return A canonical Rational, or an error (NotFinite / Overflow).
    /// @note `noexcept` but not `constexpr` — uses `std::llround` / `std::isfinite`.
    [[nodiscard]] static std::expected<Rational, RationalError> fromFloat(double value,
                                                                          DecimalPlaces wantedPrecision) noexcept;

    /// @brief Converts a `float` to a Rational scaled to @p wantedPrecision.
    /// @param value           Source value.
    /// @param wantedPrecision Decimal precision to scale to.
    /// @return A canonical Rational, or an error (NotFinite / Overflow).
    [[nodiscard]] static std::expected<Rational, RationalError> fromFloat(float value,
                                                                          DecimalPlaces wantedPrecision) noexcept;

    /// @brief Converts a `long double` to a Rational scaled to @p wantedPrecision.
    /// @param value           Source value.
    /// @param wantedPrecision Decimal precision to scale to.
    /// @return A canonical Rational, or an error (NotFinite / Overflow).
    [[nodiscard]] static std::expected<Rational, RationalError> fromFloat(long double value,
                                                                          DecimalPlaces wantedPrecision) noexcept;

    /// @brief Canonical zero (`0/1`) at the given precision.
    /// @param wantedPrecision Decimal precision of the returned value.
    /// @return `0/1` tagged with @p wantedPrecision.
    [[nodiscard]] static constexpr Rational zero(DecimalPlaces wantedPrecision) noexcept {
        return Rational{Numerator{0}, Denominator{1}, wantedPrecision};
    }

    /// @brief Canonical one (`1/1`) at the given precision.
    /// @param wantedPrecision Decimal precision of the returned value.
    /// @return `1/1` tagged with @p wantedPrecision.
    [[nodiscard]] static constexpr Rational one(DecimalPlaces wantedPrecision) noexcept {
        return Rational{Numerator{1}, Denominator{1}, wantedPrecision};
    }

    /// @brief The value's current decimal precision.
    /// @return The `decimalPlaces` tag.
    [[nodiscard]] constexpr DecimalPlaces getDecimalPlaces() const noexcept { return decimalPlaces; }

    /// @brief Whether the value equals `0/1`.
    /// @return `true` if the numerator is zero.
    [[nodiscard]] constexpr bool isZero() const noexcept { return numerator == 0; }

    /// @brief Whether the value is an integer.
    /// @return `true` if the denominator is 1.
    [[nodiscard]] constexpr bool isInteger() const noexcept { return denominator == 1; }

    /// @brief Whether the value is strictly less than zero.
    /// @return `true` if the numerator is negative.
    [[nodiscard]] constexpr bool isNegative() const noexcept { return numerator < 0; }

    /// @brief Converts to `double`, rounded to this value's `decimalPlaces`.
    /// @return The rounded floating-point reading.
    [[nodiscard]] double toDouble() const noexcept { return toDouble(decimalPlaces.value); }

    /// @brief Converts to `double`, rounded to @p requestedDecimalPlaces.
    /// @param requestedDecimalPlaces Number of decimal digits to keep. Values
    ///                               `> 18` fall back to unrounded conversion.
    /// @return The rounded floating-point reading.
    [[nodiscard]] double toDouble(std::uint32_t requestedDecimalPlaces) const noexcept;

    /// @brief Negates. @note Negating a Rational built from `INT64_MIN` overflows.
    /// @return The value with the numerator's sign flipped.
    [[nodiscard]] constexpr Rational operator-() const noexcept {
        return Rational{Numerator{-numerator}, Denominator{denominator}, decimalPlaces};
    }

    /// @brief Multiplicative inverse.
    /// @return `denominator/numerator`, or `unexpected(DivisionByZero)` if zero.
    [[nodiscard]] constexpr std::expected<Rational, RationalError> reciprocal() const noexcept {
        if (numerator == 0) {
            return std::unexpected(RationalError::DivisionByZero);
        }
        if (numerator < 0) {
            return Rational{Numerator{-denominator}, Denominator{-numerator}, decimalPlaces};
        }
        return Rational{Numerator{denominator}, Denominator{numerator}, decimalPlaces};
    }

    /// @brief Three-way comparison. Value-only: ignores `decimalPlaces`.
    ///
    /// Exact for the full int64 range: cross-products are computed in 128
    /// bits, so the ordering is always consistent with `operator==`.
    /// @param other Value to compare against.
    /// @return The ordering of the two exact values.
    [[nodiscard]] constexpr std::strong_ordering operator<=>(const Rational& other) const noexcept {
        auto const leftSign = static_cast<int>(numerator > 0) - static_cast<int>(numerator < 0);
        auto const rightSign = static_cast<int>(other.numerator > 0) - static_cast<int>(other.numerator < 0);
        if (leftSign != rightSign) {
            return leftSign <=> rightSign;
        }
        if (leftSign == 0) {
            return std::strong_ordering::equal;
        }

        auto const leftProduct = detail::mulU64(detail::absU64(numerator),
                                                static_cast<std::uint64_t>(other.denominator));
        auto const rightProduct = detail::mulU64(detail::absU64(other.numerator),
                                                 static_cast<std::uint64_t>(denominator));
        auto const magnitude = leftProduct <=> rightProduct;
        if (leftSign > 0) {
            return magnitude;
        }
        if (std::is_lt(magnitude)) {
            return std::strong_ordering::greater;
        }
        if (std::is_gt(magnitude)) {
            return std::strong_ordering::less;
        }
        return std::strong_ordering::equal;
    }

    /// @brief Equality. Value-only: ignores `decimalPlaces`.
    /// @param other Value to compare against.
    /// @return `true` when the canonical pairs are identical.
    [[nodiscard]] constexpr bool operator==(const Rational& other) const noexcept {
        return numerator == other.numerator && denominator == other.denominator;
    }

    /// @brief In-place addition. Result precision becomes `max` of the two.
    /// Uses reduce-before-multiply to extend the safe int64 range (Knuth 4.5.1).
    ///
    /// **Saturates rather than overflowing.** If the exact sum -- or any
    /// intermediate cross-term needed to reach it -- is not representable, this
    /// logs at `error` and clamps to the largest magnitude of the correct sign
    /// instead of committing signed-overflow undefined behaviour. See
    /// `saturateToward` for why a defined wrong answer beats UB here.
    /// @param rhs Value to add.
    /// @return `*this`.
    constexpr Rational& operator+=(const Rational& rhs) noexcept {
        if (addWouldOverflow(rhs)) {
            reportOverflow("operator+=");
            saturateToward(compareForSaturation(rhs, true), rhs.decimalPlaces);
            return *this;
        }
        addAssignUnchecked(rhs);
        return *this;
    }

    /// @brief In-place subtraction. Result precision becomes `max` of the two.
    ///
    /// Saturates rather than overflowing; see `operator+=`.
    /// @param rhs Value to subtract.
    /// @return `*this`.
    constexpr Rational& operator-=(const Rational& rhs) noexcept {
        if (subWouldOverflow(rhs)) {
            reportOverflow("operator-=");
            saturateToward(compareForSaturation(rhs, false), rhs.decimalPlaces);
            return *this;
        }
        subAssignUnchecked(rhs);
        return *this;
    }

    /// @brief In-place multiplication. Result precision becomes `max` of the two.
    /// Cross-cancels common factors before multiplying.
    /// @param rhs Value to multiply by.
    /// @return `*this`.
    ///
    /// Saturates rather than overflowing; see `operator+=`.
    constexpr Rational& operator*=(const Rational& rhs) noexcept {
        if (mulWouldOverflow(rhs)) {
            reportOverflow("operator*=");
            // Sign of a product is the product of the signs; zero operands
            // cannot overflow, so neither sign is zero here.
            const bool negative = (numerator < 0) != (rhs.numerator < 0);
            saturateToward(negative ? -1 : 1, rhs.decimalPlaces);
            return *this;
        }
        mulAssignUnchecked(rhs);
        return *this;
    }

    /// @brief Non-throwing division. Result precision becomes `max` of the two.
    /// @param rhs Divisor.
    /// @return `*this / rhs`, or `unexpected(DivisionByZero)` if @p rhs is zero.
    [[nodiscard]] constexpr std::expected<Rational, RationalError> dividedBy(const Rational& rhs) const noexcept {
        if (rhs.numerator == 0) {
            return std::unexpected(RationalError::DivisionByZero);
        }
        auto leftCopy = *this;
        auto const reciprocalNumerator = rhs.numerator > 0 ? rhs.denominator : -rhs.denominator;
        auto const reciprocalDenominator = rhs.numerator > 0 ? rhs.numerator : -rhs.numerator;
        leftCopy *= Rational{Numerator{reciprocalNumerator}, Denominator{reciprocalDenominator}, rhs.decimalPlaces};
        return leftCopy;
    }

    /// @brief Flat JSON representation used by the Glaze codec below.
    ///
    /// Field names are the wire contract: `{"num":..,"den":..,"dp":..}`.
    struct Wire {
        std::int64_t num{0};   ///< Signed numerator as sent/received.
        std::int64_t den{1};   ///< Denominator as sent/received; may be non-canonical.
        std::uint32_t dp{1};   ///< Decimal-precision tag as sent/received.
    };

    /// @brief Wire-codec entry (Glaze read side): rebuilds through the
    ///        canonicalising constructor, silently clamping hostile input
    ///        (`den == 0`, out-of-range `dp`, `INT64_MIN` components whose
    ///        negation would overflow) instead of asserting.
    /// @param wire Raw values decoded from JSON.
void setWire(Wire wire) noexcept {
        constexpr auto int64Min = std::numeric_limits<std::int64_t>::min();
        constexpr auto negatableMin = -std::numeric_limits<std::int64_t>::max();
        *this = Rational{Numerator{wire.num == int64Min ? negatableMin : wire.num},
                         Denominator{wire.den == int64Min ? negatableMin : wire.den},
                         DecimalPlaces{detail::clampWireDecimalPlaces(wire.dp)}};
    }

    /// @brief Wire-codec exit (Glaze write side).
    /// @return The canonical members, ready for JSON encoding.
    [[nodiscard]] Wire getWire() const noexcept { return Wire{.num = numerator, .den = denominator, .dp = decimalPlaces.value}; }

private:
    /// @brief Logs an overflow that `operator+=`/`-=`/`*=` saturated instead
    ///        of committing.
    ///
    /// Skipped during constant evaluation: `log` is not `constexpr`, and a
    /// `constexpr` arithmetic expression that saturates should still compile.
    ///
    /// The `try`/`catch` is not defensive padding. `morph::log` offers no
    /// `noexcept` guarantee -- a user-installed sink may throw, and
    /// `detail::log`'s own `scoped_lock` may throw `std::system_error` -- and
    /// an arithmetic operator must not start failing because logging failed.
    /// `CompletionState`'s destructor carries the identical workaround for the
    /// identical reason. Both can go once morph#158 makes the logging layer
    /// non-throwing.
    /// @param where Which operator saturated.
    static constexpr void reportOverflow(std::string_view where) noexcept {
        if (!std::is_constant_evaluated()) {
            // NOLINTBEGIN(bugprone-empty-catch) -- see above: logging must not
            // turn a defined-but-clamped result into a thrown exception.
            try {
                ::morph::log::logError("[Rational] {} overflowed int64 and saturated; the result is clamped, "
                                       "not exact. Use checkedAdd/checkedSub/checkedMul to detect this instead.",
                                       where);
            } catch (...) {
            }
            // NOLINTEND(bugprone-empty-catch)
        }
    }

    /// @brief Logs an `INT64_MIN` component clamped by `canonicalise`.
    ///
    /// Skipped during constant evaluation, and non-throwing, like
    /// `reportOverflow` -- see that function for why the `catch` is there.
    static constexpr void reportClamp() noexcept {
        if (!std::is_constant_evaluated()) {
            // NOLINTBEGIN(bugprone-empty-catch)
            try {
                ::morph::log::logError("[Rational] an INT64_MIN component was clamped to -INT64_MAX; "
                                       "canonicalising it would require negating a value with no positive "
                                       "counterpart.");
            } catch (...) {
            }
            // NOLINTEND(bugprone-empty-catch)
        }
    }

    /// @brief The sign the saturated result should carry, for `+=` / `-=`.
    ///
    /// Determined by exact comparison rather than by computing the true sum,
    /// which by definition does not fit. `a + b` has the sign of `a` versus
    /// `-b`; `a - b` has the sign of `a` versus `b`. Both use the type's own
    /// exact 128-bit-wide comparison, so a mixed-sign case whose cross-terms
    /// overflow still saturates in the mathematically correct direction.
    /// @param rhs      The other operand.
    /// @param addition `true` for `+=`, `false` for `-=`.
    /// @return `-1`, `0` or `1`.
    [[nodiscard]] constexpr int compareForSaturation(const Rational& rhs, bool addition) const noexcept {
        const auto ordering = addition ? (*this <=> -rhs) : (*this <=> rhs);
        // std::is_lt/is_gt, not `ordering < 0`: comparing an ordering against
        // the literal 0 trips -Wzero-as-null-pointer-constant under
        // -Weverything, which this repository builds with.
        if (std::is_lt(ordering)) {
            return -1;
        }
        return std::is_gt(ordering) ? 1 : 0;
    }

    /// @brief Clamps this value to the largest representable magnitude with
    ///        sign @p sign, at `max(decimalPlaces, other)`.
    ///
    /// Saturation, not an exception and not UB. `operator+` and friends are
    /// used inside strand-bound model code and in `constexpr` expressions;
    /// throwing would change their contract for every existing caller, while
    /// leaving the overflow undefined is what this whole change exists to
    /// stop. A clamped value is wrong, but it is *defined* wrong, it is
    /// logged, and `checkedAdd`/`checkedSub`/`checkedMul` remain available for
    /// callers that need to detect the condition rather than absorb it.
    /// @param sign  Direction to clamp toward; `0` yields zero.
    /// @param other The other operand's precision, folded in as usual.
    constexpr void saturateToward(int sign, DecimalPlaces other) noexcept {
        constexpr auto maxValue = std::numeric_limits<std::int64_t>::max();
        numerator = sign == 0 ? 0 : (sign < 0 ? -maxValue : maxValue);
        denominator = 1;
        widenPrecisionTo(other);
    }

    /// @brief `operator+=`'s arithmetic, without the overflow check.
    /// @param rhs Value to add.
    constexpr void addAssignUnchecked(const Rational& rhs) noexcept {
        auto const denominatorGcd = std::gcd(denominator, rhs.denominator);
        auto const rightDenominatorScaled = rhs.denominator / denominatorGcd;
        auto const leftDenominatorScaled = denominator / denominatorGcd;
        numerator = (numerator * rightDenominatorScaled) + (rhs.numerator * leftDenominatorScaled);
        denominator = denominator * rightDenominatorScaled;
        widenPrecisionTo(rhs.decimalPlaces);
        canonicalise();
    }

    /// @brief `operator-=`'s arithmetic, without the overflow check.
    /// @param rhs Value to subtract.
    constexpr void subAssignUnchecked(const Rational& rhs) noexcept {
        auto const denominatorGcd = std::gcd(denominator, rhs.denominator);
        auto const rightDenominatorScaled = rhs.denominator / denominatorGcd;
        auto const leftDenominatorScaled = denominator / denominatorGcd;
        numerator = (numerator * rightDenominatorScaled) - (rhs.numerator * leftDenominatorScaled);
        denominator = denominator * rightDenominatorScaled;
        widenPrecisionTo(rhs.decimalPlaces);
        canonicalise();
    }

    /// @brief `operator*=`'s arithmetic, without the overflow check.
    /// @param rhs Value to multiply by.
    constexpr void mulAssignUnchecked(const Rational& rhs) noexcept {
        auto const absoluteLeftNumerator = numerator < 0 ? -numerator : numerator;
        auto const absoluteRightNumerator = rhs.numerator < 0 ? -rhs.numerator : rhs.numerator;
        auto const crossDivisorOne = std::gcd(absoluteLeftNumerator, rhs.denominator);
        auto const crossDivisorTwo = std::gcd(absoluteRightNumerator, denominator);
        auto const reducedLeftNumerator = numerator / crossDivisorOne;
        auto const reducedRightNumerator = rhs.numerator / crossDivisorTwo;
        auto const reducedLeftDenominator = denominator / crossDivisorTwo;
        auto const reducedRightDenominator = rhs.denominator / crossDivisorOne;
        numerator = reducedLeftNumerator * reducedRightNumerator;
        denominator = reducedLeftDenominator * reducedRightDenominator;
        widenPrecisionTo(rhs.decimalPlaces);
        canonicalise();
    }

  public:
    /// @brief Applies `+=`'s arithmetic, having already proven it cannot overflow.
    ///
    /// For `checkedAdd`, which has just run `addWouldOverflow` and must not
    /// re-enter `operator+=`'s saturating path. Calling this without that
    /// check is undefined on overflow -- the check is the precondition.
    /// @param rhs Value to add.
    constexpr void addAssignChecked(const Rational& rhs) noexcept { addAssignUnchecked(rhs); }

    /// @brief Applies `-=`'s arithmetic, having already proven it cannot overflow.
    /// @param rhs Value to subtract.
    constexpr void subAssignChecked(const Rational& rhs) noexcept { subAssignUnchecked(rhs); }

    /// @brief Applies `*=`'s arithmetic, having already proven it cannot overflow.
    /// @param rhs Value to multiply by.
    constexpr void mulAssignChecked(const Rational& rhs) noexcept { mulAssignUnchecked(rhs); }

    /// @brief Whether `*this + rhs` would overflow any intermediate or the result.
    /// @param rhs The addend.
    /// @return `true` if the addition cannot be performed exactly.
    [[nodiscard]] constexpr bool addWouldOverflow(const Rational& rhs) const noexcept {
        auto const denominatorGcd = std::gcd(denominator, rhs.denominator);
        auto const rightScaled = rhs.denominator / denominatorGcd;
        auto const leftScaled = denominator / denominatorGcd;
        if (detail::mulOverflows(numerator, rightScaled) || detail::mulOverflows(rhs.numerator, leftScaled)
            || detail::mulOverflows(denominator, rightScaled)) {
            return true;
        }
        return detail::addOverflows(numerator * rightScaled, rhs.numerator * leftScaled);
    }

    /// @brief Whether `*this - rhs` would overflow any intermediate or the result.
    /// @param rhs The subtrahend.
    /// @return `true` if the subtraction cannot be performed exactly.
    [[nodiscard]] constexpr bool subWouldOverflow(const Rational& rhs) const noexcept {
        auto const denominatorGcd = std::gcd(denominator, rhs.denominator);
        auto const rightScaled = rhs.denominator / denominatorGcd;
        auto const leftScaled = denominator / denominatorGcd;
        if (detail::mulOverflows(numerator, rightScaled) || detail::mulOverflows(rhs.numerator, leftScaled)
            || detail::mulOverflows(denominator, rightScaled)) {
            return true;
        }
        return detail::subOverflows(numerator * rightScaled, rhs.numerator * leftScaled);
    }

    /// @brief Whether `*this * rhs` would overflow, after cross-cancelling.
    ///
    /// Checks the cross-cancelled factors `operator*=` actually multiplies:
    /// cross-cancelling is what keeps most products in range, so checking the
    /// raw operands would reject pairs that multiply perfectly well.
    /// @param rhs The factor.
    /// @return `true` if the product cannot be represented.
    [[nodiscard]] constexpr bool mulWouldOverflow(const Rational& rhs) const noexcept {
        auto const absoluteLeftNumerator = numerator < 0 ? -numerator : numerator;
        auto const absoluteRightNumerator = rhs.numerator < 0 ? -rhs.numerator : rhs.numerator;
        auto const crossDivisorOne = std::gcd(absoluteLeftNumerator, rhs.denominator);
        auto const crossDivisorTwo = std::gcd(absoluteRightNumerator, denominator);
        if (crossDivisorOne == 0 || crossDivisorTwo == 0) {
            return false;  // a zero numerator: the product is zero
        }
        return detail::mulOverflows(numerator / crossDivisorOne, rhs.numerator / crossDivisorTwo)
            || detail::mulOverflows(denominator / crossDivisorTwo, rhs.denominator / crossDivisorOne);
    }

private:
    /// @brief Restores the canonical-form invariants in place (denominator > 0,
    ///        gcd reduced, zero denominator clamped to 1). Leaves `decimalPlaces`
    ///        untouched (it is not a value property).
    constexpr void canonicalise() noexcept {
        if (denominator == 0) {
            denominator = 1;
            return;
        }
        constexpr auto minValue = std::numeric_limits<std::int64_t>::min();
        constexpr auto maxValue = std::numeric_limits<std::int64_t>::max();

        // Neither component may be INT64_MIN past this point. Canonicalising
        // needs `|value|` and a sign flip, and `-INT64_MIN` is not
        // representable -- negating it is undefined behaviour, which this
        // function used to commit. It was reachable two ways: constructing a
        // Rational with such a numerator directly, and *ordinary arithmetic*
        // landing on it exactly (`-INT64_MAX - 1` is a perfectly legal
        // subtraction whose result is INT64_MIN).
        //
        // Clamped to the adjacent representable magnitude, matching what
        // `setWire` already does for the same values arriving off the wire.
        // The value is off by one ulp; it is not undefined.
        if (numerator == minValue || denominator == minValue) {
            reportClamp();
            numerator = numerator == minValue ? -maxValue : numerator;
            denominator = denominator == minValue ? -maxValue : denominator;
        }

        if (denominator < 0) {
            numerator = -numerator;
            denominator = -denominator;
        }
        // Magnitudes via `absU64`, so the gcd never negates either component.
        // The result cannot exceed INT64_MAX: gcd(a, b) <= min(a, b) and the
        // denominator is at most INT64_MAX here.
        auto const divisor =
            static_cast<std::int64_t>(std::gcd(detail::absU64(numerator), detail::absU64(denominator)));
        if (divisor > 1) {
            numerator /= divisor;
            denominator /= divisor;
        }
    }

    /// @brief Sets `decimalPlaces` to `max(this, other)`.
    constexpr void widenPrecisionTo(DecimalPlaces other) noexcept {
        if (other.value > decimalPlaces.value) {
            decimalPlaces = other;
        }
    }
};

// Sanity checks: trivially copyable and standard layout.
static_assert(std::is_trivially_copyable_v<Rational>);
static_assert(std::is_standard_layout_v<Rational>);

// ---------------------------------------------------------------------------
// Free-function rounding helpers (live next to Rational so ADL finds them).
// ---------------------------------------------------------------------------

/// @brief Absolute value of a rational. Precision is preserved.
/// @param value Value to take the absolute value of.
/// @return The non-negative value with the same magnitude.
[[nodiscard]] constexpr Rational abs(const Rational& value) noexcept {
    return value.numerator < 0 ? Rational{Numerator{-value.numerator}, Denominator{value.denominator}, value.decimalPlaces}
                               : value;
}

/// @brief Rounds toward positive infinity.
/// @param value Value to round.
/// @return The smallest integer `>= value`.
[[nodiscard]] constexpr std::int64_t ceil(const Rational& value) noexcept {
    auto const quotient = value.numerator / value.denominator;
    auto const remainder = value.numerator % value.denominator;
    if (remainder != 0 && value.numerator > 0) {
        return quotient + 1;
    }
    return quotient;
}

/// @brief Rounds toward negative infinity.
/// @param value Value to round.
/// @return The largest integer `<= value`.
[[nodiscard]] constexpr std::int64_t floor(const Rational& value) noexcept {
    auto const quotient = value.numerator / value.denominator;
    auto const remainder = value.numerator % value.denominator;
    if (remainder != 0 && value.numerator < 0) {
        return quotient - 1;
    }
    return quotient;
}

/// @brief Truncates toward zero.
/// @param value Value to truncate.
/// @return The integer part of the value.
[[nodiscard]] constexpr std::int64_t trunc(const Rational& value) noexcept {
    return value.numerator / value.denominator;
}

// ---------------------------------------------------------------------------
// Plain Rational arithmetic (max-precision propagation).
// ---------------------------------------------------------------------------

/// @brief Adds two Rationals. Result precision is `max` of the two.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @return The exact sum.
[[nodiscard]] constexpr Rational operator+(const Rational& lhs, const Rational& rhs) noexcept {
    auto result = lhs;
    result += rhs;
    return result;
}

/// @brief Subtracts two Rationals. Result precision is `max` of the two.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @return The exact difference.
[[nodiscard]] constexpr Rational operator-(const Rational& lhs, const Rational& rhs) noexcept {
    auto result = lhs;
    result -= rhs;
    return result;
}

/// @brief Multiplies two Rationals. Result precision is `max` of the two.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @return The exact product.
[[nodiscard]] constexpr Rational operator*(const Rational& lhs, const Rational& rhs) noexcept {
    auto result = lhs;
    result *= rhs;
    return result;
}

/// @brief Divides two Rationals. Result precision is `max` of the two.
/// @param lhs Dividend.
/// @param rhs Divisor.
/// @return `lhs / rhs`, or `unexpected(DivisionByZero)` if @p rhs is zero.
[[nodiscard]] constexpr std::expected<Rational, RationalError> operator/(const Rational& lhs,
                                                                         const Rational& rhs) noexcept {
    return lhs.dividedBy(rhs);
}

/// @brief Adds two Rationals, reporting overflow instead of saturating.
///
/// `operator+` saturates and logs when the exact sum does not fit, so it is
/// never undefined -- but it is also silently inexact. This returns the
/// overflow instead, for callers that must not absorb it: a ledger totalling
/// rows needs to *stop*, not to carry on with a clamped balance.
///
/// Every intermediate is checked before any is formed, since detecting signed
/// overflow by performing it is itself undefined. Note the cross-terms are the
/// tighter bound: they can overflow while the final result would have been
/// perfectly representable, which is exactly the case a caller cannot spot by
/// inspecting the answer.
///
/// @param lhs Left addend.
/// @param rhs Right addend.
/// @return The exact sum, or `unexpected(RationalError::Overflow)`.
[[nodiscard]] constexpr std::expected<Rational, RationalError> checkedAdd(const Rational& lhs,
                                                                          const Rational& rhs) noexcept {
    if (lhs.addWouldOverflow(rhs)) {
        return std::unexpected(RationalError::Overflow);
    }
    auto result = lhs;
    result.addAssignChecked(rhs);
    return result;
}

/// @brief Subtracts two Rationals, reporting overflow instead of saturating.
///
/// The `checkedAdd` counterpart; see that function.
/// @param lhs Minuend.
/// @param rhs Subtrahend.
/// @return The exact difference, or `unexpected(RationalError::Overflow)`.
[[nodiscard]] constexpr std::expected<Rational, RationalError> checkedSub(const Rational& lhs,
                                                                          const Rational& rhs) noexcept {
    if (lhs.subWouldOverflow(rhs)) {
        return std::unexpected(RationalError::Overflow);
    }
    auto result = lhs;
    result.subAssignChecked(rhs);
    return result;
}

/// @brief Multiplies two Rationals, reporting overflow instead of saturating.
///
/// Checks the cross-cancelled factors `operator*` actually multiplies, not the
/// raw operands -- see `Rational::mulWouldOverflow`.
/// @param lhs Left factor.
/// @param rhs Right factor.
/// @return The exact product, or `unexpected(RationalError::Overflow)`.
[[nodiscard]] constexpr std::expected<Rational, RationalError> checkedMul(const Rational& lhs,
                                                                          const Rational& rhs) noexcept {
    if (lhs.mulWouldOverflow(rhs)) {
        return std::unexpected(RationalError::Overflow);
    }
    auto result = lhs;
    result.mulAssignChecked(rhs);
    return result;
}

// ---------------------------------------------------------------------------
// Mixed-type arithmetic with automatic expected-propagation.
// ---------------------------------------------------------------------------

namespace detail {

using ExpectedRational = std::expected<Rational, RationalError>;

/// @brief Concept: matches `Rational` (after cvref-stripping).
template <typename T>
concept IsRational = std::same_as<std::remove_cvref_t<T>, Rational>;

/// @brief Concept: matches `std::expected<Rational, RationalError>`.
template <typename T>
concept IsExpectedRational = std::same_as<std::remove_cvref_t<T>, ExpectedRational>;

/// @brief Concept: matches any standard floating-point type.
template <typename T>
concept IsFloat = std::floating_point<std::remove_cvref_t<T>>;

/// @brief Concept: a Rational-family operand (Rational or ExpectedRational).
template <typename T>
concept RationalLike = IsRational<T> || IsExpectedRational<T>;

/// @brief Concept: an operand the mixed-type overload accepts.
template <typename T>
concept LiftableOperand = RationalLike<T> || IsFloat<T>;

/// @brief Concept: an operand whose presence forces the mixed-type overload to fire.
template <typename T>
concept NeedsLifting = IsExpectedRational<T> || IsFloat<T>;

/// @brief Lifts a `Rational` to `ExpectedRational` (always succeeds).
[[nodiscard]] constexpr ExpectedRational lift(const Rational& value, DecimalPlaces /*wantedPrecision*/) noexcept {
    return ExpectedRational{value};
}

/// @brief Forwards an `ExpectedRational` unchanged.
[[nodiscard]] constexpr ExpectedRational lift(const ExpectedRational& value, DecimalPlaces /*wantedPrecision*/) noexcept {
    return value;
}

/// @brief Lifts a floating-point value via `Rational::fromFloat` at @p fallback precision.
template <std::floating_point Float>
[[nodiscard]] inline ExpectedRational lift(Float value, DecimalPlaces fallback) noexcept {
    return Rational::fromFloat(value, fallback);
}

/// @brief The precision to lift a float operand to: the Rational-family
///        operand's precision. Picks whichever side carries one (at least one
///        always does).
template <typename Left, typename Right>
[[nodiscard]] constexpr DecimalPlaces liftPrecision(const Left& lhs, const Right& rhs) noexcept {
    if constexpr (IsRational<Left>) {
        return lhs.decimalPlaces;
    } else if constexpr (IsExpectedRational<Left>) {
        return lhs ? lhs->decimalPlaces : DecimalPlaces{1};
    } else if constexpr (IsRational<Right>) {
        return rhs.decimalPlaces;
    } else if constexpr (IsExpectedRational<Right>) {
        return rhs ? rhs->decimalPlaces : DecimalPlaces{1};
    } else {
        return DecimalPlaces{1};
    }
}

}  // namespace detail

/// @brief Mixed-type addition with automatic expected-propagation.
/// @param lhs Left operand (Rational, expected-Rational, or floating point).
/// @param rhs Right operand (Rational, expected-Rational, or floating point).
/// @return The exact sum, or the first error encountered left to right.
template <typename Left, typename Right>
    requires(detail::RationalLike<Left> || detail::RationalLike<Right>)
            && (detail::NeedsLifting<Left> || detail::NeedsLifting<Right>)
            && (detail::LiftableOperand<Left> && detail::LiftableOperand<Right>)
[[nodiscard]] inline detail::ExpectedRational operator+(const Left& lhs, const Right& rhs) noexcept {
    auto const precision = detail::liftPrecision(lhs, rhs);
    auto const leftExpected = detail::lift(lhs, precision);
    if (!leftExpected) {
        return std::unexpected(leftExpected.error());
    }
    auto const rightExpected = detail::lift(rhs, precision);
    if (!rightExpected) {
        return std::unexpected(rightExpected.error());
    }
    return *leftExpected + *rightExpected;
}

/// @brief Mixed-type subtraction with automatic expected-propagation.
/// @param lhs Left operand (Rational, expected-Rational, or floating point).
/// @param rhs Right operand (Rational, expected-Rational, or floating point).
/// @return The exact difference, or the first error encountered left to right.
template <typename Left, typename Right>
    requires(detail::RationalLike<Left> || detail::RationalLike<Right>)
            && (detail::NeedsLifting<Left> || detail::NeedsLifting<Right>)
            && (detail::LiftableOperand<Left> && detail::LiftableOperand<Right>)
[[nodiscard]] inline detail::ExpectedRational operator-(const Left& lhs, const Right& rhs) noexcept {
    auto const precision = detail::liftPrecision(lhs, rhs);
    auto const leftExpected = detail::lift(lhs, precision);
    if (!leftExpected) {
        return std::unexpected(leftExpected.error());
    }
    auto const rightExpected = detail::lift(rhs, precision);
    if (!rightExpected) {
        return std::unexpected(rightExpected.error());
    }
    return *leftExpected - *rightExpected;
}

/// @brief Mixed-type multiplication with automatic expected-propagation.
/// @param lhs Left operand (Rational, expected-Rational, or floating point).
/// @param rhs Right operand (Rational, expected-Rational, or floating point).
/// @return The exact product, or the first error encountered left to right.
template <typename Left, typename Right>
    requires(detail::RationalLike<Left> || detail::RationalLike<Right>)
            && (detail::NeedsLifting<Left> || detail::NeedsLifting<Right>)
            && (detail::LiftableOperand<Left> && detail::LiftableOperand<Right>)
[[nodiscard]] inline detail::ExpectedRational operator*(const Left& lhs, const Right& rhs) noexcept {
    auto const precision = detail::liftPrecision(lhs, rhs);
    auto const leftExpected = detail::lift(lhs, precision);
    if (!leftExpected) {
        return std::unexpected(leftExpected.error());
    }
    auto const rightExpected = detail::lift(rhs, precision);
    if (!rightExpected) {
        return std::unexpected(rightExpected.error());
    }
    return *leftExpected * *rightExpected;
}

/// @brief Mixed-type division with automatic expected-propagation.
/// @param lhs Dividend (Rational, expected-Rational, or floating point).
/// @param rhs Divisor (Rational, expected-Rational, or floating point).
/// @return The exact quotient, or the first error encountered left to right.
template <typename Left, typename Right>
    requires(detail::RationalLike<Left> || detail::RationalLike<Right>)
            && (detail::NeedsLifting<Left> || detail::NeedsLifting<Right>)
            && (detail::LiftableOperand<Left> && detail::LiftableOperand<Right>)
[[nodiscard]] inline detail::ExpectedRational operator/(const Left& lhs, const Right& rhs) noexcept {
    auto const precision = detail::liftPrecision(lhs, rhs);
    auto const leftExpected = detail::lift(lhs, precision);
    if (!leftExpected) {
        return std::unexpected(leftExpected.error());
    }
    auto const rightExpected = detail::lift(rhs, precision);
    if (!rightExpected) {
        return std::unexpected(rightExpected.error());
    }
    return *leftExpected / *rightExpected;
}

// ---------------------------------------------------------------------------
// fromFloat and toDouble out-of-class implementations.
// ---------------------------------------------------------------------------

namespace detail {

/// @brief Computes `10^decimalPlaces` as `std::int64_t`. Returns `0` on overflow.
[[nodiscard]] constexpr std::int64_t powerOfTen(std::uint32_t decimalPlaces) noexcept {
    if (decimalPlaces > 18) {
        return 0;
    }
    auto result = std::int64_t{1};
    for (std::uint32_t digit = 0; digit < decimalPlaces; ++digit) {
        result *= 10;
    }
    return result;
}

/// @brief Shared implementation of the three `Rational::fromFloat` overloads.
template <std::floating_point Float>
[[nodiscard]] inline ExpectedRational fromFloatImpl(Float value, DecimalPlaces wantedPrecision) noexcept {
    if (!std::isfinite(value)) {
        return std::unexpected(RationalError::NotFinite);
    }

    auto const rawPrecision = clampDecimalPlaces(wantedPrecision.value);
    // rawPrecision is clamped to <= kMaxDecimalPlaces, so powerOfTen cannot
    // return its overflow sentinel here.
    auto const scale = powerOfTen(rawPrecision);

    auto const scaled = static_cast<long double>(value) * static_cast<long double>(scale);
    // Bound against 2^63 (representable in every long double format), minus
    // the half-ulp llround adds: values in [2^63 - 0.5, 2^63) round *up* to
    // 2^63 and would overflow int64. Casting INT64_MAX instead would itself
    // round up to 2^63 where long double == double. The negative bound is
    // asymmetric because INT64_MIN == -2^63 is a valid result and llround
    // maps (-2^63 - 0.5, -2^63] onto it, hence `<` against -2^63 exactly.
    constexpr auto twoPow63 = 0x1p63L;
    if (scaled >= twoPow63 - 0.5L || scaled < -twoPow63) {
        return std::unexpected(RationalError::Overflow);
    }

    auto const scaledNumerator = static_cast<std::int64_t>(std::llround(scaled));
    return Rational{Numerator{scaledNumerator}, Denominator{scale}, DecimalPlaces{rawPrecision}};
}

}  // namespace detail

inline std::expected<Rational, RationalError> Rational::fromFloat(double value,
                                                                  DecimalPlaces wantedPrecision) noexcept {
    return detail::fromFloatImpl(value, wantedPrecision);
}

inline std::expected<Rational, RationalError> Rational::fromFloat(float value,
                                                                  DecimalPlaces wantedPrecision) noexcept {
    return detail::fromFloatImpl(value, wantedPrecision);
}

inline std::expected<Rational, RationalError> Rational::fromFloat(long double value,
                                                                  DecimalPlaces wantedPrecision) noexcept {
    return detail::fromFloatImpl(value, wantedPrecision);
}

inline double Rational::toDouble(std::uint32_t requestedDecimalPlaces) const noexcept {
    auto const raw = static_cast<double>(numerator) / static_cast<double>(denominator);
    auto const scale = detail::powerOfTen(requestedDecimalPlaces);
    if (scale == 0) {
        return raw;
    }
    auto const scaledDouble = static_cast<double>(scale);
    return std::round(raw * scaledDouble) / scaledDouble;
}

}  // namespace morph::math

// ---------------------------------------------------------------------------
// Glaze wire codec: {"num":..,"den":..,"dp":..}, canonicalised on read.
// ---------------------------------------------------------------------------

/// @brief Routes Rational serialisation through `setWire`/`getWire` so every
///        deserialised value passes the canonicalising constructor.
template <>
struct glz::meta<morph::math::Rational> {
    static constexpr auto value = glz::custom<&morph::math::Rational::setWire, &morph::math::Rational::getWire>;
    static constexpr std::string_view name = "Rational";
};

namespace glz::detail {

/// @brief Schema for Rational: the `Wire` object shape. Without this the
///        `glz::custom` codec above would degrade the generated schema to
///        "accepts anything".
template <>
struct to_json_schema<morph::math::Rational> {
    template <auto Opts>
    static void op(auto& outSchema, auto& defs) {
        to_json_schema<morph::math::Rational::Wire>::template op<Opts>(outSchema, defs);
    }
};

}  // namespace glz::detail

// ---------------------------------------------------------------------------
// std::format support.
// ---------------------------------------------------------------------------

/// @brief std::format support for `morph::math::Rational`.
///
/// Empty specs print the exact rational form (`n/d`, or `n` when integer).
/// Non-empty specs are forwarded to `std::formatter<double>` on `toDouble()`.
template <>
struct std::formatter<morph::math::Rational> {
    bool delegateToDouble{false};
    std::formatter<double> doubleFormatter;

    constexpr auto parse(std::format_parse_context& ctx) {
        if (ctx.begin() == ctx.end() || *ctx.begin() == '}') {
            return ctx.begin();
        }
        delegateToDouble = true;
        return doubleFormatter.parse(ctx);
    }

    auto format(const morph::math::Rational& value, std::format_context& ctx) const {
        if (delegateToDouble) {
            return doubleFormatter.format(value.toDouble(), ctx);
        }
        if (value.denominator == 1) {
            return std::format_to(ctx.out(), "{}", value.numerator);
        }
        return std::format_to(ctx.out(), "{}/{}", value.numerator, value.denominator);
    }
};

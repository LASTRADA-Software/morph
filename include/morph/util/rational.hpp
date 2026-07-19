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
/// `[1, kMaxDecimalPlaces]` in release.
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
///   - `1 <= decimalPlaces.value <= kMaxDecimalPlaces`
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
    /// @brief Raw digit count. Rational's invariant keeps it in [1, kMaxDecimalPlaces].
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
    Overflow,        ///< Scaled magnitude exceeds int64_t range (fromFloat only).
};

namespace detail {

/// @brief Clamps a raw precision into `[1, kMaxDecimalPlaces]` silently — for
///        untrusted wire input.
[[nodiscard]] constexpr std::uint32_t clampWireDecimalPlaces(std::uint32_t rawDecimalPlaces) noexcept {
    if (rawDecimalPlaces < 1) {
        return 1;
    }
    if (rawDecimalPlaces > kMaxDecimalPlaces) {
        return kMaxDecimalPlaces;
    }
    return rawDecimalPlaces;
}

/// @brief Clamps like `clampWireDecimalPlaces` but asserts in debug — for
///        call sites that state a precision in code, where out-of-range is a bug.
[[nodiscard]] constexpr std::uint32_t clampDecimalPlaces(std::uint32_t rawDecimalPlaces) noexcept {
    assert(rawDecimalPlaces >= 1 && rawDecimalPlaces <= kMaxDecimalPlaces);
    return clampWireDecimalPlaces(rawDecimalPlaces);
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

    /// @brief Decimal-precision tag. Invariant: 1 <= value <= kMaxDecimalPlaces.
    DecimalPlaces decimalPlaces{1};

    /// @brief Default-constructs the canonical zero (0/1) at precision 1.
    constexpr Rational() noexcept = default;

    /// @brief Constructs from a whole integer at the given precision.
    /// @param whole            The integer value; stored as `whole/1`.
    /// @param wantedPrecision  Decimal precision; clamped to [1, kMaxDecimalPlaces].
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
    /// @param wantedPrecision   Decimal precision; clamped to [1, kMaxDecimalPlaces].
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
    /// @param wantedPrecision   Decimal precision; clamped to [1, kMaxDecimalPlaces].
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
    /// @param rhs Value to add.
    /// @return `*this`.
    constexpr Rational& operator+=(const Rational& rhs) noexcept {
        auto const denominatorGcd = std::gcd(denominator, rhs.denominator);
        auto const rightDenominatorScaled = rhs.denominator / denominatorGcd;
        auto const leftDenominatorScaled = denominator / denominatorGcd;
        numerator = (numerator * rightDenominatorScaled) + (rhs.numerator * leftDenominatorScaled);
        denominator = denominator * rightDenominatorScaled;
        widenPrecisionTo(rhs.decimalPlaces);
        canonicalise();
        return *this;
    }

    /// @brief In-place subtraction. Result precision becomes `max` of the two.
    /// @param rhs Value to subtract.
    /// @return `*this`.
    constexpr Rational& operator-=(const Rational& rhs) noexcept {
        auto const denominatorGcd = std::gcd(denominator, rhs.denominator);
        auto const rightDenominatorScaled = rhs.denominator / denominatorGcd;
        auto const leftDenominatorScaled = denominator / denominatorGcd;
        numerator = (numerator * rightDenominatorScaled) - (rhs.numerator * leftDenominatorScaled);
        denominator = denominator * rightDenominatorScaled;
        widenPrecisionTo(rhs.decimalPlaces);
        canonicalise();
        return *this;
    }

    /// @brief In-place multiplication. Result precision becomes `max` of the two.
    /// Cross-cancels common factors before multiplying.
    /// @param rhs Value to multiply by.
    /// @return `*this`.
    constexpr Rational& operator*=(const Rational& rhs) noexcept {
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
    /// @brief Restores the canonical-form invariants in place (denominator > 0,
    ///        gcd reduced, zero denominator clamped to 1). Leaves `decimalPlaces`
    ///        untouched (it is not a value property).
    constexpr void canonicalise() noexcept {
        if (denominator == 0) {
            denominator = 1;
            return;
        }
        if (denominator < 0) {
            numerator = -numerator;
            denominator = -denominator;
        }
        auto const absoluteNumerator = numerator < 0 ? -numerator : numerator;
        auto const divisor = std::gcd(absoluteNumerator, denominator);
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

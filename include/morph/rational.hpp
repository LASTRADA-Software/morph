// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file rational.hpp
/// @brief Exact rational arithmetic for morph model/action values.
///
/// `morph::math::Rational` is a small, value-semantic, trivially-copyable
/// struct representing the rational number `numerator/denominator` with
/// `std::int64_t` components. It carries a runtime decimal-precision tag,
/// `decimalPlaces`, as a strong type (`DecimalPlaces`). Arithmetic is
/// *exact* — sums, differences, products, and quotients are reduced to
/// canonical form with no floating-point rounding error. The precision tag
/// affects only decimal scaling (`Rational::FromFloat`) and rounding
/// (`Rational::ToDouble`, formatting); it never changes a stored value.
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
/// `std::expected<Rational, RationalError>`. `operator/` and `Reciprocal`
/// return `expected`; `operator+`, `operator-`, `operator*` on plain
/// `Rational` pairs cannot fail and return a bare `Rational`.
///
/// @par Mixed-type expressions
/// Whenever an arithmetic expression contains an
/// `std::expected<Rational, RationalError>` sub-expression or a
/// floating-point operand, the whole expression evaluates to
/// `std::expected<Rational, RationalError>`. The float operand is lifted via
/// `Rational::FromFloat` — its precision is taken from the Rational
/// operand's `GetDecimalPlaces()`. Errors short-circuit left to right.
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
///   - non-empty spec `"{:.3f}"`  -> delegated to `std::formatter<double>` on `ToDouble()`
///
/// @par Wire format
/// Over the morph JSON wire a `Rational` travels as the object
/// `{"num":617,"den":50,"dp":2}`. Reading goes through the canonicalising
/// constructor, so a non-canonical payload (`1234/100`) or a hostile one
/// (`den == 0`, out-of-range `dp`) always lands as a valid, reduced value.
///
/// @note Negating a `Rational` built from `INT64_MIN` overflows; avoid that
///       extreme value.
/// @note Comparison cross-multiplies through `long double`. Products beyond
///       ~2^63 per side can exceed the 64-bit mantissa and misorder extreme
///       values (on MSVC `long double == double`, so headroom is smaller).

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
    constexpr explicit DecimalPlaces(std::uint32_t raw) noexcept : value{raw} {}

    [[nodiscard]] constexpr auto operator<=>(const DecimalPlaces&) const noexcept = default;
};

/// @brief Largest decimal precision the type supports: `10^18` is the
///        greatest power of ten that fits in `std::int64_t`.
inline constexpr std::uint32_t kMaxDecimalPlaces = 18;

/// @brief Error states reachable through `Rational` operations.
enum class RationalError : std::uint8_t {
    DivisionByZero,  ///< Divisor numerator is zero (operator/, Reciprocal, From).
    NotFinite,       ///< Floating-point input was NaN or +/-Inf (FromFloat only).
    Overflow,        ///< Scaled magnitude exceeds int64_t range (FromFloat only).
};

namespace detail {

/// @brief Clamps a raw precision into `[1, kMaxDecimalPlaces]`, asserting in debug.
[[nodiscard]] inline constexpr std::uint32_t clampDecimalPlaces(std::uint32_t rawDecimalPlaces) noexcept {
    assert(rawDecimalPlaces >= 1 && rawDecimalPlaces <= kMaxDecimalPlaces);
    if (rawDecimalPlaces < 1) {
        return 1;
    }
    if (rawDecimalPlaces > kMaxDecimalPlaces) {
        return kMaxDecimalPlaces;
    }
    return rawDecimalPlaces;
}

/// @brief Clamps like `clampDecimalPlaces` but silently — for untrusted wire input.
[[nodiscard]] inline constexpr std::uint32_t clampWireDecimalPlaces(std::uint32_t rawDecimalPlaces) noexcept {
    if (rawDecimalPlaces < 1) {
        return 1;
    }
    if (rawDecimalPlaces > kMaxDecimalPlaces) {
        return kMaxDecimalPlaces;
    }
    return rawDecimalPlaces;
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
        : numerator{whole}, denominator{1}, decimalPlaces{detail::clampDecimalPlaces(wantedPrecision.value)} {}

    /// @brief Constructs from explicit numerator/denominator, then canonicalises.
    ///
    /// A negative @p wantedDenominator flips the sign of @p wantedNumerator,
    /// the pair is reduced by `gcd`, and a denominator of `0` is clamped to
    /// `1`. Use `From` for explicit zero-denominator detection.
    ///
    /// @param wantedNumerator   Signed numerator.
    /// @param wantedDenominator Denominator. May be negative or zero on input.
    /// @param wantedPrecision   Decimal precision; clamped to [1, kMaxDecimalPlaces].
    constexpr Rational(std::int64_t wantedNumerator, std::int64_t wantedDenominator,
                       DecimalPlaces wantedPrecision) noexcept
        : numerator{wantedNumerator},
          denominator{wantedDenominator},
          decimalPlaces{detail::clampDecimalPlaces(wantedPrecision.value)} {
        canonicalise();
    }

    /// @brief Validating factory: returns `DivisionByZero` if @p wantedDenominator is 0.
    [[nodiscard]] static constexpr std::expected<Rational, RationalError> From(
        std::int64_t wantedNumerator, std::int64_t wantedDenominator, DecimalPlaces wantedPrecision) noexcept {
        if (wantedDenominator == 0) {
            return std::unexpected(RationalError::DivisionByZero);
        }
        return Rational{wantedNumerator, wantedDenominator, wantedPrecision};
    }

    /// @brief Converts a `double` to a Rational scaled to @p wantedPrecision.
    /// @return A canonical Rational, or an error (NotFinite / Overflow).
    /// @note `noexcept` but not `constexpr` — uses `std::llround` / `std::isfinite`.
    [[nodiscard]] static std::expected<Rational, RationalError> FromFloat(double value,
                                                                          DecimalPlaces wantedPrecision) noexcept;

    /// @brief Converts a `float` to a Rational scaled to @p wantedPrecision.
    [[nodiscard]] static std::expected<Rational, RationalError> FromFloat(float value,
                                                                          DecimalPlaces wantedPrecision) noexcept;

    /// @brief Converts a `long double` to a Rational scaled to @p wantedPrecision.
    [[nodiscard]] static std::expected<Rational, RationalError> FromFloat(long double value,
                                                                          DecimalPlaces wantedPrecision) noexcept;

    /// @brief Canonical zero (`0/1`) at the given precision.
    [[nodiscard]] static constexpr Rational Zero(DecimalPlaces wantedPrecision) noexcept {
        return Rational{0, 1, wantedPrecision};
    }

    /// @brief Canonical one (`1/1`) at the given precision.
    [[nodiscard]] static constexpr Rational One(DecimalPlaces wantedPrecision) noexcept {
        return Rational{1, 1, wantedPrecision};
    }

    /// @brief Returns the value's current decimal precision.
    [[nodiscard]] constexpr DecimalPlaces GetDecimalPlaces() const noexcept { return decimalPlaces; }

    /// @brief Returns `true` if the value equals `0/1`.
    [[nodiscard]] constexpr bool IsZero() const noexcept { return numerator == 0; }

    /// @brief Returns `true` if the value is an integer (`denominator == 1`).
    [[nodiscard]] constexpr bool IsInteger() const noexcept { return denominator == 1; }

    /// @brief Returns `true` if the value is strictly less than zero.
    [[nodiscard]] constexpr bool IsNegative() const noexcept { return numerator < 0; }

    /// @brief Converts to `double`, rounded to this value's `decimalPlaces`.
    [[nodiscard]] double ToDouble() const noexcept { return ToDouble(decimalPlaces.value); }

    /// @brief Converts to `double`, rounded to @p requestedDecimalPlaces.
    /// @param requestedDecimalPlaces Number of decimal digits to keep. Values
    ///                               `> 18` fall back to unrounded conversion.
    [[nodiscard]] double ToDouble(std::uint32_t requestedDecimalPlaces) const noexcept;

    /// @brief Negates. @note Negating a Rational built from `INT64_MIN` overflows.
    [[nodiscard]] constexpr Rational operator-() const noexcept {
        return Rational{-numerator, denominator, decimalPlaces};
    }

    /// @brief Multiplicative inverse.
    /// @return `denominator/numerator`, or `unexpected(DivisionByZero)` if zero.
    [[nodiscard]] constexpr std::expected<Rational, RationalError> Reciprocal() const noexcept {
        if (numerator == 0) {
            return std::unexpected(RationalError::DivisionByZero);
        }
        if (numerator < 0) {
            return Rational{-denominator, -numerator, decimalPlaces};
        }
        return Rational{denominator, numerator, decimalPlaces};
    }

    /// @brief Three-way comparison. Value-only: ignores `decimalPlaces`.
    [[nodiscard]] constexpr std::strong_ordering operator<=>(const Rational& other) const noexcept {
        if (numerator == other.numerator && denominator == other.denominator) {
            return std::strong_ordering::equal;
        }

        auto const leftSign = (numerator > 0) - (numerator < 0);
        auto const rightSign = (other.numerator > 0) - (other.numerator < 0);
        if (leftSign != rightSign) {
            return leftSign <=> rightSign;
        }

        auto const leftScaled =
            static_cast<long double>(numerator) * static_cast<long double>(other.denominator);
        auto const rightScaled =
            static_cast<long double>(other.numerator) * static_cast<long double>(denominator);
        if (leftScaled < rightScaled) {
            return std::strong_ordering::less;
        }
        if (leftScaled > rightScaled) {
            return std::strong_ordering::greater;
        }
        return std::strong_ordering::equal;
    }

    /// @brief Equality. Value-only: ignores `decimalPlaces`.
    [[nodiscard]] constexpr bool operator==(const Rational& other) const noexcept {
        return numerator == other.numerator && denominator == other.denominator;
    }

    /// @brief In-place addition. Result precision becomes `max` of the two.
    /// Uses reduce-before-multiply to extend the safe int64 range (Knuth 4.5.1).
    constexpr Rational& operator+=(const Rational& rhs) noexcept {
        auto const denominatorGcd = std::gcd(denominator, rhs.denominator);
        auto const rightDenominatorScaled = rhs.denominator / denominatorGcd;
        auto const leftDenominatorScaled = denominator / denominatorGcd;
        numerator = numerator * rightDenominatorScaled + rhs.numerator * leftDenominatorScaled;
        denominator = denominator * rightDenominatorScaled;
        widenPrecisionTo(rhs.decimalPlaces);
        canonicalise();
        return *this;
    }

    /// @brief In-place subtraction. Result precision becomes `max` of the two.
    constexpr Rational& operator-=(const Rational& rhs) noexcept {
        auto const denominatorGcd = std::gcd(denominator, rhs.denominator);
        auto const rightDenominatorScaled = rhs.denominator / denominatorGcd;
        auto const leftDenominatorScaled = denominator / denominatorGcd;
        numerator = numerator * rightDenominatorScaled - rhs.numerator * leftDenominatorScaled;
        denominator = denominator * rightDenominatorScaled;
        widenPrecisionTo(rhs.decimalPlaces);
        canonicalise();
        return *this;
    }

    /// @brief In-place multiplication. Result precision becomes `max` of the two.
    /// Cross-cancels common factors before multiplying.
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
    /// @return `*this / rhs`, or `unexpected(DivisionByZero)` if @p rhs is zero.
    [[nodiscard]] constexpr std::expected<Rational, RationalError> DividedBy(const Rational& rhs) const noexcept {
        if (rhs.numerator == 0) {
            return std::unexpected(RationalError::DivisionByZero);
        }
        auto leftCopy = *this;
        auto const reciprocalNumerator = rhs.numerator > 0 ? rhs.denominator : -rhs.denominator;
        auto const reciprocalDenominator = rhs.numerator > 0 ? rhs.numerator : -rhs.numerator;
        leftCopy *= Rational{reciprocalNumerator, reciprocalDenominator, rhs.decimalPlaces};
        return leftCopy;
    }

    /// @brief Flat JSON representation used by the Glaze codec below.
    ///
    /// Field names are the wire contract: `{"num":..,"den":..,"dp":..}`.
    struct Wire {
        std::int64_t num{0};
        std::int64_t den{1};
        std::uint32_t dp{1};
    };

    /// @brief Wire-codec entry (Glaze read side): rebuilds through the
    ///        canonicalising constructor, silently clamping hostile input
    ///        (`den == 0`, out-of-range `dp`) instead of asserting.
    void setWire(Wire wire) noexcept {
        *this = Rational{wire.num, wire.den, DecimalPlaces{detail::clampWireDecimalPlaces(wire.dp)}};
    }

    /// @brief Wire-codec exit (Glaze write side).
    [[nodiscard]] Wire getWire() const noexcept { return Wire{numerator, denominator, decimalPlaces.value}; }

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
[[nodiscard]] constexpr Rational abs(const Rational& value) noexcept {
    return value.numerator < 0 ? Rational{-value.numerator, value.denominator, value.decimalPlaces} : value;
}

/// @brief Rounds toward positive infinity. @return The smallest integer `>= value`.
[[nodiscard]] constexpr std::int64_t ceil(const Rational& value) noexcept {
    auto const quotient = value.numerator / value.denominator;
    auto const remainder = value.numerator % value.denominator;
    if (remainder != 0 && value.numerator > 0) {
        return quotient + 1;
    }
    return quotient;
}

/// @brief Rounds toward negative infinity. @return The largest integer `<= value`.
[[nodiscard]] constexpr std::int64_t floor(const Rational& value) noexcept {
    auto const quotient = value.numerator / value.denominator;
    auto const remainder = value.numerator % value.denominator;
    if (remainder != 0 && value.numerator < 0) {
        return quotient - 1;
    }
    return quotient;
}

/// @brief Truncates toward zero. @return The integer part of the value.
[[nodiscard]] constexpr std::int64_t trunc(const Rational& value) noexcept {
    return value.numerator / value.denominator;
}

// ---------------------------------------------------------------------------
// Plain Rational arithmetic (max-precision propagation).
// ---------------------------------------------------------------------------

/// @brief Adds two Rationals. Result precision is `max` of the two.
[[nodiscard]] constexpr Rational operator+(const Rational& lhs, const Rational& rhs) noexcept {
    auto result = lhs;
    result += rhs;
    return result;
}

/// @brief Subtracts two Rationals. Result precision is `max` of the two.
[[nodiscard]] constexpr Rational operator-(const Rational& lhs, const Rational& rhs) noexcept {
    auto result = lhs;
    result -= rhs;
    return result;
}

/// @brief Multiplies two Rationals. Result precision is `max` of the two.
[[nodiscard]] constexpr Rational operator*(const Rational& lhs, const Rational& rhs) noexcept {
    auto result = lhs;
    result *= rhs;
    return result;
}

/// @brief Divides two Rationals. Result precision is `max` of the two.
/// @return `lhs / rhs`, or `unexpected(DivisionByZero)` if @p rhs is zero.
[[nodiscard]] constexpr std::expected<Rational, RationalError> operator/(const Rational& lhs,
                                                                         const Rational& rhs) noexcept {
    return lhs.DividedBy(rhs);
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
[[nodiscard]] inline constexpr ExpectedRational lift(const Rational& value, DecimalPlaces) noexcept {
    return ExpectedRational{value};
}

/// @brief Forwards an `ExpectedRational` unchanged.
[[nodiscard]] inline constexpr ExpectedRational lift(const ExpectedRational& value, DecimalPlaces) noexcept {
    return value;
}

/// @brief Lifts a floating-point value via `Rational::FromFloat` at @p fallback precision.
template <std::floating_point Float>
[[nodiscard]] inline ExpectedRational lift(Float value, DecimalPlaces fallback) noexcept {
    return Rational::FromFloat(value, fallback);
}

/// @brief The precision to lift a float operand to: the Rational-family
///        operand's precision. Picks whichever side carries one (at least one
///        always does).
template <typename Left, typename Right>
[[nodiscard]] inline constexpr DecimalPlaces liftPrecision(const Left& lhs, const Right& rhs) noexcept {
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
// FromFloat and ToDouble out-of-class implementations.
// ---------------------------------------------------------------------------

namespace detail {

/// @brief Computes `10^decimalPlaces` as `std::int64_t`. Returns `0` on overflow.
[[nodiscard]] inline constexpr std::int64_t powerOfTen(std::uint32_t decimalPlaces) noexcept {
    if (decimalPlaces > 18) {
        return 0;
    }
    auto result = std::int64_t{1};
    for (std::uint32_t digit = 0; digit < decimalPlaces; ++digit) {
        result *= 10;
    }
    return result;
}

/// @brief Shared implementation of the three `Rational::FromFloat` overloads.
template <std::floating_point Float>
[[nodiscard]] inline ExpectedRational fromFloatImpl(Float value, DecimalPlaces wantedPrecision) noexcept {
    if (!std::isfinite(value)) {
        return std::unexpected(RationalError::NotFinite);
    }

    auto const rawPrecision = clampDecimalPlaces(wantedPrecision.value);
    auto const scale = powerOfTen(rawPrecision);
    if (scale == 0) {
        return std::unexpected(RationalError::Overflow);
    }

    auto const scaled = static_cast<long double>(value) * static_cast<long double>(scale);
    auto const int64Max = static_cast<long double>(std::numeric_limits<std::int64_t>::max());
    auto const int64Min = static_cast<long double>(std::numeric_limits<std::int64_t>::min());
    if (scaled > int64Max || scaled < int64Min) {
        return std::unexpected(RationalError::Overflow);
    }

    auto const scaledNumerator = static_cast<std::int64_t>(std::llround(scaled));
    return Rational{scaledNumerator, scale, DecimalPlaces{rawPrecision}};
}

}  // namespace detail

inline std::expected<Rational, RationalError> Rational::FromFloat(double value,
                                                                  DecimalPlaces wantedPrecision) noexcept {
    return detail::fromFloatImpl(value, wantedPrecision);
}

inline std::expected<Rational, RationalError> Rational::FromFloat(float value,
                                                                  DecimalPlaces wantedPrecision) noexcept {
    return detail::fromFloatImpl(value, wantedPrecision);
}

inline std::expected<Rational, RationalError> Rational::FromFloat(long double value,
                                                                  DecimalPlaces wantedPrecision) noexcept {
    return detail::fromFloatImpl(value, wantedPrecision);
}

inline double Rational::ToDouble(std::uint32_t requestedDecimalPlaces) const noexcept {
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
    static void op(auto& s, auto& defs) {
        to_json_schema<morph::math::Rational::Wire>::template op<Opts>(s, defs);
    }
};

}  // namespace glz::detail

// ---------------------------------------------------------------------------
// std::format support.
// ---------------------------------------------------------------------------

/// @brief std::format support for `morph::math::Rational`.
///
/// Empty specs print the exact rational form (`n/d`, or `n` when integer).
/// Non-empty specs are forwarded to `std::formatter<double>` on `ToDouble()`.
template <>
struct std::formatter<morph::math::Rational> {
    bool delegateToDouble{false};
    std::formatter<double> doubleFormatter{};

    constexpr auto parse(std::format_parse_context& ctx) {
        auto const begin = ctx.begin();
        if (begin == ctx.end() || *begin == '}') {
            return begin;
        }
        delegateToDouble = true;
        return doubleFormatter.parse(ctx);
    }

    auto format(const morph::math::Rational& value, std::format_context& ctx) const {
        if (delegateToDouble) {
            return doubleFormatter.format(value.ToDouble(), ctx);
        }
        if (value.denominator == 1) {
            return std::format_to(ctx.out(), "{}", value.numerator);
        }
        return std::format_to(ctx.out(), "{}/{}", value.numerator, value.denominator);
    }
};

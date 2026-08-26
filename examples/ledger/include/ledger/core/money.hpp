// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <morph/util/quantity.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <string>

#include "ledger/core/units.hpp"

/// @file
/// How this rung encodes money, and the two operations that encoding needs.
///
/// Every money value in ledger -- a transaction leg, a budget limit, an
/// account balance, a report total -- is a `morph::math::Rational` carrying a
/// **whole number of the currency's minor units**, with `decimalPlaces`
/// naming the scale those units are counted in. `$4.50` is
/// `{num: 450, den: 1, dp: 2}`; `¥500` is `{num: 500, den: 1, dp: 0}`.
///
/// That is deliberately **not** `Rational`'s own reading of the same triple.
/// `include/morph/util/rational.hpp` defines the value as
/// `numerator/denominator` and calls `decimalPlaces` a display tag that
/// "never changes a stored value"; comparison is "purely value-based on the
/// canonical (numerator, denominator) pair and ignores `decimalPlaces`
/// entirely". `Rational` therefore reads `{450, 1, dp 2}` and `{450, 1, dp 1}`
/// as the same number, where this rung reads `$4.50` and `$45.00`. **The two
/// readings agree only when every operand sits on one scale**, so the model
/// restates each leg onto its own account currency's scale before it does any
/// arithmetic on it -- `restateMinorUnits` below is that step.
///
/// `examples/ledger/README.md`'s "How money is represented" carries the full
/// rationale, including why the leg amount cannot be a
/// `morph::units::Quantity` and which framework gaps this encoding surfaced.

namespace ledger {

/// @brief The number of minor-unit digits @p c is denominated in --
///        `UnitTraits<Currency>::meta(c).defaultDecimals`, named at the money
///        layer so call sites read as currency precision rather than as unit
///        metadata. 2 for USD/EUR, 0 for JPY/KRW.
/// @param c The currency to describe.
/// @return The currency's declared decimal places.
[[nodiscard]] constexpr std::uint32_t currencyDecimalPlaces(Currency c) noexcept {
    return ::morph::units::UnitTraits<Currency>::meta(c).defaultDecimals;
}

namespace detail {

/// @brief `10^exponent` as an exact `std::int64_t`.
///
///        `morph::math::detail::powerOfTen` is the same function, but it sits
///        in the framework's `detail` namespace and is therefore not part of
///        morph's public surface -- a rung calling it would depend on an
///        implementation detail. Written out here instead. That a
///        scaled-decimal application needs an integer power of ten at all is
///        recorded as a finding in `examples/ledger/README.md`.
/// @param exponent The power to raise ten to.
/// @return `10^exponent`, or `0` when that does not fit `std::int64_t` (i.e.
///         when @p exponent exceeds `morph::math::kMaxDecimalPlaces`).
[[nodiscard]] constexpr std::int64_t powerOfTen(std::uint32_t exponent) noexcept {
    if (exponent > ::morph::math::kMaxDecimalPlaces) {
        return 0;
    }
    auto result = std::int64_t{1};
    for (std::uint32_t digit = 0; digit < exponent; ++digit) {
        result *= 10;
    }
    return result;
}

}  // namespace detail

/// @brief Restates @p amount -- a whole number of minor units at its own
///        scale -- as the same money at @p targetPlaces.
///
/// This is the operation that makes the per-currency zero-sum invariant
/// sound. Leg amounts arrive at whatever scale a client chose, and
/// `Rational::operator+` cannot notice the difference: it adds numerators and
/// propagates `std::max` of the two precisions. So `$4.50` (`{450, dp 2}`)
/// and `-$45.00` (`{-450, dp 1}`) net to a numerator of zero and pass a check
/// they should fail, while `$4.50` written `{45, dp 1}` and `-$4.50` written
/// `{-450, dp 2}` net to -405 and fail one they should pass. Restating every
/// operand onto one scale first removes both.
///
/// Restating is exact or nothing. Widening goes through
/// `morph::math::checkedMul`, which reports overflow rather than saturating,
/// and narrowing is refused outright when it would drop a non-zero digit --
/// the model never rounds money.
///
/// @param amount The amount to restate. Must be a whole number of minor
///        units, i.e. canonical denominator 1; anything else -- a wire
///        payload of `{"num":9,"den":2}`, say -- is refused.
/// @param targetPlaces The scale to restate onto, normally
///        `currencyDecimalPlaces()` of the owning account's currency.
/// @return The same money expressed at @p targetPlaces, or `std::nullopt`
///         when @p amount is not a whole number of minor units, when
///         narrowing would lose a non-zero digit, or when widening would
///         overflow `std::int64_t`.
[[nodiscard]] inline std::optional<::morph::math::Rational> restateMinorUnits(const ::morph::math::Rational& amount,
                                                                              std::uint32_t targetPlaces) {
    if (amount.denominator != 1) {
        return std::nullopt;
    }
    const auto target = ::morph::math::DecimalPlaces{targetPlaces};
    const auto sourcePlaces = amount.decimalPlaces.value;
    if (sourcePlaces == targetPlaces) {
        return ::morph::math::Rational{::morph::math::Numerator{amount.numerator}, ::morph::math::Denominator{1},
                                       target};
    }
    const bool widening = sourcePlaces < targetPlaces;
    const auto exponent = widening ? targetPlaces - sourcePlaces : sourcePlaces - targetPlaces;
    const auto factor = ::morph::math::Rational{::morph::math::Numerator{detail::powerOfTen(exponent)},
                                                ::morph::math::Denominator{1}, target};
    if (factor.numerator == 0) {
        // `targetPlaces` came from a currency and `sourcePlaces` from a
        // canonical Rational, so both are within range and this is only
        // reachable through a hand-built out-of-range request.
        return std::nullopt;
    }
    const auto restated =
        widening ? ::morph::math::checkedMul(amount, factor) : ::morph::math::checkedDiv(amount, factor);
    // A narrowing quotient that does not reduce back to denominator 1 carried
    // a non-zero digit below the target scale -- `$4.505` in a USD account.
    if (!restated.has_value() || restated->denominator != 1) {
        return std::nullopt;
    }
    return ::morph::math::Rational{::morph::math::Numerator{restated->numerator}, ::morph::math::Denominator{1},
                                   target};
}

/// @brief Renders @p minorUnits, denominated in @p currency, as exact decimal
///        text -- the single rendering path every ledger view uses.
///
/// The minor-unit count is divided by its own scale to recover the decimal
/// value, that value is handed to `Money<C>` for the currency the caller
/// named, and `morph::units::toDecimalString` produces the digits by exact
/// integer long division. No `double` exists anywhere on the path, which is
/// what `examples/ledger/README.md` requires of display: a QML
/// `numerator / denominator / Math.pow(10, places)` drifts for balances past
/// 2^53 while the payload stays exact.
///
/// `toDecimalString` renders shortest-form, so `$4.50` comes back as `"4.5"`
/// and a zero balance as `"0"`; `Quantity` offers no fixed-fraction-width
/// mode to ask for `"4.50"` instead. The rung renders shortest-form rather
/// than hand-rolling a second formatter, and records the gap as a finding in
/// its README.
///
/// @param currency The currency @p minorUnits is denominated in -- normally
///        the owning account's own, never a client-supplied claim.
/// @param minorUnits The amount, as a whole number of minor units at its own
///        scale. An amount not on @p currency's scale is rendered at the
///        scale it arrived on rather than silently mis-scaled.
/// @return The exact decimal text, with no currency symbol or unit suffix.
[[nodiscard]] inline std::string formatMoney(Currency currency, const ::morph::math::Rational& minorUnits) {
    const auto onScale = restateMinorUnits(minorUnits, currencyDecimalPlaces(currency)).value_or(minorUnits);
    const auto scale =
        ::morph::math::Rational{::morph::math::Numerator{detail::powerOfTen(onScale.decimalPlaces.value)},
                                ::morph::math::Denominator{1}, onScale.decimalPlaces};
    const auto value = (onScale / scale).value_or(::morph::math::Rational::zero(onScale.decimalPlaces));
    switch (currency) {
        case Currency::EUR:
            return ::morph::units::toDecimalString(Money<Currency::EUR>{value}.atDeclaredPrecision());
        case Currency::JPY:
            return ::morph::units::toDecimalString(Money<Currency::JPY>{value}.atDeclaredPrecision());
        case Currency::KRW:
            return ::morph::units::toDecimalString(Money<Currency::KRW>{value}.atDeclaredPrecision());
        case Currency::USD:
        default:
            return ::morph::units::toDecimalString(Money<Currency::USD>{value}.atDeclaredPrecision());
    }
}

}  // namespace ledger

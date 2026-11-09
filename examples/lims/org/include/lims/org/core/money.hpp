// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <morph/util/quantity.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "lims/org/core/errors.hpp"
#include "lims/org/core/types.hpp"

/// @file
/// Money for every module: an exact amount whose currency is a field beside it, a dimensionless rate, a count, and
/// the one rounding rule posters use.

namespace lims {

/// @brief The units of lims's business quantities.
///
/// One `money` unit for every currency rather than one unit per currency: a `Quantity`'s unit is a template
/// argument, and the currency of an invoice, a price list or a ledger line is known only from its record. The
/// currency travels as a `CurrencyCode` field beside the amount, and the model that combines two amounts checks that
/// they share it, since the type no longer can.
enum class ErpUnit : std::uint8_t {
    money,  ///< An amount in the currency named beside it.
    rate,   ///< A dimensionless ratio: an exchange rate or a tax rate.
    count,  ///< A whole number: a day of the month, a number of hours.
};

}  // namespace lims

/// @brief Unit metadata for `ErpUnit`. There are no relations: no two units convert.
template <>
struct morph::units::UnitTraits<lims::ErpUnit> {
    /// @brief Display metadata for @p unit.
    /// @param unit The unit.
    /// @return Its id, display text and default decimals.
    static constexpr morph::units::UnitMeta meta(lims::ErpUnit unit) noexcept {
        switch (unit) {
            case lims::ErpUnit::rate:
                return {.id = "rate", .display = "", .defaultDecimals = 6};
            case lims::ErpUnit::count:
                return {.id = "count", .display = "", .defaultDecimals = 0};
            case lims::ErpUnit::money:
            default:
                return {.id = "money", .display = "", .defaultDecimals = 2};
        }
    }
};

namespace lims {

/// @brief An amount of money in the currency a field beside it names. Declared at two decimals for the schema; a
///        JPY amount holds whole yen, which `fitsCurrency` checks.
using Amount = ::morph::units::Quantity<ErpUnit::money, 2>;

/// @brief An exchange rate (units of the target currency per unit of the source) or a tax rate (0.19 for 19 %).
using Rate = ::morph::units::Quantity<ErpUnit::rate, 6>;

/// @brief A whole number.
using Count = ::morph::units::Quantity<ErpUnit::count, 0>;

/// @brief The minor-unit decimals of @p currency.
/// @param currency The currency.
/// @return 0 for JPY, which has no minor unit; 2 for the others.
[[nodiscard]] constexpr std::uint32_t decimalsOf(CurrencyCode currency) noexcept {
    return currency == CurrencyCode::JPY ? 0U : 2U;
}

/// @brief The ISO 4217 code of @p currency.
/// @param currency The currency.
/// @return Its three-letter code.
[[nodiscard]] constexpr std::string_view currencyName(CurrencyCode currency) noexcept {
    switch (currency) {
        case CurrencyCode::USD:
            return "USD";
        case CurrencyCode::GBP:
            return "GBP";
        case CurrencyCode::CHF:
            return "CHF";
        case CurrencyCode::JPY:
            return "JPY";
        case CurrencyCode::EUR:
        default:
            return "EUR";
    }
}

/// @brief Whether @p value is a whole number of @p currency's minor units.
/// @param value The value.
/// @param currency The currency.
/// @return `true` when the value's canonical denominator divides `10^decimalsOf(currency)`.
[[nodiscard]] constexpr bool fitsCurrency(::morph::math::Rational const& value, CurrencyCode currency) noexcept {
    std::int64_t const scale = decimalsOf(currency) == 0U ? 1 : 100;
    return scale % value.denominator == 0;
}

/// @brief Rounds @p value half-up to @p currency's decimals: the rule every poster applies, line by line.
///
/// Half away from zero, which is half-up for the positive amounts invoices and postings carry; a reversal negates an
/// amount that is already rounded, so it never rounds a negative value.
/// @param value The value.
/// @param currency The currency.
/// @return The rounded value, tagged at the currency's decimals.
/// @throws OutOfRange when the rounded value's numerator does not fit an `int64`.
[[nodiscard]] inline ::morph::math::Rational roundToCurrency(::morph::math::Rational const& value,
                                                             CurrencyCode currency) {
    auto const rounded = ::morph::math::checkedRoundToDecimalPlaces(
        value, ::morph::math::DecimalPlaces{decimalsOf(currency)}, ::morph::math::RoundingMode::HalfAwayFromZero);
    if (!rounded.has_value()) {
        throw OutOfRange{"an amount in " + std::string{currencyName(currency)} + " is too large to round exactly"};
    }
    return *rounded;
}

/// @brief A running total that is exact or refuses: the summation every ledger total and report uses.
///
/// Sums `Rational` values, not `Quantity` ones: a `Quantity` sum records a provenance node per addition, which a
/// report over millions of lines cannot afford.
class ExactSum {
public:
    /// @brief Adds @p value, unless the exact total would not fit.
    /// @param value The value.
    /// @return `true` when added; `false` when the total would overflow, which leaves the total as it was.
    [[nodiscard]] bool add(::morph::math::Rational const& value) noexcept {
        auto const next = ::morph::math::checkedAdd(_total, value);
        if (!next.has_value()) {
            return false;
        }
        _total = *next;
        return true;
    }

    /// @brief The total so far.
    /// @return The exact total of every value added.
    [[nodiscard]] ::morph::math::Rational total() const noexcept { return _total; }

private:
    ::morph::math::Rational _total{0, ::morph::math::DecimalPlaces{0}};
};

}  // namespace lims

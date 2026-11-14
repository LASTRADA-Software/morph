// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/util/quantity.hpp>
#include <morph/util/rational.hpp>
#include <string_view>
#include <utility>

/// @file
/// The units stock is kept in, and the carrier an action's quantity travels in. An item's unit is a fact of its row,
/// so an action cannot name it as a template argument: the quantity is a `Measure` and its unit a `StockUnit` field
/// beside it, converted with `convertStock`, which reads the relations `Quantity` itself converts with.

namespace lims {

/// @brief A unit stock is kept or counted in.
enum class StockUnit : std::uint8_t {
    litre,       ///< Litre.
    millilitre,  ///< Millilitre.
    kilogram,    ///< Kilogram.
    gram,        ///< Gram.
    piece,       ///< One item: a box, a column, a vial.
};

/// @brief The carrier unit of a quantity whose unit is a `StockUnit` field beside it.
enum class InventoryUnit : std::uint8_t {
    measure,  ///< An exact decimal in the unit named beside it.
};

}  // namespace lims

/// @brief Unit metadata and the exact relations within each dimension.
template <>
struct morph::units::UnitTraits<lims::StockUnit> {
    /// @brief Display metadata for @p unit.
    /// @param unit The unit.
    /// @return Its id, display text and default decimals.
    static constexpr morph::units::UnitMeta meta(lims::StockUnit unit) noexcept {
        switch (unit) {
            case lims::StockUnit::litre:
                return {.id = "litre", .display = "L", .defaultDecimals = 3};
            case lims::StockUnit::millilitre:
                return {.id = "millilitre", .display = "mL", .defaultDecimals = 0};
            case lims::StockUnit::kilogram:
                return {.id = "kilogram", .display = "kg", .defaultDecimals = 3};
            case lims::StockUnit::gram:
                return {.id = "gram", .display = "g", .defaultDecimals = 0};
            case lims::StockUnit::piece:
                return {.id = "piece", .display = "pc", .defaultDecimals = 0};
            default:
                break;
        }
        std::unreachable();
    }

    /// @brief 1 mL is 1/1000 L; 1 g is 1/1000 kg. Pieces convert to nothing.
    static constexpr std::array<morph::units::UnitRelation<lims::StockUnit>, 2> relations{{
        {lims::StockUnit::millilitre, lims::StockUnit::litre,
         morph::math::Rational{morph::math::Numerator{1}, morph::math::Denominator{1000},
                               morph::math::DecimalPlaces{3}}},
        {lims::StockUnit::gram, lims::StockUnit::kilogram,
         morph::math::Rational{morph::math::Numerator{1}, morph::math::Denominator{1000},
                               morph::math::DecimalPlaces{3}}},
    }};
};

/// @brief The carrier has no display of its own: the unit beside it is shown.
template <>
struct morph::units::UnitTraits<lims::InventoryUnit> {
    /// @brief Display metadata.
    /// @param unit The unit.
    /// @return Its id and six default decimals.
    static constexpr morph::units::UnitMeta meta(lims::InventoryUnit unit) noexcept {
        static_cast<void>(unit);
        return {.id = "measure", .display = "", .defaultDecimals = 6};
    }
};

namespace lims {

/// @brief A quantity in the `StockUnit` named beside it, exact to six places.
using Measure = ::morph::units::Quantity<InventoryUnit::measure, 6>;

/// @brief The display text of @p unit.
/// @param unit The unit.
/// @return `L`, `mL`, `kg`, `g` or `pc`.
[[nodiscard]] constexpr std::string_view stockUnitName(StockUnit unit) noexcept {
    return ::morph::units::UnitTraits<StockUnit>::meta(unit).display;
}

/// @brief Converts @p value from @p from to @p to, exactly, through `UnitTraits<StockUnit>::relations`.
/// @param value The value in @p from.
/// @param from Its unit.
/// @param to The unit wanted.
/// @return The value in @p to.
/// @throws ValidationError when the two units are not the same unit and no relation joins them.
/// @throws OutOfRange when the converted value does not fit.
[[nodiscard]] ::morph::math::Rational convertStock(::morph::math::Rational const& value, StockUnit from, StockUnit to);

}  // namespace lims

/// @brief Reflects `StockUnit` as its enumerator name.
template <>
struct glz::meta<lims::StockUnit> {
    using enum lims::StockUnit;

    /// @brief The enumerator ⇄ name mapping glaze reads and writes.
    static constexpr auto value = glz::enumerate(litre, millilitre, kilogram, gram, piece);
};

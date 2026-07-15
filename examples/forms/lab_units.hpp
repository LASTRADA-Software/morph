// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The demo's unit system: a plain enum, its `UnitTraits` metadata, and the
/// consteval algebra that lets `Quantity` deduce result units at compile
/// time. This is the application-side half of `morph/quantity.hpp` — morph
/// itself ships no units.
///
/// Naming convention: composition is explicit in the enumerator (`kg_per_m3`,
/// never `kgm3`). The `id` strings become schema/wire vocabulary: append new
/// enumerators, never renumber or rename existing ones.

#include <morph/quantity.hpp>

#include <array>
#include <cstdint>
#include <span>

namespace lab {

/// @brief Units the demo lab works in.
enum class Unit : std::uint16_t {
    scalar = 0,  ///< dimensionless ratio
    percent,     ///< dimensionless, scaled by 100 for display
    kg,          ///< mass
    m3,          ///< volume
    kg_per_m3,   ///< density
    g,           ///< mass entry alternative (1 g = 1/1000 kg)
    t,           ///< mass entry alternative (1 t = 1000 kg)
    l,           ///< volume entry alternative (1 L = 1/1000 m³)
};

}  // namespace lab

/// @brief Static unit metadata: schema id, display text, default decimals.
template <>
struct morph::units::UnitTraits<lab::Unit> {
    static constexpr morph::units::UnitMeta meta(lab::Unit unit) noexcept {
        switch (unit) {
            case lab::Unit::scalar:    return {"scalar", "", 3};
            case lab::Unit::percent:   return {"percent", "%", 1};
            case lab::Unit::kg:        return {"kg", "kg", 3};
            case lab::Unit::m3:        return {"m3", "m³", 3};
            case lab::Unit::kg_per_m3: return {"kg_per_m3", "kg/m³", 1};
            case lab::Unit::g:         return {"g", "g", 1};
            case lab::Unit::t:         return {"t", "t", 4};
            case lab::Unit::l:         return {"l", "L", 1};
        }
        return {"?", "?", 3};
    }

    /// @brief Within-dimension conversion ratios (mass g/t ↔ kg, volume L ↔ m³).
    ///        These drive `convert`, chaining, and the display-unit selector.
    static constexpr std::array<morph::units::UnitRelation<lab::Unit>, 3> relations{{
        {lab::Unit::g, lab::Unit::kg,
         morph::math::Rational{morph::math::Numerator{1}, morph::math::Denominator{1000}, morph::math::DecimalPlaces{3}}},
        {lab::Unit::t, lab::Unit::kg,
         morph::math::Rational{morph::math::Numerator{1000}, morph::math::Denominator{1}, morph::math::DecimalPlaces{3}}},
        {lab::Unit::l, lab::Unit::m3,
         morph::math::Rational{morph::math::Numerator{1}, morph::math::Denominator{1000}, morph::math::DecimalPlaces{3}}},
    }};
};

namespace lab {

/// @brief Unit product table. A combination without an entry is a
///        compile-time error at the call site that attempted it.
consteval Unit operator*(Unit lhs, Unit rhs) {
    if (lhs == Unit::scalar) {
        return rhs;
    }
    if (rhs == Unit::scalar) {
        return lhs;
    }
    if ((lhs == Unit::kg_per_m3 && rhs == Unit::m3) || (lhs == Unit::m3 && rhs == Unit::kg_per_m3)) {
        return Unit::kg;
    }
    throw "lab::Unit: unsupported unit product";
}

/// @brief Unit quotient table.
consteval Unit operator/(Unit lhs, Unit rhs) {
    if (rhs == Unit::scalar) {
        return lhs;
    }
    if (lhs == rhs) {
        return Unit::scalar;
    }
    if (lhs == Unit::kg && rhs == Unit::m3) {
        return Unit::kg_per_m3;
    }
    throw "lab::Unit: unsupported unit quotient";
}

/// @brief Shorthand for quantities in this unit system. The second argument
///        overrides the field's declared precision (defaults from `UnitTraits`).
template <Unit U, std::uint32_t Decimals = morph::units::UnitTraits<Unit>::meta(U).defaultDecimals>
using Quantity = morph::units::Quantity<U, Decimals>;

using Mass = Quantity<Unit::kg>;
using Volume = Quantity<Unit::m3>;
using Density = Quantity<Unit::kg_per_m3>;
using Percent = Quantity<Unit::percent>;

}  // namespace lab

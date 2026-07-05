// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file quantity.hpp
/// @brief Unit-tagged, optionally-empty exact values for morph actions.
///
/// `morph::units::Quantity<U>` wraps an *optional* `morph::math::Rational`
/// and carries its physical unit as a template argument — a value of the
/// application's own unit enum. The design decisions, in order:
///
/// - **One kind of empty.** The blank state ("not entered", "not measured")
///   lives *inside* the quantity as `std::optional<Rational>`; action structs
///   never wrap a `Quantity` in another `std::optional`. Whether a field may
///   still be empty at submit time is field *metadata* (see
///   `morph/forms.hpp`), not a second wrapper type.
/// - **Units are types.** `Quantity<Unit::kg>` and `Quantity<Unit::m3>`
///   cannot be mixed accidentally; `operator*` / `operator/` deduce the
///   result unit at compile time from the *application-defined* `consteval`
///   algebra on the unit enum (see below). Unsupported combinations do not
///   compile.
/// - **Units never travel.** On the morph JSON wire a quantity is just its
///   nullable Rational payload; the unit appears only in generated JSON
///   Schemas (as `ExtUnits`) and in C++ types. A client cannot send a
///   mismatched unit.
/// - **Empty propagates.** Arithmetic on an empty quantity yields an empty
///   quantity (spreadsheet/SQL-NULL semantics). Division by zero also yields
///   empty — the framework cannot distinguish "no data" from "no result"
///   without complicating every call site; validate before dividing if the
///   distinction matters.
///
/// @par Defining a unit system (application side)
/// @code{.cpp}
/// enum class Unit : std::uint16_t { scalar, kg, m3, kg_per_m3 };
///
/// // Explicit _per_ / _times_ names; ids are wire/schema vocabulary:
/// // append new enumerators, never renumber or rename existing ones.
/// template <>
/// struct morph::units::UnitTraits<Unit> {
///     static constexpr morph::units::UnitMeta meta(Unit unit) noexcept {
///         switch (unit) {
///             case Unit::scalar:    return {"scalar", "", 3};
///             case Unit::kg:        return {"kg", "kg", 3};
///             case Unit::m3:        return {"m3", "m³", 3};
///             case Unit::kg_per_m3: return {"kg_per_m3", "kg/m³", 1};
///         }
///         return {"?", "?", 3};
///     }
/// };
///
/// // The unit algebra: consteval, so an unsupported combination is a
/// // compile-time error at the call site that attempted it.
/// consteval Unit operator/(Unit lhs, Unit rhs) {
///     if (rhs == Unit::scalar) return lhs;
///     if (lhs == rhs) return Unit::scalar;
///     if (lhs == Unit::kg && rhs == Unit::m3) return Unit::kg_per_m3;
///     throw "unsupported unit quotient";
/// }
///
/// using Mass = morph::units::Quantity<Unit::kg>;
/// using Volume = morph::units::Quantity<Unit::m3>;
/// // Mass{...} / Volume{...} -> morph::units::Quantity<Unit::kg_per_m3>
/// @endcode

#include <glaze/glaze.hpp>

#include <compare>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include "rational.hpp"

namespace morph::units {

/// @brief Static description of one unit, supplied by `UnitTraits`.
struct UnitMeta {
    /// @brief Stable ascii identifier, e.g. `"kg_per_m3"`. Appears in JSON
    ///        Schemas (`ExtUnits.unitAscii`); part of the protocol vocabulary.
    std::string_view id;

    /// @brief Human display text, e.g. `"kg/m³"` (`ExtUnits.unitUnicode`).
    std::string_view display;

    /// @brief Default decimal places for parsing/formatting values of this
    ///        unit; surfaces in schemas as `x-decimalPlaces`.
    std::uint32_t defaultDecimals{3};
};

/// @brief Customisation point: the application specialises this for its unit
///        enum and returns a `UnitMeta` per enumerator.
/// @tparam E The application's unit enum type.
template <typename E>
struct UnitTraits;

/// @brief Concept: an enum with a `UnitTraits` specialisation.
template <typename E>
concept UnitEnum = std::is_enum_v<E> && requires(E unit) {
    { UnitTraits<E>::meta(unit) } -> std::convertible_to<UnitMeta>;
};

/// @brief A unit-tagged, optionally-empty exact value.
///
/// Aggregate; default state is *empty* — a form draft starts blank while the
/// unit (and its default precision) are already known from the type.
///
/// @tparam U An enumerator of an application unit enum satisfying `UnitEnum`.
template <auto U>
    requires UnitEnum<decltype(U)>
struct Quantity {
    /// @brief The payload; `std::nullopt` means "not entered / not measured".
    std::optional<math::Rational> value{};

    /// @brief The unit this quantity is denominated in (compile-time).
    static constexpr auto unit = U;

    /// @brief The unit's static metadata (id, display text, default decimals).
    /// @return The `UnitMeta` from the application's `UnitTraits` specialisation.
    [[nodiscard]] static constexpr UnitMeta unitMeta() noexcept { return UnitTraits<decltype(U)>::meta(U); }

    /// @brief Whether a value has been entered/measured.
    /// @return `true` if the payload is engaged.
    [[nodiscard]] constexpr bool hasValue() const noexcept { return value.has_value(); }

    /// @brief Unchecked access to the engaged value (UB when empty, exactly
    ///        like `std::optional`).
    /// @return The engaged exact value.
    [[nodiscard]] constexpr const math::Rational& operator*() const noexcept { return *value; }

    /// @brief Ordering/equality on the payload; empty sorts before engaged
    ///        (std::optional semantics), values compare exactly.
    /// @param other Quantity of the same unit to compare against.
    /// @return The ordering of the two payloads.
    [[nodiscard]] constexpr auto operator<=>(const Quantity& other) const noexcept = default;
};

namespace detail {

/// @brief Trait: is `T` some `Quantity<U>`?
template <typename T>
struct IsQuantity : std::false_type {};

template <auto U>
struct IsQuantity<Quantity<U>> : std::true_type {};

}  // namespace detail

/// @brief `true` when `T` (cvref-stripped) is a `morph::units::Quantity`.
template <typename T>
inline constexpr bool is_quantity_v = detail::IsQuantity<std::remove_cvref_t<T>>::value;

// ---------------------------------------------------------------------------
// Arithmetic. Empty propagates; division by zero yields empty.
// ---------------------------------------------------------------------------

/// @brief Same-unit sum. Empty if either operand is empty.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @return The exact sum, or empty.
template <auto U>
[[nodiscard]] constexpr Quantity<U> operator+(const Quantity<U>& lhs, const Quantity<U>& rhs) noexcept {
    if (!lhs.value || !rhs.value) {
        return {};
    }
    return Quantity<U>{*lhs.value + *rhs.value};
}

/// @brief Same-unit difference. Empty if either operand is empty.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @return The exact difference, or empty.
template <auto U>
[[nodiscard]] constexpr Quantity<U> operator-(const Quantity<U>& lhs, const Quantity<U>& rhs) noexcept {
    if (!lhs.value || !rhs.value) {
        return {};
    }
    return Quantity<U>{*lhs.value - *rhs.value};
}

/// @brief Negation. Empty stays empty.
/// @param operand Value to negate.
/// @return The negated value, or empty.
template <auto U>
[[nodiscard]] constexpr Quantity<U> operator-(const Quantity<U>& operand) noexcept {
    if (!operand.value) {
        return {};
    }
    return Quantity<U>{-*operand.value};
}

/// @brief Cross-unit product; the result unit is `A * B` per the application's
///        consteval unit algebra. Does not participate in overload resolution
///        when the algebra rejects the combination.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @return The exact product in the deduced unit, or empty.
template <auto A, auto B>
    requires requires { typename std::type_identity_t<Quantity<A * B>>; }
[[nodiscard]] constexpr Quantity<A * B> operator*(const Quantity<A>& lhs, const Quantity<B>& rhs) noexcept {
    if (!lhs.value || !rhs.value) {
        return {};
    }
    return Quantity<A * B>{*lhs.value * *rhs.value};
}

/// @brief Cross-unit quotient; the result unit is `A / B` per the application's
///        consteval unit algebra. Empty when either side is empty *or* the
///        divisor is zero.
/// @param lhs Dividend.
/// @param rhs Divisor.
/// @return The exact quotient in the deduced unit, or empty.
template <auto A, auto B>
    requires requires { typename std::type_identity_t<Quantity<A / B>>; }
[[nodiscard]] constexpr Quantity<A / B> operator/(const Quantity<A>& lhs, const Quantity<B>& rhs) noexcept {
    if (!lhs.value || !rhs.value) {
        return {};
    }
    // DividedBy is the single division-by-zero authority: it reports exactly
    // when the divisor is zero, which maps to the empty result here.
    auto quotient = lhs.value->DividedBy(*rhs.value);
    if (!quotient) {
        return {};
    }
    return Quantity<A / B>{*quotient};
}

/// @brief Scales by a dimensionless rational (unit unchanged).
/// @param lhs    Quantity to scale.
/// @param factor Dimensionless factor.
/// @return The scaled quantity, or empty.
template <auto U>
[[nodiscard]] constexpr Quantity<U> operator*(const Quantity<U>& lhs, const math::Rational& factor) noexcept {
    if (!lhs.value) {
        return {};
    }
    return Quantity<U>{*lhs.value * factor};
}

/// @brief Scales by a dimensionless rational (unit unchanged).
/// @param factor Dimensionless factor.
/// @param rhs    Quantity to scale.
/// @return The scaled quantity, or empty.
template <auto U>
[[nodiscard]] constexpr Quantity<U> operator*(const math::Rational& factor, const Quantity<U>& rhs) noexcept {
    return rhs * factor;
}

/// @brief Divides by a dimensionless rational (unit unchanged). Empty when the
///        quantity is empty or the divisor is zero.
/// @param lhs     Quantity to divide.
/// @param divisor Dimensionless divisor.
/// @return The divided quantity, or empty.
template <auto U>
[[nodiscard]] constexpr Quantity<U> operator/(const Quantity<U>& lhs, const math::Rational& divisor) noexcept {
    if (!lhs.value) {
        return {};
    }
    auto quotient = lhs.value->DividedBy(divisor);
    if (!quotient) {
        return {};
    }
    return Quantity<U>{*quotient};
}

}  // namespace morph::units

// ---------------------------------------------------------------------------
// Glaze integration: wire shape + schema units.
// ---------------------------------------------------------------------------

/// @brief On the wire a Quantity is its nullable Rational payload — the unit
///        never travels; it lives in the C++ type and in generated schemas.
template <auto U>
struct glz::meta<morph::units::Quantity<U>> {
    static constexpr auto value = &morph::units::Quantity<U>::value;
    static constexpr std::string_view name = morph::units::UnitTraits<decltype(U)>::meta(U).id;
};

namespace glz::detail {

/// @brief Schema generation for `Quantity<U>`: the nullable-Rational schema
///        with the unit stamped on as `ExtUnits`, sourced from `UnitTraits`.
///
/// `to_json_schema` is glaze's own per-type schema hook (every built-in type
/// specialises it); glaze is pinned in the build, so relying on it is safe.
template <auto U>
struct to_json_schema<morph::units::Quantity<U>> {
    template <auto Opts>
    static void op(auto& s, auto& defs) {
        to_json_schema<std::optional<morph::math::Rational>>::template op<Opts>(s, defs);
        constexpr auto unitMeta = morph::units::UnitTraits<decltype(U)>::meta(U);
        s.ExtUnits = ExtUnits{.unitAscii = unitMeta.id, .unitUnicode = unitMeta.display};
    }
};

}  // namespace glz::detail

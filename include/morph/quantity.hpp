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
/// - **Precision is declared in the type; actual precision is runtime
///   data.** Each field carries a *declared* decimal count as a template
///   argument — defaulting from the unit's metadata, overridable per field
///   (`Quantity<Unit::m3, 4>`) — which drives the schema's
///   `x-decimalPlaces` and `fromDouble`. The value's *actual* precision is
///   the runtime `DecimalPlaces` tag inside the Rational: it max-propagates
///   through arithmetic and can be changed at run time
///   (`withDecimalPlaces`, `atDeclaredPrecision`).
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
///
/// // Optional: convertible entry units with exact ratios. Renderers offer a
/// // unit selector and recalculate exactly; payloads stay canonical.
/// // static constexpr std::span<const UnitAlternative<Unit>> alternatives(Unit);
/// @endcode

#include <glaze/glaze.hpp>

#include <compare>
#include <cstdint>
#include <optional>
#include <span>
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

/// @brief One convertible display/entry unit for a canonical unit.
///
/// The ratio is exact: `value_in_canonical = value_in_alternative * num/den`
/// (e.g. grams -> kilograms is `{Unit::g, 1, 1000}`). Both components must
/// be positive.
/// @tparam E The application's unit enum type.
template <typename E>
struct UnitAlternative {
    /// @brief The alternative unit (must have `UnitTraits` metadata).
    E unit{};

    /// @brief Numerator of the exact alternative-to-canonical ratio.
    std::int64_t num{1};

    /// @brief Denominator of the exact alternative-to-canonical ratio.
    std::int64_t den{1};
};

/// @brief Customisation point: the application specialises this for its unit
///        enum and returns a `UnitMeta` per enumerator.
///
/// Optionally, a specialisation may also provide
/// `static constexpr std::span<const UnitAlternative<E>> alternatives(E)` —
/// the convertible display/entry units per canonical unit. Renderers then
/// offer a unit selector on such fields and recalculate entered values
/// exactly on switch; the wire and the model always stay in the canonical
/// unit.
/// @tparam E The application's unit enum type.
template <typename E>
struct UnitTraits;

/// @brief Concept: `UnitTraits<E>` also declares convertible alternatives.
template <typename E>
concept HasUnitAlternatives = requires(E unit) {
    { UnitTraits<E>::alternatives(unit) } -> std::convertible_to<std::span<const UnitAlternative<E>>>;
};

/// @brief Concept: an enum with a `UnitTraits` specialisation.
template <typename E>
concept UnitEnum = std::is_enum_v<E> && requires(E unit) {
    { UnitTraits<E>::meta(unit) } -> std::convertible_to<UnitMeta>;
};

/// @brief A unit-tagged, optionally-empty exact value with a *declared*
///        precision in its type.
///
/// Two precisions exist by design and must not be conflated:
///
/// - **Declared precision** (`DeclaredDecimals`, a template argument): a
///   property of the *field* — how many decimals this slot is specified to
///   hold. It defaults from the unit's `UnitTraits` metadata and can be
///   overridden per field (`Quantity<Unit::m3, 4>`). It feeds the generated
///   schema (`x-decimalPlaces`), `fromDouble`, and `atDeclaredPrecision`.
/// - **Actual precision**: a property of the *value* — the runtime
///   `DecimalPlaces` tag inside the `Rational` payload. It propagates through
///   arithmetic (max rule) and can be changed at run time
///   (`withDecimalPlaces`).
///
/// Default state is *empty* — a form draft starts blank while the unit and
/// the declared precision are already known from the type. Same-unit
/// quantities convert freely across declared precisions (the value,
/// including its runtime tag, carries over unchanged).
///
/// @tparam U                An enumerator of an application unit enum
///                          satisfying `UnitEnum`.
/// @tparam DeclaredDecimals Declared decimal places of the field; defaults
///                          to the unit's `UnitMeta::defaultDecimals`.
template <auto U, std::uint32_t DeclaredDecimals = UnitTraits<decltype(U)>::meta(U).defaultDecimals>
    requires UnitEnum<decltype(U)>
struct Quantity {
    static_assert(DeclaredDecimals >= 1 && DeclaredDecimals <= math::kMaxDecimalPlaces,
                  "declared decimals must be within [1, kMaxDecimalPlaces]");

    /// @brief The payload; `std::nullopt` means "not entered / not measured".
    std::optional<math::Rational> value;

    /// @brief Constructs the empty state.
    constexpr Quantity() noexcept = default;

    /// @brief Engages with @p engaged (which keeps its own runtime precision).
    /// @param engaged The exact value to hold.
    constexpr Quantity(math::Rational engaged) noexcept : value{engaged} {}

    /// @brief Adopts an optional payload as-is.
    /// @param payload Engaged or empty payload.
    constexpr Quantity(std::optional<math::Rational> payload) noexcept : value{payload} {}

    /// @brief Converts from the same unit at another declared precision; the
    ///        value and its runtime precision tag carry over unchanged.
    /// @param other Same-unit quantity with a different declared precision.
    template <std::uint32_t OtherDecimals>
        requires(OtherDecimals != DeclaredDecimals)
    constexpr Quantity(const Quantity<U, OtherDecimals>& other) noexcept : value{other.value} {}

    /// @brief The unit this quantity is denominated in (compile-time).
    static constexpr auto unit = U;

    /// @brief Declared decimal places of this field (compile-time).
    static constexpr std::uint32_t declaredDecimals = DeclaredDecimals;

    /// @brief The unit's static metadata (id, display text, default decimals).
    /// @return The `UnitMeta` from the application's `UnitTraits` specialisation.
    [[nodiscard]] static constexpr UnitMeta unitMeta() noexcept { return UnitTraits<decltype(U)>::meta(U); }

    /// @brief The declared precision as a `DecimalPlaces` strong type.
    /// @return `DecimalPlaces{declaredDecimals}`.
    [[nodiscard]] static constexpr math::DecimalPlaces declaredPrecision() noexcept {
        return math::DecimalPlaces{DeclaredDecimals};
    }

    /// @brief The convertible display/entry units declared for this field's
    ///        unit (empty when the unit system declares none).
    /// @return Exact-ratio alternatives from `UnitTraits<E>::alternatives`.
    [[nodiscard]] static constexpr std::span<const UnitAlternative<decltype(U)>> unitAlternatives() noexcept {
        if constexpr (HasUnitAlternatives<decltype(U)>) {
            return UnitTraits<decltype(U)>::alternatives(U);
        } else {
            return {};
        }
    }

    /// @brief Converts a floating-point reading at the declared precision.
    /// @param raw The value to convert.
    /// @return The engaged quantity, or empty when @p raw is not finite or
    ///         does not fit (empty-propagation philosophy: no error channel).
    [[nodiscard]] static Quantity fromDouble(double raw) noexcept {
        if (auto converted = math::Rational::fromFloat(raw, declaredPrecision()); converted.has_value()) {
            return Quantity{*converted};
        }
        return {};
    }

    /// @brief Whether a value has been entered/measured.
    /// @return `true` if the payload is engaged.
    [[nodiscard]] constexpr bool hasValue() const noexcept { return value.has_value(); }

    /// @brief Unchecked access to the engaged value (UB when empty, exactly
    ///        like `std::optional` — the unchecked contract is the point).
    /// @return The engaged exact value.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    [[nodiscard]] constexpr const math::Rational& operator*() const noexcept { return *value; }

    /// @brief Retags the value's *runtime* precision (the exact value itself
    ///        is unchanged — precision only affects rounding and formatting).
    /// @param newPrecision Runtime precision; silently clamped to the valid range.
    /// @return The retagged quantity, or empty if this is empty.
    [[nodiscard]] constexpr Quantity withDecimalPlaces(math::DecimalPlaces newPrecision) const noexcept {
        if (!value) {
            return {};
        }
        auto adjusted = *value;
        adjusted.decimalPlaces = math::DecimalPlaces{math::detail::clampWireDecimalPlaces(newPrecision.value)};
        return Quantity{adjusted};
    }

    /// @brief Retags the value's runtime precision to the declared one —
    ///        e.g. before display, after arithmetic widened it.
    /// @return The retagged quantity, or empty if this is empty.
    [[nodiscard]] constexpr Quantity atDeclaredPrecision() const noexcept {
        return withDecimalPlaces(declaredPrecision());
    }
};

/// @brief Ordering across same-unit quantities of any declared precision;
///        empty sorts before engaged, engaged values compare exactly.
/// @param lhs Left operand.
/// @param rhs Right operand (same unit, any declared precision).
/// @return The ordering of the two payloads.
template <auto U, std::uint32_t DecA, std::uint32_t DecB>
[[nodiscard]] constexpr std::strong_ordering operator<=>(const Quantity<U, DecA>& lhs,
                                                         const Quantity<U, DecB>& rhs) noexcept {
    return lhs.value <=> rhs.value;
}

/// @brief Equality across same-unit quantities of any declared precision.
/// @param lhs Left operand.
/// @param rhs Right operand (same unit, any declared precision).
/// @return `true` when both are empty or both engaged with equal values.
template <auto U, std::uint32_t DecA, std::uint32_t DecB>
[[nodiscard]] constexpr bool operator==(const Quantity<U, DecA>& lhs, const Quantity<U, DecB>& rhs) noexcept {
    return lhs.value == rhs.value;
}

namespace detail {

/// @brief Trait: is `T` some `Quantity<U, Decimals>`?
template <typename T>
struct IsQuantity : std::false_type {};

template <auto U, std::uint32_t Decimals>
struct IsQuantity<Quantity<U, Decimals>> : std::true_type {};

}  // namespace detail

/// @brief `true` when `T` (cvref-stripped) is a `morph::units::Quantity`.
template <typename T>
inline constexpr bool isQuantity = detail::IsQuantity<std::remove_cvref_t<T>>::value;

// ---------------------------------------------------------------------------
// Arithmetic. Empty propagates; division by zero yields empty.
//
// Binary results carry the unit's *default* declared precision: the declared
// tag is a property of a declared field, not of a computed temporary (the
// value's runtime precision still max-propagates inside Rational, and the
// same-unit converting constructor stores results into fields with any
// declared precision).
// ---------------------------------------------------------------------------

/// @brief Same-unit sum across any declared precisions. Empty if either
///        operand is empty.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @return The exact sum, or empty.
template <auto U, std::uint32_t DecA, std::uint32_t DecB>
[[nodiscard]] constexpr Quantity<U> operator+(const Quantity<U, DecA>& lhs, const Quantity<U, DecB>& rhs) noexcept {
    if (!lhs.value || !rhs.value) {
        return {};
    }
    return Quantity<U>{*lhs.value + *rhs.value};
}

/// @brief Same-unit difference across any declared precisions. Empty if
///        either operand is empty.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @return The exact difference, or empty.
template <auto U, std::uint32_t DecA, std::uint32_t DecB>
[[nodiscard]] constexpr Quantity<U> operator-(const Quantity<U, DecA>& lhs, const Quantity<U, DecB>& rhs) noexcept {
    if (!lhs.value || !rhs.value) {
        return {};
    }
    return Quantity<U>{*lhs.value - *rhs.value};
}

/// @brief Negation. Empty stays empty; the declared precision is kept.
/// @param operand Value to negate.
/// @return The negated value, or empty.
template <auto U, std::uint32_t Dec>
[[nodiscard]] constexpr Quantity<U, Dec> operator-(const Quantity<U, Dec>& operand) noexcept {
    if (!operand.value) {
        return {};
    }
    return Quantity<U, Dec>{-*operand.value};
}

/// @brief Cross-unit product; the result unit is `A * B` per the application's
///        consteval unit algebra. Does not participate in overload resolution
///        when the algebra rejects the combination.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @return The exact product in the deduced unit, or empty.
template <auto A, std::uint32_t DecA, auto B, std::uint32_t DecB>
    requires requires { typename std::type_identity_t<Quantity<A * B>>; }
[[nodiscard]] constexpr Quantity<A * B> operator*(const Quantity<A, DecA>& lhs,
                                                  const Quantity<B, DecB>& rhs) noexcept {
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
template <auto A, std::uint32_t DecA, auto B, std::uint32_t DecB>
    requires requires { typename std::type_identity_t<Quantity<A / B>>; }
[[nodiscard]] constexpr Quantity<A / B> operator/(const Quantity<A, DecA>& lhs,
                                                  const Quantity<B, DecB>& rhs) noexcept {
    if (!lhs.value || !rhs.value) {
        return {};
    }
    // dividedBy is the single division-by-zero authority: it reports exactly
    // when the divisor is zero, which maps to the empty result here.
    auto quotient = lhs.value->dividedBy(*rhs.value);
    if (!quotient) {
        return {};
    }
    return Quantity<A / B>{*quotient};
}

/// @brief Scales by a dimensionless rational (unit and declared precision
///        unchanged).
/// @param lhs    Quantity to scale.
/// @param factor Dimensionless factor.
/// @return The scaled quantity, or empty.
template <auto U, std::uint32_t Dec>
[[nodiscard]] constexpr Quantity<U, Dec> operator*(const Quantity<U, Dec>& lhs,
                                                   const math::Rational& factor) noexcept {
    if (!lhs.value) {
        return {};
    }
    return Quantity<U, Dec>{*lhs.value * factor};
}

/// @brief Scales by a dimensionless rational (unit and declared precision
///        unchanged).
/// @param factor Dimensionless factor.
/// @param rhs    Quantity to scale.
/// @return The scaled quantity, or empty.
template <auto U, std::uint32_t Dec>
[[nodiscard]] constexpr Quantity<U, Dec> operator*(const math::Rational& factor,
                                                   const Quantity<U, Dec>& rhs) noexcept {
    return rhs * factor;
}

/// @brief Divides by a dimensionless rational (unit and declared precision
///        unchanged). Empty when the quantity is empty or the divisor is zero.
/// @param lhs     Quantity to divide.
/// @param divisor Dimensionless divisor.
/// @return The divided quantity, or empty.
template <auto U, std::uint32_t Dec>
[[nodiscard]] constexpr Quantity<U, Dec> operator/(const Quantity<U, Dec>& lhs,
                                                   const math::Rational& divisor) noexcept {
    if (!lhs.value) {
        return {};
    }
    auto quotient = lhs.value->dividedBy(divisor);
    if (!quotient) {
        return {};
    }
    return Quantity<U, Dec>{*quotient};
}

}  // namespace morph::units

// ---------------------------------------------------------------------------
// Glaze integration: wire shape + schema units.
// ---------------------------------------------------------------------------

/// @brief On the wire a Quantity is its nullable Rational payload — neither
///        the unit nor the declared precision travels; both live in the C++
///        type and in generated schemas.
template <auto U, std::uint32_t Dec>
struct glz::meta<morph::units::Quantity<U, Dec>> {
    static constexpr auto value = &morph::units::Quantity<U, Dec>::value;
    static constexpr std::string_view name = morph::units::UnitTraits<decltype(U)>::meta(U).id;
};

namespace glz::detail {

/// @brief Schema generation for `Quantity<U, Dec>`: the nullable-Rational
///        schema with the unit stamped on as `ExtUnits`, sourced from
///        `UnitTraits` (the declared precision surfaces separately as
///        `x-decimalPlaces` via `morph::forms::schemaJson`).
///
/// `to_json_schema` is glaze's own per-type schema hook (every built-in type
/// specialises it); glaze is pinned in the build, so relying on it is safe.
template <auto U, std::uint32_t Dec>
struct to_json_schema<morph::units::Quantity<U, Dec>> {
    template <auto Opts>
    static void op(auto& outSchema, auto& defs) {
        to_json_schema<std::optional<morph::math::Rational>>::template op<Opts>(outSchema, defs);
        constexpr auto unitMeta = morph::units::UnitTraits<decltype(U)>::meta(U);
        outSchema.ExtUnits = ExtUnits{.unitAscii = unitMeta.id, .unitUnicode = unitMeta.display};
    }
};

}  // namespace glz::detail

// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms.hpp
/// @brief JSON-Forms-style schema generation for morph actions.
///
/// Given an action type `A` (a plain aggregate, as registered with
/// `BRIDGE_REGISTER_ACTION`), this header produces a standard JSON Schema a
/// client can render a form from at runtime — the "describe" half of an
/// auto-generated GUI. It builds on glaze's `write_json_schema` (which
/// already contributes types, `$defs`, per-field metadata declared via
/// `glz::json_schema<A>`, and the `ExtUnits` stamped by
/// `morph::units::Quantity`) and closes the two gaps glaze leaves open:
///
/// - **`required`** — glaze's schema writer emits no `required` array at
///   all. `schemaJson<A>()` derives one: a member is *required* unless it is
///   a `std::optional<...>` or its name is listed in the action's opt-out
///   list (see below).
/// - **`x-decimalPlaces`** — for `Quantity` members, the unit's default
///   decimal count from `UnitTraits`, so a client knows the input step
///   without hardcoding unit knowledge.
/// - **`x-order`** — the member's declaration index on every property, so a
///   renderer can lay fields out in declaration order (JSON object key order
///   is not reliable once schemas pass through DOMs/maps).
/// - **`x-unitAlternatives`** — for `Quantity` members whose unit system
///   declares convertible units (`UnitTraits<E>::alternatives`): an array of
///   `{id, display, decimals, num, den}` entries, where `num/den` is the
///   exact alternative-to-canonical ratio. Renderers offer a unit selector
///   and recalculate entered values exactly on switch; payloads always carry
///   the canonical unit.
/// - **`x-optionsAction` / `x-optionValue` / `x-optionLabel`** — for
///   `morph::forms::Choice` members: which registered action serves the
///   options, and which result-row fields carry the submitted value and the
///   display label. Renderers turn these into combo boxes populated by
///   executing the named action.
///
/// `morph::time::Timestamp` members need no extension keys: their schema
/// carries the standard `"format": "date-time"` annotation.
///
/// @par Declaring optional fields
/// Required is the default. An action opts individual fields out with a
/// static member list:
/// @code{.cpp}
/// struct RecordMeasurement {
///     std::int64_t sampleId = 0;
///     Density density{};
///     Moisture moisture{};   // may stay empty
///
///     static constexpr std::array optionalFields{std::string_view{"moisture"}};
///     [[nodiscard]] bool validate() const { return morph::forms::allRequiredEngaged(*this); }
/// };
/// @endcode
///
/// @par Readiness helper
/// `allRequiredEngaged(action)` returns `true` when every *required*
/// empty-capable member (`Quantity`, `Choice`, `Timestamp` — anything with a
/// `hasValue()`) is engaged. Wire it up as the action's `validate()`
/// (the existing `ActionValidator` machinery picks that method up
/// automatically) so the same declaration drives the schema's `required`
/// array, the client-side submit gate, and the fielded-action readiness
/// check. Non-quantity members are not checked — a plain `int64_t` cannot
/// express "not filled in"; use a `Quantity` (or a custom `validate()`) when
/// that distinction matters.

#include <glaze/glaze.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "choice.hpp"
#include "quantity.hpp"

namespace morph::forms {

/// @brief Concept: a field type with an internal empty state (`Quantity`,
///        `Choice`, `Timestamp`, or any user type exposing `hasValue()`).
template <typename T>
concept EmptyCapableField = requires(const T& field) {
    { field.hasValue() } -> std::convertible_to<bool>;
};

namespace detail {

/// @brief Trait: is `T` a `std::optional<...>` (and therefore never required)?
template <typename T>
struct IsStdOptional : std::false_type {};

template <typename T>
struct IsStdOptional<std::optional<T>> : std::true_type {};

template <typename T>
inline constexpr bool isStdOptional = IsStdOptional<std::remove_cvref_t<T>>::value;

/// @brief Concept: action declares a `static constexpr` iterable
///        `optionalFields` list of field names.
template <typename A>
concept HasOptionalFields = requires {
    std::begin(A::optionalFields);
    std::end(A::optionalFields);
};

/// @brief Returns `true` when @p fieldName appears in `A::optionalFields`.
template <typename A>
[[nodiscard]] constexpr bool declaredOptional(std::string_view fieldName) noexcept {
    if constexpr (HasOptionalFields<A>) {
        for (std::string_view candidate : A::optionalFields) {
            if (candidate == fieldName) {
                return true;
            }
        }
    } else {
        static_cast<void>(fieldName);
    }
    return false;
}

/// @brief Invokes `visitor.operator()<I>(name, member)` for every reflected
///        member of @p action (glaze pure reflection).
template <typename A, typename Visitor>
constexpr void forEachNamedMember(A&& action, Visitor&& visitor) {
    using Plain = std::remove_cvref_t<A>;
    constexpr auto memberCount = glz::reflect<Plain>::size;
    auto memberTie = glz::to_tie(action);
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (visitor.template operator()<I>(glz::reflect<Plain>::keys[I], glz::get_member(action, get<I>(memberTie))),
         ...);
    }(std::make_index_sequence<memberCount>{});
}

/// @brief The DOM post-merge behind `schemaJson`: adds the derived `required`
///        array, `x-order`, and `x-decimalPlaces` to a glaze-produced schema.
///
/// Separated from `schemaJson` so the fallback path (malformed input passes
/// through unchanged) is directly testable.
template <typename A>
[[nodiscard]] std::string mergeSchemaExtras(std::string rawSchema) {
    // u64 number mode: the schema carries int64/uint64 bounds in $defs, which
    // the default double-only DOM would silently round.
    glz::generic_u64 dom{};
    if (glz::read_json(dom, rawSchema)) {
        return rawSchema;
    }

    glz::generic_u64::array_t requiredNames{};
    A probe{};
    forEachNamedMember(probe, [&]<std::size_t I>(std::string_view name, const auto& member) {
        using Member = std::remove_cvref_t<decltype(member)>;
        static_cast<void>(member);
        const bool isOptional = isStdOptional<Member> || declaredOptional<A>(name);
        if (!isOptional) {
            requiredNames.emplace_back(std::string{name});
        }
        auto& property = dom["properties"][std::string{name}];
        property["x-order"] = std::uint64_t{I};
        if constexpr (units::isQuantity<Member>) {
            // The field's *declared* precision: the unit default unless the
            // field's type overrides it (Quantity<Unit::m3, 4>).
            property["x-decimalPlaces"] = std::uint64_t{Member::declaredDecimals};

            // Convertible display/entry units with their exact ratios.
            auto const alternatives = Member::unitAlternatives();
            if (!alternatives.empty()) {
                glz::generic_u64::array_t list{};
                for (auto const& alternative : alternatives) {
                    auto const meta = units::UnitTraits<std::remove_const_t<decltype(Member::unit)>>::meta(alternative.unit);
                    glz::generic_u64 entry{};
                    entry["id"] = std::string{meta.id};
                    entry["display"] = std::string{meta.display};
                    entry["decimals"] = std::uint64_t{meta.defaultDecimals};
                    entry["num"] = alternative.num;
                    entry["den"] = alternative.den;
                    list.emplace_back(std::move(entry));
                }
                property["x-unitAlternatives"] = list;
            }
        }
        if constexpr (isChoice<Member>) {
            // Which action serves the options, and which result-row fields
            // carry the submitted value / display label.
            property["x-optionsAction"] = std::string{Member::optionsAction()};
            property["x-optionValue"] = std::string{Member::valueField()};
            property["x-optionLabel"] = std::string{Member::labelField()};
        }
    });
    // Always assign — an explicit empty array beats leaving whatever the
    // schema writer may have emitted (or omitted) for `required`.
    dom["required"] = requiredNames;

    // value_or without a move: the copy is irrelevant (schemaJson memoises),
    // and keeping the fallback branch inside glaze's expected avoids an
    // untestable line here (write_json of a DOM we just built cannot fail).
    return glz::write_json(dom).value_or(rawSchema);
}

}  // namespace detail

/// @brief Whether every required empty-capable member of @p action is
///        engaged (has a value).
///
/// Empty-capable covers `Quantity`, `Choice`, `Timestamp`, and any user type
/// satisfying `EmptyCapableField`. Required means: not a `std::optional<...>`
/// member and not listed in `A::optionalFields`. Intended as the body of the
/// action's `validate()`.
/// @tparam A     Action type (a reflectable aggregate).
/// @param action Draft whose fields are checked.
/// @return `true` when no required empty-capable field is empty.
template <typename A>
[[nodiscard]] constexpr bool allRequiredEngaged(const A& action) noexcept {
    bool allEngaged = true;
    detail::forEachNamedMember(action, [&]<std::size_t I>(std::string_view name, const auto& member) {
        using Member = std::remove_cvref_t<decltype(member)>;
        if constexpr (EmptyCapableField<Member>) {
            if (!detail::declaredOptional<A>(name) && !member.hasValue()) {
                allEngaged = false;
            }
        } else {
            static_cast<void>(name);
            static_cast<void>(member);
        }
    });
    return allEngaged;
}

/// @brief Generates the JSON Schema for action type @p A, ready for a
///        client-side form renderer.
///
/// glaze's `write_json_schema<A>()` output, post-processed with:
///   - a top-level `required` array (see file docs for the rule),
///   - `x-decimalPlaces` on every `Quantity` property (the unit's default),
///   - `x-order` (declaration index) on every property.
///
/// The result is fixed per type, so it is computed once and cached. On any
/// internal failure the unmerged glaze schema (or an empty string if even
/// that failed) is returned rather than throwing — schema generation is a
/// description facility, never worth crashing a server over.
/// @tparam A Action type (a reflectable aggregate).
/// @return The merged schema JSON.
template <typename A>
[[nodiscard]] std::string schemaJson() {
    static const std::string cached =
        detail::mergeSchemaExtras<A>(glz::write_json_schema<A>().value_or(std::string{}));
    return cached;
}

}  // namespace morph::forms

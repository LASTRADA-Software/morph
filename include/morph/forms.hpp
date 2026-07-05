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
/// `Quantity` member is engaged. Wire it up as the action's `validate()`
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

#include "quantity.hpp"

namespace morph::forms {

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

/// @brief Invokes @p visitor as `visitor(name, member)` for every reflected
///        member of @p action (glaze pure reflection).
template <typename A, typename Visitor>
constexpr void forEachNamedMember(A&& action, Visitor&& visitor) {
    using Plain = std::remove_cvref_t<A>;
    constexpr auto memberCount = glz::reflect<Plain>::size;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (visitor.template operator()<I>(glz::reflect<Plain>::keys[I],
                                        glz::get_member(action, get<I>(glz::to_tie(action)))),
         ...);
    }(std::make_index_sequence<memberCount>{});
}

}  // namespace detail

/// @brief Returns `true` when every required `morph::units::Quantity` member
///        of @p action is engaged (has a value).
///
/// Required means: not a `std::optional<...>` member and not listed in
/// `A::optionalFields`. Intended as the body of the action's `validate()`.
template <typename A>
[[nodiscard]] constexpr bool allRequiredEngaged(const A& action) noexcept {
    bool allEngaged = true;
    detail::forEachNamedMember(action, [&]<std::size_t I>(std::string_view name, const auto& member) {
        using Member = std::remove_cvref_t<decltype(member)>;
        if constexpr (units::is_quantity_v<Member>) {
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
///   - `x-decimalPlaces` on every `Quantity` property (the unit's default).
///
/// On any internal failure the unmerged glaze schema (or an empty string if
/// even that failed) is returned rather than throwing — schema generation
/// is a description facility, never worth crashing a server over.
template <typename A>
[[nodiscard]] std::string schemaJson() {
    auto rawSchema = glz::write_json_schema<A>();
    if (!rawSchema) {
        return {};
    }

    // u64 number mode: the schema carries int64/uint64 bounds in $defs, which
    // the default double-only DOM would silently round.
    glz::generic_u64 dom{};
    if (glz::read_json(dom, *rawSchema)) {
        return *rawSchema;
    }

    glz::generic_u64::array_t requiredNames{};
    A probe{};
    detail::forEachNamedMember(probe, [&]<std::size_t I>(std::string_view name, const auto& member) {
        using Member = std::remove_cvref_t<decltype(member)>;
        static_cast<void>(member);
        const bool isOptional = detail::isStdOptional<Member> || detail::declaredOptional<A>(name);
        if (!isOptional) {
            requiredNames.emplace_back(std::string{name});
        }
        auto& property = dom["properties"][std::string{name}];
        property["x-order"] = std::uint64_t{I};
        if constexpr (units::is_quantity_v<Member>) {
            property["x-decimalPlaces"] = std::uint64_t{Member::unitMeta().defaultDecimals};
        }
    });
    if (!requiredNames.empty()) {
        dom["required"] = std::move(requiredNames);
    }

    auto merged = glz::write_json(dom);
    return merged ? *merged : *rawSchema;
}

}  // namespace morph::forms

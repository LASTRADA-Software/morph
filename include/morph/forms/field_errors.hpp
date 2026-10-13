// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

#include "forms.hpp"

namespace morph::forms {

/// @brief The message `fieldErrors` gives a required field with no value.
inline constexpr std::string_view kFieldRequired = "required";
/// @brief The message `fieldErrors` gives a `Quantity` outside its unit's declared bounds.
inline constexpr std::string_view kFieldOutOfRange = "out of range";
/// @brief The message `fieldErrors` gives a value violating its `fieldMetadata` `minimum`, `maximum` or `multipleOf`.
inline constexpr std::string_view kFieldOutOfBounds = "out of bounds";

namespace detail::field_errors {

/// Whether a member is a required empty-capable field with no value.
template <typename A, typename Member>
[[nodiscard]] bool missing(A const& action, std::string_view name, Member const& member) {
    if constexpr (EmptyCapableField<Member>) {
        bool const isComputed =
            detail::isComputedDestinationMember(action, static_cast<void const*>(std::addressof(member)));
        return !detail::declaredOptional<A>(name) && !isComputed && !member.hasValue();
    } else {
        static_cast<void>(action);
        static_cast<void>(name);
        static_cast<void>(member);
        return false;
    }
}

/// Whether a member is a `Quantity` outside its unit's declared bounds.
template <typename Member>
[[nodiscard]] bool outOfRange(Member const& member) {
    if constexpr (units::isQuantity<Member>) {
        return !member.withinDeclaredBounds();
    } else {
        static_cast<void>(member);
        return false;
    }
}

/// Whether a member's value violates the bounds its `fieldMetadata` entry declares; an unengaged one never does.
template <typename A, typename Member>
[[nodiscard]] bool outOfBounds(std::string_view name, Member const& member) {
    if constexpr (detail::HasFieldMetadata<A>) {
        FieldMeta const* const meta = detail::findFieldMeta<A>(name);
        if (meta == nullptr || !detail::declaresAnyBound(*meta)) {
            return false;
        }
        if constexpr (units::isQuantity<Member>) {
            return member.hasValue() && !detail::satisfiesDeclaredBounds(*meta, *member);
        } else if constexpr (std::same_as<Member, ::morph::math::Rational>) {
            return !detail::satisfiesDeclaredBounds(*meta, member);
        } else if constexpr (detail::BoundCheckableInteger<Member>) {
            return !detail::satisfiesDeclaredBounds(
                *meta, ::morph::math::Rational{static_cast<std::int64_t>(member), ::morph::math::DecimalPlaces{0}});
        } else {
            return false;
        }
    } else {
        static_cast<void>(name);
        static_cast<void>(member);
        return false;
    }
}

}  // namespace detail::field_errors

/// @brief The per-field verdicts of the checks `allRequiredEngaged`, `checkQuantityBounds` and
///        `allFieldBoundsSatisfied` make, keyed by field name.
///
/// The same three checks, made field by field instead of answering one boolean, so a caller can attach each failure
/// to the element that shows the field. A field fails at most one check, reported in this order: `required` (a
/// required empty-capable field with no value), `out of range` (a `Quantity` outside its unit's bounds), `out of
/// bounds` (a declared `minimum`, `maximum` or `multipleOf` violated). The walk is as flat as theirs: only the
/// action's own top-level members.
///
/// The action's own `validate()` is not consulted: it answers for the whole action, not for a field.
/// @tparam A     Action type (a reflectable aggregate).
/// @param action The action, after `recomputeAll`, so a computed field is judged on the value it will have.
/// @return Field name to message; empty when every check passes.
template <typename A>
[[nodiscard]] std::map<std::string, std::string, std::less<>> fieldErrors(A const& action) {
    using Plain = std::remove_cvref_t<A>;
    std::map<std::string, std::string, std::less<>> errors;
    if constexpr (glz::reflectable<Plain> || glz::glaze_object_t<Plain>) {
        detail::forEachNamedMember(action, [&]<std::size_t I>(std::string_view name, auto const& member) {
            if (detail::field_errors::missing<Plain>(action, name, member)) {
                errors.emplace(name, kFieldRequired);
            } else if (detail::field_errors::outOfRange(member)) {
                errors.emplace(name, kFieldOutOfRange);
            } else if (detail::field_errors::outOfBounds<Plain>(name, member)) {
                errors.emplace(name, kFieldOutOfBounds);
            }
        });
    } else {
        static_cast<void>(action);
    }
    return errors;
}

}  // namespace morph::forms

// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file detail/reflected_member.hpp
/// @brief The value glaze reads and writes under a reflected key.

#include <cstddef>
#include <glaze/glaze.hpp>
#include <type_traits>

namespace morph::detail {

/// @brief The value glaze reads and writes under `glz::reflect<T>::keys[I]`.
///
/// `glz::reflect<T>` follows a `glz::meta<T>` when one exists, and that meta
/// may omit, rename, reorder or compute members. `glz::to_tie` always follows
/// the aggregate's declaration order. Indexing one with the other's `I` hands
/// one member's value to another member's name — a hidden member offered under
/// a public key, or a key bound to a value of a different type. This reads the
/// value from the same source the key comes from: the declaration-order tie for
/// a type glaze reflects by itself, the meta's `I`-th value otherwise.
///
/// For a member of the object, the result is a reference to it, `const` when
/// @p object is. A meta entry that computes its value (a lambda, a glaze
/// wrapper such as `glz::quoted_num`) yields whatever glaze's `get_member`
/// yields for it, which may be a value rather than a reference.
///
/// @tparam I Index of the key in `glz::reflect<std::remove_cvref_t<T>>::keys`.
/// @tparam T The object's type, possibly `const`-qualified; a type glaze
///           reflects by itself or one with a `glz::meta` object.
/// @param object The object to read from; it must outlive the returned
///               reference.
/// @return The value glaze reads and writes under key `I` of @p object.
template <std::size_t I, typename T>
[[nodiscard]] constexpr decltype(auto) reflectedMember(T& object) {
    using Plain = std::remove_cv_t<T>;
    if constexpr (glz::reflectable<Plain>) {
        auto tie = glz::to_tie(object);
        return glz::get_member(object, glz::get<I>(tie));
    } else {
        return glz::get_member(object, glz::get<I>(glz::reflect<Plain>::values));
    }
}

}  // namespace morph::detail

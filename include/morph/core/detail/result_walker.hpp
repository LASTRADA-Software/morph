// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file core/detail/result_walker.hpp
/// @brief Where an action's result holds which reflected types.
///
/// Every action registration walks its result type once and stores, under the
/// action id, each reflected aggregate the result can hold and the JSON path to
/// it. The walk needs no record to be registered: it reports every aggregate,
/// and matching those types against the record registry happens when that
/// registry is first read.

#include <algorithm>
#include <cstddef>
#include <functional>
#include <glaze/glaze.hpp>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <vector>

#include "../../detail/reflected_member.hpp"

namespace morph::model::detail {

/// @brief One place an action's result holds a reflected aggregate.
struct ResultSlot {
    /// The aggregate's C++ type.
    std::type_index type;
    /// The JSON path to it: member names joined with `.`, empty for the result itself.
    /// A `std::vector`, `std::optional` or `std::map` value adds no segment.
    std::string path;
};

/// @brief Every action's result slots, by action id.
using ActionResultSlotMap = std::map<std::string, std::vector<ResultSlot>, std::less<>>;

namespace result_walker {

/// @brief The value type a container the walker looks through holds; `void` for any other type.
/// @tparam T The type being walked.
template <typename T>
struct Element {
    /// The contained value type.
    using type = void;
};
template <typename T, typename Alloc>
struct Element<std::vector<T, Alloc>> {
    /// The contained value type.
    using type = T;
};
template <typename T>
struct Element<std::optional<T>> {
    /// The contained value type.
    using type = T;
};
template <typename K, typename V, typename Compare, typename Alloc>
struct Element<std::map<K, V, Compare, Alloc>> {
    /// The contained value type.
    using type = V;
};

/// @brief Whether a type's name places it in an anonymous namespace.
///
/// Glaze reads a pure-reflection type's member names through an
/// external-linkage object, which a type without linkage cannot have, so such a
/// type cannot be reflected at all.
/// @tparam T A class type.
/// @return True when the type, or a template argument of it, has no linkage.
template <typename T>
consteval bool hasAnonymousName() {
    constexpr std::string_view name = glz::type_name<T>;
    return name.contains("(anonymous namespace)") || name.contains("{anonymous}") ||
           name.contains("`anonymous namespace'");
}

/// @brief A type whose members the walker visits: one glaze writes as an object, member by member.
///
/// A `glz::meta` that replaces the codec (`custom_read`/`custom_write`, or a
/// `value` mapping) leaves the JSON shape unknown, so such a type is skipped. A
/// `glz::meta` object mapping, renamed members included, is visited.
template <typename T>
concept Object = std::is_class_v<T> && !glz::custom_read<T> && !glz::custom_write<T> &&
                 (glz::glaze_object_t<T> || (glz::reflectable<T> && !hasAnonymousName<T>()));

/// @brief Whether walking a type can yield a slot, so members that cannot are not walked at all.
/// @tparam T The type being walked.
/// @return True for an `Object`, or a container the walker looks through whose value type can.
template <typename T>
consteval bool mayHoldSlots() {
    if constexpr (!std::is_void_v<typename Element<T>::type>) {
        return mayHoldSlots<std::remove_cvref_t<typename Element<T>::type>>();
    } else {
        return Object<T>;
    }
}

/// @brief The state of one walk.
struct Walk {
    /// The slots found so far.
    std::vector<ResultSlot> found;
    /// The aggregates on the current path, which end the walk where they recur.
    std::vector<std::type_index> onPath;
};

template <typename T>
void visit(Walk& walk, std::string const& path);

/// @brief Visits each member of an `Object` that can yield a slot.
/// @tparam T The aggregate.
/// @tparam I Its reflected member indices.
/// @param walk The walk's state.
/// @param path The aggregate's own path.
template <typename T, std::size_t... I>
void visitMembers(Walk& walk, std::string const& path, std::index_sequence<I...> /*indices*/) {
    auto const member = [&]<std::size_t Index>() {
        using Member = std::remove_cvref_t<decltype(morph::detail::reflectedMember<Index>(std::declval<T&>()))>;
        if constexpr (mayHoldSlots<Member>()) {
            constexpr std::string_view key = glz::reflect<T>::keys[Index];
            visit<Member>(walk, path.empty() ? std::string{key} : path + "." + std::string{key});
        }
    };
    (member.template operator()<I>(), ...);
}

/// @brief Visits a value of type @p T at @p path.
/// @tparam T The type being walked.
/// @param walk The walk's state.
/// @param path Where the value sits in the result.
template <typename T>
void visit(Walk& walk, std::string const& path) {
    if constexpr (!std::is_void_v<typename Element<T>::type>) {
        visit<std::remove_cvref_t<typename Element<T>::type>>(walk, path);
    } else if constexpr (Object<T>) {
        std::type_index const self{typeid(T)};
        if (std::ranges::find(walk.onPath, self) != walk.onPath.end()) {
            return;
        }
        walk.found.push_back(ResultSlot{.type = self, .path = path});
        walk.onPath.push_back(self);
        visitMembers<T>(walk, path, std::make_index_sequence<glz::reflect<T>::size>{});
        walk.onPath.pop_back();
    }
}

}  // namespace result_walker

/// @brief Every reflected aggregate a value of type @p R can hold, and the path to each.
///
/// The result itself, a `std::vector`, `std::optional` or `std::map` value of a
/// visited type, and the members of a reflected aggregate are visited,
/// depth-first. Member names are glaze's reflected keys, which are the names
/// the codec writes. `void`, scalars, strings, types whose `glz::meta` replaces
/// the codec, and pure-reflection types in an anonymous namespace are skipped.
/// The walk stops at a type already on its path, so recursive types end.
/// @tparam R The result type, `void` included.
/// @return The slots in visiting order.
template <typename R>
[[nodiscard]] std::vector<ResultSlot> walkResult() {
    result_walker::Walk walk;
    if constexpr (!std::is_void_v<R>) {
        if constexpr (result_walker::mayHoldSlots<std::remove_cvref_t<R>>()) {
            result_walker::visit<std::remove_cvref_t<R>>(walk, std::string{});
        }
    }
    return std::move(walk.found);
}

/// @brief The process-level map of action result slots, filled by static registrars.
/// @return The mutable map.
inline ActionResultSlotMap& mutableActionResultSlots() {
    static ActionResultSlotMap entries;
    return entries;
}

/// @brief Every registered action's result slots, by action id.
/// @return The map, unmatched against records.
inline ActionResultSlotMap const& actionResultSlots() { return mutableActionResultSlots(); }

}  // namespace morph::model::detail

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
#include <concepts>
#include <cstddef>
#include <functional>
#include <glaze/glaze.hpp>
#include <map>
#include <morph/export.hpp>
#include <optional>
#include <ranges>
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
    /// True when the result holds the type with no path a key can be read at: the
    /// action holds the record, but no reader finds its rows in the value.
    bool opaque = false;
};

/// @brief The members of one result holder that open or make partial what holds the result, by path.
///
/// A holder is a visited aggregate whose type declares `openMembers()` or `partialMember()`. Only members its
/// reflection has are listed.
struct HolderMembers {
    /// The holder's own path in the result.
    std::string path;
    /// The members that open an entry while any is set, by name.
    std::vector<std::string> open;
    /// The member that makes an entry partial while set, by name; empty when the holder names none.
    std::string partial;
};

/// @brief What one action id's registration recorded about its result.
///
/// The action type is kept beside the slots because an action id is unique per
/// model, not per process: two action types may register under one id. The
/// first registration's slots are kept, and a later registration with a
/// different action type is listed in `actionResultConflicts()` instead.
struct ActionResult {
    /// The action type that registered under the id first.
    std::type_index action;
    /// The places that action's result holds a reflected aggregate.
    std::vector<ResultSlot> resultSlots;
    /// The holders in that action's result that declare open or partial members.
    std::vector<HolderMembers> holders;
};

/// @brief Every action's result, by action id.
using ActionResultSlotMap = std::map<std::string, ActionResult, std::less<>>;

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
           name.contains("`anonymous namespace'") || name.contains("`anonymous-namespace'");
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

/// @brief A type that holds a record's rows at one of its members, under another type than the rows': it names
///        the record as `heldRecord` and the member as `heldMember()`.
template <typename T>
concept HoldsRecord = requires {
    typename T::heldRecord;
    { T::heldMember() } -> std::convertible_to<std::string_view>;
};

/// @brief A type that names the members of itself which open what holds it: `openMembers()`, a range of names.
template <typename T>
concept DeclaresOpenMembers =
    requires { T::openMembers(); } && std::ranges::range<decltype(T::openMembers())> &&
    std::convertible_to<std::ranges::range_value_t<decltype(T::openMembers())>, std::string_view>;

/// @brief A type that names the member of itself which makes what holds it partial: `partialMember()`.
template <typename T>
concept DeclaresPartialMember = requires {
    { T::partialMember() } -> std::convertible_to<std::string_view>;
};

/// @brief Whether the reflection of @p T has a member named @p name.
/// @tparam T An `Object`.
/// @param name The member's name.
/// @return `true` when one of glaze's keys for @p T is @p name.
template <typename T>
[[nodiscard]] bool reflectsMember(std::string_view name) {
    constexpr auto keys = glz::reflect<T>::keys;
    return std::ranges::find(keys, name) != keys.end();
}

/// @brief The state of one walk.
struct Walk {
    /// The slots found so far.
    std::vector<ResultSlot> found;
    /// The holders that declare open or partial members, found so far.
    std::vector<HolderMembers> holders;
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
    // Unused for an aggregate with no members: the fold below is then empty.
    [[maybe_unused]] auto const member = [&]<std::size_t Index>() {
        using Member = std::remove_cvref_t<decltype(morph::detail::reflectedMember<Index>(std::declval<T&>()))>;
        if constexpr (mayHoldSlots<Member>()) {
            constexpr std::string_view key = glz::reflect<T>::keys[Index];
            result_walker::visit<Member>(walk, path.empty() ? std::string{key} : path + "." + std::string{key});
        }
    };
    (member.template operator()<I>(), ...);
}

/// @brief Visits a value of type @p T at @p path.
/// @tparam T The type being walked.
/// @param walk The walk's state.
/// @param path Where the value sits in the result.
template <typename T>
// NOLINTNEXTLINE(readability-function-cognitive-complexity): `visit` dispatches on the shape of T (optional, variant, map, sequence, struct) and every branch extends the same walk path and error state
void visit(Walk& walk, std::string const& path) {
    if constexpr (!std::is_void_v<typename Element<T>::type>) {
        result_walker::visit<std::remove_cvref_t<typename Element<T>::type>>(walk, path);
    } else if constexpr (Object<T>) {
        std::type_index const self{typeid(T)};
        if (std::ranges::find(walk.onPath, self) != walk.onPath.end()) {
            return;
        }
        walk.found.push_back(ResultSlot{.type = self, .path = path});
        if constexpr (DeclaresOpenMembers<T> || DeclaresPartialMember<T>) {
            HolderMembers holder{.path = path, .open = {}, .partial = {}};
            if constexpr (DeclaresOpenMembers<T>) {
                for (std::string_view const member : T::openMembers()) {
                    if (reflectsMember<T>(member)) {
                        holder.open.emplace_back(member);
                    }
                }
            }
            if constexpr (DeclaresPartialMember<T>) {
                if (reflectsMember<T>(T::partialMember())) {
                    holder.partial = std::string{T::partialMember()};
                }
            }
            walk.holders.push_back(std::move(holder));
        }
        if constexpr (HoldsRecord<T>) {
            std::string const member{T::heldMember()};
            walk.found.push_back(ResultSlot{.type = std::type_index{typeid(typename T::heldRecord)},
                                            .path = path.empty() ? member : path + "." + member});
        }
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
/// the codec writes. A type with a `heldRecord` and a `heldMember()` also
/// reports its `heldRecord` at that member, beside its own slot. `void`, scalars, strings, types whose `glz::meta`
/// replaces the codec, and pure-reflection types in an anonymous namespace are skipped. The walk stops at a type
/// already on its path, so recursive types end.
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

/// @brief The holders a value of type @p R can hold that declare open or partial members, with each one's path.
///
/// The same walk as `walkResult`: a visited aggregate whose type has `openMembers()` or `partialMember()` is a
/// holder, and a member it names that its reflection lacks is left out.
/// @tparam R The result type, `void` included.
/// @return The holders in visiting order.
template <typename R>
[[nodiscard]] std::vector<HolderMembers> walkHolders() {
    result_walker::Walk walk;
    if constexpr (!std::is_void_v<R>) {
        if constexpr (result_walker::mayHoldSlots<std::remove_cvref_t<R>>()) {
            result_walker::visit<std::remove_cvref_t<R>>(walk, std::string{});
        }
    }
    return std::move(walk.holders);
}

/// @brief The process-level map of action result slots, filled by static registrars.
/// @return The mutable map.
MORPH_API ActionResultSlotMap& mutableActionResultSlots();

/// @brief Every registered action's result slots, by action id.
/// @return The map, unmatched against records.
inline ActionResultSlotMap const& actionResultSlots() { return mutableActionResultSlots(); }

/// @brief The process-level list of action ids registered by more than one action type.
/// @return The mutable list.
MORPH_API std::vector<std::string>& mutableActionResultConflicts();

/// @brief The action ids that a second, different action type registered under.
///
/// Each id appears once. Its entry in `actionResultSlots()` holds the first
/// type's slots; matching refuses the id, since which result a cached query
/// holds would depend on link order.
/// @return The ids in the order the conflicts were found.
inline std::vector<std::string> const& actionResultConflicts() { return mutableActionResultConflicts(); }

}  // namespace morph::model::detail

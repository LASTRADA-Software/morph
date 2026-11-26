// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/derive.hpp
/// @brief Derived values: members the model sets from the domain's data (`derived()`, `x-derived`), the derive action
///        a form asks for them through (`DeriveOf`, `Derived`), the server's clearing of what a client sent for them
///        (`clearDerived`), and the round types a `FormSession` asks for derived and computed values with.
///
/// Specified in `docs/spec/table/editing.md`, "Derived values", and `docs/spec/forms/engine.md`, "Derived values".

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <glaze/glaze.hpp>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "../core/completion.hpp"
#include "../reactive/scheduler.hpp"
#include "forms.hpp"

namespace morph::forms {

/// @brief The input of a derive action: the form's body as far as it encodes.
///
/// A derive action is declared as a type deriving from it, `struct DeriveSieve : forms::DeriveOf<RecordSieve> {};`,
/// and registered as any action of the form's model, `Loggable::No` and with no effects, so a derive call is a read.
/// Its JSON is `{"body": …}`; the in-class glaze meta is inherited by the deriving type.
/// @tparam Action The form's action.
template <typename Action>
struct DeriveOf {
    /// @brief The form's body; a member that was blank or did not encode is at its default.
    Action body;

    /// @brief glaze's meta: the one member `body`.
    // NOLINTNEXTLINE(readability-identifier-naming): glaze looks the meta type up by this name.
    struct glaze {
        /// @brief The described type.
        using T = DeriveOf;
        /// @brief `{"body": body}`.
        static constexpr auto value = glz::object("body", &T::body);
    };
};

/// @brief The values of derived members, by path into the body that was sent: what a derive action returns. Its JSON
///        is `{"values": [{"path": "specimens[3].density", "value": …}, …]}`.
struct Derived {
    /// @brief One derived member's value.
    struct Value {
        /// @brief The member's path into the body sent: `name`, or `rows[i].member` for a row's member.
        std::string path;
        /// @brief The member's wire value.
        glz::raw_json value;
    };
    /// @brief The values, each path once.
    std::vector<Value> values;
};

/// @brief Builds the `FieldMeta` of a derived member: `describe<MemberPtr>(label, help).withDerived()`.
/// @tparam MemberPtr Pointer to the member, e.g. `&Specimen::limit`.
/// @param label Display label; empty infers one from the member name.
/// @param help  Help text; empty omits `description`.
/// @return The descriptor, `derived` set.
template <auto MemberPtr>
[[nodiscard]] FieldMeta derived(std::string_view label = {}, std::string_view help = {}) noexcept {
    return describe<MemberPtr>(label, help).withDerived();
}

namespace detail {

/// @brief Value-initialises the members of @p action its type's `fieldMetadata` marks derived, not descending into
///        nested aggregates.
/// @tparam A The action or row type, one with field metadata.
/// @param action The value cleared in place.
template <typename A>
void clearOwnDerived(A& action) {
    using Plain = std::remove_cvref_t<A>;
    constexpr auto memberCount = reflectedCount<Plain>();
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        [[maybe_unused]] auto clear = [&]<std::size_t Idx>() {
            // A `glz::meta` entry that computes its value yields a value: there is no stored member to clear.
            [[maybe_unused]] decltype(auto) member = morph::detail::reflectedMember<Idx>(action);
            using Member = std::remove_cvref_t<decltype(member)>;
            // Assignability of the reference itself: a const member, or a const accessor, is never cleared.
            if constexpr (std::is_lvalue_reference_v<decltype(member)> && std::is_default_constructible_v<Member> &&
                          std::is_assignable_v<decltype(member), Member>) {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): Idx < size.
                if (detail::declaredDerived<Plain>(glz::reflect<Plain>::keys[Idx])) {
                    member = Member{};
                }
            }
        };
        (clear.template operator()<I>(), ...);
    }(std::make_index_sequence<memberCount>{});
}

}  // namespace detail

/// @brief Value-initialises every derived member of @p action (`FieldMeta::derived` in its type's `fieldMetadata`),
///        and of every nested aggregate of it (each element of a `std::vector` member, a nested struct member, either
///        inside an engaged `std::optional`, recursively: `detail::forEachNested`): what every execution path runs
///        before the model, so a derived value a caller sent is never the one kept.
/// @tparam A The action or row type.
/// @param action The value cleared in place.
template <typename A>
void clearDerived(A& action) {
    using Plain = std::remove_cvref_t<A>;
    if constexpr (detail::WrittenAsMember<Plain>) {
        clearDerived(action.*(glz::meta<Plain>::value));
    } else if constexpr (glz::reflectable<Plain> || glz::glaze_object_t<Plain>) {
        // Only a type with field metadata can mark a member derived, and the member walk reads the names, which needs
        // the type to have linkage. The check stays out here, not in the walk's lambda: MSVC instantiates a discarded
        // branch there whose condition does not depend on the lambda's own parameters.
        if constexpr (detail::HasFieldMetadata<Plain>) {
            detail::clearOwnDerived(action);
        }
        detail::forEachNested(action, [](auto& nested) { clearDerived(nested); });
    } else {
        static_cast<void>(action);
    }
}

/// @brief Where a form's derived values stand.
enum class DeriveState : std::uint8_t {
    Current,  ///< No round is in flight, and the last one succeeded (or none was made).
    Running,  ///< A round is in flight.
    Failed,   ///< The last round's evaluation or derive call failed; the values are the last ones loaded.
};

/// @brief How a `FormSession` asks for its derived and computed values.
struct DeriveOptions {
    /// @brief Evaluates the action over a body (`ActionDispatcher::evaluate`): the recomputed body in, as JSON. Empty
    ///        makes no evaluation.
    std::function<async::Completion<std::string>(std::string bodyJson)> evaluate;
    /// @brief Calls the form's derive action with `{"body": <body>}`: a `Derived` reply. Empty makes no derive call.
    std::function<async::Completion<std::string>(std::string deriveBodyJson)> derive;
    /// @brief The timers the debounce runs on; null starts a round at each commit.
    reactive::Scheduler* scheduler = nullptr;
    /// @brief How long after a commit, with no further commit, a round starts.
    std::chrono::milliseconds debounce{150};
    /// @brief Told the paths a derive reply named that are no derived member, which the round dropped; empty tells
    ///        no one.
    std::function<void(std::vector<std::string> const&)> refused;
};

/// @brief One round as it was sent: its sequence, its body, and the rows each collection held then.
struct DeriveRound {
    /// @brief The round's sequence; a later round has a greater one.
    std::uint64_t sequence = 0;
    /// @brief The body sent (`FormSession::partialBody`).
    std::string body;
    /// @brief The row keys of each collection, by the collection's path, in the order the body wrote them.
    std::map<std::string, std::vector<std::uint64_t>, std::less<>> rowKeys;
};

/// @brief What a load wrote, and what it refused.
struct DeriveLoad {
    /// @brief How many members it wrote.
    std::size_t loaded = 0;
    /// @brief The paths it refused: a path that names no member of the kind the reply writes.
    std::vector<std::string> refused;
};

}  // namespace morph::forms

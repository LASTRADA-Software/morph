// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <charconv>
#include <concepts>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

/// @file
/// Primary keys for model instances.
///
/// A model type declares itself *keyed* by exposing a nested `PrimaryKey` type
/// alias; the alias is detected structurally (a `requires`-expression), never by
/// inheritance or a marker base — the same detection style
/// `morph::views::ViewTraits` uses for a view's `kind`/`query` members. A model
/// without the alias is unkeyed and behaves exactly as it always has.
///
/// Actions say *which of their fields carries* that key via `BRIDGE_KEY_FROM`,
/// or — for an action that creates the entity rather than naming it — which
/// field of their *result* establishes it, via `BRIDGE_KEY_FROM_RESULT`. Actions
/// with neither declaration are keyless and run against whichever instance the
/// handler is already attached to; that is the common case.
///
/// Keys travel the wire as strings (`wire::Envelope::primary`) regardless of
/// their C++ type, so the directory in `RemoteServer` needs exactly one map type
/// rather than one per key type. See docs/planned/shared_model_instances.md.

namespace morph::model {

/// @brief Key types a model may declare as its `PrimaryKey`.
///
/// Restricted to integral types and `std::string` because a key must round-trip
/// losslessly through the wire's string encoding and be usable as a map key.
/// `bool` is excluded: it carries one bit of identity, which is never a
/// meaningful primary key and is far more likely to be a mistake.
template <typename K>
concept ModelKey =
    (std::integral<K> && !std::same_as<std::remove_cv_t<K>, bool>) || std::same_as<std::remove_cv_t<K>, std::string>;

/// @brief Satisfied by model types that declare a `PrimaryKey` alias.
///
/// Declaring the alias is what opts a model into keyed, shareable instances.
template <typename M>
concept KeyedModel = requires { typename M::PrimaryKey; } && ModelKey<typename M::PrimaryKey>;

/// @brief The declared key type of a keyed model.
/// @tparam M Keyed model type.
template <KeyedModel M>
using PrimaryKeyOf = typename M::PrimaryKey;

/// @brief Encodes a primary key as its canonical wire string.
///
/// Integral keys are decimal; `std::string` keys pass through unchanged. The
/// encoding is total — every valid key has exactly one representation — so two
/// clients naming the same key always land on the same directory entry.
/// @tparam K Key type satisfying `ModelKey`.
/// @param key Key value to encode.
/// @return The canonical string form of @p key.
template <ModelKey K>
[[nodiscard]] std::string keyToString(const K& key) {
    if constexpr (std::same_as<std::remove_cv_t<K>, std::string>) {
        return key;
    } else {
        return std::to_string(key);
    }
}

/// @brief Decodes a primary key from its canonical wire string.
///
/// @tparam K Key type satisfying `ModelKey`.
/// @param text Canonical string form, as produced by `keyToString`.
/// @return The decoded key.
/// @throws std::runtime_error if @p text is not a valid encoding of a `K`
///         (non-numeric text, trailing garbage, or a value out of range). A key
///         that cannot be decoded is a protocol error, not a value to clamp:
///         silently yielding 0 would route the caller to the wrong instance.
template <ModelKey K>
[[nodiscard]] K keyFromString(std::string_view text) {
    if constexpr (std::same_as<std::remove_cv_t<K>, std::string>) {
        return std::string{text};
    } else {
        K value{};
        const auto* const first = text.data();
        const auto* const last = first + text.size();
        auto [ptr, errc] = std::from_chars(first, last, value);
        if (errc != std::errc{} || ptr != last) {
            throw std::runtime_error("invalid primary key encoding: '" + std::string{text} + "'");
        }
        return value;
    }
}

/// @brief Declares where an action's model key comes from.
///
/// The primary template is the *keyless* case, which is the default and the
/// common one: such an action says nothing about identity and runs against
/// whichever instance its handler is already attached to. Specialise via
/// `BRIDGE_KEY_FROM` or `BRIDGE_KEY_FROM_RESULT` rather than by hand.
/// @tparam Action Concrete action type.
template <typename Action>
struct ActionKeyTraits {
    /// @brief Whether this action carries or establishes its model's key.
    static constexpr bool hasKey = false;

    /// @brief Whether the key comes from the action's result rather than its payload.
    static constexpr bool fromResult = false;
};

namespace detail {

/// @brief Satisfied by actions whose key is carried in the action payload.
template <typename A>
concept PayloadKeyed = ActionKeyTraits<A>::hasKey && !ActionKeyTraits<A>::fromResult;

/// @brief Satisfied by actions whose key is established by the action's result.
template <typename A>
concept ResultKeyed = ActionKeyTraits<A>::hasKey && ActionKeyTraits<A>::fromResult;

}  // namespace detail

}  // namespace morph::model

/// @brief Declares that action `A` carries its model's primary key in `MEMBER`.
///
/// `MEMBER` is a pointer-to-data-member of `A` (e.g. `&GetAccount::id`) whose
/// type satisfies `morph::model::ModelKey`. Executing such an action on a
/// shareable handler attaches (or re-points) that handler to the instance
/// holding the named key, creating it if no instance holds it yet.
///
/// Must appear at global scope, in exactly one translation unit, like the other
/// `BRIDGE_REGISTER_*` macros.
#define BRIDGE_KEY_FROM(A, MEMBER)                                                                    \
    template <>                                                                                       \
    struct morph::model::ActionKeyTraits<A> {                                                         \
        static constexpr bool hasKey = true;                                                          \
        static constexpr bool fromResult = false;                                                     \
        static std::string key(const A& action) { return morph::model::keyToString(action.*MEMBER); } \
    }

/// @brief Declares that action `A`'s *result* establishes its model's primary key.
///
/// For actions that create the entity rather than name it: the key is not in the
/// request, it is generated and returned, exactly as a database insert returns
/// its generated primary key. `MEMBER` is a pointer-to-data-member of `A`'s
/// result type (e.g. `&AccountInfo::id`).
///
/// Must appear at global scope, in exactly one translation unit.
#define BRIDGE_KEY_FROM_RESULT(A, MEMBER)                     \
    template <>                                               \
    struct morph::model::ActionKeyTraits<A> {                 \
        static constexpr bool hasKey = true;                  \
        static constexpr bool fromResult = true;              \
        template <typename R>                                 \
        static std::string keyOfResult(const R& result) {     \
            return morph::model::keyToString(result.*MEMBER); \
        }                                                     \
    }

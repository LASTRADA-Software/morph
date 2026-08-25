// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <charconv>
#include <concepts>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

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

/// @brief Key types that are themselves a wire-encodable scalar.
///
/// Restricted to integral types and `std::string` because a key must round-trip
/// losslessly through the wire's string encoding and be usable as a map key.
/// `bool` is excluded: it carries one bit of identity, which is never a
/// meaningful primary key and is far more likely to be a mistake.
template <typename K>
concept RawModelKey =
    (std::integral<K> && !std::same_as<std::remove_cv_t<K>, bool>) || std::same_as<std::remove_cv_t<K>, std::string>;

/// @brief Key types that *wrap* a `RawModelKey` — the ladder's strong ids.
///
/// `examples/IMPLEMENTATION.md` rule 3 requires entity identity to be a
/// per-entity strong id type (`struct PasteId`, `struct OptionId`) exposing
/// `hasValue()`, so that it joins the forms palette as an empty-capable field.
/// Such a type is not integral and is not `std::string`, so before this it
/// satisfied no arm of `ModelKey` at all — and a rung obeying rule 3 could not
/// use `BRIDGE_MODEL_KEY`/`BRIDGE_KEY_FROM`, the very macros the framework
/// provides for keying. Three rungs hand-wrote the traits instead, each
/// re-stating the `*id` unwrapping convention the macro exists to hide.
///
/// The shape is deduced structurally rather than declared, matching how a
/// keyed model is detected: any type with `hasValue()` and an `operator*`
/// yielding a `RawModelKey`, and constructible back from that raw value so the
/// key round-trips from the wire. Both ladder spellings qualify — an
/// `std::optional`-backed id whose `operator*` returns a reference, and a
/// sentinel-backed one (`0` means empty) whose `operator*` returns by value.
template <typename K>
concept WrappedModelKey = !RawModelKey<K> && requires(const K& key) {
    { key.hasValue() } -> std::convertible_to<bool>;
    { *key };
    requires RawModelKey<std::remove_cvref_t<decltype(*key)>>;
    requires requires(std::remove_cvref_t<decltype(*key)> raw) { K{std::move(raw)}; };
};

/// @brief Key types a model may declare as its `PrimaryKey`.
///
/// Either a wire-encodable scalar (`RawModelKey`) or a strong id wrapping one
/// (`WrappedModelKey`).
template <typename K>
concept ModelKey = RawModelKey<K> || WrappedModelKey<K>;

namespace detail {

/// @brief The scalar a `WrappedModelKey` wraps.
/// @tparam K The wrapping strong id type.
template <WrappedModelKey K>
using UnwrappedKeyOf = std::remove_cvref_t<decltype(*std::declval<const K&>())>;

}  // namespace detail

/// @brief The key type of a model, when one has been declared *for* it.
///
/// Specialised by `BRIDGE_KEY_FROM`, which deduces the type from the action
/// member it is given — so a model does not have to say anything about keys
/// inside its own class. The primary template is deliberately empty: a model
/// with neither this specialisation nor a nested alias is simply unkeyed.
/// @tparam Model Concrete model type.
template <typename Model>
struct ModelKeyTraits {};

namespace detail {

/// @brief Satisfied by a model that names its own key with a nested alias.
template <typename M>
concept SelfDeclaredKey = requires { typename M::PrimaryKey; } && ModelKey<typename M::PrimaryKey>;

/// @brief Satisfied by a model whose key was deduced from a keyed action.
template <typename M>
concept DeducedKey =
    requires { typename ModelKeyTraits<M>::PrimaryKey; } && ModelKey<typename ModelKeyTraits<M>::PrimaryKey>;

}  // namespace detail

/// @brief Satisfied by model types that have a primary key, however it was named.
///
/// Two ways in, and neither requires touching the model's own class body beyond
/// the first: a nested `PrimaryKey` alias, or a `BRIDGE_KEY_FROM` declaration
/// that deduces the type from the action field carrying it. Following
/// `morph::forms`' standing rule — *infer by default, declare to override* — a
/// nested alias wins when both are present, which is what lets a model whose
/// key type differs from the field's type (an `int` column keyed as a
/// `std::string`, say) state that explicitly.
template <typename M>
concept KeyedModel = detail::SelfDeclaredKey<M> || detail::DeducedKey<M>;

namespace detail {

/// @brief Picks the nested alias when present, else the deduced one.
template <typename M>
struct KeyTypeOf {
    /// @brief The resolved key type.
    using type = ModelKeyTraits<M>::PrimaryKey;
};

template <SelfDeclaredKey M>
struct KeyTypeOf<M> {
    /// @brief The resolved key type — the model's own alias takes precedence.
    using type = M::PrimaryKey;
};

}  // namespace detail

/// @brief The primary key type of a keyed model.
/// @tparam M Keyed model type.
template <KeyedModel M>
using PrimaryKeyOf = detail::KeyTypeOf<M>::type;

/// @brief Encodes a primary key as its canonical wire string.
///
/// Integral keys are decimal; `std::string` keys pass through unchanged; a
/// strong id encodes as whatever it wraps, so it shares a directory entry with
/// the raw key of the same value. The encoding is total for every *engaged*
/// key — each has exactly one representation — so two clients naming the same
/// key always land on the same directory entry.
/// @tparam K Key type satisfying `ModelKey`.
/// @param key Key value to encode.
/// @return The canonical string form of @p key.
/// @throws std::runtime_error if @p key is a strong id with no value. Such an
///         id names no instance, and encoding it as `""` or `"0"` would route
///         every caller holding an unset id to a single shared instance.
template <ModelKey K>
[[nodiscard]] std::string keyToString(const K& key) {
    if constexpr (WrappedModelKey<K>) {
        // An empty strong id names no instance. Unwrapping it is undefined
        // behaviour (`operator*` on a disengaged optional), and encoding it as
        // "" or "0" would silently route every caller holding an unset id to
        // one shared instance -- the worst possible failure, because it looks
        // like it worked. Both call sites in `BridgeHandler` turn a throw here
        // into a rejected `Completion`, so this surfaces as an error the
        // caller can see.
        if (!key.hasValue()) {
            throw std::runtime_error("primary key is empty: a strong id with no value names no model instance");
        }
        return keyToString(*key);
    } else if constexpr (std::same_as<std::remove_cv_t<K>, std::string>) {
        return key;
    } else {
        return std::to_string(key);
    }
}

/// @brief Decodes a primary key from its canonical wire string.
///
/// A strong id is reconstructed by decoding what it wraps and rewrapping it,
/// so `keyFromString<PasteId>(keyToString(id)) == id` for any engaged `id`.
/// @tparam K Key type satisfying `ModelKey`.
/// @param text Canonical string form, as produced by `keyToString`.
/// @return The decoded key.
/// @throws std::runtime_error if @p text is not a valid encoding of a `K`
///         (non-numeric text, trailing garbage, or a value out of range). A key
///         that cannot be decoded is a protocol error, not a value to clamp:
///         silently yielding 0 would route the caller to the wrong instance.
template <ModelKey K>
[[nodiscard]] K keyFromString(std::string_view text) {
    if constexpr (WrappedModelKey<K>) {
        return K{keyFromString<detail::UnwrappedKeyOf<K>>(text)};
    } else if constexpr (std::same_as<std::remove_cv_t<K>, std::string>) {
        return std::string{text};
    } else {
        K value{};
        // from_chars is a [first, last) pointer API; a string_view's data()+size()
        // is the only way to express its end, and is exactly what the standard
        // intends here.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        auto [ptr, errc] = std::from_chars(text.data(), text.data() + text.size(), value);
        const auto* const last = text.data() + text.size();  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
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

/// @brief The type of the data member a pointer-to-member points at.
///
/// Lets `BRIDGE_KEY_FROM` deduce the model's key type from the action field it
/// is handed, so the key type is never written out twice.
template <typename T>
struct MemberType;

template <typename V, typename C>
struct MemberType<V C::*> {
    /// @brief The member's own type.
    using type = V;
};

/// @brief Convenience alias for `MemberType<T>::type`.
template <typename T>
using MemberTypeOf = MemberType<std::remove_cv_t<T>>::type;

/// @brief Satisfied by actions whose key is carried in the action payload.
template <typename A>
concept PayloadKeyed = ActionKeyTraits<A>::hasKey && !ActionKeyTraits<A>::fromResult;

/// @brief Satisfied by actions whose key is established by the action's result.
template <typename A>
concept ResultKeyed = ActionKeyTraits<A>::hasKey && ActionKeyTraits<A>::fromResult;

}  // namespace detail

}  // namespace morph::model

// NOLINTBEGIN(cppcoreguidelines-macro-usage) — declaration macros are the intended public API,
// matching BRIDGE_REGISTER_MODEL/ACTION in registry.hpp: they must emit a template
// specialisation at global scope, which no function template can do.

/// @brief Declares that action `A` is the one that defines model `M`'s primary key.
///
/// One line does both jobs, so the model's own class body needs to say nothing
/// about keys: the key *type* is deduced from `MEMBER`'s type (defining
/// `ModelKeyTraits<M>`), and `A` is recorded as an action that carries it
/// (defining `ActionKeyTraits<A>`).
///
/// Executing such an action on a shareable handler attaches — or re-points —
/// that handler to the instance holding the named key, constructing one only if
/// no instance holds it yet. Every *keyless* action on that handler afterwards
/// lands on the same instance, which is what makes the common case free of
/// ceremony:
///
/// ```cpp
/// BRIDGE_KEY_FROM(AccountModel, LoadAccount, &LoadAccount::id);
///
/// BridgeHandler<AccountModel, AllowShared> first{bridge, gui}, second{bridge, gui};
/// first .execute(LoadAccount{.id = 32});   // constructs the instance for 32
/// second.execute(LoadAccount{.id = 32});   // attaches to it; constructs nothing
/// first .execute(Deposit{.amount = 100});  // keyless -> instance 32
/// second.execute(GetBalance{});            // keyless -> instance 32, sees the 100
/// ```
///
/// `MEMBER` is a pointer-to-data-member of `A` (e.g. `&LoadAccount::id`) whose
/// type satisfies `morph::model::ModelKey`. Must appear at global scope, in
/// exactly one translation unit, like the other `BRIDGE_REGISTER_*` macros.
#define BRIDGE_MODEL_KEY(M, A, MEMBER)                                                                \
    template <>                                                                                       \
    struct morph::model::ActionKeyTraits<A> {                                                         \
        static constexpr bool hasKey = true;                                                          \
        static constexpr bool fromResult = false;                                                     \
        static std::string key(const A& action) { return morph::model::keyToString(action.*MEMBER); } \
    };                                                                                                \
    template <>                                                                                       \
    struct morph::model::ModelKeyTraits<M> {                                                          \
        using PrimaryKey = morph::model::detail::MemberTypeOf<decltype(MEMBER)>;                      \
    }

/// @brief Declares that action `A` also carries its model's primary key in `MEMBER`.
///
/// The companion to `BRIDGE_MODEL_KEY`, for the *other* actions that name the
/// same entity — `CloseAccount{.id = ...}` alongside `GetAccount{.id = ...}`.
/// It records only that `A` carries the key; the key's type has already been
/// established by the model's one `BRIDGE_MODEL_KEY` line, and an explicit
/// specialisation cannot be repeated.
///
/// Must appear at global scope, in exactly one translation unit.
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
#define BRIDGE_MODEL_KEY_FROM_RESULT(M, A, MEMBER)                               \
    template <>                                                                  \
    struct morph::model::ActionKeyTraits<A> {                                    \
        static constexpr bool hasKey = true;                                     \
        static constexpr bool fromResult = true;                                 \
        template <typename R>                                                    \
        static std::string keyOfResult(const R& result) {                        \
            return morph::model::keyToString(result.*MEMBER);                    \
        }                                                                        \
    };                                                                           \
    template <>                                                                  \
    struct morph::model::ModelKeyTraits<M> {                                     \
        using PrimaryKey = morph::model::detail::MemberTypeOf<decltype(MEMBER)>; \
    }

/// @brief Declares that action `A`'s result also establishes its model's key.
///
/// The companion to `BRIDGE_MODEL_KEY_FROM_RESULT`, for a second creating
/// action on a model whose key type is already established.
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

// NOLINTEND(cppcoreguidelines-macro-usage)

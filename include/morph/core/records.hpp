// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cassert>
#include <cstddef>
#include <glaze/glaze.hpp>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <vector>

#include "../detail/reflected_member.hpp"
#include "model_key.hpp"
#include "registry.hpp"

/// @file
/// The registry of record types: the rows a server can push changes about.
///
/// A record is a plain reflected struct identified by a key member. Registration
/// is a static registrar, like an action's, so it is checked by the same
/// registration-phase latch. The registry stores what was declared; cross-entry
/// validation (duplicate names, derivation cycles, keyless records that an effect
/// names) happens when the registry is read for matching, not here.

namespace morph::record {

/// @brief Option for `BRIDGE_REGISTER_RECORD`: the record's key is the named member instead of `id`.
/// @tparam Member Pointer to a data member of the record, e.g. `&Row::code`.
template <auto Member>
struct key {};

/// @brief Option for `BRIDGE_REGISTER_RECORD`: the record has no key.
struct keyless {};

/// @brief Option for `BRIDGE_REGISTER_RECORD`: a change to any of @p S is a change to this record type.
/// @tparam S The source record types.
template <typename... S>
struct derivedFrom {};

/// @brief Option for `BRIDGE_REGISTER_RECORD`: the record's keys must not reach other principals.
struct privateKeys {};

/// @brief What one `BRIDGE_REGISTER_RECORD` declared.
struct RecordInfo {
    /// The wire name of the record type.
    std::string name;
    /// The record's C++ type; records and effects are matched by this identity.
    std::type_index type;
    /// The glaze JSON name of the key member; empty for a keyless record or one without an `id` member.
    std::optional<std::string> keyField;
    /// The source types this record is derived from.
    std::vector<std::type_index> derivedFrom;
    /// True when the record's keys must not reach other principals.
    bool privateKeys = false;
    /// True when the key's raw type is `std::string`; false when it is integral or there is no key.
    bool keyIsString = false;
};

namespace detail {

/// @brief Detects a `key<>` option and exposes its member pointer.
/// @tparam T An option.
template <typename T>
struct KeyOption {
    /// True for a `key<>` option.
    static constexpr bool value = false;
};
template <auto Member>
struct KeyOption<key<Member>> {
    /// True for a `key<>` option.
    static constexpr bool value = true;
    /// The member pointer the option names.
    static constexpr auto member = Member;
};

/// @brief Detects a `derivedFrom<>` option.
/// @tparam T An option.
template <typename T>
struct IsDerivedFrom : std::false_type {};
template <typename... S>
struct IsDerivedFrom<derivedFrom<S...>> : std::true_type {};

/// @brief The type of the data member a member pointer names.
/// @tparam M A pointer-to-data-member type.
template <typename M>
struct MemberType;
template <typename Class, typename Member>
struct MemberType<Member Class::*> {
    using type = Member;
};

/// @brief Whether a key member's raw type is `std::string`, looking through a wrapped strong id.
/// @tparam K The key member's type.
/// @return True when the raw key type is `std::string`.
template <typename K>
constexpr bool keyTypeIsString() {
    using Plain = std::remove_cvref_t<K>;
    if constexpr (model::WrappedModelKey<Plain>) {
        return std::is_same_v<model::detail::UnwrappedKeyOf<Plain>, std::string>;
    } else {
        return std::is_same_v<Plain, std::string>;
    }
}

/// @brief The reflected name of a data member, found by its address in a value-initialised object.
/// @tparam Class The record type.
/// @tparam Member The member's type.
/// @param member The member.
/// @return The glaze JSON key of the member, or empty when glaze does not reflect it.
template <typename Class, typename Member>
[[nodiscard]] std::string memberKeyName(Member Class::* member) {
    Class const probe{};
    void const* const target = std::addressof(probe.*member);
    std::string found;
    [&]<std::size_t... Index>(std::index_sequence<Index...>) {
        auto visit = [&]<std::size_t I>() {
            decltype(auto) value = morph::detail::reflectedMember<I>(probe);
            if constexpr (std::is_reference_v<decltype(value)>) {
                if (static_cast<void const*>(std::addressof(value)) == target) {
                    found = std::string{glz::reflect<Class>::keys[I]};
                }
            }
        };
        (visit.template operator()<Index>(), ...);
    }(std::make_index_sequence<glz::reflect<Class>::size>{});
    return found;
}

/// @brief The index of the reflected member named `id`, or `size` when there is none.
/// @tparam T The record type.
/// @return The index, or `glz::reflect<T>::keys.size()` when no member is named `id`.
template <typename T>
constexpr std::size_t idIndex() {
    constexpr auto keys = glz::reflect<T>::keys;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] == "id") {
            return i;
        }
    }
    return keys.size();
}

/// @brief Fills the key fields of @p info from the options.
/// @tparam T The record type.
/// @tparam Opts The options.
/// @param info The entry being built.
template <typename T, typename... Opts>
void applyKey(RecordInfo& info) {
    constexpr bool keyless_ = (std::is_same_v<Opts, keyless> || ...);
    constexpr int explicitKeys = (static_cast<int>(KeyOption<Opts>::value) + ... + 0);
    static_assert(explicitKeys <= 1, "BRIDGE_REGISTER_RECORD: at most one key<> option");
    static_assert(!(keyless_ && explicitKeys > 0), "BRIDGE_REGISTER_RECORD: keyless and key<> contradict");
    if constexpr (keyless_) {
        return;
    } else if constexpr (explicitKeys == 1) {
        auto const apply = []<typename O>(RecordInfo& target) {
            if constexpr (KeyOption<O>::value) {
                using Raw = typename MemberType<std::remove_cv_t<decltype(KeyOption<O>::member)>>::type;
                static_assert(model::ModelKey<std::remove_cvref_t<Raw>>,
                              "BRIDGE_REGISTER_RECORD: a key member must be a ModelKey");
                auto name = memberKeyName<T>(KeyOption<O>::member);
                assert(!name.empty() && "BRIDGE_REGISTER_RECORD: key<> names a member glaze does not reflect");
                if (!name.empty()) {
                    target.keyField = std::move(name);
                }
                target.keyIsString = keyTypeIsString<Raw>();
            }
        };
        (apply.template operator()<Opts>(info), ...);
    } else if constexpr (constexpr std::size_t index = idIndex<T>(); index < glz::reflect<T>::size) {
        using Raw = std::remove_cvref_t<decltype(morph::detail::reflectedMember<index>(std::declval<T&>()))>;
        static_assert(model::ModelKey<Raw>, "BRIDGE_REGISTER_RECORD: the `id` member must be a ModelKey");
        info.keyField = "id";
        info.keyIsString = keyTypeIsString<Raw>();
    }
}

/// @brief The process-level record list, appended to by static registrars.
/// @return The mutable list.
inline std::vector<RecordInfo>& mutableRecordRegistry() {
    static std::vector<RecordInfo> entries;
    return entries;
}

/// @brief The records declared so far, raw and unvalidated.
/// @return The entries in registration order.
inline std::vector<RecordInfo> const& recordRegistry() { return mutableRecordRegistry(); }

/// @brief Static-init helper for `BRIDGE_REGISTER_RECORD`.
/// @tparam T The record type.
/// @tparam Opts Any of `key<>`, `keyless`, `derivedFrom<>`, `privateKeys`.
/// @param name The record's wire name.
/// @return Always true, so the result can initialise a namespace-scope variable.
template <typename T, typename... Opts>
inline bool registerRecordOnce(std::string_view name) noexcept {
    // NOLINTNEXTLINE(misc-static-assert,cert-dcl03-c)
    assert(!::morph::model::registrationPhaseClosed() &&
           "registerRecordOnce: registration after the registration phase closed. The process-level "
           "registries are unsynchronised and are read-only once dispatch begins -- registering now "
           "races their internals against concurrent lookups. Load and register plugin modules before "
           "the first dispatch.");
    RecordInfo info{.name = std::string{name}, .type = std::type_index{typeid(T)}, .keyField = {}, .derivedFrom = {}};
    applyKey<T, Opts...>(info);
    info.privateKeys = (std::is_same_v<Opts, privateKeys> || ... || false);
    // Unused when the record takes no options: the fold below is then empty.
    [[maybe_unused]] auto const addSources = []<typename O>(RecordInfo& target) {
        if constexpr (IsDerivedFrom<O>::value) {
            []<typename... S>(RecordInfo& out, derivedFrom<S...>) { (out.derivedFrom.emplace_back(typeid(S)), ...); }(
                target, O{});
        }
    };
    (addSources.template operator()<Opts>(info), ...);
    mutableRecordRegistry().push_back(std::move(info));
    return true;
}

}  // namespace detail
}  // namespace morph::record

/// @def BRIDGE_REGISTER_RECORD
/// @brief Registers a record type under a wire name, with optional key, derivation and privacy options.
///
/// The key defaults to the member reflected as `id`. Options are `morph::record::key<&T::m>`,
/// `morph::record::keyless`, `morph::record::derivedFrom<S...>` and `morph::record::privateKeys`. Expands to
/// nothing under `MORPH_CLIENT_ONLY`.
/// @param T The record type.
/// @param NAME The record's wire name.
/// @param ... The options.
// Each translation unit that includes a registration registers its own copy, which is the point, so the unnamed
// namespace in a header and its static initialiser are suppressed here, once, rather than at every use.
// NOLINTBEGIN(bugprone-throwing-static-initialization,cert-err58-cpp,misc-anonymous-namespace-in-header,cert-dcl59-cpp)
#ifdef MORPH_CLIENT_ONLY
#define BRIDGE_REGISTER_RECORD(T, NAME, ...)
#else
#define BRIDGE_REGISTER_RECORD(T, NAME, ...)                                           \
    namespace {                                                                        \
    [[maybe_unused]] const bool BRIDGE_DETAIL_CAT(bridge_record_reg_, __COUNTER__) =   \
        morph::record::detail::registerRecordOnce<T __VA_OPT__(, ) __VA_ARGS__>(NAME); \
    }
#endif
// NOLINTEND(bugprone-throwing-static-initialization,cert-err58-cpp,misc-anonymous-namespace-in-header,cert-dcl59-cpp)

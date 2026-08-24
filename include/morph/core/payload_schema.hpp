// SPDX-License-Identifier: Apache-2.0

/// @file
/// @brief Structural fingerprint of an action payload's JSON shape.
///
/// The journal records an action's request as JSON and replays it, later,
/// through `ActionTraits<A>::fromJson` — a *lenient* decode that ignores
/// unknown keys and default-constructs absent ones. That leniency is deliberate
/// (it is what makes an additive field a non-event for every already-deployed
/// reader), but on the replay path it also means a **renamed** field silently
/// becomes "key absent + unknown key ignored": the entry decodes, the model
/// reconstructs, and the value that was actually recorded is gone. Nothing in
/// the entry said which shape wrote it, so nothing could notice.
///
/// This header supplies the missing discriminator: a short, stable string
/// derived from the payload struct's reflected shape, cheap enough to stamp on
/// every recorded entry and compare on every replayed one. See
/// `docs/spec/journal/journal.md`, "Payload schema fingerprint".

#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace morph::model {

/// @brief Version of the fingerprint *algorithm*, not of any one payload.
///
/// Stamped as the prefix of every fingerprint (`"1:…"`) so a future build that
/// computes fingerprints differently can still recognise — and reject, or
/// re-derive — a fingerprint written under an earlier scheme, instead of
/// mistaking a scheme change for a payload change. Bumped only when
/// `detail::payloadShape` changes what it emits for an unchanged struct.
inline constexpr std::uint32_t kPayloadFingerprintScheme = 1;

namespace detail {

/// @brief Recursion limit for `payloadShape`.
///
/// A payload nested deeper than this contributes the opaque tag rather than its
/// structure — the fingerprint stays finite for a self-referential type (a
/// `struct Node { std::vector<Node> kids; }` is a legal Glaze payload) without
/// needing cycle detection. Eight levels is far past any action struct in this
/// repository or its examples.
inline constexpr int kPayloadShapeMaxDepth = 8;

/// @brief Tag emitted for a type whose JSON shape this header does not
///        decompose — a type with a custom Glaze codec (`glz::meta` naming a
///        value rather than an object), a function pointer, a variant.
///
/// Deliberately carries no type name: `glz::name_v` falls back to a
/// `__PRETTY_FUNCTION__`/`__FUNCSIG__`-derived spelling that differs between
/// compilers, and a fingerprint that changes when the *compiler* changes would
/// make every journal unreadable by a peer build. See the spec's "What the
/// fingerprint does not catch".
inline constexpr std::string_view kOpaqueShapeTag = "x";

template <typename T>
[[nodiscard]] std::string payloadShape(int depth);

/// @brief Trait detecting `std::optional<T>` exactly (not "nullable in general").
/// @tparam T Type under test.
template <typename T>
struct IsStdOptional : std::false_type {};

/// @brief Specialisation matching `std::optional<T>`.
/// @tparam T Contained type.
template <typename T>
struct IsStdOptional<std::optional<T>> : std::true_type {};

/// @brief Renders a reflected aggregate as `(key:shape,key:shape,…)`, with the
///        members sorted by key.
///
/// **Sorted, so member reordering is not a schema change.** JSON objects are
/// unordered and the decode matches by name, so moving a field within the
/// struct changes nothing about which bytes decode to which member; making the
/// fingerprint order-sensitive would turn a purely cosmetic edit into a replay
/// break for every retained journal. Renaming, adding, or removing a member
/// does change the rendering, which is the whole point.
///
/// @tparam T     Reflected aggregate (or `glz::meta`-declared object) type.
/// @param  depth Current recursion depth; members are rendered at `depth + 1`.
/// @return The parenthesised, comma-joined, key-sorted member rendering.
template <typename T>
[[nodiscard]] std::string payloadShapeObject(int depth) {
    using V = std::remove_cvref_t<T>;
    constexpr std::size_t kMemberCount = glz::reflect<V>::size;
    std::vector<std::string> members;
    members.reserve(kMemberCount);
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (members.emplace_back(std::string{glz::reflect<V>::keys[I]} + ':' +
                              payloadShape<glz::refl_t<V, I>>(depth + 1)),
         ...);
    }(std::make_index_sequence<kMemberCount>{});
    std::ranges::sort(members);
    std::string out{"("};
    bool first = true;
    for (const auto& member : members) {
        if (!first) {
            out += ',';
        }
        first = false;
        out += member;
    }
    out += ')';
    return out;
}

/// @brief Renders @p T's JSON shape as a compact, compiler-independent string.
///
/// The grammar, in full:
///
/// | C++ type | Rendering |
/// |---|---|
/// | `bool` | `b` |
/// | `char` | `c` (its signedness is platform-dependent; the tag is not) |
/// | signed integral | `i` + `sizeof` (e.g. `i4`) |
/// | unsigned integral | `u` + `sizeof` |
/// | enumeration | `e` + `sizeof` of the underlying type |
/// | floating point | `f` + `sizeof` |
/// | string-like | `s` |
/// | `std::optional<T>` | `?` + T's rendering |
/// | map | `{` key `>` mapped `}` |
/// | other range | `[` element `]` |
/// | reflected object | `(` sorted `key:shape` list `)` |
/// | anything else | `x` (see `kOpaqueShapeTag`) |
///
/// Every tag is derived from `std::` type traits or from Glaze's *reflected
/// key strings*, never from a compiler-spelled type name, so two builds of the
/// same sources on different compilers, standard libraries, or platforms
/// render an unchanged struct identically.
///
/// @tparam T     Type to render.
/// @param  depth Current recursion depth; at `kPayloadShapeMaxDepth` the
///               opaque tag is emitted instead of recursing further.
/// @return The rendering described above.
template <typename T>
[[nodiscard]] std::string payloadShape(int depth) {
    using U = std::remove_cvref_t<T>;
    if (depth > kPayloadShapeMaxDepth) {
        return std::string{kOpaqueShapeTag};
    }
    if constexpr (std::is_same_v<U, bool>) {
        return "b";
    } else if constexpr (std::is_same_v<U, char>) {
        return "c";
    } else if constexpr (std::is_enum_v<U>) {
        return "e" + std::to_string(sizeof(std::underlying_type_t<U>));
    } else if constexpr (std::is_integral_v<U>) {
        return (std::is_signed_v<U> ? std::string{"i"} : std::string{"u"}) + std::to_string(sizeof(U));
    } else if constexpr (std::is_floating_point_v<U>) {
        return "f" + std::to_string(sizeof(U));
    } else if constexpr (glz::str_t<U> || glz::string_view_t<U>) {
        return "s";
    } else if constexpr (IsStdOptional<U>::value) {
        return "?" + payloadShape<typename U::value_type>(depth + 1);
    } else if constexpr (glz::readable_map_t<U>) {
        return "{" + payloadShape<typename U::key_type>(depth + 1) + ">" +
               payloadShape<typename U::mapped_type>(depth + 1) + "}";
    } else if constexpr (glz::range<U>) {
        return "[" + payloadShape<glz::range_value_t<U>>(depth + 1) + "]";
    } else if constexpr (glz::reflectable<U> || glz::glaze_object_t<U>) {
        return payloadShapeObject<U>(depth);
    } else {
        return std::string{kOpaqueShapeTag};
    }
}

/// @brief FNV-1a over @p bytes, 64-bit.
///
/// Chosen for being three lines of dependency-free, byte-exact-on-every-platform
/// arithmetic — this hash is a *discriminator*, not a security primitive, and
/// nothing downstream must be able to trust it against an adversary who can
/// also rewrite the journal line it is stamped on.
/// @param bytes Input to hash.
/// @return The 64-bit digest.
[[nodiscard]] inline std::uint64_t fnv1a64(std::string_view bytes) noexcept {
    constexpr std::uint64_t kOffsetBasis = 14695981039346656037ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    std::uint64_t hash = kOffsetBasis;
    for (const char byte : bytes) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(byte));
        hash *= kPrime;
    }
    return hash;
}

/// @brief Renders @p value as exactly 16 lowercase hex digits.
/// @param value Digest to render.
/// @return The zero-padded hex string.
[[nodiscard]] inline std::string hex64(std::uint64_t value) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out(16, '0');
    for (std::size_t i = 0; i < 16; ++i) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) — fixed 16-char buffer
        out[15 - i] = kDigits[(value >> (4 * i)) & 0xFULL];
    }
    return out;
}

}  // namespace detail

/// @brief Returns the human-readable shape rendering behind
///        `payloadFingerprint<A>()`.
///
/// Exposed because a fingerprint mismatch is otherwise two opaque hex strings:
/// with this, a diagnostic (or a developer at a prompt) can print both shapes
/// and see *which* member moved. Not stamped on entries — it is unbounded in
/// length, whereas the fingerprint is 18 bytes.
/// @tparam A Action payload type.
/// @return The shape rendering; see `detail::payloadShape` for the grammar.
template <typename A>
[[nodiscard]] inline const std::string& payloadShapeString() {
    static const std::string kShape = detail::payloadShape<A>(0);
    return kShape;
}

/// @brief Returns `A`'s payload fingerprint: `"<scheme>:<16 hex digits>"`.
///
/// Two payload types fingerprint identically exactly when
/// `payloadShapeString()` renders them identically — i.e. when they have the
/// same member names, at the same nesting positions, with the same JSON-shape
/// tags. Computed once per type per process (a function-local static) and
/// returned by reference; stamping it on a journal entry is a string copy of
/// 18 bytes.
///
/// @tparam A Action payload type.
/// @return Reference to the process-lifetime fingerprint string for `A`.
template <typename A>
[[nodiscard]] inline const std::string& payloadFingerprint() {
    static const std::string kFingerprint = [] {
        return std::to_string(kPayloadFingerprintScheme) + ':' +
               detail::hex64(detail::fnv1a64(payloadShapeString<A>()));
    }();
    return kFingerprint;
}

}  // namespace morph::model

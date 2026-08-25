// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file util/tagged.hpp
/// @brief `Tagged<T, "Name">` — an opaque, type-safe newtype for protocol
///        scalars that serializes transparently as its underlying `T`.
///
/// A protocol scalar (a pagination cursor, an event id, a job id, a bearer
/// token) is a `T` (usually `std::string` or an integer) that must not be
/// interchangeable with a different scalar of the same underlying type —
/// `Tagged<std::string, "UserId">` and `Tagged<std::string, "AccountId">`
/// carry the same wire representation but are distinct C++ types, so passing
/// one where the other is expected is a compile error rather than a silent
/// mix-up caught (or not) at runtime.
///
/// `Tag` is a `morph::detail::FixedString` non-type template parameter (the
/// same structural string type `morph::forms::Choice` and
/// `morph::units::NamedQuantity` use), so the identity lives in the type
/// itself — two `Tagged<T, "X">` written identically in different
/// translation units are one and the same type, and no companion `enum` or
/// registry is needed to keep tags distinct.
///
/// **Wire.** On the morph JSON wire a `Tagged<T, Tag>` is exactly its `T`
/// payload — `glz::meta` maps the instance to `value` alone, so it reads and
/// writes identically to a bare `T`, and `to_json_schema` delegates to `T`'s
/// own schema (the tag never appears on the wire or in the generated
/// schema).
///
/// **Forms palette.** `Tagged` is a *required* scalar, not an
/// optionally-empty field like `Quantity`/`Choice`/`Timestamp`: it always
/// holds a `T` (default-constructed, never `std::nullopt`). Its `hasValue()`
/// therefore always returns `true`, satisfying
/// `morph::forms::EmptyCapableField` so it composes with the palette (rule
/// vocabulary, `allRequiredEngaged`, etc.) the same way any other required
/// field does — nothing in the palette special-cases it away like a plain,
/// non-empty-capable scalar member would be.

#include <compare>
#include <cstddef>
#include <glaze/glaze.hpp>
#include <morph/core/payload_shape_tag.hpp>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "../detail/fixed_string.hpp"

namespace morph::util {

/// @brief An opaque, type-safe newtype wrapping a protocol scalar `T`,
///        distinguished at compile time by the string tag `Tag`.
///
/// @tparam T   The underlying wire scalar (e.g. `std::string`, `std::int64_t`).
/// @tparam Tag A `morph::detail::FixedString` NTTP naming this newtype (e.g.
///             `"UserId"`); part of the C++ type, never part of the wire.
template <typename T, morph::detail::FixedString Tag>
struct Tagged {
    /// @brief The underlying scalar value.
    T value{};

    /// @brief Constructs the default-valued state (`T{}`).
    constexpr Tagged() noexcept(std::is_nothrow_default_constructible_v<T>) = default;

    /// @brief Wraps @p wrapped.
    /// @param wrapped The scalar value to tag.
    constexpr explicit Tagged(T wrapped) noexcept(std::is_nothrow_move_constructible_v<T>)
        : value{std::move(wrapped)} {}

    /// @brief The compile-time tag naming this newtype.
    /// @return The tag text, e.g. `"UserId"`.
    [[nodiscard]] static constexpr std::string_view tag() noexcept { return Tag.view(); }

    /// @brief Always `true`: `Tagged` is a required scalar, never empty.
    /// @return `true`.
    [[nodiscard]] constexpr bool hasValue() const noexcept { return true; }

    /// @brief Read access to the wrapped value.
    /// @return The underlying scalar.
    [[nodiscard]] constexpr const T& get() const noexcept { return value; }

    /// @brief Unchecked access to the wrapped value, mirroring `Quantity`'s
    ///        and `Choice`'s `operator*` for consistency across the
    ///        empty-capable-field family.
    /// @return The underlying scalar.
    [[nodiscard]] constexpr const T& operator*() const noexcept { return value; }

    /// @brief Equality on the wrapped value.
    /// @param other Tagged value to compare against (same `T`/`Tag`).
    /// @return `true` when both wrap equal values.
    [[nodiscard]] constexpr bool operator==(const Tagged& other) const noexcept(noexcept(value == other.value))
        requires std::equality_comparable<T>
    = default;

    /// @brief Three-way comparison on the wrapped value, when `T` supports it.
    /// @param other Tagged value to compare against (same `T`/`Tag`).
    /// @return The ordering of the wrapped values.
    [[nodiscard]] constexpr auto operator<=>(const Tagged& other) const noexcept(noexcept(value <=> other.value))
        requires std::three_way_comparable<T>
    = default;
};

namespace detail {

/// @brief Trait: is `T` some `Tagged<...>`?
template <typename T>
struct IsTagged : std::false_type {};

template <typename U, morph::detail::FixedString Tag>
struct IsTagged<Tagged<U, Tag>> : std::true_type {};

}  // namespace detail

/// @brief `true` when `T` (cvref-stripped) is a `morph::util::Tagged`.
template <typename T>
inline constexpr bool isTagged = detail::IsTagged<std::remove_cvref_t<T>>::value;

}  // namespace morph::util

/// @brief On the wire a `Tagged<T, Tag>` is exactly its underlying `value` —
///        the tag never travels; `T`'s own codec runs unchanged.
template <typename T, morph::detail::FixedString Tag>
struct glz::meta<morph::util::Tagged<T, Tag>> {
    /// @brief The single wire field: the wrapped scalar.
    static constexpr auto value = &morph::util::Tagged<T, Tag>::value;

    /// @brief The tag as the schema type name, so distinct tags produce
    ///        distinctly named (if structurally identical) schema entries.
    static constexpr std::string_view name = Tag.view();
};

namespace glz::detail {

/// @brief Schema for `Tagged<T, Tag>`: identical to `T`'s own schema — the
///        tag is a compile-time-only distinction and never surfaces in the
///        generated JSON Schema.
/// @tparam T   The underlying wire scalar.
/// @tparam Tag The compile-time tag (schema-invisible).
template <typename T, morph::detail::FixedString Tag>
struct to_json_schema<morph::util::Tagged<T, Tag>> {
    /// @brief Emits the schema.
    /// @tparam Opts Glaze options.
    /// @param outSchema Schema being built.
    /// @param defs      Schema definitions.
    template <auto Opts>
    static void op(auto& outSchema, auto& defs) {
        to_json_schema<T>::template op<Opts>(outSchema, defs);
    }
};

}  // namespace glz::detail

/// @brief Stable shape tag for `Tagged<T, Tag>`, carrying both the tag text
///        and the wrapped type's own shape.
///
/// `Tagged` is a transparent wrapper — its JSON simply *is* `T`'s — so hiding
/// `T` behind the wrapper's name would lose real information: retyping
/// `Tagged<std::string, "acct">` to `Tagged<std::int64_t, "acct">` genuinely
/// changes the recorded JSON. `Inner` keeps that visible, and the tag text
/// separates two wrappers that are structurally identical but semantically
/// different (`"acct"` versus `"user"`), which is the swap nothing else in the
/// system can see: the tag never travels on the wire.
///
/// `Tag.view()` is a `FixedString` NTTP spelled at the use site in these
/// sources, so it is stable across compilers in a way `glz::name_v` is not.
/// See `morph/core/payload_shape_tag.hpp`.
/// @tparam T   The wrapped scalar type.
/// @tparam Tag The compile-time tag.
template <typename T, morph::detail::FixedString Tag>
struct morph::model::PayloadShapeTag<morph::util::Tagged<T, Tag>> {
    /// @brief The wrapped type, rendered inside this tag.
    using Inner = T;

    /// @brief This type's stable shape name, e.g. `"tagged.acct"`.
    /// @return The name; the referenced storage lives for the whole process.
    static std::string_view name() {
        static const std::string kName = "tagged." + std::string{Tag.view()};
        return kName;
    }
};

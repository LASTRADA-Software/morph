// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/detail/session_common.hpp
/// @brief Machinery shared by the two form session types: `morph::flows::FlowSession`
///        (ordered wizard steps) and `morph::forms::SectionSet` (unordered sections).
///
/// Both declare their units the same way — a registered action, a title, and
/// zero or more `Bind` prefill declarations — and both walk those declarations
/// to emit a schema document. Only the sequencing differs, so the declaration
/// vocabulary and the pack/tuple walkers live here rather than being written
/// twice.

#include <cstddef>
#include <glaze/glaze.hpp>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "../forms.hpp"

namespace morph::forms {

/// @brief One `field -> "<ActionTypeId>.<field>"` prefill binding declared on a
///        wizard step or an unordered section.
///
/// A declaration, not a write. Nothing in the framework assigns a bound field:
/// `wizardSchemaJson`/`sectionGroupSchemaJson` emit it for a renderer, and the
/// captured value is read back through `resolved()`.
/// @tparam Field The action's field name to prefill.
/// @tparam Path  Source path, `"<ActionTypeId>.<field>"`, into captured values.
template <morph::forms::FixedString Field, morph::forms::FixedString Path>
struct Bind {
    /// @brief The action field name this binding fills.
    /// @return The declared field name.
    [[nodiscard]] static constexpr std::string_view field() noexcept { return Field.view(); }

    /// @brief The source path into captured values.
    /// @return The declared `"<ActionTypeId>.<field>"` path.
    [[nodiscard]] static constexpr std::string_view path() noexcept { return Path.view(); }
};

namespace detail {

/// @brief Invokes `visitor.template operator()<std::tuple_element_t<I, Tuple>, I>()`
///        for every element of @p Tuple, in order.
/// @tparam Tuple   A `std::tuple<...>` type (only its element types/arity are used).
/// @tparam Visitor Callable with a `template<typename Element, std::size_t I> operator()()`.
/// @param visitor Callable invoked once per tuple element.
template <typename Tuple, typename Visitor>
// NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward) — invoked once per element, never moved from
constexpr void forEachTupleElement(Visitor&& visitor) {
    // NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved) — same: one call per element
    []<std::size_t... I>(std::index_sequence<I...>, Visitor&& innerVisitor) {
        (innerVisitor.template operator()<std::tuple_element_t<I, Tuple>, I>(), ...);
    }(std::make_index_sequence<std::tuple_size_v<Tuple>>{}, std::forward<Visitor>(visitor));
}

/// @brief Invokes `visitor.template operator()<T>()` for the pack element of
///        `Ts...` at runtime position @p index. A no-op when
///        `index >= sizeof...(Ts)`.
/// @tparam Ts      The pack to index into.
/// @tparam Visitor Callable with a `template<typename T> operator()()`.
/// @param index   0-based position to visit.
/// @param visitor Callable invoked for the element at @p index.
template <typename... Ts, typename Visitor>
// NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward) — the visited element is a run-time choice
constexpr void forPackElement(std::size_t index, Visitor&& visitor) {
    std::size_t i = 0;
    (void)((i++ == index ? (visitor.template operator()<Ts>(), true) : false) || ...);
}

/// @brief Trait: `true` when every type in `Ts...` is pairwise distinct.
/// @tparam Ts Types to check for pairwise distinctness.
template <typename... Ts>
struct AllDistinct : std::true_type {};

/// @brief Recursive case: `T` distinct from every type in `Rest...`, and `Rest...` pairwise distinct.
/// @tparam T    The type being checked against `Rest...`.
/// @tparam Rest The remaining types.
template <typename T, typename... Rest>
struct AllDistinct<T, Rest...> : std::bool_constant<(!std::is_same_v<T, Rest> && ...) && AllDistinct<Rest...>::value> {
};

/// @brief Writes each `Bind` in @p BindsTuple into @p node as `"<field>": "<path>"`.
///
/// A no-op for an empty tuple, so callers guard on arity only to avoid
/// materialising an empty `prefill` object.
/// @tparam BindsTuple `std::tuple<Bind<...>...>`.
/// @param node Destination JSON object node.
template <typename BindsTuple>
void emitBindsInto(glz::generic_u64& node) {
    forEachTupleElement<BindsTuple>([&]<typename BindT, std::size_t J>() {
        static_cast<void>(J);
        node[std::string{BindT::field()}] = std::string{BindT::path()};
    });
}

}  // namespace detail
}  // namespace morph::forms

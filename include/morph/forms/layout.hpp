// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/layout.hpp
/// @brief Visual structure (sections, tabs, accordions, column spans) layered
///        over an action's flat field list.
///
/// `morph::forms::schemaJson<A>()` (`forms.hpp`) already lays every field out
/// in a single flat list, ordered by `x-order`. This header adds two
/// **optional** compile-time declarations an action may expose: `formLayout`,
/// a list of `FieldGroup`s that bucket fields into titled sections, tabs, or
/// an accordion panel; and `fieldSpans`, a list of `FieldSpan`s that widen
/// individual fields in a grid renderer. Both mirror the existing
/// `optionalFields` convention (`forms.hpp`) — a `static constexpr` list
/// `mergeSchemaExtras` looks for by name, present only when an action opts
/// in.
///
/// Absent either declaration, `schemaJson<A>()`'s output is unchanged: no
/// `x-layout`, `x-group`, `x-section`, or `x-colspan` key is emitted, and a
/// renderer lays every field out exactly as it does today (flat, `x-order`
/// order). See docs/spec/forms/forms.md, "Layout & grouping".

#include <cstdint>
#include <iterator>
#include <span>
#include <string_view>

namespace morph::forms {

/// @brief The container kind a `FieldGroup` renders as.
enum class GroupKind : std::uint8_t {
    Section,   ///< A titled fieldset, stacked vertically (the default).
    Tab,       ///< A pane of a shared tab bar; consecutive `Tab` groups share one bar.
    Accordion  ///< A collapsible panel.
};

/// @brief Maps a `GroupKind` to the string `mergeSchemaExtras` emits for it
///        in `x-layout.groups[].kind`.
/// @param kind Group kind to name.
/// @return `"section"`, `"tab"`, or `"accordion"`. The `default` branch (all
///         three enumerators are already listed explicitly) is an
///         out-of-range-value fallback to `"section"`, the renderer's
///         documented downgrade for an unsupported kind.
[[nodiscard]] constexpr std::string_view groupKindName(GroupKind kind) noexcept {
    switch (kind) {
        case GroupKind::Section:
            return "section";
        case GroupKind::Tab:
            return "tab";
        case GroupKind::Accordion:
            return "accordion";
        default:
            // Unreachable through any real code path: GroupKind is a closed
            // enum and every enumerator is already handled explicitly above.
            // This arm only exists to satisfy the compiler that the function
            // returns on every enum value, including one manufactured by an
            // out-of-range `static_cast` -- mirrors ruleKindName's identical
            // default: arm in forms.hpp.
            return "section";
    }
}

/// @brief One named group of fields in an action's `formLayout`.
///
/// `fields` gives **membership only** — which wire keys belong to this
/// group — not intra-group order: the renderer still lays fields out by
/// ascending `x-order` within the group. The array position of a `FieldGroup`
/// inside `A::formLayout` gives the cross-group order.
struct FieldGroup {
    /// @brief Section / tab / panel heading shown to the user.
    std::string_view title;

    /// @brief Container this group renders as. Defaults to `Section`.
    GroupKind kind{GroupKind::Section};

    /// @brief Member wire keys belonging to this group (membership only; see
    ///        the struct documentation for intra-group order).
    std::span<const std::string_view> fields;
};

/// @brief A field's declared column span in a grid renderer.
struct FieldSpan {
    /// @brief Wire key of the field this span applies to.
    std::string_view field;

    /// @brief Number of grid columns the field should span. `1` (the
    ///        default) is the ordinary single-column width and is never
    ///        emitted as `x-colspan` — only values greater than `1` are.
    int colspan{1};
};

namespace detail {

/// @brief Concept: action declares a `static constexpr` iterable
///        `formLayout` list of `FieldGroup`.
template <typename A>
concept HasFormLayout = requires {
    std::begin(A::formLayout);
    std::end(A::formLayout);
};

/// @brief Concept: action declares a `static constexpr` iterable
///        `fieldSpans` list of `FieldSpan`.
template <typename A>
concept HasFieldSpans = requires {
    std::begin(A::fieldSpans);
    std::end(A::fieldSpans);
};

}  // namespace detail

}  // namespace morph::forms

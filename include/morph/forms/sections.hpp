// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/sections.hpp
/// @brief Unordered sections: N independently editable action drafts on one
///        screen, each with its own readiness gate and no sequencing.
///
/// The sibling to `morph::flows::FlowSession`. A wizard has one active step and
/// throws on a field belonging to any other; a section set has no active
/// section at all -- every declared section is editable at every moment, and
/// each dispatches on its own as soon as its draft validates.
///
/// Choose this when order carries no meaning (tabs, cards, a settings page) and
/// `FlowSession` when it does. Like flows, this is additive metadata and
/// dispatch bookkeeping over the ordinary `BridgeHandler<Model>::execute<A>()`
/// path: no new wire format, no new execution mode.
/// See docs/spec/forms/sections.md.

#include <cstddef>
#include <exception>
#include <functional>
#include <glaze/glaze.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "../attributes.hpp"
#include "../core/bridge.hpp"
#include "../core/callback_scope.hpp"
#include "../core/logger.hpp"
#include "detail/session_common.hpp"

namespace morph::forms {

/// @brief One section of a `SectionGroup`: a registered action, a display
///        title, and zero or more `Bind` prefill declarations.
/// @tparam Action Registered action type (`BRIDGE_REGISTER_ACTION`) this section fires.
/// @tparam Title  Human title for the section (tab label / card header).
/// @tparam Binds  Zero or more `Bind<Field, Path>` prefill declarations.
template <typename Action, morph::forms::FixedString Title, typename... Binds>
struct Section {
    /// @brief The section's action type.
    using action = Action;

    /// @brief Tuple of this section's `Bind<...>` prefill declarations (possibly empty).
    using binds = std::tuple<Binds...>;

    /// @brief The section's display title.
    /// @return The declared title.
    [[nodiscard]] static constexpr std::string_view title() noexcept { return Title.view(); }
};

/// @brief An unordered set of `Section`s sharing one screen.
/// @tparam Title    Human title for the whole group.
/// @tparam Sections One or more `Section<Action, Title, Binds...>` types.
template <morph::forms::FixedString Title, typename... Sections>
struct SectionGroup {
    /// @brief Tuple of this group's `Section<...>` types.
    using sections = std::tuple<Sections...>;

    /// @brief The group's display title.
    /// @return The declared title.
    [[nodiscard]] static constexpr std::string_view title() noexcept { return Title.view(); }
};

/// @brief Traits specialisation mapping a `SectionGroup` type to its string type-id.
///
/// Specialise via `BRIDGE_REGISTER_SECTION_GROUP` rather than by hand. The
/// default is a forward declaration — using it without a specialisation is an
/// incomplete-type error. The schema needs a stable name for the group;
/// deriving one from the C++ type would tie the wire format to a mangled name.
/// @tparam G Concrete `SectionGroup<...>` type.
template <typename G>
struct SectionGroupTraits;  // forward — specialise or use BRIDGE_REGISTER_SECTION_GROUP

/// @brief Generates the `s-*` JSON document for section group @p G.
///
/// Emits `s-id`, `s-title`, and an `s-sections` array; each entry carries
/// `action` (the section's registered action type-id), `title`, and — only when
/// the section declares at least one `Bind` — a `prefill` object mapping field
/// name to `"<ActionTypeId>.<field>"` path.
///
/// Deliberately emits no index or order field. A renderer arranges the sections
/// itself, and a position in the document would suggest a sequence this type
/// does not have.
/// @tparam G Concrete `SectionGroup<Title, Sections...>` type.
/// @return The group's JSON document. Empty string only if glaze's own JSON
///         writer fails on the assembled DOM (schema generation never throws).
template <typename G>
[[nodiscard]] std::string sectionGroupSchemaJson() {
    glz::generic_u64 dom{};
    dom["s-id"] = std::string{SectionGroupTraits<G>::typeId()};
    dom["s-title"] = std::string{G::title()};

    glz::generic_u64::array_t sections{};
    detail::forEachTupleElement<typename G::sections>([&]<typename SectionT, std::size_t I>() {
        static_cast<void>(I);
        glz::generic_u64 entry{};
        entry["action"] = std::string{::morph::model::ActionTraits<typename SectionT::action>::typeId()};
        entry["title"] = std::string{SectionT::title()};
        if constexpr (std::tuple_size_v<typename SectionT::binds> != 0) {
            detail::emitBindsInto<typename SectionT::binds>(entry["prefill"]);
        }
        sections.emplace_back(std::move(entry));
    });
    dom["s-sections"] = sections;

    return glz::write_json(dom).value_or(std::string{});
}

}  // namespace morph::forms

// clang-format off -- public macro surface; see CONTRIBUTING.md, "Formatting/linting".
/// @brief Specialises `morph::forms::SectionGroupTraits<G>` with the string type-id @p NAME.
#define BRIDGE_REGISTER_SECTION_GROUP(G, NAME)                               \
    template <>                                                              \
    struct morph::forms::SectionGroupTraits<G> {                             \
        static constexpr std::string_view typeId() noexcept { return NAME; } \
    };
// clang-format on

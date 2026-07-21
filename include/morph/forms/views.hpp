// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/views.hpp
/// @brief View-schema generation: composes a registered query action's
///        result rows with an optional row-opener action and row/collection
///        action buttons into a list/table or master-detail **screen**
///        descriptor.
///
/// Where `morph::forms::schemaJson<A>()` describes *one action*, this header
/// describes a *set* of related actions — a query, an edit, a delete — as a
/// single, additional JSON document, `morph::views::viewSchemaJson<V>()`,
/// emitted alongside (never merged into) the action schemas. See
/// docs/spec/forms/views.md.

#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

#include "../core/registry.hpp"

namespace morph::views {

/// @brief Where a view action button appears: opened per row, or once for
///        the whole collection.
enum class ActionScope : std::uint8_t { Row, Collection };

/// @brief Maps one target-action wire field to a row wire field, so a row's
///        current value prefills the action's draft before it fires.
struct BindEntry {
    /// @brief Wire field name on the target action.
    std::string_view actionField;
    /// @brief Wire field name on the query action's row type.
    std::string_view rowField;
};

/// @brief One action a view screen can fire — the row opener (`v-rowAction`)
///        or an entry of `v-actions`.
///
/// Always built by `describeAction<Action>(...)`, never constructed by hand,
/// so `actionTypeId` always reflects a real, registered
/// `ActionTraits<Action>::typeId()` rather than a hand-typed, unchecked
/// string (contrast `morph::forms::Choice`'s `OptionsAction` NTTP, which
/// *is* an unchecked string — see docs/spec/forms/choice.md, "Limitations").
struct ActionDescriptor {
    /// @brief Registered type id of the target action.
    std::string_view actionTypeId;
    /// @brief Button label. Empty means "use the action type id as-is".
    std::string_view label{};
    /// @brief Row button vs. collection-wide button.
    ActionScope scope{ActionScope::Row};
    /// @brief Field-name mapping applied to prefill the target action's
    ///        draft from the activating row. Empty for an action that needs
    ///        no row context (e.g. a collection-scope "create").
    std::span<const BindEntry> bind{};
    /// @brief Whether a renderer must confirm before firing this action.
    bool confirm{false};
};

/// @brief Builds an `ActionDescriptor` for @p Action, resolving its wire type
///        id from the real, registered `ActionTraits<Action>` rather than a
///        hand-typed string.
/// @tparam Action Registered action type — `BRIDGE_REGISTER_ACTION` for
///                @p Action must already have run earlier in this
///                translation unit, or `ActionTraits<Action>` is incomplete
///                and this fails to compile.
/// @param label   Button label. Defaults to empty ("use the action type id").
/// @param scope   Row button vs. collection-wide button. Defaults to `Row`.
/// @param bind    Field-name mapping prefilling the action's draft from the
///                activating row. Defaults to empty (no prefill).
/// @param confirm Whether a renderer must confirm before firing. Defaults to
///                `false`.
/// @return The populated descriptor.
template <typename Action>
[[nodiscard]] consteval ActionDescriptor describeAction(std::string_view label = {},
                                                        ActionScope scope = ActionScope::Row,
                                                        std::span<const BindEntry> bind = {}, bool confirm = false) {
    return ActionDescriptor{
        .actionTypeId = ::morph::model::ActionTraits<Action>::typeId(),
        .label = label,
        .scope = scope,
        .bind = bind,
        .confirm = confirm,
    };
}

/// @brief Declare-to-override entry for one derived column of a collection
///        view.
///
/// Supplying a `static constexpr std::array<ColumnOverride, N> columns` on a
/// view descriptor reorders, relabels, hides, or subsets the columns
/// `viewSchemaJson` would otherwise derive from the query's row type; a
/// `field` naming a wire key the row type does not have is emitted as a bare
/// column (schema generation never throws — see docs/spec/forms/forms.md).
struct ColumnOverride {
    /// @brief Wire field name on the query action's row type.
    std::string_view field;
    /// @brief Column header. Empty means "use `field` as-is".
    std::string_view label{};
    /// @brief Column present in the row model but not displayed.
    bool hidden{false};
};

/// @brief Tag type: `V::kind` selects a plain list/table screen.
struct CollectionView {};

/// @brief Tag type: `V::kind` selects a list + inline/side editor screen.
///        Introduces no JSON keys beyond `CollectionView` — purely a
///        rendering choice a conformant renderer may act on.
struct MasterDetailView {};

namespace detail {

/// @brief `false` for every type; specialised `true` for `std::vector<...>`.
template <typename T>
inline constexpr bool isStdVector = false;

/// @brief Specialisation: `true` for any `std::vector<T, Alloc>`.
template <typename T, typename Alloc>
inline constexpr bool isStdVector<std::vector<T, Alloc>> = true;

/// @brief `"collection"` / `"master-detail"` for the two kind tags.
template <typename Kind>
struct ViewKindNameImpl;

template <>
struct ViewKindNameImpl<CollectionView> {
    static constexpr std::string_view value = "collection";
};

template <>
struct ViewKindNameImpl<MasterDetailView> {
    static constexpr std::string_view value = "master-detail";
};

/// @brief The `v-kind` wire string for kind tag @p Kind.
template <typename Kind>
inline constexpr std::string_view viewKindName = ViewKindNameImpl<Kind>::value;

/// @brief Concept: `V` declares an optional `static constexpr` title.
template <typename V>
concept HasViewTitle = requires { V::title; };

/// @brief Concept: `V` declares an optional `static constexpr` row key.
template <typename V>
concept HasRowKey = requires { V::rowKey; };

/// @brief Concept: `V` declares an optional column-override list.
template <typename V>
concept HasColumnOverrides = requires {
    std::begin(V::columns);
    std::end(V::columns);
};

/// @brief Concept: `V` declares an optional row-opener action descriptor.
template <typename V>
concept HasRowActionDescriptor = requires { V::rowAction; };

/// @brief Concept: `V` declares an optional action-button list.
template <typename V>
concept HasViewActions = requires {
    std::begin(V::actions);
    std::end(V::actions);
};

}  // namespace detail

}  // namespace morph::views

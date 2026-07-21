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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "../core/registry.hpp"
#include "forms.hpp"

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

/// @brief Builds one `v-columns` entry, copying `x-decimalPlaces` /
///        `ExtUnits` off @p rowDom's property node (and, via its `$ref`, the
///        resolved `$def`) when @p field names a `Quantity` property.
/// @param rowDom  The row type's own `morph::forms::schemaJson<Row>()`,
///                parsed into a DOM.
/// @param field   Wire field name on the row type.
/// @param label   Column header.
/// @param hidden  Whether the column is present in the row model but not
///                displayed.
/// @return The `v-columns` entry.
[[nodiscard]] inline glz::generic_u64 buildColumnEntry(glz::generic_u64 const& rowDom, std::string const& field,
                                                       std::string const& label, bool hidden) {
    glz::generic_u64 entry{};
    entry["field"] = field;
    entry["label"] = label;
    if (hidden) {
        entry["v-hidden"] = true;
    }
    auto const& propsObj = rowDom["properties"].get_object();
    auto iter = propsObj.find(field);
    if (iter == propsObj.end()) {
        return entry;  // declared/derived field not on the row type: bare column, no crash
    }
    auto const& prop = iter->second;
    if (prop.contains("x-decimalPlaces")) {
        entry["x-decimalPlaces"] = prop["x-decimalPlaces"];
    }
    // A Quantity field's ExtUnits sits directly on the property node when its
    // Quantity type occurs only once in Row (glaze inlines a single-use
    // struct schema in place); glaze only promotes the schema to a `$defs`
    // entry (referenced back via `$ref`) when the *same* Quantity type
    // occurs on more than one property (see forms.md's "CFSharedDefFields"
    // fixture) — deriveColumns must read whichever shape schemaJson<Row>()
    // actually produced for this particular row type.
    if (prop.contains("ExtUnits")) {
        entry["ExtUnits"] = prop["ExtUnits"];
    } else if (prop.contains("$ref") && rowDom.contains("$defs")) {
        auto const ref = prop["$ref"].get_string();
        auto const defName = ref.substr(ref.find_last_of('/') + 1);
        auto const& defs = rowDom["$defs"].get_object();
        auto defIter = defs.find(defName);
        if (defIter != defs.end() && defIter->second.contains("ExtUnits")) {
            entry["ExtUnits"] = defIter->second["ExtUnits"];
        }
    }
    return entry;
}

/// @brief Derives (or, when `V::columns` is declared, overrides) the
///        `v-columns` array for row type @p Row, reusing
///        `morph::forms::schemaJson<Row>()` as the single source of each
///        field's declaration order, `x-decimalPlaces`, and `ExtUnits` — the
///        same schema a standalone form for `Row` would carry.
/// @tparam V   View descriptor (only `HasColumnOverrides<V>` is consulted).
/// @tparam Row The query action's row element type (a plain, reflectable,
///             default-constructible aggregate).
/// @return The `v-columns` array, JSON-encoded.
template <typename V, typename Row>
[[nodiscard]] std::string deriveColumns() {
    glz::generic_u64 rowDom{};
    if (glz::read_json(rowDom, ::morph::forms::schemaJson<Row>()) || !rowDom.contains("properties")) {
        return "[]";
    }
    glz::generic_u64::array_t columns{};
    if constexpr (HasColumnOverrides<V>) {
        for (auto const& colOverride : V::columns) {
            auto const label =
                colOverride.label.empty() ? std::string{colOverride.field} : std::string{colOverride.label};
            columns.emplace_back(buildColumnEntry(rowDom, std::string{colOverride.field}, label, colOverride.hidden));
        }
    } else {
        auto const& propsObj = rowDom["properties"].get_object();
        std::vector<std::pair<std::string, std::uint64_t>> ordered;
        ordered.reserve(propsObj.size());
        for (auto const& [name, prop] : propsObj) {
            auto const order = prop.contains("x-order") ? prop["x-order"].template get<std::uint64_t>() : 0;
            ordered.emplace_back(name, order);
        }
        std::ranges::sort(ordered, [](auto const& a, auto const& b) { return a.second < b.second; });
        for (auto const& [name, order] : ordered) {
            static_cast<void>(order);
            columns.emplace_back(buildColumnEntry(rowDom, name, name, false));
        }
    }
    return glz::write_json(columns).value_or("[]");
}

/// @brief Builds one `v-rowAction` / `v-actions` entry from an
///        `ActionDescriptor`.
/// @param descriptor          The descriptor to serialise.
/// @param includeButtonFields `true` for `v-actions` entries (adds `label`,
///                            `scope`, and `confirm` when set); `false` for
///                            `v-rowAction` (only `action` and `bind`).
/// @return The JSON node.
[[nodiscard]] inline glz::generic_u64 buildActionNode(ActionDescriptor const& descriptor, bool includeButtonFields) {
    glz::generic_u64 node{};
    node["action"] = std::string{descriptor.actionTypeId};
    if (includeButtonFields) {
        node["label"] = std::string{descriptor.label.empty() ? descriptor.actionTypeId : descriptor.label};
        node["scope"] = std::string{descriptor.scope == ActionScope::Row ? "row" : "collection"};
        if (descriptor.confirm) {
            node["confirm"] = true;
        }
    }
    if (!descriptor.bind.empty()) {
        glz::generic_u64 bindNode{};
        for (auto const& entry : descriptor.bind) {
            bindNode[std::string{entry.actionField}] = std::string{entry.rowField};
        }
        node["bind"] = bindNode;
    }
    return node;
}

/// @brief Builds the complete `viewSchemaJson<V>()` document.
/// @tparam V View descriptor.
/// @return The view-schema JSON, or an empty string on internal DOM failure
///         (schema generation never throws — see docs/spec/forms/forms.md).
template <typename V>
[[nodiscard]] std::string buildViewSchema() {
    using Query = typename V::query;
    using Result = typename ::morph::model::ActionTraits<Query>::Result;

    glz::generic_u64 dom{};
    dom["v-kind"] = std::string{viewKindName<typename V::kind>};
    dom["v-query"] = std::string{::morph::model::ActionTraits<Query>::typeId()};
    if constexpr (HasViewTitle<V>) {
        dom["v-title"] = std::string{V::title};
    } else {
        dom["v-title"] = std::string{::morph::model::ActionTraits<Query>::typeId()};
    }
    if constexpr (HasRowKey<V>) {
        dom["v-rowKey"] = std::string{V::rowKey};
    } else {
        dom["v-rowKey"] = std::string{"id"};
    }

    // The row element type is the query result itself when it is a
    // std::vector<...>, else its first array-valued member, in declaration
    // order — the same two shapes Choice's optionRows reads (DynamicForm.qml).
    std::string columnsJson = "[]";
    if constexpr (isStdVector<Result>) {
        using Row = typename std::remove_cvref_t<Result>::value_type;
        columnsJson = deriveColumns<V, Row>();
    } else {
        Result probe{};
        bool found = false;
        ::morph::forms::detail::forEachNamedMember(probe,
                                                   [&]<std::size_t I>(std::string_view name, const auto& member) {
                                                       static_cast<void>(name);
                                                       static_cast<void>(I);
                                                       using Member = std::remove_cvref_t<decltype(member)>;
                                                       if constexpr (isStdVector<Member>) {
                                                           if (!found) {
                                                               using Row = typename Member::value_type;
                                                               columnsJson = deriveColumns<V, Row>();
                                                               found = true;
                                                           }
                                                       }
                                                   });
    }
    glz::generic_u64 columnsDom{};
    if (!glz::read_json(columnsDom, columnsJson)) {
        dom["v-columns"] = columnsDom;
    }

    if constexpr (HasRowActionDescriptor<V>) {
        dom["v-rowAction"] = buildActionNode(V::rowAction, false);
    }
    if constexpr (HasViewActions<V>) {
        glz::generic_u64::array_t actionsArray{};
        for (auto const& descriptor : V::actions) {
            actionsArray.emplace_back(buildActionNode(descriptor, true));
        }
        dom["v-actions"] = actionsArray;
    }

    return glz::write_json(dom).value_or(std::string{});
}

}  // namespace detail

/// @brief Generates the view-schema JSON document for view descriptor @p V.
///
/// A **new, separate top-level document** from `morph::forms::schemaJson<A>()`
/// — never merged into any action schema. Computed once per type and cached,
/// exactly like `schemaJson<A>()`.
/// @tparam V View descriptor: `using kind = CollectionView` (or
///           `MasterDetailView`); `using query = <registered query action>`;
///           optional `static constexpr std::string_view title`, `rowKey`,
///           `std::array<ColumnOverride, N> columns`,
///           `ActionDescriptor rowAction`, and
///           `std::array<ActionDescriptor, N> actions`.
/// @return The view-schema JSON.
template <typename V>
[[nodiscard]] std::string viewSchemaJson() {
    static const std::string cached = detail::buildViewSchema<V>();
    return cached;
}

}  // namespace morph::views

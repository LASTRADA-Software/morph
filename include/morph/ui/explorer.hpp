// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <morph/export.hpp>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "../attributes.hpp"
#include "../forms/engine/field_model.hpp"
#include "backend.hpp"
#include "bundle.hpp"
#include "document.hpp"
#include "host.hpp"
#include "value.hpp"

/// @file
/// @brief The app explorer: what a morph application is made of — its screens, the models they use with every action,
///        and for one action its facts, its fields, the places that use it and the widget the framework builds for
///        it, mounted live.
///
/// Specified in `docs/spec/ui/explorer.md`.

namespace morph::ui {

/// @brief What names an action in a document.
enum class ActionUseKind : std::uint8_t {
    Query,       ///< A query declaration.
    Mutation,    ///< A mutation declaration.
    Form,        ///< A `form` node.
    Collection,  ///< A `collection` node.
    Wizard,      ///< A `wizard` node.
};

/// @brief The name of an action use's kind.
/// @param kind The kind.
/// @return `"query"`, `"mutation"`, `"form"`, `"collection"` or `"wizard"`.
[[nodiscard]] constexpr std::string_view actionUseKindName(ActionUseKind kind) noexcept {
    constexpr std::array<std::string_view, 5> kNames{"query", "mutation", "form", "collection", "wizard"};
    return kNames.at(static_cast<std::size_t>(kind));
}

/// @brief One place a document names an action.
struct ActionUse {
    /// @brief The action id.
    std::string actionId;
    /// @brief The model type the alias it goes through binds; empty when neither the document nor the catalog says.
    std::string modelType;
    /// @brief What names it.
    ActionUseKind kind = ActionUseKind::Query;
    /// @brief The JSON Pointer of the declaration or node.
    std::string pointer;
};

/// @brief Every place a loaded document names an action: its queries and mutations at any scope, and its `form`,
///        `collection` and `wizard` nodes, in document order.
///
/// A model type is the alias's, from the nearest enclosing scope that declares it. An alias that borrows another has
/// no type in the document, and takes the catalog's `ActionInfo::modelType`. A `run` command names a mutation, not an
/// action, so it is no use of its own.
/// @param document The document: a screen, or the app document.
/// @param catalog  The facts a borrowed alias's model type is read from; empty leaves such a type empty.
/// @return The uses.
[[nodiscard]] MORPH_API std::vector<ActionUse> actionUses(Document const& document, Catalog const& catalog = {});

/// @brief One field of an action, as the explorer lists it.
struct ActionField {
    /// @brief The wire name.
    std::string name;
    /// @brief The type: the field kind's name, or for an enum "one of: …", a quantity "quantity in <unit>" with its
    ///        alternatives, a choice the action its options come from.
    std::string type;
    /// @brief Whether the action requires it.
    bool required = false;
    /// @brief The bounds and rules: `min …`, `max …`, `multiple of …`, `read-only`, `hidden`, and the name of each
    ///        `x-rules` rule that names the field.
    std::vector<std::string> rules;
};

/// @brief The fields of an action, in `x-order`, as the forms engine reads its schema.
/// @param schemaJson The action's schema, as `forms::schemaJson<A>()` emits it.
/// @return The rows; none for a schema without properties. Or why the schema cannot be read.
[[nodiscard]] MORPH_API std::expected<std::vector<ActionField>, std::string> actionFields(std::string_view schemaJson);

/// @brief Which widget "Try it" builds for an action.
enum class TryItKind : std::uint8_t {
    Form,        ///< A `form` over the action's schema.
    Collection,  ///< A `collection` over the catalog's view document of the action.
    Wizard,      ///< A `wizard` over the catalog's wizard document of the action.
};

/// @brief The screen id "Try it" mounts an action's document under.
/// @param actionId The action id.
/// @return `explorer:try:<action>`.
[[nodiscard]] inline std::string tryItScreenId(std::string_view actionId) {
    return "explorer:try:" + std::string{actionId};
}

/// @brief The one-node document "Try it" mounts for an action.
///
/// A private instance of the model under the alias `target`, the states `reply` and `failure`, and a column holding
/// the node over the action (test id `tryit`) and, for a form, two texts showing the states (`tryit_result`,
/// `tryit_error`). A form submits from its button only, and its `onSuccess` and `onError` set the states; a
/// collection and a wizard show their own results and errors.
/// @param actionId  The action id.
/// @param modelType The model type the action belongs to.
/// @param kind      The node.
/// @return The document's JSON text, with the id `tryItScreenId(actionId)`.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): an action id and a model type read alike, and neither reads.
[[nodiscard]] MORPH_API std::string tryItDocument(std::string_view actionId, std::string_view modelType,
                                                  TryItKind kind);

/// @brief A model's `schemas` document: `{actionType: schema}`, or why the server would not describe the model.
using ModelSchemas = std::expected<std::string, std::string>;

/// @brief The schemas documents of the models, by model type.
using SchemasByModel = std::map<std::string, ModelSchemas, std::less<>>;

/// @brief The actions a model's `schemas` document lists, with each one's schema.
/// @param document The document's JSON text.
/// @return The schemas by action id, or why the document cannot be read.
[[nodiscard]] MORPH_API std::expected<std::map<std::string, std::string, std::less<>>, std::string> schemasOf(
    std::string_view document);

/// @brief The catalog a "Try it" document loads with: @p catalog, whose `action` lookup also answers an action
/// only a
///        model's `schemas` document lists (load-time validation refuses an action the catalog does not know, and
///        the catalog knows only those a served document names), and whose `schema` lookup also answers from the
///        documents.
/// @param catalog The client's catalog.
/// @param schemas The models' `schemas` documents.
/// @return The catalog.
[[nodiscard]] MORPH_API Catalog tryItCatalog(Catalog catalog, SchemasByModel const& schemas);

/// @brief What an `Explorer` shows, and what it reports.
struct ExplorerOptions {
    /// @brief The catalog for the current principal: its entries, `permitted`, and the facts.
    CatalogReply catalog;
    /// @brief The client's catalog facts, which "Try it" documents load with; empty uses `catalog.facts()`.
    Catalog facts;
    /// @brief The `schemas` document of every model the catalog names, by model type.
    SchemasByModel schemas;
    /// @brief The loaded documents by screen id, the app document under `app`.
    std::map<std::string, std::shared_ptr<Document const>, std::less<>> documents;
    /// @brief What "Try it" mounts its document through. Borrowed; null offers no "Try it".
    DocumentScreenHost* host = nullptr;
    /// @brief Called with a "Try it" document before it is mounted, for the host's `documents` lookup to answer
    /// its
    ///        screen id (`tryItScreenId`) with; null offers no "Try it".
    std::function<void(std::string const& screenId, std::shared_ptr<Document const> document)> provide;
    /// @brief Called with a screen id when the person asks to open it.
    std::function<void(std::string const& screenId)> onOpen;
};

/// @brief The app explorer, as a host screen (spec 6 §15): a splitter with the list — a search, the catalog's
/// screens,
///        and every model the catalog names with every action of it — and the chosen action's sections: its facts,
///        its fields, the places that use it, and "Try it", the widget the framework builds for it, mounted live.
///
/// Built from backend widgets, so every frontend shows it. Its title is "Explorer"; it is never dirty, and closes
/// when asked. Neither copyable nor movable; owned by the owner thread of its backend.
///
/// Test ids: `explorer`, `explorer_search`, `explorer_screen_<id>`, `explorer_model_<type>`,
/// `explorer_action_<id>`, `explorer_title`, `explorer_facts`, `explorer_fields`, `explorer_uses`,
/// `explorer_open_<n>`, `explorer_tryit`, `explorer_tryit_status`, `explorer_schema`, and inside "Try it" the
/// generated document's own (`tryit`, …).
class MORPH_API Explorer final : public ScreenHandle {
public:
    /// @brief The explorer, appended to @p page.
    /// @param runtime The reactive runtime its title is computed in. Borrowed.
    /// @param backend The backend its widgets are made with. Borrowed.
    /// @param page    The container it is appended to; it outlives the explorer.
    /// @param options What it shows and reports.
    Explorer(reactive::Runtime& runtime MORPH_LIFETIMEBOUND, IViewBackend& backend MORPH_LIFETIMEBOUND,
             ContainerWidget& page MORPH_LIFETIMEBOUND, ExplorerOptions options);

    Explorer(Explorer const&) = delete;
    Explorer& operator=(Explorer const&) = delete;
    Explorer(Explorer&&) = delete;
    Explorer& operator=(Explorer&&) = delete;
    /// @brief Unmounts the "Try it" screen first, then the widgets it sits in.
    ~Explorer() override { clearDetail(); }

    /// @brief The title.
    /// @return "Explorer".
    [[nodiscard]] reactive::Computed<std::string> const& title() const override { return _title; }

    /// @brief Never dirty.
    /// @return `false`.
    [[nodiscard]] reactive::Computed<bool> const& dirty() const override { return _dirty; }

    /// @brief No identity: one explorer per workspace.
    /// @return `null`.
    [[nodiscard]] reactive::Computed<Value> const& identity() const override { return _identity; }

    /// @brief Closes when asked.
    /// @param done Called with `true`.
    void requestClose(std::function<void(bool closed)> done) override;

    /// @brief Closes the "Try it" screen.
    void forceClose() override { clearDetail(); }

    /// @brief Shows an action's sections, as choosing it in the list does.
    /// @param actionId The action id.
    /// @return Whether a listed model has the action.
    bool select(std::string const& actionId);

    /// @brief The action shown.
    /// @return Its id; empty with none.
    [[nodiscard]] std::string const& selected() const noexcept { return _selected; }

private:
    // The panes' sizes, in units: the list as wide as the shell's sidebar plus a margin, the detail the rest.
    static constexpr int kListWidth = 32;
    static constexpr int kDetailWidth = 96;
    // How much of an unreadable schema is shown.
    static constexpr std::size_t kShownBytes = 200;

    struct ActionEntry {
        std::string id;
        std::string schema;
        std::unique_ptr<ButtonWidget> button;
    };

    struct Model {
        std::string type;
        std::string refusal;
        std::unique_ptr<CollapsibleWidget> box;
        std::unique_ptr<StackWidget> column;
        std::unique_ptr<TextWidget> note;
        std::vector<ActionEntry> actions;
    };

    struct ScreenItem {
        std::string title;
        std::unique_ptr<ButtonWidget> button;
    };

    [[nodiscard]] static std::string lower(std::string_view text);

    [[nodiscard]] static std::string titleOf(CatalogEntry const& entry);

    // The model types the catalog names: its models and every action's model, in name order.
    [[nodiscard]] std::set<std::string, std::less<>> modelTypes() const;

    [[nodiscard]] std::unique_ptr<TextWidget> text(ContainerWidget& parent, std::string_view content,
                                                   TextRole role = TextRole::Normal) const;

    void listAll();

    // Shows what a search matches: a screen by its title, an action by its id, a model by its type or any of its
    // actions; a model with a matching action opens.
    void filter(std::string const& wanted);

    void open(std::string const& screenId) const;

    // Takes the detail's widgets in reverse, so each child goes before its parent; the "Try it" screen goes first.
    void clearDetail();

    // Keeps a widget of the detail until the next choice.
    template <class W>
    W& keep(std::unique_ptr<W> widget) {
        W& kept = *widget;
        _sections.push_back(std::move(widget));
        return kept;
    }

    void showNothingChosen();

    // A card with a title, its content in a column the caller fills.
    StackWidget& section(std::string_view title, std::string const& testId);

    // A grid of texts, @p columns wide: the first row a heading when @p headed, else the first column labels.
    void grid(StackWidget& parent, int columns, std::vector<std::string> const& cells, bool headed);

    [[nodiscard]] std::string_view permission(std::string const& actionId) const;

    // The catalog's facts of an action, as label and value cells; none when the catalog has none for it.
    [[nodiscard]] std::vector<std::string> catalogFacts(std::string const& actionId) const;

    // What an action declares it changes (spec 8 §5): why a screen did or did not update after it ran.
    [[nodiscard]] static std::string effectsText(ActionInfo const& info);

    // The records an action's result holds, each with its key field and where the result holds it.
    [[nodiscard]] static std::string recordsText(ActionInfo const& info);

    // The payload fingerprint and shape a schema carries, as label and value cells.
    [[nodiscard]] static std::vector<std::string> payloadFacts(std::string const& schema);

    void showFacts(Model const& model, ActionEntry const& action);

    // Returns whether the schema reads.
    bool showFields(ActionEntry const& action);

    void showUses(std::string const& actionId);

    [[nodiscard]] TryItKind tryItKind(std::string const& actionId) const;

    void showTryIt(Model const& model, ActionEntry const& action, bool readable);

    void showSchema(std::string const& actionId, ActionEntry const& action);

    void showAction(Model const& model, std::string const& actionId);

    IViewBackend* _backend;
    ExplorerOptions _options;
    reactive::Scope _scope;
    reactive::Computed<std::string>& _title;
    reactive::Computed<bool>& _dirty;
    reactive::Computed<Value>& _identity;
    std::set<std::string, std::less<>> _permitted;
    // Every use of each action, with the screen whose document names it.
    std::map<std::string, std::vector<std::pair<std::string, ActionUse>>, std::less<>> _uses;
    // Declared parent before child, so destruction takes the children first.
    std::unique_ptr<SplitterWidget> _root;
    std::unique_ptr<ScrollWidget> _listScroll;
    std::unique_ptr<StackWidget> _list;
    std::unique_ptr<TextInputWidget> _search;
    std::unique_ptr<TextWidget> _screensHeading;
    std::vector<ScreenItem> _screens;
    std::unique_ptr<TextWidget> _modelsHeading;
    std::vector<Model> _models;
    std::unique_ptr<ScrollWidget> _detailScroll;
    std::unique_ptr<StackWidget> _detail;
    // The detail's widgets, each after its parent, so clearing them in reverse takes the children first.
    std::vector<std::unique_ptr<Widget>> _sections;
    std::unique_ptr<ScreenHandle> _tryIt;
    std::string _selected;
};

}  // namespace morph::ui

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

namespace detail::explorer {

/// @brief The scopes around a node, innermost last: the aliases in its reach.
using ScopeChain = std::vector<ScopeDecl const*>;

/// @brief The model type an alias binds, from the innermost scope that declares it.
/// @param scopes The scopes around the use.
/// @param alias  The alias.
/// @return The type; empty when no scope declares the alias.
[[nodiscard]] inline std::string typeOf(ScopeChain const& scopes, std::string_view alias) {
    for (ScopeDecl const* const scope : scopes | std::views::reverse) {
        auto const found = std::ranges::find(scope->models, alias, &ModelDecl::name);
        if (found != scope->models.end()) {
            return found->type;
        }
    }
    return {};
}

/// @brief Records one use, its model type the alias's, else the catalog's for the action.
/// @param uses    Where it is recorded.
/// @param scopes  The scopes around the use.
/// @param catalog The facts a borrowed alias's type is read from.
/// @param alias   The alias the use goes through.
/// @param action  The action id.
/// @param kind    What names it.
/// @param pointer Its JSON Pointer.
inline void addUse(std::vector<ActionUse>& uses, ScopeChain const& scopes, Catalog const& catalog,
                   std::string_view alias, std::string action, ActionUseKind kind, std::string pointer) {
    std::string type = typeOf(scopes, alias);
    // A borrowed alias has no type of its own in the document: the catalog says which model has the action.
    if (type.empty() && catalog.action) {
        if (std::optional<ActionInfo> const info = catalog.action(action)) {
            type = info->modelType;
        }
    }
    uses.push_back(ActionUse{
        .actionId = std::move(action), .modelType = std::move(type), .kind = kind, .pointer = std::move(pointer)});
}

/// @brief Records the queries and mutations of the innermost scope.
/// @param uses    Where they are recorded.
/// @param scopes  The scopes, the one read last.
/// @param catalog The facts a borrowed alias's type is read from.
inline void scopeUses(std::vector<ActionUse>& uses, ScopeChain const& scopes, Catalog const& catalog) {
    ScopeDecl const& scope = *scopes.back();
    for (QueryDecl const& query : scope.queries) {
        addUse(uses, scopes, catalog, query.model, query.action, ActionUseKind::Query, query.path);
    }
    for (MutationDecl const& mutation : scope.mutations) {
        addUse(uses, scopes, catalog, mutation.model, mutation.action, ActionUseKind::Mutation, mutation.path);
    }
}

/// @brief Records the uses of a node and everything under it, with the scope it creates.
/// @param uses    Where they are recorded.
/// @param scopes  The scopes around the node; restored on return.
/// @param catalog The facts a borrowed alias's type is read from.
/// @param node    The node.
// NOLINTNEXTLINE(misc-no-recursion): a node's children are nodes.
inline void nodeUses(std::vector<ActionUse>& uses, ScopeChain& scopes, Catalog const& catalog, DocNode const& node) {
    if (node.scope) {
        scopes.push_back(&*node.scope);
        scopeUses(uses, scopes, catalog);
    }
    static constexpr std::array<std::pair<std::string_view, ActionUseKind>, 3> kNamed{
        {{"form", ActionUseKind::Form}, {"collection", ActionUseKind::Collection}, {"wizard", ActionUseKind::Wizard}}};
    for (auto const& [kind, use] : kNamed) {
        if (node.kind == kind) {
            addUse(uses, scopes, catalog, node.name("model"), std::string{node.name("action")}, use, node.path);
        }
    }
    for (auto const& [key, nodes] : node.children) {
        static_cast<void>(key);
        for (DocNodePtr const& child : nodes) {
            nodeUses(uses, scopes, catalog, *child);
        }
    }
    for (auto const& [key, parts] : node.parts) {
        static_cast<void>(key);
        for (DocNodePtr const& part : parts) {
            nodeUses(uses, scopes, catalog, *part);
        }
    }
    if (node.scope) {
        scopes.pop_back();
    }
}

}  // namespace detail::explorer

/// @brief Every place a loaded document names an action: its queries and mutations at any scope, and its `form`,
///        `collection` and `wizard` nodes, in document order.
///
/// A model type is the alias's, from the nearest enclosing scope that declares it. An alias that borrows another has
/// no type in the document, and takes the catalog's `ActionInfo::modelType`. A `run` command names a mutation, not an
/// action, so it is no use of its own.
/// @param document The document: a screen, or the app document.
/// @param catalog  The facts a borrowed alias's model type is read from; empty leaves such a type empty.
/// @return The uses.
[[nodiscard]] inline std::vector<ActionUse> actionUses(Document const& document, Catalog const& catalog = {}) {
    std::vector<ActionUse> uses;
    detail::explorer::ScopeChain scopes{&document.scope};
    detail::explorer::scopeUses(uses, scopes, catalog);
    if (document.view) {
        detail::explorer::nodeUses(uses, scopes, catalog, *document.view);
    }
    return uses;
}

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

namespace detail::explorer {

/// @brief Texts joined by a separator.
/// @param parts     The texts.
/// @param separator What goes between two.
/// @return The joined text.
[[nodiscard]] inline std::string joined(std::vector<std::string> const& parts, std::string_view separator) {
    std::string out;
    for (std::string const& part : parts) {
        out += out.empty() ? part : std::string{separator} + part;
    }
    return out;
}

/// @brief A field's type as the explorer lists it.
/// @param field The field, as the forms engine reads it.
/// @return The type.
[[nodiscard]] inline std::string typeText(forms::FieldSpec const& field) {
    using forms::FieldKind;
    if (field.kind == FieldKind::Enum) {
        std::vector<std::string> labels;
        labels.reserve(field.enumOptions.size());
        for (forms::ChoiceOption const& option : field.enumOptions) {
            labels.push_back(option.label);
        }
        return "one of: " + joined(labels, ", ");
    }
    if (field.kind == FieldKind::Quantity) {
        std::string const text = "quantity in " + (field.unit.empty() ? field.unitAscii : field.unit);
        std::vector<std::string> others;
        for (std::size_t index = 1; index < field.units.size(); ++index) {
            others.push_back(field.units.at(index).display);
        }
        return others.empty() ? text : text + " (or " + joined(others, ", ") + ")";
    }
    if (field.kind == FieldKind::Decimal && field.decimalPlaces) {
        return "decimal, " + std::to_string(*field.decimalPlaces) + " places";
    }
    if (field.kind == FieldKind::Choice && field.choice) {
        return "choice from " + field.choice->optionsAction;
    }
    if (field.kind == FieldKind::Array) {
        return "list of " + field.itemType;
    }
    if (field.kind == FieldKind::ObjectArray) {
        return "list of objects";
    }
    return std::string{forms::fieldKindName(field.kind)};
}

}  // namespace detail::explorer

/// @brief The fields of an action, in `x-order`, as the forms engine reads its schema.
/// @param schemaJson The action's schema, as `forms::schemaJson<A>()` emits it.
/// @return The rows; none for a schema without properties. Or why the schema cannot be read.
[[nodiscard]] inline std::expected<std::vector<ActionField>, std::string> actionFields(std::string_view schemaJson) {
    auto model = forms::FormModel::fromSchema("", schemaJson);
    if (!model) {
        std::string const& where = model.error().path;
        return std::unexpected(where.empty() ? model.error().message : where + ": " + model.error().message);
    }
    std::vector<ActionField> fields;
    for (forms::FieldSpec const& spec : model->fields()) {
        ActionField field{.name = spec.name, .type = detail::explorer::typeText(spec), .required = spec.required};
        if (auto const& minimum = spec.exactMinimum ? spec.exactMinimum : spec.minimum) {
            field.rules.push_back("min " + *minimum);
        }
        if (auto const& maximum = spec.exactMaximum ? spec.exactMaximum : spec.maximum) {
            field.rules.push_back("max " + *maximum);
        }
        if (spec.multipleOf) {
            field.rules.push_back("multiple of " + *spec.multipleOf);
        }
        if (spec.readOnly) {
            field.rules.emplace_back("read-only");
        }
        if (spec.hidden) {
            field.rules.emplace_back("hidden");
        }
        for (forms::RuleExpr const& rule : model->rules()) {
            if (rule.names(spec.name)) {
                field.rules.emplace_back(rule.kindName());
            }
        }
        fields.push_back(std::move(field));
    }
    return fields;
}

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
[[nodiscard]] inline std::string tryItDocument(std::string_view actionId, std::string_view modelType, TryItKind kind) {
    auto const quoted = [](std::string_view text) { return glz::write_json(std::string{text}).value_or("\"\""); };
    std::string const action = quoted(actionId);
    std::string node;
    if (kind == TryItKind::Form) {
        node = R"({"kind":"form","testId":"tryit","model":"target","action":)" + action +
               R"(,"submit":"explicit",
                     "onSuccess":[{"set":"reply","value":{"ref":"event"}},{"set":"failure","value":null}],
                     "onError":[{"set":"failure","value":{"ref":"event"}},{"set":"reply","value":null}]},
                   {"kind":"text","testId":"tryit_result","role":"success","text":{"ref":"reply"}},
                   {"kind":"text","testId":"tryit_error","role":"error","text":{"if":[
                     {"fn":"engaged","args":[{"ref":"failure"}]},
                     {"fn":"format","args":["Failed: {0}",{"ref":"failure"}]},""]}})";
    } else {
        node = std::string{R"({"kind":")"} + (kind == TryItKind::Collection ? "collection" : "wizard") +
               R"(","testId":"tryit","model":"target","action":)" + action + "}";
    }
    return R"({"vocab":"ui/1","id":)" + quoted(tryItScreenId(actionId)) + R"(,"title":)" +
           quoted("Try " + std::string{actionId}) + R"(,
      "models":{"target":{"type":)" +
           quoted(modelType) + R"(,"instance":"private"}},
      "state":{"reply":{"init":null},"failure":{"init":null}},
      "view":{"kind":"column","gap":3,"children":[)" +
           node + "]}}";
}

/// @brief A model's `schemas` document: `{actionType: schema}`, or why the server would not describe the model.
using ModelSchemas = std::expected<std::string, std::string>;

/// @brief The schemas documents of the models, by model type.
using SchemasByModel = std::map<std::string, ModelSchemas, std::less<>>;

/// @brief The actions a model's `schemas` document lists, with each one's schema.
/// @param document The document's JSON text.
/// @return The schemas by action id, or why the document cannot be read.
[[nodiscard]] inline std::expected<std::map<std::string, std::string, std::less<>>, std::string> schemasOf(
    std::string_view document) {
    auto parsed = parseJson(document);
    if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    auto const* const actions = std::get_if<Json::object_t>(&parsed->data);
    if (actions == nullptr) {
        return std::unexpected(std::string{"the schemas document is not an object"});
    }
    std::map<std::string, std::string, std::less<>> schemas;
    for (auto const& [action, schema] : *actions) {
        schemas.emplace(action, glz::write_json(schema).value_or(std::string{}));
    }
    return schemas;
}

/// @brief The catalog a "Try it" document loads with: @p catalog, whose `action` lookup also answers an action
/// only a
///        model's `schemas` document lists (load-time validation refuses an action the catalog does not know, and
///        the catalog knows only those a served document names), and whose `schema` lookup also answers from the
///        documents.
/// @param catalog The client's catalog.
/// @param schemas The models' `schemas` documents.
/// @return The catalog.
[[nodiscard]] inline Catalog tryItCatalog(Catalog catalog, SchemasByModel const& schemas) {
    auto const known = std::make_shared<std::map<std::string, std::pair<std::string, std::string>, std::less<>>>();
    for (auto const& [type, document] : schemas) {
        if (!document) {
            continue;
        }
        if (auto read = schemasOf(*document)) {
            for (auto& [action, schema] : *read) {
                known->emplace(action, std::pair{type, std::move(schema)});
            }
        }
    }
    catalog.action = [inner = std::move(catalog.action),
                      known](std::string_view actionId) -> std::optional<ActionInfo> {
        if (inner) {
            if (std::optional<ActionInfo> info = inner(actionId)) {
                return info;
            }
        }
        auto const found = known->find(actionId);
        if (found == known->end()) {
            return std::nullopt;
        }
        ActionInfo info;
        info.modelType = found->second.first;
        return info;
    };
    catalog.schema = [inner = std::move(catalog.schema),
                      known](std::string_view actionId) -> std::optional<std::string> {
        if (inner) {
            if (std::optional<std::string> schema = inner(actionId)) {
                return schema;
            }
        }
        auto const found = known->find(actionId);
        if (found == known->end()) {
            return std::nullopt;
        }
        return found->second.second;
    };
    return catalog;
}

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
class Explorer final : public ScreenHandle {
public:
    /// @brief The explorer, appended to @p page.
    /// @param runtime The reactive runtime its title is computed in. Borrowed.
    /// @param backend The backend its widgets are made with. Borrowed.
    /// @param page    The container it is appended to; it outlives the explorer.
    /// @param options What it shows and reports.
    Explorer(reactive::Runtime& runtime MORPH_LIFETIMEBOUND, IViewBackend& backend MORPH_LIFETIMEBOUND,
             ContainerWidget& page MORPH_LIFETIMEBOUND, ExplorerOptions options)
        : _backend{&backend},
          _options{std::move(options)},
          _scope{runtime},
          _title{_scope.computed([] { return std::string{"Explorer"}; })},
          _dirty{_scope.computed([] { return false; })},
          _identity{_scope.computed([] { return Value{}; })},
          _root{backend.createSplitter(&page, Axis::Horizontal)},
          _listScroll{backend.createScroll(_root.get(), Axis::Vertical)},
          _list{backend.createStack(_listScroll.get(), Axis::Vertical)},
          _detailScroll{backend.createScroll(_root.get(), Axis::Vertical)},
          _detail{backend.createStack(_detailScroll.get(), Axis::Vertical)} {
        if (!_options.facts.action && !_options.facts.model) {
            _options.facts = _options.catalog.facts();
        }
        _permitted.insert(_options.catalog.permitted.begin(), _options.catalog.permitted.end());
        for (auto const& [screenId, document] : _options.documents) {
            for (ActionUse& use : actionUses(*document, _options.facts)) {
                std::string action = use.actionId;
                _uses[std::move(action)].emplace_back(screenId, std::move(use));
            }
        }
        _root->setLayout({.width = Sizing::stretch(), .height = Sizing::stretch()});
        _root->setTestId("explorer");
        _root->setSizes({kListWidth, kDetailWidth});
        _listScroll->setLayout({.width = Sizing::stretch(), .height = Sizing::stretch()});
        _detailScroll->setLayout({.width = Sizing::stretch(), .height = Sizing::stretch()});
        _list->setGap(2);
        _list->setLayout({.width = Sizing::stretch(), .height = {}});
        _detail->setGap(4);
        _detail->setLayout({.width = Sizing::stretch(), .height = {}});
        _detail->setTestId("explorer_detail");
        listAll();
        showNothingChosen();
    }

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
    void requestClose(std::function<void(bool closed)> done) override {
        clearDetail();
        if (done) {
            done(true);
        }
    }

    /// @brief Closes the "Try it" screen.
    void forceClose() override { clearDetail(); }

    /// @brief Shows an action's sections, as choosing it in the list does.
    /// @param actionId The action id.
    /// @return Whether a listed model has the action.
    bool select(std::string const& actionId) {
        for (Model const& model : _models) {
            if (std::ranges::contains(model.actions, actionId, &ActionEntry::id)) {
                showAction(model, actionId);
                return true;
            }
        }
        return false;
    }

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

    [[nodiscard]] static std::string lower(std::string_view text) {
        std::string out{text};
        std::ranges::transform(out, out.begin(), [](char byte) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(byte)));
        });
        return out;
    }

    [[nodiscard]] static std::string titleOf(CatalogEntry const& entry) {
        if (std::string const* const text = std::get_if<std::string>(&entry.title.data)) {
            return *text;
        }
        if (auto const* const object = std::get_if<Json::object_t>(&entry.title.data)) {
            for (auto const& [key, value] : *object) {
                std::string const* const text = std::get_if<std::string>(&value.data);
                if (key == "fallback" && text != nullptr) {
                    return *text;
                }
            }
        }
        return entry.id;
    }

    // The model types the catalog names: its models and every action's model, in name order.
    [[nodiscard]] std::set<std::string, std::less<>> modelTypes() const {
        std::set<std::string, std::less<>> types;
        if (_options.catalog.models) {
            for (auto const& [type, info] : *_options.catalog.models) {
                static_cast<void>(info);
                types.insert(type);
            }
        }
        if (_options.catalog.actions) {
            for (auto const& [action, info] : *_options.catalog.actions) {
                static_cast<void>(action);
                if (!info.modelType.empty()) {
                    types.insert(info.modelType);
                }
            }
        }
        for (auto const& [type, document] : _options.schemas) {
            static_cast<void>(document);
            types.insert(type);
        }
        return types;
    }

    [[nodiscard]] std::unique_ptr<TextWidget> text(ContainerWidget& parent, std::string_view content,
                                                   TextRole role = TextRole::Normal) const {
        auto made = _backend->createText(&parent);
        made->setText(content);
        made->setRole(role);
        return made;
    }

    void listAll() {
        _search = _backend->createTextInput(_list.get(), TextInputMode::SingleLine);
        _search->setTestId("explorer_search");
        _search->setPlaceholder("Search");
        _search->setAccessibleName("Search screens and actions");
        _search->setOnChange([this](std::string const& wanted) { filter(wanted); });
        _screensHeading = text(*_list, {}, TextRole::Heading);
        for (CatalogEntry const& entry : _options.catalog.entries) {
            ScreenItem item{.title = titleOf(entry), .button = _backend->createButton(_list.get())};
            item.button->setLabel(item.title);
            item.button->setTestId("explorer_screen_" + entry.id);
            item.button->setEnabled(entry.available || entry.isPublic);
            item.button->setOnClick([this, screenId = entry.id] { open(screenId); });
            _screens.push_back(std::move(item));
        }
        _screensHeading->setText("Screens (" + std::to_string(_screens.size()) + ")");
        _modelsHeading = text(*_list, {}, TextRole::Heading);
        for (std::string const& type : modelTypes()) {
            Model model{.type = type,
                        .refusal = {},
                        .box = _backend->createCollapsible(_list.get()),
                        .column = {},
                        .note = {},
                        .actions = {}};
            model.box->setTitle(type);
            model.box->setTestId("explorer_model_" + type);
            model.column = _backend->createStack(model.box.get(), Axis::Vertical);
            model.column->setGap(1);
            auto const found = _options.schemas.find(type);
            if (found == _options.schemas.end()) {
                model.refusal = "not described: the schemas did not arrive";
            } else if (!found->second) {
                model.refusal = "not described: " + found->second.error();
            } else if (auto read = schemasOf(*found->second)) {
                for (auto& [action, schema] : *read) {
                    ActionEntry entry{.id = action, .schema = std::move(schema), .button = {}};
                    entry.button = _backend->createButton(model.column.get());
                    entry.button->setLabel(action);
                    entry.button->setTestId("explorer_action_" + action);
                    entry.button->setOnClick([this, id = action] { select(id); });
                    model.actions.push_back(std::move(entry));
                }
            } else {
                model.refusal = "not described: " + read.error();
            }
            if (!model.refusal.empty()) {
                model.note = text(*model.column, model.refusal, TextRole::Muted);
            }
            _models.push_back(std::move(model));
        }
        _modelsHeading->setText("Models (" + std::to_string(_models.size()) + ")");
    }

    // Shows what a search matches: a screen by its title, an action by its id, a model by its type or any of its
    // actions; a model with a matching action opens.
    void filter(std::string const& wanted) {
        std::string const needle = lower(wanted);
        auto const matches = [&needle](std::string_view text) {
            return needle.empty() || lower(text).contains(needle);
        };
        for (ScreenItem& item : _screens) {
            item.button->setVisible(matches(item.title));
        }
        for (Model& model : _models) {
            bool const typeMatches = matches(model.type);
            bool any = false;
            for (ActionEntry& action : model.actions) {
                bool const shown = typeMatches || matches(action.id);
                action.button->setVisible(shown);
                any = any || shown;
            }
            model.box->setVisible(typeMatches || any);
            if (!needle.empty() && any) {
                model.box->setOpen(true);
            }
        }
    }

    void open(std::string const& screenId) const {
        if (_options.onOpen) {
            _options.onOpen(screenId);
        }
    }

    // Takes the detail's widgets in reverse, so each child goes before its parent; the "Try it" screen goes first.
    void clearDetail() {
        _tryIt.reset();
        while (!_sections.empty()) {
            _sections.pop_back();
        }
    }

    // Keeps a widget of the detail until the next choice.
    template <class W>
    W& keep(std::unique_ptr<W> widget) {
        W& kept = *widget;
        _sections.push_back(std::move(widget));
        return kept;
    }

    void showNothingChosen() {
        clearDetail();
        _selected.clear();
        keep(text(*_detail, "Choose an action to see its facts, its fields, where it is used, and to try it.",
                  TextRole::Muted))
            .setTestId("explorer_hint");
    }

    // A card with a title, its content in a column the caller fills.
    StackWidget& section(std::string_view title, std::string const& testId) {
        PanelWidget& panel = keep(_backend->createPanel(_detail.get()));
        panel.setTitle(title);
        panel.setTestId(testId);
        StackWidget& column = keep(_backend->createStack(&panel, Axis::Vertical));
        column.setGap(2);
        return column;
    }

    // A grid of texts, @p columns wide: the first row a heading when @p headed, else the first column labels.
    void grid(StackWidget& parent, int columns, std::vector<std::string> const& cells, bool headed) {
        GridWidget& made = keep(_backend->createGrid(&parent));
        made.setColumns(columns);
        made.setGap(2);
        made.setLayout({.width = Sizing::stretch(), .height = {}});
        auto const width = static_cast<std::size_t>(columns);
        for (std::size_t index = 0; index < cells.size(); ++index) {
            bool const muted = headed ? index < width : index % width == 0;
            keep(text(made, cells.at(index), muted ? TextRole::Muted : TextRole::Normal));
        }
    }

    [[nodiscard]] std::string_view permission(std::string const& actionId) const {
        if (_permitted.contains(actionId)) {
            return "permitted";
        }
        // The catalog's `permitted` covers only actions a served document names.
        bool const named = _options.catalog.actions && _options.catalog.actions->contains(actionId);
        return named ? "not permitted" : "decided when it runs";
    }

    // The catalog's facts of an action, as label and value cells; none when the catalog has none for it.
    [[nodiscard]] std::vector<std::string> catalogFacts(std::string const& actionId) const {
        if (!_options.catalog.actions || !_options.catalog.actions->contains(actionId)) {
            return {};
        }
        ActionInfo const& info = _options.catalog.actions->at(actionId);
        auto const listed = [](std::vector<std::string> const& names) {
            return names.empty() ? std::string{"—"} : detail::explorer::joined(names, ", ");
        };
        std::vector<std::string> cells{"sends a credential",
                                       info.credential ? "yes" : "no",
                                       "server-calculated",
                                       listed(info.calculatedFields),
                                       "secret",
                                       listed(info.secretFields)};
        if (info.listFields) {
            cells.insert(cells.end(), {"list fields", listed(*info.listFields)});
        }
        return cells;
    }

    // The payload fingerprint and shape a schema carries, as label and value cells.
    [[nodiscard]] static std::vector<std::string> payloadFacts(std::string const& schema) {
        std::vector<std::string> cells;
        auto parsed = parseJson(schema);
        auto const* const object = parsed ? std::get_if<Json::object_t>(&parsed->data) : nullptr;
        if (object == nullptr) {
            return cells;
        }
        for (auto const& [key, value] : *object) {
            if (key != "x-payloadFingerprint" && key != "x-payloadShape") {
                continue;
            }
            std::string const* const plain = std::get_if<std::string>(&value.data);
            cells.emplace_back(key == "x-payloadFingerprint" ? "payload fingerprint" : "payload shape");
            cells.push_back(plain != nullptr ? *plain : glz::write_json(value).value_or(std::string{}));
        }
        return cells;
    }

    void showFacts(Model const& model, ActionEntry const& action) {
        std::vector<std::string> cells{"model", model.type};
        std::ranges::move(catalogFacts(action.id), std::back_inserter(cells));
        std::ranges::move(payloadFacts(action.schema), std::back_inserter(cells));
        cells.insert(cells.end(), {"may run it", std::string{permission(action.id)}});
        grid(section("Facts", "explorer_facts"), 2, cells, false);
    }

    // Returns whether the schema reads.
    bool showFields(ActionEntry const& action) {
        StackWidget& content = section("Fields", "explorer_fields");
        auto const fields = actionFields(action.schema);
        if (!fields) {
            keep(text(content, "unreadable schema: " + fields.error(), TextRole::Error));
            keep(text(content, action.schema.substr(0, kShownBytes), TextRole::Muted));
            return false;
        }
        if (fields->empty()) {
            keep(text(content, "No fields.", TextRole::Muted));
            return true;
        }
        std::vector<std::string> cells{"name", "type", "required", "rules"};
        for (ActionField const& field : *fields) {
            cells.insert(cells.end(),
                         {field.name, field.type, field.required ? "required" : "optional",
                          field.rules.empty() ? std::string{"—"} : detail::explorer::joined(field.rules, ", ")});
        }
        grid(content, 4, cells, true);
        return true;
    }

    void showUses(std::string const& actionId) {
        StackWidget& content = section("Used by", "explorer_uses");
        auto const found = _uses.find(actionId);
        if (found == _uses.end()) {
            keep(text(content, "No loaded document names it.", TextRole::Muted));
            return;
        }
        std::size_t index = 0;
        for (auto const& [screenId, use] : found->second) {
            StackWidget& row = keep(_backend->createStack(&content, Axis::Horizontal));
            row.setGap(3);
            keep(text(row, screenId + "  " + use.pointer + "  (" + std::string{actionUseKindName(use.kind)} + ")"))
                .setLayout({.width = Sizing::stretch(), .height = {}});
            // The app document is no screen to open.
            if (screenId != "app") {
                ButtonWidget& button = keep(_backend->createButton(&row));
                button.setLabel("Open");
                button.setTestId("explorer_open_" + std::to_string(index));
                button.setOnClick([this, target = screenId] { open(target); });
            }
            ++index;
        }
    }

    [[nodiscard]] TryItKind tryItKind(std::string const& actionId) const {
        if (_options.catalog.views && _options.catalog.views->contains(actionId)) {
            return TryItKind::Collection;
        }
        if (_options.catalog.wizards && _options.catalog.wizards->contains(actionId)) {
            return TryItKind::Wizard;
        }
        return TryItKind::Form;
    }

    void showTryIt(Model const& model, ActionEntry const& action, bool readable) {
        StackWidget& content = section("Try it", "explorer_tryit");
        auto const status = [this, &content](std::string const& reason) {
            keep(text(content, reason, TextRole::Muted)).setTestId("explorer_tryit_status");
        };
        if (_options.host == nullptr || !_options.provide) {
            status("Not offered here.");
            return;
        }
        if (!readable) {
            status("Not offered: the schema cannot be read.");
            return;
        }
        if (permission(action.id) == "not permitted") {
            status("not permitted");
            return;
        }
        std::string const screenId = tryItScreenId(action.id);
        auto loaded = loadDocumentText(tryItDocument(action.id, model.type, tryItKind(action.id)),
                                       LoadOptions{.catalog = tryItCatalog(_options.facts, _options.schemas)});
        if (!loaded) {
            status("Cannot build it: " + loaded.error().message());
            return;
        }
        _options.provide(screenId, std::make_shared<Document const>(std::move(*loaded)));
        SlotWidget& slot = keep(_backend->createSlot(&content));
        slot.setLayout({.width = Sizing::stretch(), .height = {}});
        auto mounted = _options.host->mountInto(screenId, {}, slot);
        if (!mounted) {
            status("Cannot mount it: " + mounted.error().reason);
            return;
        }
        _tryIt = std::move(*mounted);
    }

    void showSchema(std::string const& actionId, ActionEntry const& action) {
        CollapsibleWidget& box = keep(_backend->createCollapsible(_detail.get()));
        box.setTitle("Schema JSON");
        box.setTestId("explorer_schema");
        StackWidget& column = keep(_backend->createStack(&box, Axis::Vertical));
        column.setGap(2);
        keep(text(column, action.schema, TextRole::Muted));
        for (auto const* const documents : {&_options.catalog.views, &_options.catalog.wizards}) {
            if (!*documents) {
                continue;
            }
            if (auto const found = (*documents)->find(actionId); found != (*documents)->end()) {
                keep(text(column, found->second, TextRole::Muted));
            }
        }
    }

    void showAction(Model const& model, std::string const& actionId) {
        clearDetail();
        _selected = actionId;
        auto const& action = *std::ranges::find(model.actions, actionId, &ActionEntry::id);
        keep(text(*_detail, model.type + " / " + actionId, TextRole::Heading)).setTestId("explorer_title");
        showFacts(model, action);
        bool const readable = showFields(action);
        showUses(actionId);
        showTryIt(model, action, readable);
        showSchema(actionId, action);
    }

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

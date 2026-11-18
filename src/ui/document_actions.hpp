// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <functional>
#include <map>
#include <morph/ui/document.hpp>
#include <morph/ui/service.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morph::ui::detail {

// NOLINTBEGIN(misc-no-recursion): view nodes nest, and the walk follows them; parseJson bounds the depth at 64.

/// @brief The model types the model aliases open at one point of a document name, innermost declaration winning.
class ModelAliases {
public:
    /// @brief Opens @p scope's aliases.
    /// @param scope A scope being entered.
    void enter(ScopeDecl const& scope) {
        for (ModelDecl const& model : scope.models) {
            _types[model.name].push_back(model.type);
        }
    }

    /// @brief Closes the aliases `enter(scope)` opened.
    /// @param scope The scope being left.
    void leave(ScopeDecl const& scope) {
        for (ModelDecl const& model : scope.models) {
            _types.at(model.name).pop_back();
        }
    }

    /// @brief The type the innermost open alias named @p alias names. Load-time validation requires every alias to
    ///        be declared, so an undeclared one cannot reach here; it throws rather than resolve to a made-up type.
    /// @param alias An alias name.
    /// @return Its type.
    /// @throws std::logic_error When no open alias is named @p alias.
    [[nodiscard]] std::string resolve(std::string_view alias) const {
        auto const found = _types.find(alias);
        if (found == _types.end() || found->second.empty()) {
            throw std::logic_error{"the model alias `" + std::string{alias} + "` is not declared"};
        }
        return found->second.back();
    }

private:
    std::map<std::string, std::vector<std::string>, std::less<>> _types;
};

/// @brief One place a document names an action: the action, the kind of position it is named in, and where.
struct ActionUse {
    /// @brief The action and its model type.
    ActionRef action;
    /// @brief The position's kind: `query`, `evaluate`, `mutation`, the kind of a model-bound node (`collection`, `wizard`,
    ///        `table`), or for a form its literal `submit`: `form` when absent, `explicit form`, `auto form`.
    std::string position;
    /// @brief The JSON Pointer of the query, mutation or node.
    std::string path;
};

/// @brief Collects the actions and model types a loaded document names, resolving each query's, mutation's and
///        model-bound node's alias through the enclosing scopes, and where each action is first named.
///
/// Load-time validation has already resolved every alias and requires every alias to name a type, so each lookup
/// here finds one. Component fallbacks are view trees like any other and are walked with the view.
class DocumentActions {
public:
    /// @brief The position kind of a form whose literal `submit` is @p submit. Without a catalog the schema's submit
    ///        mode is unknown, so a form with no `submit` is a position of its own.
    /// @param submit The form's `submit`, empty when absent.
    /// @return `form`, `explicit form` or `auto form`.
    [[nodiscard]] static std::string formPosition(std::string_view submit) {
        if (submit.empty()) {
            return "form";
        }
        return submit == "explicit" ? "explicit form" : "auto form";
    }

    /// @brief Walks @p document.
    /// @param document The loaded document.
    explicit DocumentActions(Document const& document) {
        enter(document.scope, true);
        if (document.view) {
            node(*document.view);
        }
        for (auto const& [name, component] : document.components) {
            static_cast<void>(name);
            if (component.fallback) {
                node(*component.fallback);
            }
        }
    }

    /// @brief The top-level queries.
    /// @return Them, in declaration order.
    [[nodiscard]] std::vector<ActionRef> const& topQueries() const noexcept { return _top; }

    /// @brief Every action named.
    /// @return Them, sorted and unique.
    [[nodiscard]] std::vector<ActionRef> actions() const { return {_all.begin(), _all.end()}; }

    /// @brief Every model type named.
    /// @return Them, sorted and unique.
    [[nodiscard]] std::vector<std::string> modelTypes() const { return {_models.begin(), _models.end()}; }

    /// @brief Every place an action is named, in the order walked.
    /// @return The uses; an action named twice in the same kind of position is listed once, at its first place.
    [[nodiscard]] std::vector<ActionUse> const& uses() const noexcept { return _uses; }

    /// @brief Whether @p ref is named in a position of kind @p position.
    /// @param ref      An action.
    /// @param position A position kind, as `ActionUse::position` spells it.
    /// @return Whether it is.
    [[nodiscard]] bool names(ActionRef const& ref, std::string const& position) const {
        return _positions.contains({ref, position});
    }

    /// @brief Where @p ref is first named: the JSON Pointer of its query, mutation or model-bound node.
    /// @param ref An action `actions()` lists.
    /// @return The pointer; empty for an action the document does not name.
    [[nodiscard]] std::string const& pathOf(ActionRef const& ref) const {
        static std::string const none;
        auto const found = _paths.find(ref);
        return found == _paths.end() ? none : found->second;
    }

private:
    void name(ActionRef ref, std::string const& position, std::string const& path) {
        if (_positions.emplace(ref, position).second) {
            _uses.push_back(ActionUse{.action = ref, .position = position, .path = path});
        }
        _paths.try_emplace(ref, path);
        _all.insert(std::move(ref));
    }

    void enter(ScopeDecl const& scope, bool top) {
        _aliases.enter(scope);
        for (ModelDecl const& model : scope.models) {
            _models.insert(model.type);
        }
        for (QueryDecl const& query : scope.queries) {
            ActionRef ref{.modelType = resolve(query.model), .action = query.action};
            if (top) {
                _top.push_back(ref);
            }
            // An evaluate is a dry run of the action's checks: a position of its own, since a call there would run
            // the action's handler.
            name(std::move(ref), query.evaluate ? "evaluate" : "query", query.path);
        }
        for (MutationDecl const& mutation : scope.mutations) {
            name(ActionRef{.modelType = resolve(mutation.model), .action = mutation.action}, "mutation",
                 mutation.path);
        }
    }

    void leave(ScopeDecl const& scope) { _aliases.leave(scope); }

    void node(DocNode const& view) {
        if (view.scope) {
            enter(*view.scope, false);
        }
        if (std::string_view const model = view.name("model"); !model.empty()) {
            std::string position = view.kind;
            if (view.kind == "form") {
                position = formPosition(view.name("submit"));
            }
            name(ActionRef{.modelType = resolve(model), .action = std::string{view.name("action")}}, position,
                 view.path);
        }
        for (auto const& [key, children] : view.children) {
            static_cast<void>(key);
            for (DocNodePtr const& child : children) {
                node(*child);
            }
        }
        for (auto const& [key, parts] : view.parts) {
            static_cast<void>(key);
            for (DocNodePtr const& part : parts) {
                node(*part);
            }
        }
        if (view.scope) {
            leave(*view.scope);
        }
    }

    [[nodiscard]] std::string resolve(std::string_view alias) const { return _aliases.resolve(alias); }

    ModelAliases _aliases;
    std::vector<ActionRef> _top;
    std::set<ActionRef> _all;
    std::map<ActionRef, std::string> _paths;
    std::vector<ActionUse> _uses;
    std::set<std::pair<ActionRef, std::string>> _positions;
    std::set<std::string> _models;
};

// NOLINTEND(misc-no-recursion)

}  // namespace morph::ui::detail

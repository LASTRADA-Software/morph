// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <functional>
#include <map>
#include <morph/ui/document.hpp>
#include <morph/ui/service.hpp>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morph::ui::detail {

// NOLINTBEGIN(misc-no-recursion): view nodes nest, and the walk follows them; parseJson bounds the depth at 64.

/// @brief One place a document names an action: the action, the kind of position it is named in, and where.
struct ActionUse {
    /// @brief The action and its model type.
    ActionRef action;
    /// @brief The position's kind: `query`, `mutation`, the kind of a model-bound node (`form`, `collection`,
    ///        `wizard`, `table`), or `auto form` for a form that submits itself.
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
        for (ModelDecl const& model : scope.models) {
            _types.try_emplace(model.name).first->second.push_back(model.type);
            _models.insert(model.type);
        }
        for (QueryDecl const& query : scope.queries) {
            ActionRef ref{.modelType = resolve(query.model), .action = query.action};
            if (top) {
                _top.push_back(ref);
            }
            name(std::move(ref), "query", query.path);
        }
        for (MutationDecl const& mutation : scope.mutations) {
            name(ActionRef{.modelType = resolve(mutation.model), .action = mutation.action}, "mutation", mutation.path);
        }
    }

    void leave(ScopeDecl const& scope) {
        for (ModelDecl const& model : scope.models) {
            _types.at(model.name).pop_back();
        }
    }

    void node(DocNode const& view) {
        if (view.scope) {
            enter(*view.scope, false);
        }
        if (std::string_view const model = view.name("model"); !model.empty()) {
            std::string position = view.kind;
            if (view.kind == "form" && view.name("submit") == "auto") {
                position = "auto form";
            }
            name(ActionRef{.modelType = resolve(model), .action = std::string{view.name("action")}}, position, view.path);
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

    /// @brief The type of the innermost alias named @p alias. Load-time validation has resolved it already, so the
    ///        lookup cannot miss; `at` throws rather than read past the end if that ever changes.
    [[nodiscard]] std::string const& resolve(std::string_view alias) const {
        std::vector<std::string> const& types = _types.at(std::string{alias});
        return types.at(types.size() - 1);
    }

    /// @brief The types an alias names in the open scopes, innermost last.
    std::map<std::string, std::vector<std::string>, std::less<>> _types;
    std::vector<ActionRef> _top;
    std::set<ActionRef> _all;
    std::map<ActionRef, std::string> _paths;
    std::vector<ActionUse> _uses;
    std::set<std::pair<ActionRef, std::string>> _positions;
    std::set<std::string> _models;
};

// NOLINTEND(misc-no-recursion)

}  // namespace morph::ui::detail

// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <functional>
#include <map>
#include <morph/ui/document.hpp>
#include <morph/ui/service.hpp>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace morph::ui::detail {

// NOLINTBEGIN(misc-no-recursion): view nodes nest, and the walk follows them; parseJson bounds the depth at 64.

/// @brief Collects the actions and model types a loaded document names, resolving each query's, mutation's and
///        model-bound node's alias through the enclosing scopes, and where each action is first named.
///
/// Load-time validation has already resolved every alias and requires every alias to name a type, so each lookup
/// here finds one.
class DocumentActions {
public:
    /// @brief Walks @p document.
    /// @param document The loaded document.
    explicit DocumentActions(Document const& document) {
        enter(document.scope, true);
        if (document.view) {
            node(*document.view);
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

    /// @brief Where @p ref is first named: the JSON Pointer of its query, mutation or model-bound node.
    /// @param ref An action `actions()` lists.
    /// @return The pointer; empty for an action the document does not name.
    [[nodiscard]] std::string const& pathOf(ActionRef const& ref) const {
        static std::string const none;
        auto const found = _paths.find(ref);
        return found == _paths.end() ? none : found->second;
    }

private:
    void name(ActionRef ref, std::string const& path) {
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
            name(std::move(ref), query.path);
        }
        for (MutationDecl const& mutation : scope.mutations) {
            name(ActionRef{.modelType = resolve(mutation.model), .action = mutation.action}, mutation.path);
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
            name(ActionRef{.modelType = resolve(model), .action = std::string{view.name("action")}}, view.path);
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
    std::set<std::string> _models;
};

// NOLINTEND(misc-no-recursion)

}  // namespace morph::ui::detail

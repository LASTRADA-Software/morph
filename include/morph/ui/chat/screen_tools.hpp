// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../document.hpp"
#include "../value.hpp"
#include "protocol.hpp"
#include "provider.hpp"

/// @file
/// @brief An assistant's tools, from the screen it is on. Specified in `docs/spec/ui/chat.md`.

namespace morph::ui::chat {

/// @brief A tool, and what calling it runs.
struct BoundTool {
    /// @brief What the model is offered.
    ToolSpec spec;
    /// @brief The alias and instance it runs on.
    ModelRef model;
    /// @brief The action's registered type id.
    std::string action;
};

namespace detail {

// A tool name's limit, the strictest a provider's tool format is known to set.
inline constexpr std::size_t kMaxToolName = 64;
inline constexpr std::size_t kToolHashDigits = 8;

[[nodiscard]] inline bool validToolName(std::string_view name) {
    return !name.empty() && name.size() <= kMaxToolName && std::ranges::all_of(name, [](char const letter) {
        return (letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z') ||
               (letter >= '0' && letter <= '9') || letter == '_' || letter == '-';
    });
}

// FNV-1a: stable across runs, so a name a conversation has already used keeps its meaning.
[[nodiscard]] inline std::string toolHash(std::string_view text) {
    std::uint32_t hash = 2166136261U;
    for (char const letter : text) {
        hash ^= static_cast<std::uint8_t>(letter);
        hash *= 16777619U;
    }
    static constexpr std::array<char, 16> kDigits = {'0', '1', '2', '3', '4', '5', '6', '7',
                                                     '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string out(kToolHashDigits, '0');
    for (std::size_t index = kToolHashDigits; index > 0; --index) {
        out.at(index - 1) = kDigits.at(hash & 0xFU);
        hash >>= 4U;
    }
    return out;
}

// A stem made valid and short enough to take the hash's suffix.
[[nodiscard]] inline std::string hashedToolName(std::string const& stem, std::string_view identity) {
    std::string made = stem.substr(0, kMaxToolName - kToolHashDigits - 1);
    std::ranges::replace_if(made, [](char const letter) { return !validToolName(std::string_view{&letter, 1}); }, '_');
    return made + '_' + toolHash(identity);
}

// One action a document names on one alias.
struct NamedAction {
    ModelDecl const* model = nullptr;
    std::string action;
    bool changes = false;
};

// NOLINTBEGIN(misc-no-recursion): view nodes nest; parseJson bounds the depth at 64.
// Collects what a document names, resolving each alias through the enclosing scopes as loading did.
class ScreenWalk {
public:
    explicit ScreenWalk(Document const& document) {
        enter(document.scope);
        if (document.view) {
            node(*document.view);
        }
    }

    [[nodiscard]] std::vector<NamedAction> const& named() const noexcept { return _named; }

private:
    void enter(ScopeDecl const& scope) {
        for (ModelDecl const& model : scope.models) {
            _aliases[model.name].push_back(&model);
        }
        for (QueryDecl const& query : scope.queries) {
            add(query.model, query.action, false);
        }
        for (MutationDecl const& mutation : scope.mutations) {
            add(mutation.model, mutation.action, true);
        }
    }

    void leave(ScopeDecl const& scope) {
        for (ModelDecl const& model : scope.models) {
            _aliases.at(model.name).pop_back();
        }
    }

    void node(DocNode const& view) {
        if (view.scope) {
            enter(*view.scope);
        }
        // A model-bound node such as a form submits its action, so it changes data.
        if (std::string_view const model = view.name("model"); !model.empty()) {
            add(model, view.name("action"), true);
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

    void add(std::string_view alias, std::string_view action, bool changes) {
        auto const found = _aliases.find(alias);
        if (action.empty() || found == _aliases.end() || found->second.empty()) {
            return;
        }
        ModelDecl const* const model = found->second.back();
        auto const seen = std::ranges::find_if(
            _named, [&](NamedAction const& named) { return named.model == model && named.action == action; });
        if (seen != _named.end()) {
            seen->changes = seen->changes || changes;
            return;
        }
        _named.push_back(NamedAction{.model = model, .action = std::string{action}, .changes = changes});
    }

    std::map<std::string, std::vector<ModelDecl const*>, std::less<>> _aliases;
    std::vector<NamedAction> _named;
};
// NOLINTEND(misc-no-recursion)

[[nodiscard]] inline Json toolSchema(std::map<std::string, Json, std::less<>> const& schemas,
                                     std::string const& action) {
    auto const found = schemas.find(action);
    Json schema = found == schemas.end() ? Json(Json::object_t{}) : found->second;
    if (!std::holds_alternative<Json::object_t>(schema.data)) {
        schema = Json::object_t{};
    }
    std::get<Json::object_t>(schema.data).insert_or_assign("type", "object");
    return schema;
}

[[nodiscard]] inline std::string toolDescription(NamedAction const& named, Json const& schema) {
    std::string description = (named.changes ? "Changes data: runs " : "Reads data: runs ") + named.action + " on " +
                              named.model->name + ".";
    Json::object_t const& object = std::get<Json::object_t>(schema.data);
    if (auto const found = object.find("description"); found != object.end()) {
        if (auto const* const text = std::get_if<std::string>(&found->second.data)) {
            description += ' ' + *text;
        }
    }
    return description;
}

}  // namespace detail

/// @brief The tools of one screen: each query, mutation and model-bound node the document names, at any scope, whose
///        alias the context bound and whose action the principal may run.
///
/// A tool is named `<alias>__<action>`. A name a provider's tool format would refuse, or one a tool already took, gets
/// a shortened stem and a hash of its scope, alias and action, so it stays valid and unique. The tools are sorted by
/// name, and an action named more than once on one alias gives one tool, offered as changing data if any naming runs
/// it as a mutation. A credential action gets no tool: a model should not be handed a password to pass along.
/// @param document  The screen's document, loaded.
/// @param models    The aliases the client says it holds. One counts only if its scope, alias and type all match the
///                  document's declaration, so a context cannot point a tool at a model the screen does not declare.
/// @param permitted The actions the principal may run.
/// @param schemas   Each action's schema, by action id.
/// @param actions   The catalog's action facts, when there are any.
/// @return The tools.
[[nodiscard]] inline std::vector<BoundTool> screenTools(
    Document const& document, std::span<ModelRef const> models, std::set<std::string, std::less<>> const& permitted,
    std::map<std::string, Json, std::less<>> const& schemas,
    std::map<std::string, ActionInfo, std::less<>> const* actions = nullptr) {
    struct Candidate {
        std::string stem;
        std::string identity;
        detail::NamedAction const* named = nullptr;
        ModelRef const* model = nullptr;
    };
    detail::ScreenWalk const walk{document};
    std::vector<Candidate> candidates;
    for (detail::NamedAction const& named : walk.named()) {
        if (!permitted.contains(named.action)) {
            continue;
        }
        if (actions != nullptr) {
            if (auto const facts = actions->find(named.action); facts != actions->end() && facts->second.credential) {
                continue;
            }
        }
        auto const bound = std::ranges::find_if(models, [&](ModelRef const& model) {
            return model.alias == named.model->name && model.path == named.model->path &&
                   model.modelType == named.model->type;
        });
        if (bound == models.end()) {
            continue;
        }
        candidates.push_back(Candidate{.stem = named.model->name + "__" + named.action,
                                       .identity = named.model->path + '/' + named.model->name + '/' + named.action,
                                       .named = &named,
                                       .model = &*bound});
    }
    // Sorted by a stable identity first, so which of two colliding tools keeps the plain name does not depend on
    // order.
    std::ranges::sort(candidates, [](Candidate const& left, Candidate const& right) {
        return std::pair{left.stem, left.identity} < std::pair{right.stem, right.identity};
    });
    std::set<std::string, std::less<>> taken;
    std::vector<BoundTool> tools;
    tools.reserve(candidates.size());
    for (Candidate const& candidate : candidates) {
        std::string name = candidate.stem;
        if (!detail::validToolName(name) || taken.contains(name)) {
            name = detail::hashedToolName(candidate.stem, candidate.identity);
        }
        taken.insert(name);
        Json schema = detail::toolSchema(schemas, candidate.named->action);
        std::string description = detail::toolDescription(*candidate.named, schema);
        tools.push_back(BoundTool{.spec = ToolSpec{.name = std::move(name),
                                                   .description = std::move(description),
                                                   .inputSchema = std::move(schema)},
                                  .model = *candidate.model,
                                  .action = candidate.named->action});
    }
    std::ranges::sort(tools, {}, [](BoundTool const& tool) { return tool.spec.name; });
    return tools;
}

}  // namespace morph::ui::chat

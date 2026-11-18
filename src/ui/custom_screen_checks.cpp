// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <morph/ui/custom_screens.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/value.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "document_actions.hpp"

namespace morph::ui {
namespace {

[[nodiscard]] Refusal refusalAt(std::string path, std::string reason) {
    return Refusal{.document = {}, .path = std::move(path), .reason = std::move(reason)};
}

// NOLINTBEGIN(misc-no-recursion): view nodes, command lists and JSON nest; parseJson bounds the depth at 64.

/// @brief Everything a document can cause that the base rule compares, each as a capability string, with the JSON
///        Pointer where the document first uses it.
class Capabilities {
public:
    /// @brief Walks @p document and its JSON @p json.
    /// @param document The loaded document.
    /// @param json     Its parsed JSON, for the `host` names.
    Capabilities(Document const& document, Json const& json) {
        enter(document.scope);
        Table const& top = *_open.back();
        for (auto const& [trigger, commands] : document.lifecycle) {
            walk(commands, trigger, top);
        }
        for (auto const& [chord, commands] : document.keys) {
            static_cast<void>(chord);
            walk(commands, {}, top);
        }
        for (auto const* parts :
             {&document.status, &document.actions, &document.menu, &document.tools, &document.navigation}) {
            for (DocNodePtr const& part : *parts) {
                node(*part);
            }
        }
        if (document.view) {
            node(*document.view);
        }
        for (auto const& [name, component] : document.components) {
            if (!component.qml.empty()) {
                note("component `" + name + "` " + component.qml, component.path);
            }
            if (component.fallback) {
                node(*component.fallback);
            }
        }
        leave(document.scope);
        hostNames(json, "");
    }

    /// @brief Every capability, in the order first used, with its pointer.
    /// @return The uses.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> const& uses() const noexcept { return _uses; }

    /// @brief Whether the document uses @p capability.
    /// @param capability The capability string.
    /// @return Whether it does.
    [[nodiscard]] bool has(std::string const& capability) const { return _seen.contains(capability); }

private:
    struct Table;

    /// @brief A mutation resolved where it is declared: its model type, and the names open in its declaring scope,
    ///        which its `onSuccess` and `onError` run against.
    struct Mutation {
        MutationDecl const* decl;
        std::string type;
        Table const* table;
    };

    /// @brief The mutations a scope can name: its own and every enclosing scope's, the innermost winning.
    struct Table {
        std::map<std::string, Mutation const*, std::less<>> mutations;
    };

    void note(std::string capability, std::string const& path) {
        if (_seen.insert(capability).second) {
            _uses.emplace_back(std::move(capability), path);
        }
    }

    void enter(ScopeDecl const& scope) {
        for (ModelDecl const& model : scope.models) {
            _types[model.name].push_back(model.type);
        }
        Table& table = _tables.emplace_back(_open.empty() ? Table{} : *_open.back());
        for (MutationDecl const& mutation : scope.mutations) {
            auto const types = _types.find(mutation.model);
            std::string type = types == _types.end() || types->second.empty() ? mutation.model : types->second.back();
            Mutation const& resolved = _resolved.emplace_back(Mutation{&mutation, std::move(type), &table});
            table.mutations.insert_or_assign(mutation.name, &resolved);
        }
        _open.push_back(&table);
        for (MutationDecl const& mutation : scope.mutations) {
            walk(mutation.onSuccess, {}, table);
            walk(mutation.onError, {}, table);
        }
        // A watch runs when a value changes, whoever changed it, so its commands run without a press.
        for (WatchDecl const& watch : scope.watches) {
            walk(watch.body, "watch", table);
        }
    }

    void leave(ScopeDecl const& scope) {
        for (ModelDecl const& model : scope.models) {
            _types.at(model.name).pop_back();
        }
        _open.pop_back();
    }

    void node(DocNode const& view) {
        if (view.scope) {
            enter(*view.scope);
        }
        Table const& table = *_open.back();
        for (auto const& [key, commands] : view.events) {
            // A custom component's own code decides when its events fire, so they run without a press.
            std::string trigger;
            if (view.kind == "custom" && key.starts_with("on.")) {
                trigger = "component:" + std::string{view.name("name")} + "." + key.substr(3);
            }
            walk(commands, trigger, table);
        }
        for (auto const& [chord, commands] : view.keys) {
            static_cast<void>(chord);
            walk(commands, {}, table);
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

    /// @brief Notes @p commands' capabilities. @p trigger names what runs them without a press (`onMount`, `watch`,
    ///        …), and is empty for commands a person's press runs. @p table is the names open where they are written.
    void walk(Commands const& commands, std::string const& trigger, Table const& table) {
        for (Command const& command : commands) {
            std::string const capability = capabilityOf(command, table);
            if (!capability.empty()) {
                // Host commands are noted by name from the JSON with every other host name; runs by the actions row.
                if (command.kind != CommandKind::Run && command.kind != CommandKind::Host) {
                    note(capability, command.path);
                }
                if (!trigger.empty()) {
                    note(trigger + ": " + capability, command.path);
                }
            }
            // `run`, `upload` and `signIn` each name a mutation, and run it.
            bool const runs = command.kind == CommandKind::Run || command.kind == CommandKind::Upload ||
                              command.kind == CommandKind::SignIn;
            if (runs && !trigger.empty()) {
                if (command.kind != CommandKind::Run) {
                    note(trigger + ": run " + actionOf(command.target, table), command.path);
                }
                follow(command.target, trigger, table);
            }
            walk(command.commands, trigger, table);
            walk(command.alternative, trigger, table);
        }
    }

    /// @brief A mutation run without a press runs its `onSuccess` and `onError` without one too, resolving their
    ///        names in the scope that declares the mutation, as the runtime does.
    void follow(std::string const& name, std::string const& trigger, Table const& table) {
        Mutation const* const mutation = find(name, table);
        if (mutation == nullptr || !_followed.insert({mutation, trigger}).second) {
            return;
        }
        walk(mutation->decl->onSuccess, trigger, *mutation->table);
        walk(mutation->decl->onError, trigger, *mutation->table);
    }

    [[nodiscard]] static Mutation const* find(std::string const& name, Table const& table) {
        auto const found = table.mutations.find(name);
        return found == table.mutations.end() ? nullptr : found->second;
    }

    [[nodiscard]] static std::string actionOf(std::string const& name, Table const& table) {
        Mutation const* const mutation = find(name, table);
        return mutation == nullptr ? name : mutation->type + "/" + mutation->decl->action;
    }

    [[nodiscard]] static std::string capabilityOf(Command const& command, Table const& table) {
        switch (command.kind) {
        case CommandKind::Run:
            return "run " + actionOf(command.target, table);
        case CommandKind::Host:
            return "host `" + command.target + "`";
        case CommandKind::Upload:
            return "upload";
        case CommandKind::Download:
            return "download";
        case CommandKind::SwitchBackend:
            return "switchBackend `" + command.target + "`";
        case CommandKind::SignIn:
            return "signIn";
        case CommandKind::SignOut:
            return "signOut";
        case CommandKind::ResetPrefs:
            return "resetPrefs";
        case CommandKind::Set:
        case CommandKind::Reset:
        case CommandKind::Refetch:
        case CommandKind::Open:
        case CommandKind::Close:
        case CommandKind::Navigate:
        case CommandKind::Return:
        case CommandKind::Focus:
        case CommandKind::SetLocale:
        case CommandKind::SetContext:
        case CommandKind::Notify:
        case CommandKind::Seq:
        case CommandKind::All:
        case CommandKind::If:
        case CommandKind::CancelClose:
        case CommandKind::Retry:
        default:
            return {};
        }
    }

    /// @brief Every string member named `host`, anywhere: a host command, a host value or a host component.
    void hostNames(Json const& json, std::string const& path) {
        if (auto const* const object = std::get_if<Json::object_t>(&json.data)) {
            for (auto const& [key, value] : *object) {
                std::string const at = pointerAppend(path, key);
                if (auto const* const text = std::get_if<std::string>(&value.data); text != nullptr && key == "host") {
                    note("host `" + *text + "`", at);
                }
                hostNames(value, at);
            }
        } else if (auto const* const array = std::get_if<Json::array_t>(&json.data)) {
            for (std::size_t index = 0; index < array->size(); ++index) {
                hostNames((*array)[index], pointerAppend(path, index));
            }
        }
    }

    std::map<std::string, std::vector<std::string>, std::less<>> _types;
    std::deque<Table> _tables;
    std::deque<Mutation> _resolved;
    std::vector<Table const*> _open;
    std::set<std::pair<Mutation const*, std::string>> _followed;
    std::vector<std::pair<std::string, std::string>> _uses;
    std::set<std::string, std::less<>> _seen;
};

// NOLINTEND(misc-no-recursion)

}  // namespace

std::optional<Refusal> checkBaseRule(Document const& base, std::string_view baseText, Document const& custom,
                                     std::string_view customText) {
    std::string const of = "the base screen `" + base.id + "`";
    if (custom.params != base.params) {
        return refusalAt("/params", "a custom screen declares exactly the params of " + of);
    }
    detail::DocumentActions const baseNames{base};
    detail::DocumentActions const customNames{custom};
    std::vector<ActionRef> const baseActions = baseNames.actions();
    for (detail::ActionUse const& use : customNames.uses()) {
        ActionRef const& ref = use.action;
        if (!std::ranges::binary_search(baseActions, ref)) {
            return refusalAt(use.path, of + " does not use action `" + ref.action + "` of `" + ref.modelType + "`");
        }
        // The same action in another kind of position runs differently: a mutation, a query and a form that submits
        // itself each reach the server by a different trigger.
        if (!baseNames.names(ref, use.position)) {
            return refusalAt(use.path, of + " does not use action `" + ref.action + "` of `" + ref.modelType +
                                           "` as a " + use.position);
        }
    }
    auto const baseJson = parseJson(baseText);
    auto const customJson = parseJson(customText);
    if (!baseJson || !customJson) {
        return refusalAt("", "a document the base rule compares is JSON text");
    }
    Capabilities const had{base, *baseJson};
    Capabilities const has{custom, *customJson};
    for (auto const& [capability, path] : has.uses()) {
        if (!had.has(capability)) {
            return refusalAt(path, of + " has no `" + capability + "`");
        }
    }
    return std::nullopt;
}

}  // namespace morph::ui

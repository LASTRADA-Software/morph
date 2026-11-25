// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <deque>
#include <expected>
#include <functional>
#include <glaze/glaze.hpp>
#include <list>
#include <map>
#include <memory>
#include <morph/core/wire.hpp>
#include <morph/ui/author.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/custom_screens.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/value.hpp>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "document_actions.hpp"

namespace morph::ui {
namespace {

[[nodiscard]] Refusal refusalAt(std::string path, std::string reason) {
    return Refusal{.document = {}, .path = std::move(path), .reason = std::move(reason)};
}

using detail::EventClass;

/// @brief What runs the commands under an event key of a node or part kind, as `eventKeys` classifies it.
/// @param kind The node or part kind.
/// @param key  A key of `DocNode::events`: a command-list key, or `record.event` for a record of command lists.
/// @return What runs its commands, or nothing for a key the table does not list.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the node's kind, then its key.
[[nodiscard]] std::optional<EventClass> classifyEvent(std::string_view kind, std::string_view key) {
    std::string_view const name = key.substr(0, key.find('.'));
    for (detail::EventKey const& entry : detail::eventKeys()) {
        if (entry.kind == kind && entry.key == name) {
            return entry.eventClass;
        }
    }
    return std::nullopt;
}

// NOLINTBEGIN(misc-no-recursion): view nodes, command lists and JSON nest; parseJson bounds the depth at 64.

/// @brief Everything a document can cause that the base rule compares, each as a capability string, with the JSON
///        Pointer where the document first uses it.
class Capabilities {
public:
    /// @brief Walks @p document and its JSON @p json.
    /// @param document The loaded document.
    /// @param json     Its parsed JSON, for the `host` names.
    Capabilities(Document const& document, Json const& json) : _json{&json} {
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
        leave(document.scope);
        // A component's fallback is a view of its own: it sees no name of the screen around it, as at load and at
        // runtime, so it is walked with nothing open.
        _open.push_back(&_tables.emplace_back());
        for (auto const& [name, component] : document.components) {
            if (!component.qml.empty()) {
                note("component `" + name + "` " + component.qml, component.path);
            }
            if (component.fallback) {
                node(*component.fallback);
            }
        }
        _open.pop_back();
        hostNames(json, "");
    }

    /// @brief Every capability, in the order first used, with its pointer.
    /// @return The uses.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> const& uses() const noexcept { return _uses; }

    /// @brief Whether the document uses @p capability.
    /// @param capability The capability string.
    /// @return Whether it does.
    [[nodiscard]] bool has(std::string const& capability) const { return _seen.contains(capability); }

    /// @brief One host command: its canonical JSON, its pointer, and whether its arguments are literals.
    struct HostCommand {
        std::string json;
        std::string path;
        bool literalArgs;
    };

    /// @brief Every host command.
    /// @return The commands, in the order walked.
    [[nodiscard]] std::vector<HostCommand> const& hostCommands() const noexcept { return _hostCommands; }

    /// @brief The first host command whose JSON could not be found or written, which cannot be compared.
    /// @return Its pointer, or nothing.
    [[nodiscard]] std::optional<std::string> const& unreadableHost() const noexcept { return _unreadableHost; }

    /// @brief The first upload that is not a file the person just chose, where `{"ref": "event"}` sits directly in a
    ///        `filePicker`'s `onPick` or a `dropZone`'s `onDrop`.
    /// @return The upload's pointer, or nothing.
    [[nodiscard]] std::optional<std::string> const& strayUpload() const noexcept { return _strayUpload; }

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
        _aliases.enter(scope);
        Table& table = _tables.emplace_back(_open.empty() ? Table{} : *_open.back());
        for (MutationDecl const& mutation : scope.mutations) {
            Mutation const& resolved = _resolved.emplace_back(
                Mutation{.decl = &mutation, .type = _aliases.resolve(mutation.model), .table = &table});
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
        _aliases.leave(scope);
        _open.pop_back();
    }

    void node(DocNode const& view) {
        if (view.scope) {
            enter(*view.scope);
        }
        Table const& table = *_open.back();
        // A form that submits itself runs its submit commands and chains without a press.
        // Its submit mode, when it names none, is the action schema's, which is automatic unless the schema says
        // otherwise, and no catalog is at hand here.
        bool const autoForm = view.kind == "form" && view.name("submit") != "explicit";
        std::string autoTrigger;
        if (autoForm) {
            autoTrigger = "form-auto:" + _aliases.resolve(view.name("model")) + "/" + std::string{view.name("action")};
        }
        for (auto const& [key, commands] : view.events) {
            std::string trigger;
            bool picked = false;
            if (std::optional<EventClass> const eventClass = classifyEvent(view.kind, key)) {
                switch (*eventClass) {
                    case EventClass::ChosenFile:
                        picked = view.kind != "filePicker" || opensFiles(view.path);
                        break;
                    case EventClass::FormOutcome:
                        trigger = autoForm ? autoTrigger : std::string{};
                        break;
                    case EventClass::ComponentEvent:
                        // A custom component's own code decides when its events fire, so they run without a press.
                        trigger = "component:" + std::string{view.name("name")} + "." + key.substr(key.find('.') + 1);
                        break;
                    case EventClass::Press:
                    default:
                        break;
                }
            } else {
                // A key the table does not classify is unattended, under a trigger of its own: its commands can only
                // be allowed by a base that has the same key with the same commands.
                trigger = "event:" + view.kind + "." + key;
            }
            walk(commands, trigger, table, picked);
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
    ///        @p chosen is whether @p commands is the handler a chosen file arrives in.
    void walk(Commands const& commands, std::string const& trigger, Table const& table, bool chosen = false) {
        for (Command const& command : commands) {
            if (command.kind == CommandKind::Upload &&
                !(chosen && trigger.empty() && isEventRef(command.path, "upload"))) {
                if (!_strayUpload) {
                    _strayUpload = command.path;
                }
            }
            if (command.kind == CommandKind::Host) {
                noteHostCommand(command.path);
            }
            std::string const capability = capabilityOf(command, table);
            if (!capability.empty()) {
                // Host commands are noted by name from the JSON with every other host name; runs by the actions row.
                if (command.kind != CommandKind::Run && command.kind != CommandKind::Host) {
                    note(capability, command.path);
                }
                if (!trigger.empty()) {
                    std::string pressed = trigger;
                    pressed += ": ";
                    pressed += capability;
                    note(std::move(pressed), command.path);
                }
            }
            // `run`, `upload` and `signIn` each name a mutation, and run it.
            bool const runs = command.kind == CommandKind::Run || command.kind == CommandKind::Upload ||
                              command.kind == CommandKind::SignIn;
            if (runs && !trigger.empty()) {
                if (command.kind != CommandKind::Run) {
                    std::string unattended = trigger;
                    unattended += ": run ";
                    unattended += actionOf(command.target, table);
                    note(std::move(unattended), command.path);
                }
                follow(command.target, trigger, table);
            }
            walk(command.commands, trigger, table);
            walk(command.alternative, trigger, table);
        }
    }

    /// @brief A mutation run without a press runs its `onSuccess` and `onError` without one too, resolving their
    ///        names in the scope that declares the mutation, as the runtime does.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the mutation's name, then the trigger that ran it.
    void follow(std::string const& name, std::string const& trigger, Table const& table) {
        Mutation const* const mutation = find(name, table);
        if (mutation == nullptr || !_followed.insert({mutation, trigger}).second) {
            return;
        }
        walk(mutation->decl->onSuccess, trigger, *mutation->table);
        walk(mutation->decl->onError, trigger, *mutation->table);
    }

    void noteHostCommand(std::string const& path) {
        Json const* const json = at(path);
        auto const* const object = json == nullptr ? nullptr : std::get_if<Json::object_t>(&json->data);
        if (object != nullptr) {
            if (auto written = glz::write<::morph::wire::detail::EscapingWriteOpts{}>(*json)) {
                Json::object_t::const_iterator const args = object->find("args");
                _hostCommands.push_back(HostCommand{.json = *std::move(written),
                                                    .path = path,
                                                    .literalArgs = args == object->end() || isLiteral(args->second)});
                return;
            }
        }
        // A command that cannot be found or written cannot be compared, so it is refused rather than skipped.
        if (!_unreadableHost) {
            _unreadableHost = path;
        }
    }

    /// @brief Whether @p json reads nothing from the document: a scalar or a `lit`. Any other object is an expression
    ///        (`ref`, `fn`, …) whose names resolve in the custom document; an array is no expression, which load
    ///        refuses, so none reaches here.
    [[nodiscard]] static bool isLiteral(Json const& json) {
        if (auto const* const object = std::get_if<Json::object_t>(&json.data)) {
            return object->size() == 1 && object->contains("lit");
        }
        return true;
    }

    /// @brief Whether the `filePicker` at @p pointer opens files: its `mode` is absent or the literal `open`. A save
    ///        picker's `onPick` receives a place to write, not a file.
    [[nodiscard]] bool opensFiles(std::string const& pointer) const {
        Json const* const picker = at(pointer);
        auto const* const object = picker == nullptr ? nullptr : std::get_if<Json::object_t>(&picker->data);
        if (object == nullptr) {
            return false;
        }
        Json::object_t::const_iterator const mode = object->find("mode");
        if (mode == object->end()) {
            return true;
        }
        auto const text = glz::write<::morph::wire::detail::EscapingWriteOpts{}>(mode->second);
        return text && (*text == R"("open")" || *text == R"({"lit":"open"})");
    }

    /// @brief The JSON at @p pointer, or `nullptr`.
    [[nodiscard]] Json const* at(std::string const& pointer) const {
        Json const* json = _json;
        std::size_t start = 0;
        while (json != nullptr && start < pointer.size()) {
            std::size_t const end = std::min(pointer.find('/', start + 1), pointer.size());
            std::string token = pointer.substr(start + 1, end - start - 1);
            for (std::size_t tilde = token.find('~'); tilde != std::string::npos; tilde = token.find('~', tilde + 1)) {
                if (tilde + 1 < token.size()) {
                    token.replace(tilde, 2, token.at(tilde + 1) == '1' ? "/" : "~");
                }
            }
            if (auto const* const object = std::get_if<Json::object_t>(&json->data)) {
                Json::object_t::const_iterator const found = object->find(token);
                json = found == object->end() ? nullptr : &found->second;
            } else if (auto const* const array = std::get_if<Json::array_t>(&json->data)) {
                std::size_t index = 0;
                char const* const last = std::to_address(token.end());
                auto const [stop, error] = std::from_chars(std::to_address(token.begin()), last, index);
                json = error == std::errc{} && stop == last && index < array->size() ? &array->at(index) : nullptr;
            } else {
                json = nullptr;
            }
            start = end;
        }
        return json;
    }

    /// @brief Whether the command at @p pointer has exactly `{"ref": "event"}` as its @p key.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the command's pointer, then the key under it.
    [[nodiscard]] bool isEventRef(std::string const& pointer, std::string const& key) const {
        Json const* const command = at(pointer);
        auto const* const object = command == nullptr ? nullptr : std::get_if<Json::object_t>(&command->data);
        if (object == nullptr || !object->contains(key)) {
            return false;
        }
        return glz::write<::morph::wire::detail::EscapingWriteOpts{}>(object->at(key)).value_or("") ==
               R"({"ref":"event"})";
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
                // `default:` only because the project builds with -Wswitch-default. -Wswitch-enum, which it also
                // builds with, refuses a switch over `CommandKind` that lacks a case for any enumerator even with
                // a default, so a command kind added to the vocabulary must be classified here before this compiles.
                return {};
        }
    }

    /// @brief Every string member named `host`, anywhere: a host command, a host value or a host component.
    void hostNames(Json const& json, std::string const& path) {
        if (auto const* const object = std::get_if<Json::object_t>(&json.data)) {
            for (auto const& [key, value] : *object) {
                std::string const keyPath = pointerAppend(path, key);
                if (auto const* const text = std::get_if<std::string>(&value.data); text != nullptr && key == "host") {
                    note("host `" + *text + "`", keyPath);
                }
                hostNames(value, keyPath);
            }
        } else if (auto const* const array = std::get_if<Json::array_t>(&json.data)) {
            for (std::size_t index = 0; index < array->size(); ++index) {
                hostNames(array->at(index), pointerAppend(path, index));
            }
        }
    }

    Json const* _json;
    detail::ModelAliases _aliases;
    std::vector<HostCommand> _hostCommands;
    std::optional<std::string> _unreadableHost;
    std::optional<std::string> _strayUpload;
    std::deque<Table> _tables;
    std::deque<Mutation> _resolved;
    std::vector<Table const*> _open;
    std::set<std::pair<Mutation const*, std::string>> _followed;
    std::vector<std::pair<std::string, std::string>> _uses;
    std::set<std::string, std::less<>> _seen;
};

// NOLINTEND(misc-no-recursion)

}  // namespace

namespace detail {

namespace {

// NOLINTBEGIN(modernize-use-designated-initializers): the table reads as rows.
constexpr std::array<EventKey, 46> kEventKeys = {{
    {"button", "onClick", EventClass::Press},
    {"button", "onActivate", EventClass::Press},
    {"textInput", "onChange", EventClass::Press},
    {"textInput", "onCommit", EventClass::Press},
    {"textInput", "onSubmit", EventClass::Press},
    {"checkbox", "onToggle", EventClass::Press},
    {"checkbox", "onChange", EventClass::Press},
    {"select", "onChoose", EventClass::Press},
    {"select", "onChange", EventClass::Press},
    {"panel", "onToggle", EventClass::Press},
    {"tabs", "onChoose", EventClass::Press},
    {"dialog", "onDismiss", EventClass::Press},
    {"dialog", "default", EventClass::Press},
    {"dialog", "cancel", EventClass::Press},
    {"drawer", "onDismiss", EventClass::Press},
    {"drawer", "default", EventClass::Press},
    {"drawer", "cancel", EventClass::Press},
    {"table", "onSelectionChange", EventClass::Press},
    {"table", "onSortChange", EventClass::Press},
    {"table", "onActivate", EventClass::Press},
    {"dateTimeInput", "onChange", EventClass::Press},
    {"dateTimeInput", "onCommit", EventClass::Press},
    {"slider", "onChange", EventClass::Press},
    {"slider", "onCommit", EventClass::Press},
    {"filePicker", "onPick", EventClass::ChosenFile},
    {"banner", "onDismiss", EventClass::Press},
    {"badge", "onRemove", EventClass::Press},
    {"collapsible", "onToggle", EventClass::Press},
    {"dropZone", "onDrop", EventClass::ChosenFile},
    {"form", "onSubmit", EventClass::FormOutcome},
    {"form", "onSuccess", EventClass::FormOutcome},
    {"form", "onError", EventClass::FormOutcome},
    {"custom", "on", EventClass::ComponentEvent},
    {"menuItem", "onClick", EventClass::Press},
    {"action", "onClick", EventClass::Press},
    {"drop", "onDrop", EventClass::Press},
    {"actionButton", "onClick", EventClass::Press},
    {"headMenuItem", "onClick", EventClass::Press},
    {"headTool", "onChange", EventClass::Press},
    {"headTool", "onSubmit", EventClass::Press},
    {"headTool", "onChoose", EventClass::Press},
    {"table", "onFiltersChange", EventClass::Press},
    {"table", "onLayoutChange", EventClass::Press},
    {"fieldGrid", "onActivate", EventClass::Press},
    {"fieldGrid", "onFiltersChange", EventClass::Press},
    {"fieldGrid", "onLayoutChange", EventClass::Press},
}};
// NOLINTEND(modernize-use-designated-initializers)

}  // namespace

std::span<EventKey const> eventKeys() { return kEventKeys; }

}  // namespace detail

std::optional<Refusal> checkBaseRule(Document const& base, std::string_view baseText, Document const& custom,
                                     std::string_view customText) {
    std::string const baseName = "the base screen `" + base.id + "`";
    if (custom.params != base.params) {
        return refusalAt("/params", "a custom screen declares exactly the params of " + baseName);
    }
    detail::DocumentActions const baseNames{base};
    detail::DocumentActions const customNames{custom};
    std::vector<ActionRef> const baseActions = baseNames.actions();
    for (detail::ActionUse const& use : customNames.uses()) {
        ActionRef const& ref = use.action;
        if (!std::ranges::binary_search(baseActions, ref)) {
            return refusalAt(use.path,
                             baseName + " does not use action `" + ref.action + "` of `" + ref.modelType + "`");
        }
        // The same action in another kind of position runs differently: a mutation, a query and a form that submits
        // itself each reach the server by a different trigger.
        if (!baseNames.names(ref, use.position)) {
            return refusalAt(use.path, baseName + " does not use action `" + ref.action + "` of `" + ref.modelType +
                                           "` in a position of kind `" + use.position + "`");
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
            std::string message = baseName;
            message += " has no `";
            message += capability;
            message += "`";
            return refusalAt(path, message);
        }
    }
    if (has.unreadableHost()) {
        return refusalAt(*has.unreadableHost(),
                         "a host command of a document the base rule compares is a JSON object");
    }
    for (auto const& command : has.hostCommands()) {
        if (!command.literalArgs) {
            return refusalAt(command.path,
                             "a host command of a custom screen has literal arguments, not names that "
                             "resolve in the screen");
        }
        if (!std::ranges::any_of(had.hostCommands(), [&](auto const& known) { return known.json == command.json; })) {
            return refusalAt(command.path, baseName + " has no host command `" + command.json + "`");
        }
    }
    if (has.strayUpload()) {
        return refusalAt(*has.strayUpload(),
                         "an `upload` of a custom screen sends `{\"ref\": \"event\"}` directly in a `filePicker`'s "
                         "`onPick` or a `dropZone`'s `onDrop`");
    }
    return std::nullopt;
}

namespace {

/// @brief Appends a line `heading` and, indented below it, each of @p items; nothing when @p items is empty.
void section(std::string& out, std::string_view heading, std::vector<std::string> const& items) {
    if (items.empty()) {
        return;
    }
    out += heading;
    out += '\n';
    for (std::string const& item : items) {
        out += "  ";
        out += item;
        out += '\n';
    }
}

}  // namespace

std::expected<std::string, Refusal> describeAllowance(Document const& base, std::string_view baseText) {
    auto const json = parseJson(baseText);
    if (!json) {
        return std::unexpected(refusalAt("", "a document the base rule describes is JSON text"));
    }
    detail::DocumentActions const names{base};
    Capabilities const has{base, *json};
    std::vector<std::string> params;
    if (auto const* const object = std::get_if<Json::object_t>(&json->data)) {
        if (Json::object_t::const_iterator const declared = object->find("params"); declared != object->end()) {
            if (auto const* const members = std::get_if<Json::object_t>(&declared->second.data)) {
                for (auto const& [name, type] : *members) {
                    auto const written = glz::write<::morph::wire::detail::EscapingWriteOpts{}>(type);
                    params.push_back("`" + name + "`: " + written.value_or("?"));
                }
            }
        }
    }
    std::vector<std::string> actions;
    for (detail::ActionUse const& use : names.uses()) {
        actions.push_back(use.action.modelType + "/" + use.action.action + " (" + use.position + ")");
    }
    std::vector<std::string> unattended;
    std::vector<std::string> clientSide;
    std::vector<std::string> hosts;
    std::vector<std::string> components;
    for (auto const& [capability, path] : has.uses()) {
        static_cast<void>(path);
        if (capability.contains(": ")) {
            unattended.push_back(capability);
        } else if (capability.starts_with("host `")) {
            hosts.push_back(capability);
        } else if (capability.starts_with("component `")) {
            components.push_back(capability);
        } else {
            clientSide.push_back(capability);
        }
    }
    std::vector<std::string> hostCommands;
    for (auto const& command : has.hostCommands()) {
        hostCommands.push_back(command.json);
    }
    std::string out = "A custom screen of the base screen `" + base.id + "` may use only the following.\n";
    out += params.empty() ? "Params: none; a custom screen declares exactly its base's.\n" : "";
    section(out, "Params, which a custom screen declares exactly:", params);
    section(out, "Actions, each only in a position of the kind shown in brackets:", actions);
    section(out, "Run without a person's press, each only from the trigger shown:", unattended);
    section(out, "Client-side commands:", clientSide);
    section(out, "Host names:", hosts);
    section(out, "Host commands, each with exactly these arguments, which are literals:", hostCommands);
    section(out, "Delivered components:", components);
    out += "Text, layout, styles, states and expressions are free.\n";
    return out;
}

std::optional<std::optional<Refusal>> VerdictCache::find(std::string const& key) {
    std::scoped_lock const lock{_mutex};
    auto const found = _entries.find(key);
    if (found == _entries.end()) {
        return std::nullopt;
    }
    _order.splice(_order.begin(), _order, found->second.order);
    return found->second.verdict;
}

void VerdictCache::store(std::string key, std::optional<Refusal> verdict) {
    std::scoped_lock const lock{_mutex};
    if (auto const found = _entries.find(key); found != _entries.end()) {
        found->second.verdict = std::move(verdict);
        _order.splice(_order.begin(), _order, found->second.order);
        return;
    }
    _order.push_front(key);
    _entries.emplace(std::move(key), Entry{.verdict = std::move(verdict), .order = _order.begin()});
    while (_entries.size() > _capacity) {
        _entries.erase(_order.back());
        _order.pop_back();
    }
}

std::size_t VerdictCache::size() const {
    std::scoped_lock const lock{_mutex};
    return _entries.size();
}

std::string verdictKey(std::string_view basedOn, std::string_view document, std::string_view screenId,
                       std::string_view digest, std::vector<std::string> const& permitted) {
    std::string joined;
    for (std::string const& action : permitted) {
        joined += action;
        joined += '\n';
    }
    return contentHash(document) + ' ' + std::string{basedOn} + ' ' + std::string{screenId} + ' ' +
           std::string{digest} + ' ' + contentHash(joined);
}

namespace {

/// @brief Check 2 of `checkCustomScreen`: the base screen, or the refusal.
///
/// Read from the bundle and the authoriser on every call, never from the verdict cache: the bundle's digest hashes
/// the manifest, which does not say whether an entry is available, so a cached verdict cannot vouch for the base.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the base, then the session.
[[nodiscard]] std::expected<ServedScreen const*, Refusal> baseOf(ServedBundle const& bundle,
                                                                 ::morph::session::Context const& session,
                                                                 ::morph::session::IAuthorizer const& authorizer,
                                                                 std::string_view basedOn) {
    auto const base =
        std::ranges::find(bundle.screens, basedOn, [](ServedScreen const& screen) { return screen.entry.id; });
    if (base == bundle.screens.end() || base->hash.empty()) {
        return std::unexpected(
            refusalAt("", "`basedOn` names no screen document of the application: `" + std::string{basedOn} + "`"));
    }
    if (!screenAdmitted(*base, session, authorizer)) {
        return std::unexpected(
            refusalAt("", "this person may not open the base screen `" + std::string{basedOn} + "`"));
    }
    return &*base;
}

/// @brief Checks 3 to 6 of `checkCustomScreen`, against the base screen `base`.
[[nodiscard]] std::optional<Refusal> checkAgainstBase(
    ServedBundle const& bundle, ServedScreen const& base,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the base's id, the custom text, then its id.
    std::string_view basedOn, std::string_view document, std::string_view screenId,
    std::vector<std::string> const& permitted) {
    std::string const& baseText = bundle.files.at(base.hash).bytes;
    LoadOptions const load{.catalog = bundle.catalog};
    auto const baseDocument = loadDocumentText(baseText, load);
    if (!baseDocument) {
        return refusalAt(
            "", "the base screen `" + std::string{basedOn} + "` does not load: " + baseDocument.error().message());
    }
    auto custom = loadDocumentText(document, load);
    if (!custom) {
        return std::move(custom.error());
    }
    if (custom->id != screenId) {
        return refusalAt("/id",
                         "a custom screen's `id` is the id it is stored under: `" + std::string{screenId} + "`");
    }
    if (auto refusal = author::checkAccessibility(*custom)) {
        return refusal;
    }
    if (auto refusal = checkBaseRule(*baseDocument, baseText, *custom, document)) {
        return refusal;
    }
    detail::DocumentActions const named{*custom};
    for (ActionRef const& ref : named.actions()) {
        if (std::ranges::find(permitted, ref.action) == permitted.end()) {
            return refusalAt(named.pathOf(ref), "this person may not run action `" + ref.action + "`");
        }
    }
    return std::nullopt;
}

}  // namespace

std::optional<Refusal> checkCustomScreen(ServedBundle const& bundle, ::morph::session::Context const& session,
                                         ::morph::session::IAuthorizer const& authorizer,
                                         std::vector<std::string> const& permitted, std::string_view basedOn,
                                         std::string_view document, std::string_view screenId,
                                         CustomScreensOptions const& options, VerdictCache* cache) {
    if (document.size() > options.maxDocumentBytes) {
        return refusalAt("", "a custom screen is longer than " + std::to_string(options.maxDocumentBytes) + " bytes");
    }
    if (detail::nestsTooDeep(document)) {
        return refusalAt("", "a custom screen nests deeper than " + std::to_string(kMaxJsonDepth) + " levels");
    }
    if (!screenId.starts_with(kCustomScreenIdPrefix)) {
        return refusalAt("/id", "a custom screen's id starts with `" + std::string{kCustomScreenIdPrefix} + "`");
    }
    auto const base = baseOf(bundle, session, authorizer, basedOn);
    if (!base) {
        return base.error();
    }
    std::string key;
    if (cache != nullptr) {
        key = verdictKey(basedOn, document, screenId, bundle.digest, permitted);
        if (auto hit = cache->find(key)) {
            return std::move(*hit);
        }
    }
    std::optional<Refusal> verdict = checkAgainstBase(bundle, **base, basedOn, document, screenId, permitted);
    if (cache != nullptr) {
        cache->store(std::move(key), verdict);
    }
    return verdict;
}

std::optional<Refusal> checkCustomScreen(ServedBundle const& bundle, ::morph::session::Context const& session,
                                         ::morph::session::IAuthorizer const& authorizer, std::string_view basedOn,
                                         std::string_view document, std::string_view screenId,
                                         CustomScreensOptions const& options, VerdictCache* cache) {
    return checkCustomScreen(bundle, session, authorizer, permittedActions(bundle, session, authorizer), basedOn,
                             document, screenId, options, cache);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the document, then its new id.
std::expected<std::string, Refusal> withScreenId(std::string_view document, std::string_view screenId) {
    auto parsed = parseJson(document);
    if (!parsed) {
        return std::unexpected(refusalAt("", "a custom screen is JSON text: " + parsed.error()));
    }
    auto* const object = std::get_if<Json::object_t>(&parsed->data);
    if (object == nullptr) {
        return std::unexpected(refusalAt("", "a custom screen is a JSON object"));
    }
    object->try_emplace("id").first->second.data = std::string{screenId};
    // Control bytes are escaped inside the writer, as the wire does: glaze's default leaves them raw, which is
    // invalid JSON and can corrupt the byte after an earlier escaped character.
    auto written = glz::write<::morph::wire::detail::EscapingWriteOpts{}>(*parsed);
    if (!written) {
        return std::unexpected(refusalAt("", "a custom screen could not be written back as JSON"));
    }
    return std::move(*written);
}

}  // namespace morph::ui

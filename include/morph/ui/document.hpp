// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "expression.hpp"

/// @file
/// @brief The UI document (`ui/1`): a screen's declarations, scopes, view nodes and commands, parsed from JSON.
///
/// Specified in `docs/spec/ui/document.md`.

namespace morph::ui {

/// @brief The vocabulary a screen document is written in.
inline constexpr std::string_view kDocumentVocabulary = "ui/1";

/// @brief An expression under a name: a node's property, a record field of a declaration.
struct NamedExpr {
    /// @brief The name.
    std::string name;
    /// @brief The expression.
    Expr expr;
};

/// @brief A literal string under a name: an id, a model alias, an action id, a policy.
struct NamedText {
    /// @brief The name.
    std::string name;
    /// @brief The text.
    std::string text;
};

struct Command;

/// @brief A command list: the commands one event runs, in one batch.
using CommandList = std::vector<Command>;

/// @brief A command list under a name: a node's event, a command's branch.
struct NamedCommands {
    /// @brief The event or branch name, for example `onClick` or `then`.
    std::string name;
    /// @brief The commands.
    CommandList commands;
};

/// @brief A command (spec 5 §8).
///
/// One shape serves every verb: `target` holds the verb's name operand (`set`'s path, `run`'s mutation, `open`'s
/// dialog), and the other operands sit under their keys in `exprs`, `texts` and `lists`.
struct Command {
    /// @brief The verb: `set`, `run`, `navigate`, `seq`, ...
    std::string verb;
    /// @brief The JSON pointer of the command.
    std::string path;
    /// @brief The verb's name operand, when it takes one.
    std::string target;
    /// @brief The expression operands: `value`, `body`, `each`, `params`, `args`, `route`, and the verb's own
    ///        expression (`return`, `notify`, `upload`, `download`, an `if`'s condition) under the verb's name.
    std::vector<NamedExpr> exprs;
    /// @brief The literal operands: `as`, `to`, `id`, `role`, `save`, `from`.
    std::vector<NamedText> texts;
    /// @brief The nested command lists: `seq`'s or `all`'s steps under the verb's name, `if`'s `then` and `else`,
    ///        `navigate`'s `onResult` and `onCancel`.
    std::vector<NamedCommands> lists;

    /// @brief The expression operand @p name.
    /// @param name The operand's key.
    /// @return The expression, or null when the command has none.
    [[nodiscard]] Expr const* expr(std::string_view name) const noexcept {
        auto const found = std::ranges::find(exprs, name, &NamedExpr::name);
        return found == exprs.end() ? nullptr : &found->expr;
    }

    /// @brief The literal operand @p name.
    /// @param name The operand's key.
    /// @return The text, or null when the command has none.
    [[nodiscard]] std::string const* text(std::string_view name) const noexcept {
        auto const found = std::ranges::find(texts, name, &NamedText::name);
        return found == texts.end() ? nullptr : &found->text;
    }

    /// @brief The nested command list @p name.
    /// @param name The list's key.
    /// @return The commands, or null when the command has none.
    [[nodiscard]] CommandList const* list(std::string_view name) const noexcept {
        auto const found = std::ranges::find(lists, name, &NamedCommands::name);
        return found == lists.end() ? nullptr : &found->commands;
    }
};

/// @brief A model alias of a scope (spec 5 §5).
struct ModelDecl {
    /// @brief How the alias picks its instance.
    enum class Instance : std::uint8_t {
        Default,  ///< Not written: `shared` for a stateless model, `private` otherwise.
        Private,  ///< `"private"`: an instance of the scope's own.
        Shared,   ///< `"shared"`: the model's singleton.
        Keyed,    ///< An expression: the shared instance with that key.
    };

    /// @brief The alias.
    std::string name;
    /// @brief The JSON pointer of the declaration.
    std::string path;
    /// @brief The model type id.
    std::string type;
    /// @brief How it picks its instance.
    Instance instance = Instance::Default;
    /// @brief The key, for `Keyed`.
    std::optional<Expr> key;
};

/// @brief A state variable of a scope.
struct StateDecl {
    /// @brief The name.
    std::string name;
    /// @brief The JSON pointer of the declaration.
    std::string path;
    /// @brief The declared type, when written; otherwise the type of `init`.
    std::optional<Type> type;
    /// @brief The initial value; `null` when not written.
    Expr init;
};

/// @brief A `let` entry: a value derived on the client.
struct LetDecl {
    /// @brief The name.
    std::string name;
    /// @brief The JSON pointer of the declaration.
    std::string path;
    /// @brief The expression.
    Expr value;
};

/// @brief A row patch: the row of a query's list whose key equals `key` becomes `with`.
struct PatchDecl {
    /// @brief The query whose cached list is patched.
    std::string query;
    /// @brief The JSON pointer of the declaration.
    std::string path;
    /// @brief The key of the row replaced.
    Expr key;
    /// @brief The row it becomes.
    Expr with;
};

/// @brief A query of a scope (spec 5 §5).
struct QueryDecl {
    /// @brief The name.
    std::string name;
    /// @brief The JSON pointer of the declaration.
    std::string path;
    /// @brief The model alias it goes through.
    std::string model;
    /// @brief The action id it calls (or evaluates).
    std::string action;
    /// @brief Whether it evaluates the action (`"evaluate"`) instead of calling it.
    bool evaluate = false;
    /// @brief The request body, and the cache key; `null` makes the query idle.
    Expr body;
    /// @brief The gate; absent is `true`.
    std::optional<Expr> when;
    /// @brief Milliseconds the key must be stable before a fetch.
    std::int64_t debounce = 0;
    /// @brief Action ids whose successful completion refetches it.
    std::vector<std::string> refreshOn;
    /// @brief Polling period in milliseconds; 0 for none.
    std::int64_t refreshEvery = 0;
    /// @brief Milliseconds a cached value counts as fresh.
    std::int64_t staleTime = 0;
};

/// @brief A mutation of a scope (spec 5 §5).
struct MutationDecl {
    /// @brief What a run does while another is in flight.
    enum class Concurrency : std::uint8_t {
        Exclusive,  ///< The run is refused.
        Serial,     ///< Runs queue, one queue per `serialKey`.
        Latest,     ///< Every run is sent; only the newest reply is applied.
        Parallel,   ///< Every run is sent; replies apply as they arrive.
    };

    /// @brief The name.
    std::string name;
    /// @brief The JSON pointer of the declaration.
    std::string path;
    /// @brief The model alias it goes through.
    std::string model;
    /// @brief The action id it runs.
    std::string action;
    /// @brief The request body; a `run` may override it.
    std::optional<Expr> body;
    /// @brief The concurrency policy.
    Concurrency concurrency = Concurrency::Exclusive;
    /// @brief The queue key for `Serial`.
    std::optional<Expr> serialKey;
    /// @brief Queries refetched after a success.
    std::vector<std::string> invalidates;
    /// @brief The row patch applied from the reply (bound as `result`).
    std::optional<PatchDecl> patch;
    /// @brief The row patch applied when the run starts (the body bound as `body`).
    std::optional<PatchDecl> optimistic;
    /// @brief Commands after a success.
    CommandList onSuccess;
    /// @brief Commands after a failure.
    CommandList onError;
    /// @brief Whether a run that cannot reach the server goes through the offline queue.
    bool offlineQueue = false;
};

/// @brief A watch: commands run when a value changes (spec 5 §6).
struct WatchDecl {
    /// @brief The JSON pointer of the declaration.
    std::string path;
    /// @brief The value watched.
    Expr on;
    /// @brief Whether only the user's own edits trigger it (`source: "user"`).
    bool userOnly = false;
    /// @brief The commands it runs.
    CommandList commands;
};

/// @brief A check: validation across elements (spec 5 §6).
struct CheckDecl {
    /// @brief The check's id.
    std::string id;
    /// @brief The JSON pointer of the declaration.
    std::string path;
    /// @brief The assertion; `false` fails it.
    Expr assertion;
    /// @brief The message a failure adds to its targets.
    std::optional<Expr> message;
    /// @brief The element ids it marks.
    std::vector<std::string> targets;
    /// @brief Whether a failure blocks `ready`.
    bool blocking = true;
};

/// @brief A scope's declarations: the screen's, a dialog's, a `switch` case's, a row's.
struct Scope {
    /// @brief The JSON pointer of the scope's object.
    std::string path;
    /// @brief The model aliases.
    std::vector<ModelDecl> models;
    /// @brief The state variables.
    std::vector<StateDecl> state;
    /// @brief The `let` entries.
    std::vector<LetDecl> lets;
    /// @brief The queries.
    std::vector<QueryDecl> queries;
    /// @brief The mutations.
    std::vector<MutationDecl> mutations;
    /// @brief The watches.
    std::vector<WatchDecl> watches;
    /// @brief The checks.
    std::vector<CheckDecl> checks;
};

struct Node;

/// @brief A node list under a name: a node's `children`, a boundary's `loading`, a column's `cell`.
struct NodeSlot {
    /// @brief The slot's key.
    std::string name;
    /// @brief The nodes.
    std::vector<Node> nodes;
};

/// @brief A view node (spec 5 §9), or a structured entry of one: a table's column, a `switch` case, a tab, a menu
///        item, a banner's action, a drop target.
struct Node {
    /// @brief The kind: a node kind of `ui/1`, or for an entry its shape (`column`, `case`, `tab`, `menuItem`,
    ///        `action`, `drop`).
    std::string kind;
    /// @brief The JSON pointer of the node.
    std::string path;
    /// @brief The properties, each an expression.
    std::vector<NamedExpr> props;
    /// @brief The literal keys: `id`, `model`, `action`, `name`, `point`, `surface`, policies.
    std::vector<NamedText> texts;
    /// @brief The events, each a command list; a custom component's `on` events are named `on.<event>`.
    std::vector<NamedCommands> events;
    /// @brief The node lists and entries: `children`, `cell`, `columns`, `cases`, `items`, `loading`, `error`.
    std::vector<NodeSlot> slots;
    /// @brief An object of expressions kept by key: `a11y`, `drag`, a custom component's `props`.
    std::vector<std::pair<std::string, std::vector<NamedExpr>>> maps;
    /// @brief JSON passed through unread: a form's `overrides` and `fields`, read by the forms engine.
    std::vector<std::pair<std::string, Json>> raw;
    /// @brief The scope the node opens: a row's, a dialog's, a case's.
    std::shared_ptr<Scope const> scope;
    /// @brief Why the node is refused: a must-understand key this client does not know. A refused node renders
    ///        its fallback; empty for a node that is not refused.
    std::string refused;

    /// @brief The property @p name.
    /// @param name The property's key.
    /// @return The expression, or null when the node has none.
    [[nodiscard]] Expr const* prop(std::string_view name) const noexcept {
        auto const found = std::ranges::find(props, name, &NamedExpr::name);
        return found == props.end() ? nullptr : &found->expr;
    }

    /// @brief The literal key @p name.
    /// @param name The key.
    /// @return The text, or null when the node has none.
    [[nodiscard]] std::string const* text(std::string_view name) const noexcept {
        auto const found = std::ranges::find(texts, name, &NamedText::name);
        return found == texts.end() ? nullptr : &found->text;
    }

    /// @brief The event @p name.
    /// @param name The event's key.
    /// @return Its commands, or null when the node has none.
    [[nodiscard]] CommandList const* event(std::string_view name) const noexcept {
        auto const found = std::ranges::find(events, name, &NamedCommands::name);
        return found == events.end() ? nullptr : &found->commands;
    }

    /// @brief The slot @p name.
    /// @param name The slot's key.
    /// @return Its nodes, or null when the node has none.
    [[nodiscard]] std::vector<Node> const* slot(std::string_view name) const noexcept {
        auto const found = std::ranges::find(slots, name, &NodeSlot::name);
        return found == slots.end() ? nullptr : &found->nodes;
    }
};

/// @brief A component of the bundle (spec 5 §10): a delivered QML file or a host component, its props and events,
///        and the built-in subtree a renderer that cannot load it shows.
struct ComponentDecl {
    /// @brief The name a `custom` node uses.
    std::string name;
    /// @brief The JSON pointer of the declaration.
    std::string path;
    /// @brief The QML file's content hash, for a delivered component.
    std::string qml;
    /// @brief The host component's name, for one the client carries.
    std::string host;
    /// @brief The props and their prop types (`string`, `bool`, `int`, `exact`, `number`, `json`).
    std::vector<NamedText> props;
    /// @brief The events and their payload types.
    std::vector<NamedText> events;
    /// @brief The fallback subtree; empty when the component declares none.
    std::vector<Node> fallback;
};

/// @brief A customization point (spec 5 §12b).
struct CustomizePoint {
    /// @brief The point's id, read as `{"pref": "id"}`.
    std::string name;
    /// @brief The JSON pointer of the declaration.
    std::string path;
    /// @brief `columns`, `choice`, `value`, `layout`, `saved`, `pins` or `arrange`.
    std::string kind;
    /// @brief The node id it customizes, for `columns`, `layout` and `arrange`.
    std::string of;
    /// @brief The options of a `choice`.
    std::vector<std::string> options;
    /// @brief The type of a `value`.
    std::optional<Type> type;
    /// @brief The default.
    Value defaultValue;
};

/// @brief A screen document.
struct Document {
    /// @brief The vocabulary it is written in; `ui/1`.
    std::string vocab;
    /// @brief The screen id.
    std::string id;
    /// @brief The typed inputs navigation sets.
    std::vector<std::pair<std::string, Type>> params;
    /// @brief The title, an expression.
    std::optional<Expr> title;
    /// @brief The unsaved indicator, a bool expression.
    std::optional<Expr> dirty;
    /// @brief Equal identities are one screen.
    std::optional<Expr> identity;
    /// @brief The lifecycle command lists: `onMount`, `onUnmount`, `onClose`, `onSave`, `onBackendChange`.
    std::vector<NamedCommands> lifecycle;
    /// @brief When an element's errors show: `touched`, `submit` or `always`.
    std::string showErrors = "touched";
    /// @brief The screen's own scope.
    Scope scope;
    /// @brief Queries started before the screen mounts.
    std::vector<std::string> prefetch;
    /// @brief The bundle's components the document declares.
    std::vector<ComponentDecl> components;
    /// @brief The customization points.
    std::vector<CustomizePoint> customize;
    /// @brief The view; a refused document has none.
    std::optional<Node> view;
};

/// @brief What a key of a node or entry holds.
enum class KeyRole : std::uint8_t {
    Expr,      ///< An expression.
    Text,      ///< A literal string.
    Commands,  ///< A command list.
    Nodes,     ///< A list of nodes (a single node is read as a list of one).
    Entries,   ///< A list of structured entries of a shape.
    Entry,     ///< One structured entry of a shape.
    ExprMap,   ///< An object of expressions.
    EventMap,  ///< An object of command lists (`on`).
    Scope,     ///< A scope's declarations.
    Raw,       ///< JSON kept for another engine.
};

/// @brief One key a node kind or entry shape accepts.
struct KeySpec {
    /// @brief The key.
    std::string_view key;
    /// @brief What it holds.
    KeyRole role;
    /// @brief For `Entries` and `Entry`, the shape of the entries.
    std::string_view shape = {};
};

/// @brief The keys every node accepts (spec 5 §9, spec 6 §6 and §17).
inline constexpr std::array<KeySpec, 18> kCommonKeys{{
    {.key = "id", .role = KeyRole::Text},
    {.key = "visible", .role = KeyRole::Expr},
    {.key = "enabled", .role = KeyRole::Expr},
    {.key = "readonly", .role = KeyRole::Expr},
    {.key = "required", .role = KeyRole::Expr},
    {.key = "errors", .role = KeyRole::Expr},
    {.key = "stale", .role = KeyRole::Expr},
    {.key = "tooltip", .role = KeyRole::Expr},
    {.key = "testId", .role = KeyRole::Expr},
    {.key = "keys", .role = KeyRole::Expr},
    {.key = "autofocus", .role = KeyRole::Expr},
    {.key = "sizing", .role = KeyRole::Expr},
    {.key = "span", .role = KeyRole::Expr},
    {.key = "surface", .role = KeyRole::Text},
    {.key = "a11y", .role = KeyRole::ExprMap},
    {.key = "drag", .role = KeyRole::ExprMap},
    {.key = "drop", .role = KeyRole::Entry, .shape = "drop"},
    {.key = "kind", .role = KeyRole::Text},
}};

/// @brief A node kind or an entry shape, and the keys it accepts beyond the common ones.
struct KindSpec {
    /// @brief The kind or shape.
    std::string_view kind;
    /// @brief Its own keys.
    std::vector<KeySpec> keys;
    /// @brief Whether it is an entry shape, which takes none of the common keys.
    bool entry = false;
};

/// @brief The node kinds of `ui/1` and the entry shapes they hold.
/// @return The vocabulary, by kind.
[[nodiscard]] inline std::vector<KindSpec> const& nodeVocabulary() {
    using enum KeyRole;
    static std::vector<KindSpec> const kVocabulary{
        {.kind = "text", .keys = {{"text", Expr}, {"role", Expr}}},
        {.kind = "button",
         .keys = {{"label", Expr}, {"icon", Expr}, {"role", Expr}, {"onClick", Commands}, {"onActivate", Commands}}},
        {.kind = "textInput",
         .keys = {{"value", Expr},
                  {"placeholder", Expr},
                  {"mode", Expr},
                  {"commitOn", Text},
                  {"onChange", Commands},
                  {"onCommit", Commands},
                  {"onSubmit", Commands}}},
        {.kind = "checkbox", .keys = {{"label", Expr}, {"checked", Expr}, {"onToggle", Commands}}},
        {.kind = "select", .keys = {{"options", Expr}, {"selected", Expr}, {"onStale", Text}, {"onChoose", Commands}}},
        {.kind = "menu", .keys = {{"label", Expr}, {"icon", Expr}, {"items", Entries, "menuItem"}}},
        {.kind = "column", .keys = {{"gap", Expr}, {"children", Nodes}}},
        {.kind = "row", .keys = {{"gap", Expr}, {"children", Nodes}}},
        {.kind = "grid", .keys = {{"gap", Expr}, {"columns", Expr}, {"children", Nodes}}},
        {.kind = "spacer", .keys = {}},
        {.kind = "panel", .keys = {{"title", Expr}, {"children", Nodes}}},
        {.kind = "scroll", .keys = {{"children", Nodes}}},
        {.kind = "switch", .keys = {{"value", Expr}, {"cases", Entries, "case"}, {"default", Nodes}}},
        {.kind = "tabs", .keys = {{"tabs", Entries, "tab"}, {"selected", Expr}, {"onChoose", Commands}}},
        {.kind = "dialog",
         .keys = {{"title", Expr},
                  {"open", Expr},
                  {"showErrors", Text},
                  {"children", Nodes},
                  {"actions", Nodes},
                  {"onDismiss", Commands},
                  {"scope", Scope}}},
        {.kind = "drawer",
         .keys = {{"title", Expr},
                  {"open", Expr},
                  {"side", Text},
                  {"showErrors", Text},
                  {"children", Nodes},
                  {"actions", Nodes},
                  {"onDismiss", Commands},
                  {"scope", Scope}}},
        {.kind = "busy", .keys = {{"busy", Expr}, {"label", Expr}, {"children", Nodes}}},
        {.kind = "forEach", .keys = {{"rows", Expr}, {"key", Expr}, {"scope", Scope}, {"children", Nodes}}},
        {.kind = "table",
         .keys = {{"rows", Expr},
                  {"key", Expr},
                  {"model", Text},
                  {"action", Text},
                  {"selection", Text},
                  {"pageSize", Expr},
                  {"highlightKey", Expr},
                  {"columns", Entries, "column"},
                  {"scope", Scope},
                  {"onActivate", Commands}}},
        {.kind = "dateTimeInput", .keys = {{"value", Expr}, {"onChange", Commands}, {"onCommit", Commands}}},
        {.kind = "slider",
         .keys = {{"value", Expr},
                  {"min", Expr},
                  {"max", Expr},
                  {"step", Expr},
                  {"onChange", Commands},
                  {"onCommit", Commands}}},
        {.kind = "filePicker", .keys = {{"accept", Expr}, {"multiple", Expr}, {"onPick", Commands}}},
        {.kind = "banner",
         .keys = {{"tone", Expr},
                  {"text", Expr},
                  {"dismissible", Expr},
                  {"action", Entry, "action"},
                  {"onDismiss", Commands}}},
        {.kind = "badge", .keys = {{"tone", Expr}, {"text", Expr}, {"icon", Expr}}},
        {.kind = "progress", .keys = {{"value", Expr}, {"label", Expr}}},
        {.kind = "steps", .keys = {{"items", Expr}, {"current", Expr}}},
        {.kind = "keyValue", .keys = {{"items", Expr}}},
        {.kind = "emptyState", .keys = {{"title", Expr}, {"text", Expr}, {"icon", Expr}, {"action", Entry, "action"}}},
        {.kind = "splitter", .keys = {{"orientation", Expr}, {"sizes", Expr}, {"children", Nodes}}},
        {.kind = "collapsible",
         .keys = {{"title", Expr}, {"open", Expr}, {"onToggle", Commands}, {"header", Nodes}, {"children", Nodes}}},
        {.kind = "dropZone", .keys = {{"accept", Expr}, {"multiple", Expr}, {"onDrop", Commands}}},
        {.kind = "customize", .keys = {{"point", Text}}},
        {.kind = "boundary", .keys = {{"loading", Nodes}, {"error", Nodes}, {"children", Nodes}}},
        {.kind = "form",
         .keys = {{"model", Text},
                  {"action", Text},
                  {"prefill", Expr},
                  {"submit", Text},
                  {"showErrors", Text},
                  {"overrides", Raw},
                  {"fields", Raw},
                  {"onSuccess", Commands},
                  {"onError", Commands}}},
        {.kind = "collection", .keys = {{"model", Text}, {"action", Text}, {"overrides", Raw}}},
        {.kind = "wizard", .keys = {{"model", Text}, {"action", Text}, {"overrides", Raw}, {"onSuccess", Commands}}},
        {.kind = "custom", .keys = {{"name", Text}, {"props", ExprMap}, {"on", EventMap}}},
        {.kind = "menuItem",
         .keys = {{"label", Expr},
                  {"icon", Expr},
                  {"keys", Expr},
                  {"checked", Expr},
                  {"enabled", Expr},
                  {"onClick", Commands},
                  {"items", Entries, "menuItem"}},
         .entry = true},
        {.kind = "case", .keys = {{"when", Expr}, {"children", Nodes}, {"scope", Scope}}, .entry = true},
        {.kind = "tab",
         .keys = {{"id", Text}, {"label", Expr}, {"icon", Expr}, {"children", Nodes}, {"scope", Scope}},
         .entry = true},
        {.kind = "column",
         .keys = {{"id", Text}, {"label", Expr}, {"width", Expr}, {"sizing", Expr}, {"cell", Nodes}},
         .entry = true},
        {.kind = "action", .keys = {{"label", Expr}, {"enabled", Expr}, {"onClick", Commands}}, .entry = true},
        {.kind = "drop", .keys = {{"accepts", Expr}, {"onDrop", Commands}}, .entry = true},
    };
    return kVocabulary;
}

/// @brief A node kind, or an entry shape, of the vocabulary.
/// @param kind The kind or shape.
/// @param entry Whether to find an entry shape (a table's `column`) rather than a node kind (`column`).
/// @return Its spec, or null when `ui/1` has no such kind.
[[nodiscard]] inline KindSpec const* findKind(std::string_view kind, bool entry = false) {
    auto const& vocabulary = nodeVocabulary();
    auto const found = std::ranges::find_if(
        vocabulary, [kind, entry](KindSpec const& spec) { return spec.kind == kind && spec.entry == entry; });
    return found == vocabulary.end() ? nullptr : &*found;
}

/// @brief What a command's operand holds.
enum class OperandRole : std::uint8_t {
    Target,    ///< The verb's own value is a name (`{"run": "m"}`).
    Expr,      ///< The verb's own value, or the operand, is an expression.
    Text,      ///< A literal string.
    Commands,  ///< A command list.
    Flag,      ///< The verb's own value is `true`.
};

/// @brief A command verb of `ui/1`: what its own value is, and the operands it takes.
struct VerbSpec {
    /// @brief The verb.
    std::string_view verb;
    /// @brief What the verb's own value is.
    OperandRole own;
    /// @brief The operands it takes beside it, and whether each is required.
    std::vector<std::pair<KeySpec, bool>> operands;
};

/// @brief The commands of `ui/1` (spec 5 §8, spec 6).
/// @return The verbs.
[[nodiscard]] inline std::vector<VerbSpec> const& commandVocabulary() {
    using R = OperandRole;
    auto const expr = [](std::string_view key, bool required = false) {
        return std::pair{KeySpec{.key = key, .role = KeyRole::Expr}, required};
    };
    auto const text = [](std::string_view key) {
        return std::pair{KeySpec{.key = key, .role = KeyRole::Text}, false};
    };
    auto const commands = [](std::string_view key) {
        return std::pair{KeySpec{.key = key, .role = KeyRole::Commands}, false};
    };
    static std::vector<VerbSpec> const kVerbs{
        {.verb = "set", .own = R::Target, .operands = {expr("value", true)}},
        {.verb = "reset", .own = R::Target, .operands = {}},
        {.verb = "run", .own = R::Target, .operands = {expr("body"), text("as"), expr("each")}},
        {.verb = "refetch", .own = R::Target, .operands = {}},
        {.verb = "open", .own = R::Target, .operands = {}},
        {.verb = "close", .own = R::Target, .operands = {}},
        {.verb = "navigate",
         .own = R::Target,
         .operands = {expr("params"), text("as"), commands("onResult"), commands("onCancel")}},
        {.verb = "return", .own = R::Expr, .operands = {}},
        {.verb = "focus", .own = R::Target, .operands = {}},
        {.verb = "host", .own = R::Target, .operands = {expr("args")}},
        {.verb = "switchBackend", .own = R::Target, .operands = {}},
        {.verb = "resetPrefs", .own = R::Target, .operands = {}},
        {.verb = "setLocale", .own = R::Target, .operands = {}},
        {.verb = "notify", .own = R::Expr, .operands = {text("role")}},
        {.verb = "seq", .own = R::Commands, .operands = {}},
        {.verb = "all", .own = R::Commands, .operands = {}},
        {.verb = "if", .own = R::Commands, .operands = {}},
        {.verb = "cancelClose", .own = R::Flag, .operands = {}},
        {.verb = "retry", .own = R::Flag, .operands = {}},
        {.verb = "signIn", .own = R::Target, .operands = {}},
        {.verb = "signOut", .own = R::Flag, .operands = {}},
        {.verb = "upload", .own = R::Expr, .operands = {text("to"), text("as"), text("id")}},
        {.verb = "download", .own = R::Expr, .operands = {text("save"), text("to"), text("as"), expr("name")}},
    };
    return kVerbs;
}

/// @brief The lifecycle command lists a screen may declare (spec 6 §3).
inline constexpr std::array<std::string_view, 5> kLifecycleKeys{"onMount", "onUnmount", "onClose", "onSave",
                                                                "onBackendChange"};

namespace detail {

// A name as spec 5 §3 allows: [A-Za-z_][A-Za-z0-9_]*.
[[nodiscard]] inline bool isName(std::string_view name) noexcept {
    if (name.empty() || std::isdigit(static_cast<unsigned char>(name.front())) != 0) {
        return false;
    }
    return std::ranges::all_of(
        name, [](char letter) { return std::isalnum(static_cast<unsigned char>(letter)) != 0 || letter == '_'; });
}

// Reads a document, stopping at the first problem.
// NOLINTBEGIN(misc-no-recursion): a document nests as deep as its JSON does.
class DocumentParser {
public:
    std::optional<Problem> problem;

    [[nodiscard]] bool ok() const noexcept { return !problem.has_value(); }

    bool refuse(std::string path, std::string reason) {
        if (!problem) {
            problem = Problem{.path = std::move(path), .reason = std::move(reason)};
        }
        return false;
    }

    // The key @p key with its must-understand mark removed.
    [[nodiscard]] static std::string_view plain(std::string_view key) noexcept {
        return key.starts_with('!') ? key.substr(1) : key;
    }

    std::optional<Expr> expression(Json const& json, std::string const& path) {
        std::expected<Expr, Problem> tree = parseExpression(json, path);
        if (!tree) {
            refuse(tree.error().path, tree.error().reason);
            return std::nullopt;
        }
        return std::move(*tree);
    }

    std::optional<std::string> text(Json const& json, std::string const& path, std::string_view what) {
        if (!json.is_string()) {
            refuse(path, std::string{what} + " is a string");
            return std::nullopt;
        }
        return json.get_string();
    }

    std::optional<std::int64_t> integer(Json const& json, std::string const& path, std::string_view what) {
        std::optional<std::int64_t> const number = exactInt(json);
        if (!number || *number < 0) {
            refuse(path, std::string{what} + " is a non-negative int");
        }
        return number;
    }

    std::vector<std::string> names(Json const& json, std::string const& path, std::string_view what) {
        std::vector<std::string> out;
        if (!json.is_array()) {
            refuse(path, std::string{what} + " is a list of names");
            return out;
        }
        std::size_t index = 0;
        for (Json const& entry : json.get_array()) {
            if (std::optional<std::string> name = text(entry, path + "/" + std::to_string(index++), what)) {
                out.push_back(std::move(*name));
            }
        }
        return out;
    }

    // An object's members, refusing the whole object at a must-understand key outside @p known.
    bool members(Json const& json, std::string const& path, std::string_view what,
                 std::initializer_list<std::string_view> known) {
        if (!json.is_object()) {
            return refuse(path, std::string{what} + " is an object");
        }
        for (auto const& [key, member] : json.get_object()) {
            if (key.starts_with('!') && std::ranges::find(known, plain(key)) == known.end()) {
                return refuse(path + "/" + pointerToken(key),
                              "`" + key + "` must be understood, and " + std::string{what} + " has no such key");
            }
        }
        return true;
    }

    // The member @p key of @p json, written plain or with the must-understand mark.
    [[nodiscard]] static Json const* member(Json const& json, std::string_view key) {
        if (!json.is_object()) {
            return nullptr;
        }
        auto const& object = json.get_object();
        if (auto const found = object.find(key); found != object.end()) {
            return &found->second;
        }
        if (auto const found = object.find("!" + std::string{key}); found != object.end()) {
            return &found->second;
        }
        return nullptr;
    }

    // The JSON pointer of member @p key, as written.
    [[nodiscard]] static std::string memberPath(Json const& json, std::string const& path, std::string_view key) {
        bool const marked = json.is_object() && !json.get_object().contains(key);
        return path + "/" + (marked ? "!" : "") + pointerToken(key);
    }

    CommandList commands(Json const& json, std::string const& path) {
        CommandList out;
        if (json.is_object()) {
            // A single command stands for a list of one.
            if (std::optional<Command> one = command(json, path)) {
                out.push_back(std::move(*one));
            }
            return out;
        }
        if (!json.is_array()) {
            refuse(path, "a command list is a list of commands");
            return out;
        }
        std::size_t index = 0;
        for (Json const& entry : json.get_array()) {
            std::optional<Command> one = command(entry, path + "/" + std::to_string(index++));
            if (!one) {
                return out;
            }
            out.push_back(std::move(*one));
        }
        return out;
    }

    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one branch per operand role.
    std::optional<Command> command(Json const& json, std::string const& path) {
        if (!json.is_object()) {
            refuse(path, "a command is an object");
            return std::nullopt;
        }
        auto const& verbs = commandVocabulary();
        VerbSpec const* spec = nullptr;
        for (auto const& [key, member] : json.get_object()) {
            auto const found = std::ranges::find(verbs, plain(key), &VerbSpec::verb);
            if (found != verbs.end()) {
                spec = &*found;
                break;
            }
        }
        if (spec == nullptr) {
            std::string const first = json.get_object().empty() ? std::string{} : json.get_object().begin()->first;
            refuse(path, json.get_object().empty() ? "an empty object is not a command"
                                                   : "`" + first + "` is not a command of ui/1");
            return std::nullopt;
        }
        Command out{
            .verb = std::string{spec->verb}, .path = path, .target = {}, .exprs = {}, .texts = {}, .lists = {}};
        for (auto const& [key, member] : json.get_object()) {
            std::string_view const bare = plain(key);
            bool const known = bare == spec->verb || std::ranges::any_of(spec->operands, [bare](auto const& operand) {
                                   return operand.first.key == bare;
                               });
            if (!known && key.starts_with('!')) {
                refuse(path + "/" + pointerToken(key),
                       "`" + key + "` must be understood, and `" + out.verb + "` has no such operand");
                return std::nullopt;
            }
        }
        Json const& own = *member(json, spec->verb);
        std::string const ownPath = memberPath(json, path, spec->verb);
        switch (spec->own) {
            case OperandRole::Target:
                if (out.verb == "navigate" && own.is_object()) {
                    // `{"navigate": {"route": e}}` opens a screen by its route.
                    Json const* const route = member(own, "route");
                    if (route == nullptr) {
                        refuse(ownPath, "`navigate` takes a screen id or {\"route\": e}");
                        return std::nullopt;
                    }
                    std::optional<Expr> routeExpr = expression(*route, ownPath + "/route");
                    if (!routeExpr) {
                        return std::nullopt;
                    }
                    out.exprs.push_back(NamedExpr{.name = "route", .expr = std::move(*routeExpr)});
                } else if (out.verb == "signIn") {
                    Json const* const from = member(own, "from");
                    if (from == nullptr || !from->is_string()) {
                        refuse(ownPath, "`signIn` takes {\"from\": \"mutation\"}");
                        return std::nullopt;
                    }
                    out.target = from->get_string();
                } else {
                    std::optional<std::string> target = text(own, ownPath, "`" + out.verb + "`'s operand");
                    if (!target) {
                        return std::nullopt;
                    }
                    out.target = std::move(*target);
                }
                break;
            case OperandRole::Expr: {
                std::optional<Expr> value = expression(own, ownPath);
                if (!value) {
                    return std::nullopt;
                }
                out.exprs.push_back(NamedExpr{.name = out.verb, .expr = std::move(*value)});
                break;
            }
            case OperandRole::Flag:
                if (!own.is_boolean() || !own.get_boolean()) {
                    refuse(ownPath, "`" + out.verb + "` is written `true`");
                    return std::nullopt;
                }
                break;
            case OperandRole::Commands:
                if (out.verb == "if") {
                    if (!own.is_array() || own.get_array().size() != 3) {
                        refuse(ownPath, "`if` takes [condition, [then...], [else...]]");
                        return std::nullopt;
                    }
                    std::optional<Expr> condition = expression(own.get_array().at(0), ownPath + "/0");
                    if (!condition) {
                        return std::nullopt;
                    }
                    out.exprs.push_back(NamedExpr{.name = "if", .expr = std::move(*condition)});
                    out.lists.push_back(
                        NamedCommands{.name = "then", .commands = commands(own.get_array().at(1), ownPath + "/1")});
                    out.lists.push_back(
                        NamedCommands{.name = "else", .commands = commands(own.get_array().at(2), ownPath + "/2")});
                } else {
                    out.lists.push_back(NamedCommands{.name = out.verb, .commands = commands(own, ownPath)});
                }
                break;
            case OperandRole::Text:
            default:
                break;
        }
        for (auto const& [operand, required] : spec->operands) {
            Json const* const value = member(json, operand.key);
            std::string const operandPath = memberPath(json, path, operand.key);
            if (value == nullptr) {
                if (required) {
                    refuse(path, "`" + out.verb + "` needs `" + std::string{operand.key} + "`");
                    return std::nullopt;
                }
                continue;
            }
            if (operand.role == KeyRole::Expr) {
                std::optional<Expr> parsed = expression(*value, operandPath);
                if (!parsed) {
                    return std::nullopt;
                }
                out.exprs.push_back(NamedExpr{.name = std::string{operand.key}, .expr = std::move(*parsed)});
            } else if (operand.role == KeyRole::Text) {
                std::optional<std::string> parsed = text(*value, operandPath, "`" + std::string{operand.key} + "`");
                if (!parsed) {
                    return std::nullopt;
                }
                out.texts.push_back(NamedText{.name = std::string{operand.key}, .text = std::move(*parsed)});
            } else {
                out.lists.push_back(
                    NamedCommands{.name = std::string{operand.key}, .commands = commands(*value, operandPath)});
            }
        }
        if (!ok()) {
            return std::nullopt;
        }
        return out;
    }

    // An object of declarations: each member a name, in order.
    template <typename Each>
    void declarations(Json const& json, std::string const& path, std::string_view what, Each&& each) {
        if (!json.is_object()) {
            refuse(path, std::string{what} + " is an object of declarations");
            return;
        }
        for (auto const& [name, member] : json.get_object()) {
            std::string const memberPathText = path + "/" + pointerToken(name);
            if (!isName(name)) {
                refuse(memberPathText, "`" + name + "` is not a name: a name is [A-Za-z_][A-Za-z0-9_]*");
                return;
            }
            each(name, member, memberPathText);
            if (!ok()) {
                return;
            }
        }
    }

    std::optional<PatchDecl> patch(Json const& json, std::string const& path) {
        if (!members(json, path, "a patch", {"query", "key", "with"})) {
            return std::nullopt;
        }
        Json const* const query = member(json, "query");
        Json const* const key = member(json, "key");
        Json const* const with = member(json, "with");
        if (query == nullptr || key == nullptr || with == nullptr) {
            refuse(path, "a patch is {\"query\": \"q\", \"key\": e, \"with\": e}");
            return std::nullopt;
        }
        std::optional<std::string> queryName = text(*query, memberPath(json, path, "query"), "`query`");
        std::optional<Expr> keyExpr = expression(*key, memberPath(json, path, "key"));
        std::optional<Expr> withExpr = expression(*with, memberPath(json, path, "with"));
        if (!queryName || !keyExpr || !withExpr) {
            return std::nullopt;
        }
        return PatchDecl{
            .query = std::move(*queryName), .path = path, .key = std::move(*keyExpr), .with = std::move(*withExpr)};
    }

    // Reads the scope keys of @p json into @p scope; other keys are left to the caller.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one block per declaration kind.
    void scopeKeys(Json const& json, std::string const& path, Scope& scope) {
        scope.path = path;
        if (Json const* const models = member(json, "models")) {
            declarations(
                *models, memberPath(json, path, "models"), "`models`",
                [&](std::string const& name, Json const& entry, std::string const& at) {
                    if (!members(entry, at, "a model", {"type", "instance"})) {
                        return;
                    }
                    Json const* const type = member(entry, "type");
                    if (type == nullptr) {
                        refuse(at, "a model names its `type`");
                        return;
                    }
                    std::optional<std::string> typeName = text(*type, memberPath(entry, at, "type"), "`type`");
                    if (!typeName) {
                        return;
                    }
                    ModelDecl decl{.name = name, .path = at, .type = std::move(*typeName), .instance = {}, .key = {}};
                    if (Json const* const instance = member(entry, "instance")) {
                        if (instance->is_string() && instance->get_string() == "private") {
                            decl.instance = ModelDecl::Instance::Private;
                        } else if (instance->is_string() && instance->get_string() == "shared") {
                            decl.instance = ModelDecl::Instance::Shared;
                        } else {
                            decl.instance = ModelDecl::Instance::Keyed;
                            decl.key = expression(*instance, memberPath(entry, at, "instance"));
                        }
                    }
                    scope.models.push_back(std::move(decl));
                });
        }
        if (Json const* const state = member(json, "state")) {
            declarations(*state, memberPath(json, path, "state"), "`state`",
                         [&](std::string const& name, Json const& entry, std::string const& at) {
                             if (!members(entry, at, "a state entry", {"init", "type"})) {
                                 return;
                             }
                             StateDecl decl{.name = name, .path = at, .type = {}, .init = Expr{.path = at}};
                             if (Json const* const type = member(entry, "type")) {
                                 std::optional<std::string> written =
                                     text(*type, memberPath(entry, at, "type"), "`type`");
                                 if (!written) {
                                     return;
                                 }
                                 std::expected<Type, std::string> parsed = parseType(*written);
                                 if (!parsed) {
                                     refuse(memberPath(entry, at, "type"), parsed.error());
                                     return;
                                 }
                                 decl.type = *parsed;
                             }
                             if (Json const* const init = member(entry, "init")) {
                                 std::optional<Expr> value = expression(*init, memberPath(entry, at, "init"));
                                 if (!value) {
                                     return;
                                 }
                                 decl.init = std::move(*value);
                             }
                             scope.state.push_back(std::move(decl));
                         });
        }
        if (Json const* const lets = member(json, "let")) {
            declarations(*lets, memberPath(json, path, "let"), "`let`",
                         [&](std::string const& name, Json const& entry, std::string const& at) {
                             if (std::optional<Expr> value = expression(entry, at)) {
                                 scope.lets.push_back(LetDecl{.name = name, .path = at, .value = std::move(*value)});
                             }
                         });
        }
        if (Json const* const queries = member(json, "queries")) {
            declarations(*queries, memberPath(json, path, "queries"), "`queries`",
                         [&](std::string const& name, Json const& entry, std::string const& at) {
                             if (std::optional<QueryDecl> decl = query(name, entry, at)) {
                                 scope.queries.push_back(std::move(*decl));
                             }
                         });
        }
        if (Json const* const mutations = member(json, "mutations")) {
            declarations(*mutations, memberPath(json, path, "mutations"), "`mutations`",
                         [&](std::string const& name, Json const& entry, std::string const& at) {
                             if (std::optional<MutationDecl> decl = mutation(name, entry, at)) {
                                 scope.mutations.push_back(std::move(*decl));
                             }
                         });
        }
        if (Json const* const watches = member(json, "watch")) {
            std::string const at = memberPath(json, path, "watch");
            if (!watches->is_array()) {
                refuse(at, "`watch` is a list of watches");
                return;
            }
            std::size_t index = 0;
            for (Json const& entry : watches->get_array()) {
                std::string const entryPath = at + "/" + std::to_string(index++);
                if (!members(entry, entryPath, "a watch", {"on", "source", "do"})) {
                    return;
                }
                Json const* const on = member(entry, "on");
                Json const* const run = member(entry, "do");
                if (on == nullptr || run == nullptr) {
                    refuse(entryPath, "a watch is {\"on\": e, \"do\": [...]}");
                    return;
                }
                std::optional<Expr> observed = expression(*on, memberPath(entry, entryPath, "on"));
                if (!observed) {
                    return;
                }
                WatchDecl decl{.path = entryPath, .on = std::move(*observed), .userOnly = false, .commands = {}};
                if (Json const* const source = member(entry, "source")) {
                    if (!source->is_string() || (source->get_string() != "user" && source->get_string() != "any")) {
                        refuse(memberPath(entry, entryPath, "source"), "`source` is `user` or `any`");
                        return;
                    }
                    decl.userOnly = source->get_string() == "user";
                }
                decl.commands = commands(*run, memberPath(entry, entryPath, "do"));
                scope.watches.push_back(std::move(decl));
            }
        }
        if (Json const* const checks = member(json, "checks")) {
            std::string const at = memberPath(json, path, "checks");
            if (!checks->is_array()) {
                refuse(at, "`checks` is a list of checks");
                return;
            }
            std::size_t index = 0;
            for (Json const& entry : checks->get_array()) {
                std::string const entryPath = at + "/" + std::to_string(index++);
                if (!members(entry, entryPath, "a check", {"id", "assert", "message", "targets", "blocking"})) {
                    return;
                }
                Json const* const assertion = member(entry, "assert");
                if (assertion == nullptr) {
                    refuse(entryPath, "a check has an `assert`");
                    return;
                }
                std::optional<Expr> asserted = expression(*assertion, memberPath(entry, entryPath, "assert"));
                if (!asserted) {
                    return;
                }
                CheckDecl decl{.id = {},
                               .path = entryPath,
                               .assertion = std::move(*asserted),
                               .message = {},
                               .targets = {},
                               .blocking = true};
                if (Json const* const id = member(entry, "id")) {
                    decl.id = text(*id, memberPath(entry, entryPath, "id"), "`id`").value_or(std::string{});
                }
                if (Json const* const message = member(entry, "message")) {
                    decl.message = expression(*message, memberPath(entry, entryPath, "message"));
                }
                if (Json const* const targets = member(entry, "targets")) {
                    decl.targets = names(*targets, memberPath(entry, entryPath, "targets"), "`targets`");
                }
                if (Json const* const blocking = member(entry, "blocking")) {
                    if (!blocking->is_boolean()) {
                        refuse(memberPath(entry, entryPath, "blocking"), "`blocking` is a bool");
                        return;
                    }
                    decl.blocking = blocking->get_boolean();
                }
                scope.checks.push_back(std::move(decl));
            }
        }
    }

    std::optional<QueryDecl> query(std::string const& name, Json const& entry, std::string const& at) {
        if (!members(entry, at, "a query",
                     {"model", "action", "evaluate", "body", "when", "debounce", "refreshOn", "refreshEvery",
                      "staleTime"})) {
            return std::nullopt;
        }
        QueryDecl decl{.name = name,
                       .path = at,
                       .model = {},
                       .action = {},
                       .evaluate = false,
                       .body = Expr{.path = at},
                       .when = {},
                       .debounce = 0,
                       .refreshOn = {},
                       .refreshEvery = 0,
                       .staleTime = 0};
        Json const* const model = member(entry, "model");
        Json const* const action = member(entry, "action");
        Json const* const evaluate = member(entry, "evaluate");
        if (model == nullptr || (action == nullptr) == (evaluate == nullptr)) {
            refuse(at, "a query names its `model` and one of `action` or `evaluate`");
            return std::nullopt;
        }
        decl.model = text(*model, memberPath(entry, at, "model"), "`model`").value_or(std::string{});
        decl.evaluate = evaluate != nullptr;
        decl.action = text(decl.evaluate ? *evaluate : *action,
                           memberPath(entry, at, decl.evaluate ? "evaluate" : "action"), "the action id")
                          .value_or(std::string{});
        if (Json const* const body = member(entry, "body")) {
            std::optional<Expr> parsed = expression(*body, memberPath(entry, at, "body"));
            if (!parsed) {
                return std::nullopt;
            }
            decl.body = std::move(*parsed);
        } else {
            // No body is an empty request, not an idle query.
            decl.body.form = Expr::Form::Object;
        }
        if (Json const* const when = member(entry, "when")) {
            decl.when = expression(*when, memberPath(entry, at, "when"));
        }
        for (auto [key, field] :
             {std::pair{"debounce", &QueryDecl::debounce}, std::pair{"refreshEvery", &QueryDecl::refreshEvery},
              std::pair{"staleTime", &QueryDecl::staleTime}}) {
            if (Json const* const value = member(entry, key)) {
                decl.*field = integer(*value, memberPath(entry, at, key), "`" + std::string{key} + "`").value_or(0);
            }
        }
        if (Json const* const refreshOn = member(entry, "refreshOn")) {
            decl.refreshOn = names(*refreshOn, memberPath(entry, at, "refreshOn"), "`refreshOn`");
        }
        if (!ok()) {
            return std::nullopt;
        }
        return decl;
    }

    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one block per key of a mutation.
    std::optional<MutationDecl> mutation(std::string const& name, Json const& entry, std::string const& at) {
        if (!members(entry, at, "a mutation",
                     {"model", "action", "body", "concurrency", "serialKey", "invalidates", "patch", "optimistic",
                      "onSuccess", "onError", "offline"})) {
            return std::nullopt;
        }
        MutationDecl decl{.name = name,
                          .path = at,
                          .model = {},
                          .action = {},
                          .body = {},
                          .concurrency = {},
                          .serialKey = {},
                          .invalidates = {},
                          .patch = {},
                          .optimistic = {},
                          .onSuccess = {},
                          .onError = {},
                          .offlineQueue = false};
        Json const* const model = member(entry, "model");
        Json const* const action = member(entry, "action");
        if (model == nullptr || action == nullptr) {
            refuse(at, "a mutation names its `model` and `action`");
            return std::nullopt;
        }
        decl.model = text(*model, memberPath(entry, at, "model"), "`model`").value_or(std::string{});
        decl.action = text(*action, memberPath(entry, at, "action"), "`action`").value_or(std::string{});
        if (Json const* const body = member(entry, "body")) {
            decl.body = expression(*body, memberPath(entry, at, "body"));
        }
        if (Json const* const concurrency = member(entry, "concurrency")) {
            static constexpr std::array<std::pair<std::string_view, MutationDecl::Concurrency>, 4> kPolicies{{
                {"exclusive", MutationDecl::Concurrency::Exclusive},
                {"serial", MutationDecl::Concurrency::Serial},
                {"latest", MutationDecl::Concurrency::Latest},
                {"parallel", MutationDecl::Concurrency::Parallel},
            }};
            auto const found = concurrency->is_string()
                                   ? std::ranges::find(kPolicies, concurrency->get_string(),
                                                       &std::pair<std::string_view, MutationDecl::Concurrency>::first)
                                   : kPolicies.end();
            if (found == kPolicies.end()) {
                refuse(memberPath(entry, at, "concurrency"),
                       "`concurrency` is `exclusive`, `serial`, `latest` or `parallel`");
                return std::nullopt;
            }
            decl.concurrency = found->second;
        }
        if (Json const* const serialKey = member(entry, "serialKey")) {
            decl.serialKey = expression(*serialKey, memberPath(entry, at, "serialKey"));
        }
        if (Json const* const invalidates = member(entry, "invalidates")) {
            decl.invalidates = names(*invalidates, memberPath(entry, at, "invalidates"), "`invalidates`");
        }
        if (Json const* const patchJson = member(entry, "patch")) {
            decl.patch = patch(*patchJson, memberPath(entry, at, "patch"));
        }
        if (Json const* const optimistic = member(entry, "optimistic")) {
            decl.optimistic = patch(*optimistic, memberPath(entry, at, "optimistic"));
        }
        if (Json const* const onSuccess = member(entry, "onSuccess")) {
            decl.onSuccess = commands(*onSuccess, memberPath(entry, at, "onSuccess"));
        }
        if (Json const* const onError = member(entry, "onError")) {
            decl.onError = commands(*onError, memberPath(entry, at, "onError"));
        }
        if (Json const* const offline = member(entry, "offline")) {
            if (!offline->is_string() || offline->get_string() != "queue") {
                refuse(memberPath(entry, at, "offline"), "`offline` is `queue`");
                return std::nullopt;
            }
            decl.offlineQueue = true;
        }
        if (!ok()) {
            return std::nullopt;
        }
        return decl;
    }

    std::vector<Node> nodes(Json const& json, std::string const& path, std::string_view shape = {}) {
        std::vector<Node> out;
        if (json.is_object()) {
            if (std::optional<Node> one = node(json, path, shape)) {
                out.push_back(std::move(*one));
            }
            return out;
        }
        if (!json.is_array()) {
            refuse(path, shape.empty() ? "a node list is a list of nodes"
                                       : "a list of `" + std::string{shape} + "` entries");
            return out;
        }
        std::size_t index = 0;
        for (Json const& entry : json.get_array()) {
            std::optional<Node> one = node(entry, path + "/" + std::to_string(index++), shape);
            if (!one) {
                return out;
            }
            out.push_back(std::move(*one));
        }
        return out;
    }

    // A node, or with @p shape an entry of that shape.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one branch per key role.
    std::optional<Node> node(Json const& json, std::string const& path, std::string_view shape = {}) {
        if (!json.is_object()) {
            refuse(path, shape.empty() ? "a node is an object" : "a `" + std::string{shape} + "` entry is an object");
            return std::nullopt;
        }
        Node out;
        out.path = path;
        if (shape.empty()) {
            Json const* const kind = member(json, "kind");
            if (kind == nullptr || !kind->is_string()) {
                refuse(path, "a node names its `kind`");
                return std::nullopt;
            }
            out.kind = kind->get_string();
        } else {
            out.kind = std::string{shape};
        }
        KindSpec const* const spec = findKind(out.kind, !shape.empty());
        if (spec == nullptr) {
            refuse(memberPath(json, path, "kind"), "`" + out.kind + "` is not a node kind of ui/1");
            return std::nullopt;
        }
        auto const specOf = [&](std::string_view key) -> KeySpec const* {
            auto const own = std::ranges::find(spec->keys, key, &KeySpec::key);
            if (own != spec->keys.end()) {
                return &*own;
            }
            if (spec->entry) {
                return nullptr;
            }
            auto const common = std::ranges::find(kCommonKeys, key, &KeySpec::key);
            return common == kCommonKeys.end() ? nullptr : &*common;
        };
        for (auto const& [key, member] : json.get_object()) {
            std::string const at = path + "/" + pointerToken(key);
            KeySpec const* const keySpec = specOf(plain(key));
            if (keySpec == nullptr) {
                if (key.starts_with('!')) {
                    out.refused = "`" + key + "` must be understood, and `" + out.kind + "` has no such key";
                }
                continue;
            }
            std::string const name{keySpec->key};
            switch (keySpec->role) {
                case KeyRole::Expr:
                    if (std::optional<Expr> value = expression(member, at)) {
                        out.props.push_back(NamedExpr{.name = name, .expr = std::move(*value)});
                    }
                    break;
                case KeyRole::Text:
                    if (name == "kind") {
                        break;
                    }
                    if (std::optional<std::string> value = text(member, at, "`" + name + "`")) {
                        out.texts.push_back(NamedText{.name = name, .text = std::move(*value)});
                    }
                    break;
                case KeyRole::Commands:
                    out.events.push_back(NamedCommands{.name = name, .commands = commands(member, at)});
                    break;
                case KeyRole::Nodes:
                    out.slots.push_back(NodeSlot{.name = name, .nodes = nodes(member, at)});
                    break;
                case KeyRole::Entries:
                    out.slots.push_back(NodeSlot{.name = name, .nodes = nodes(member, at, keySpec->shape)});
                    break;
                case KeyRole::Entry:
                    if (std::optional<Node> entry = node(member, at, keySpec->shape)) {
                        out.slots.push_back(NodeSlot{.name = name, .nodes = {std::move(*entry)}});
                    }
                    break;
                case KeyRole::ExprMap: {
                    if (!member.is_object()) {
                        refuse(at, "`" + name + "` is an object of expressions");
                        break;
                    }
                    std::vector<NamedExpr> entries;
                    for (auto const& [field, value] : member.get_object()) {
                        if (std::optional<Expr> parsed = expression(value, at + "/" + pointerToken(field))) {
                            entries.push_back(NamedExpr{.name = field, .expr = std::move(*parsed)});
                        }
                    }
                    out.maps.emplace_back(name, std::move(entries));
                    break;
                }
                case KeyRole::EventMap:
                    if (!member.is_object()) {
                        refuse(at, "`on` is an object of command lists");
                        break;
                    }
                    for (auto const& [event, list] : member.get_object()) {
                        out.events.push_back(NamedCommands{
                            .name = "on." + event, .commands = commands(list, at + "/" + pointerToken(event))});
                    }
                    break;
                case KeyRole::Scope: {
                    if (!members(member, at, "a scope",
                                 {"models", "state", "let", "queries", "mutations", "watch", "checks"})) {
                        break;
                    }
                    auto scope = std::make_shared<Scope>();
                    scopeKeys(member, at, *scope);
                    out.scope = std::move(scope);
                    break;
                }
                case KeyRole::Raw:
                default:
                    out.raw.emplace_back(name, member);
                    break;
            }
            if (!ok()) {
                return std::nullopt;
            }
        }
        return out;
    }

    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one block per top-level key.
    void topLevel(Json const& json, Document& document) {
        std::initializer_list<std::string_view> const known{
            "vocab",     "id",     "title",           "params",     "models",   "state",      "let",      "queries",
            "mutations", "watch",  "checks",          "view",       "dirty",    "identity",   "onMount",  "onUnmount",
            "onClose",   "onSave", "onBackendChange", "showErrors", "prefetch", "components", "customize"};
        if (!members(json, "", "a screen document", known)) {
            return;
        }
        auto const textKey = [&](std::string_view key, std::string& into) {
            if (Json const* const value = member(json, key)) {
                into = text(*value, memberPath(json, "", key), "`" + std::string{key} + "`").value_or(std::string{});
            }
        };
        textKey("vocab", document.vocab);
        textKey("id", document.id);
        textKey("showErrors", document.showErrors);
        for (auto [key, into] : {std::pair{"title", &document.title}, std::pair{"dirty", &document.dirty},
                                 std::pair{"identity", &document.identity}}) {
            if (Json const* const value = member(json, key)) {
                *into = expression(*value, memberPath(json, "", key));
            }
        }
        if (Json const* const params = member(json, "params")) {
            declarations(*params, memberPath(json, "", "params"), "`params`",
                         [&](std::string const& name, Json const& entry, std::string const& at) {
                             std::optional<std::string> written = text(entry, at, "a param's type");
                             if (!written) {
                                 return;
                             }
                             std::expected<Type, std::string> type = parseType(*written);
                             if (!type) {
                                 refuse(at, type.error());
                                 return;
                             }
                             document.params.emplace_back(name, *type);
                         });
        }
        for (std::string_view const key : kLifecycleKeys) {
            if (Json const* const list = member(json, key)) {
                document.lifecycle.push_back(
                    NamedCommands{.name = std::string{key}, .commands = commands(*list, memberPath(json, "", key))});
            }
        }
        scopeKeys(json, "", document.scope);
        if (Json const* const prefetch = member(json, "prefetch")) {
            document.prefetch = names(*prefetch, memberPath(json, "", "prefetch"), "`prefetch`");
        }
        if (Json const* const components = member(json, "components")) {
            declarations(*components, memberPath(json, "", "components"), "`components`",
                         [&](std::string const& name, Json const& entry, std::string const& at) {
                             component(name, entry, at, document);
                         });
        }
        if (Json const* const customize = member(json, "customize")) {
            declarations(*customize, memberPath(json, "", "customize"), "`customize`",
                         [&](std::string const& name, Json const& entry, std::string const& at) {
                             point(name, entry, at, document);
                         });
        }
        if (!ok()) {
            return;
        }
        Json const* const view = member(json, "view");
        if (view == nullptr) {
            refuse("", "a screen document has a `view`");
            return;
        }
        document.view = node(*view, memberPath(json, "", "view"));
    }

    void component(std::string const& name, Json const& entry, std::string const& at, Document& document) {
        if (!members(entry, at, "a component", {"qml", "host", "version", "props", "events", "fallback"})) {
            return;
        }
        ComponentDecl decl{.name = name, .path = at, .qml = {}, .host = {}, .props = {}, .events = {}, .fallback = {}};
        Json const* const qml = member(entry, "qml");
        Json const* const host = member(entry, "host");
        if ((qml == nullptr) == (host == nullptr)) {
            refuse(at, "a component names one of `qml` or `host`");
            return;
        }
        if (qml != nullptr) {
            decl.qml = text(*qml, memberPath(entry, at, "qml"), "`qml`").value_or(std::string{});
        } else {
            decl.host = text(*host, memberPath(entry, at, "host"), "`host`").value_or(std::string{});
        }
        for (auto [key, into] : {std::pair{"props", &decl.props}, std::pair{"events", &decl.events}}) {
            Json const* const map = member(entry, key);
            if (map == nullptr) {
                continue;
            }
            std::string const mapPath = memberPath(entry, at, key);
            if (!map->is_object()) {
                refuse(mapPath, "`" + std::string{key} + "` is an object of types");
                return;
            }
            for (auto const& [field, type] : map->get_object()) {
                into->push_back(NamedText{
                    .name = field,
                    .text = text(type, mapPath + "/" + pointerToken(field), "a type").value_or(std::string{})});
            }
        }
        if (Json const* const fallback = member(entry, "fallback")) {
            decl.fallback = nodes(*fallback, memberPath(entry, at, "fallback"));
        }
        document.components.push_back(std::move(decl));
    }

    void point(std::string const& name, Json const& entry, std::string const& at, Document& document) {
        if (!members(entry, at, "a customization point", {"kind", "of", "options", "type", "default"})) {
            return;
        }
        CustomizePoint decl{
            .name = name, .path = at, .kind = {}, .of = {}, .options = {}, .type = {}, .defaultValue = {}};
        Json const* const kind = member(entry, "kind");
        static constexpr std::array<std::string_view, 7> kKinds{"columns", "choice", "value",  "layout",
                                                                "saved",   "pins",   "arrange"};
        if (kind == nullptr || !kind->is_string() || std::ranges::find(kKinds, kind->get_string()) == kKinds.end()) {
            refuse(memberPath(entry, at, "kind"),
                   "a point's `kind` is `columns`, `choice`, `value`, `layout`, `saved`, `pins` or `arrange`");
            return;
        }
        decl.kind = kind->get_string();
        if (Json const* const of = member(entry, "of")) {
            decl.of = text(*of, memberPath(entry, at, "of"), "`of`").value_or(std::string{});
        }
        if (Json const* const options = member(entry, "options")) {
            decl.options = names(*options, memberPath(entry, at, "options"), "`options`");
        }
        if (Json const* const type = member(entry, "type")) {
            std::expected<Type, std::string> parsed =
                parseType(text(*type, memberPath(entry, at, "type"), "`type`").value_or(std::string{}));
            if (!parsed) {
                refuse(memberPath(entry, at, "type"), parsed.error());
                return;
            }
            decl.type = *parsed;
        }
        if (Json const* const fallback = member(entry, "default")) {
            std::expected<Value, std::string> value =
                decl.type ? decode(*fallback, *decl.type) : decodeUntyped(*fallback);
            if (!value) {
                refuse(memberPath(entry, at, "default"), value.error());
                return;
            }
            decl.defaultValue = std::move(*value);
        }
        document.customize.push_back(std::move(decl));
    }
};
// NOLINTEND(misc-no-recursion)

}  // namespace detail

/// @brief Parses a screen document.
///
/// A key the parser does not know is ignored, so a newer server's optional keys reach an older client harmlessly.
/// A key marked must-understand (`"!name"`) is read as `name` when the parser knows it; when it does not, the node
/// that holds it is marked `refused` (it renders its fallback), and at the top level, or in a declaration, the
/// document is refused.
/// @param json The document's JSON.
/// @return The document, or the first problem, with its JSON pointer.
[[nodiscard]] inline std::expected<Document, Problem> parseDocument(Json const& json) {
    detail::DocumentParser parser;
    Document document;
    parser.topLevel(json, document);
    if (parser.problem) {
        return std::unexpected(std::move(*parser.problem));
    }
    return document;
}

}  // namespace morph::ui

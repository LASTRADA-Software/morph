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
#include <ranges>
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

// A command holds command lists and a node holds nodes, so their copies and comparisons recurse as deep as the
// document nests.
// NOLINTBEGIN(misc-no-recursion)
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

struct DocNode;

/// @brief A node list under a name: a node's `children`, a boundary's `loading`, a column's `cell`.
struct NodeSlot {
    /// @brief The slot's key.
    std::string name;
    /// @brief The nodes.
    std::vector<DocNode> nodes;
};

/// @brief A view node (spec 5 §9), or a structured entry of one: a table's column, a `switch` case, a tab, a menu
///        item, a banner's action, a drop target.
struct DocNode {
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
    [[nodiscard]] std::vector<DocNode> const* slot(std::string_view name) const noexcept {
        auto const found = std::ranges::find(slots, name, &NodeSlot::name);
        return found == slots.end() ? nullptr : &found->nodes;
    }
};

// NOLINTEND(misc-no-recursion)

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
    std::vector<DocNode> fallback;
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
    std::optional<DocNode> view;
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
    std::string_view shape;
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
        {.kind = "text", .keys = {{.key = "text", .role = Expr}, {.key = "role", .role = Expr}}},
        {.kind = "button",
         .keys = {{.key = "label", .role = Expr},
                  {.key = "icon", .role = Expr},
                  {.key = "role", .role = Expr},
                  {.key = "onClick", .role = Commands},
                  {.key = "onActivate", .role = Commands}}},
        {.kind = "textInput",
         .keys = {{.key = "value", .role = Expr},
                  {.key = "placeholder", .role = Expr},
                  {.key = "mode", .role = Expr},
                  {.key = "commitOn", .role = Text},
                  {.key = "onChange", .role = Commands},
                  {.key = "onCommit", .role = Commands},
                  {.key = "onSubmit", .role = Commands}}},
        {.kind = "checkbox",
         .keys = {{.key = "label", .role = Expr},
                  {.key = "checked", .role = Expr},
                  {.key = "onToggle", .role = Commands}}},
        {.kind = "select",
         .keys = {{.key = "options", .role = Expr},
                  {.key = "selected", .role = Expr},
                  {.key = "onStale", .role = Text},
                  {.key = "onChoose", .role = Commands}}},
        {.kind = "menu",
         .keys = {{.key = "label", .role = Expr},
                  {.key = "icon", .role = Expr},
                  {.key = "items", .role = Entries, .shape = "menuItem"}}},
        {.kind = "column", .keys = {{.key = "gap", .role = Expr}, {.key = "children", .role = Nodes}}},
        {.kind = "row", .keys = {{.key = "gap", .role = Expr}, {.key = "children", .role = Nodes}}},
        {.kind = "grid",
         .keys = {{.key = "gap", .role = Expr}, {.key = "columns", .role = Expr}, {.key = "children", .role = Nodes}}},
        {.kind = "spacer", .keys = {}},
        {.kind = "panel", .keys = {{.key = "title", .role = Expr}, {.key = "children", .role = Nodes}}},
        {.kind = "scroll", .keys = {{.key = "children", .role = Nodes}}},
        {.kind = "switch",
         .keys = {{.key = "value", .role = Expr},
                  {.key = "cases", .role = Entries, .shape = "case"},
                  {.key = "default", .role = Nodes}}},
        {.kind = "tabs",
         .keys = {{.key = "tabs", .role = Entries, .shape = "tab"},
                  {.key = "selected", .role = Expr},
                  {.key = "onChoose", .role = Commands}}},
        {.kind = "dialog",
         .keys = {{.key = "title", .role = Expr},
                  {.key = "open", .role = Expr},
                  {.key = "showErrors", .role = Text},
                  {.key = "children", .role = Nodes},
                  {.key = "actions", .role = Nodes},
                  {.key = "onDismiss", .role = Commands},
                  {.key = "scope", .role = Scope}}},
        {.kind = "drawer",
         .keys = {{.key = "title", .role = Expr},
                  {.key = "open", .role = Expr},
                  {.key = "side", .role = Text},
                  {.key = "showErrors", .role = Text},
                  {.key = "children", .role = Nodes},
                  {.key = "actions", .role = Nodes},
                  {.key = "onDismiss", .role = Commands},
                  {.key = "scope", .role = Scope}}},
        {.kind = "busy",
         .keys = {{.key = "busy", .role = Expr}, {.key = "label", .role = Expr}, {.key = "children", .role = Nodes}}},
        {.kind = "forEach",
         .keys = {{.key = "rows", .role = Expr},
                  {.key = "key", .role = Expr},
                  {.key = "scope", .role = Scope},
                  {.key = "children", .role = Nodes}}},
        {.kind = "table",
         .keys = {{.key = "rows", .role = Expr},
                  {.key = "key", .role = Expr},
                  {.key = "model", .role = Text},
                  {.key = "action", .role = Text},
                  {.key = "selection", .role = Text},
                  {.key = "pageSize", .role = Expr},
                  {.key = "highlightKey", .role = Expr},
                  {.key = "columns", .role = Entries, .shape = "column"},
                  {.key = "scope", .role = Scope},
                  {.key = "onActivate", .role = Commands}}},
        {.kind = "dateTimeInput",
         .keys = {{.key = "value", .role = Expr},
                  {.key = "onChange", .role = Commands},
                  {.key = "onCommit", .role = Commands}}},
        {.kind = "slider",
         .keys = {{.key = "value", .role = Expr},
                  {.key = "min", .role = Expr},
                  {.key = "max", .role = Expr},
                  {.key = "step", .role = Expr},
                  {.key = "onChange", .role = Commands},
                  {.key = "onCommit", .role = Commands}}},
        {.kind = "filePicker",
         .keys = {{.key = "accept", .role = Expr},
                  {.key = "multiple", .role = Expr},
                  {.key = "onPick", .role = Commands}}},
        {.kind = "banner",
         .keys = {{.key = "tone", .role = Expr},
                  {.key = "text", .role = Expr},
                  {.key = "dismissible", .role = Expr},
                  {.key = "action", .role = Entry, .shape = "action"},
                  {.key = "onDismiss", .role = Commands}}},
        {.kind = "badge",
         .keys = {{.key = "tone", .role = Expr}, {.key = "text", .role = Expr}, {.key = "icon", .role = Expr}}},
        {.kind = "progress", .keys = {{.key = "value", .role = Expr}, {.key = "label", .role = Expr}}},
        {.kind = "steps", .keys = {{.key = "items", .role = Expr}, {.key = "current", .role = Expr}}},
        {.kind = "keyValue", .keys = {{.key = "items", .role = Expr}}},
        {.kind = "emptyState",
         .keys = {{.key = "title", .role = Expr},
                  {.key = "text", .role = Expr},
                  {.key = "icon", .role = Expr},
                  {.key = "action", .role = Entry, .shape = "action"}}},
        {.kind = "splitter",
         .keys = {{.key = "orientation", .role = Expr},
                  {.key = "sizes", .role = Expr},
                  {.key = "children", .role = Nodes}}},
        {.kind = "collapsible",
         .keys = {{.key = "title", .role = Expr},
                  {.key = "open", .role = Expr},
                  {.key = "onToggle", .role = Commands},
                  {.key = "header", .role = Nodes},
                  {.key = "children", .role = Nodes}}},
        {.kind = "dropZone",
         .keys = {{.key = "accept", .role = Expr},
                  {.key = "multiple", .role = Expr},
                  {.key = "onDrop", .role = Commands}}},
        {.kind = "customize", .keys = {{.key = "point", .role = Text}}},
        {.kind = "boundary",
         .keys = {{.key = "loading", .role = Nodes},
                  {.key = "error", .role = Nodes},
                  {.key = "children", .role = Nodes}}},
        {.kind = "form",
         .keys = {{.key = "model", .role = Text},
                  {.key = "action", .role = Text},
                  {.key = "prefill", .role = Expr},
                  {.key = "submit", .role = Text},
                  {.key = "showErrors", .role = Text},
                  {.key = "overrides", .role = Raw},
                  {.key = "fields", .role = Raw},
                  {.key = "onSuccess", .role = Commands},
                  {.key = "onError", .role = Commands}}},
        {.kind = "collection",
         .keys = {{.key = "model", .role = Text}, {.key = "action", .role = Text}, {.key = "overrides", .role = Raw}}},
        {.kind = "wizard",
         .keys = {{.key = "model", .role = Text},
                  {.key = "action", .role = Text},
                  {.key = "overrides", .role = Raw},
                  {.key = "onSuccess", .role = Commands}}},
        {.kind = "custom",
         .keys = {{.key = "name", .role = Text}, {.key = "props", .role = ExprMap}, {.key = "on", .role = EventMap}}},
        {.kind = "menuItem",
         .keys = {{.key = "label", .role = Expr},
                  {.key = "icon", .role = Expr},
                  {.key = "keys", .role = Expr},
                  {.key = "checked", .role = Expr},
                  {.key = "enabled", .role = Expr},
                  {.key = "onClick", .role = Commands},
                  {.key = "items", .role = Entries, .shape = "menuItem"}},
         .entry = true},
        {.kind = "case",
         .keys = {{.key = "when", .role = Expr}, {.key = "children", .role = Nodes}, {.key = "scope", .role = Scope}},
         .entry = true},
        {.kind = "tab",
         .keys = {{.key = "id", .role = Text},
                  {.key = "label", .role = Expr},
                  {.key = "icon", .role = Expr},
                  {.key = "children", .role = Nodes},
                  {.key = "scope", .role = Scope}},
         .entry = true},
        {.kind = "column",
         .keys = {{.key = "id", .role = Text},
                  {.key = "label", .role = Expr},
                  {.key = "width", .role = Expr},
                  {.key = "sizing", .role = Expr},
                  {.key = "cell", .role = Nodes}},
         .entry = true},
        {.kind = "action",
         .keys = {{.key = "label", .role = Expr},
                  {.key = "enabled", .role = Expr},
                  {.key = "onClick", .role = Commands}},
         .entry = true},
        {.kind = "drop",
         .keys = {{.key = "accepts", .role = Expr}, {.key = "onDrop", .role = Commands}},
         .entry = true},
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

/// @brief Whether @p name is a name as spec 5 §3 allows: `[A-Za-z_][A-Za-z0-9_]*`.
/// @param name The candidate.
/// @return Whether it is one.
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
        if (const auto* const found = object.find(key); found != object.end()) {
            return &found->second;
        }
        if (const auto* const found = object.find("!" + std::string{key}); found != object.end()) {
            return &found->second;
        }
        return nullptr;
    }

    // The JSON pointer of member @p key, as written: with its must-understand mark when it carries one.
    [[nodiscard]] static std::string memberPath(Json const& json, std::string const& path, std::string_view key) {
        bool const marked =
            json.is_object() && !json.get_object().contains(key) && json.get_object().contains("!" + std::string{key});
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
                        refuse(ownPath, R"(`signIn` takes {"from": "mutation"})");
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
    void declarations(Json const& json, std::string const& path, std::string_view what, Each const& each) {
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
            refuse(path, R"(a patch is {"query": "q", "key": e, "with": e})");
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
            declarations(*models, memberPath(json, path, "models"), "`models`",
                         [&](std::string const& name, Json const& entry, std::string const& place) {
                             if (!members(entry, place, "a model", {"type", "instance"})) {
                                 return;
                             }
                             Json const* const type = member(entry, "type");
                             if (type == nullptr) {
                                 refuse(place, "a model names its `type`");
                                 return;
                             }
                             std::optional<std::string> typeName =
                                 text(*type, memberPath(entry, place, "type"), "`type`");
                             if (!typeName) {
                                 return;
                             }
                             ModelDecl decl{
                                 .name = name, .path = place, .type = std::move(*typeName), .instance = {}, .key = {}};
                             if (Json const* const instance = member(entry, "instance")) {
                                 if (instance->is_string() && instance->get_string() == "private") {
                                     decl.instance = ModelDecl::Instance::Private;
                                 } else if (instance->is_string() && instance->get_string() == "shared") {
                                     decl.instance = ModelDecl::Instance::Shared;
                                 } else {
                                     decl.instance = ModelDecl::Instance::Keyed;
                                     decl.key = expression(*instance, memberPath(entry, place, "instance"));
                                 }
                             }
                             scope.models.push_back(std::move(decl));
                         });
        }
        if (Json const* const state = member(json, "state")) {
            declarations(*state, memberPath(json, path, "state"), "`state`",
                         [&](std::string const& name, Json const& entry, std::string const& place) {
                             if (!members(entry, place, "a state entry", {"init", "type"})) {
                                 return;
                             }
                             StateDecl decl{.name = name, .path = place, .type = {}, .init = Expr{.path = place}};
                             if (Json const* const type = member(entry, "type")) {
                                 std::optional<std::string> written =
                                     text(*type, memberPath(entry, place, "type"), "`type`");
                                 if (!written) {
                                     return;
                                 }
                                 std::expected<Type, std::string> parsed = parseType(*written);
                                 if (!parsed) {
                                     refuse(memberPath(entry, place, "type"), parsed.error());
                                     return;
                                 }
                                 decl.type = *parsed;
                             }
                             if (Json const* const init = member(entry, "init")) {
                                 std::optional<Expr> value = expression(*init, memberPath(entry, place, "init"));
                                 if (!value) {
                                     return;
                                 }
                                 decl.init = std::move(*value);
                             }
                             scope.state.push_back(std::move(decl));
                         });
        }
        if (Json const* const lets = member(json, "let")) {
            declarations(
                *lets, memberPath(json, path, "let"), "`let`",
                [&](std::string const& name, Json const& entry, std::string const& place) {
                    if (std::optional<Expr> value = expression(entry, place)) {
                        scope.lets.push_back(LetDecl{.name = name, .path = place, .value = std::move(*value)});
                    }
                });
        }
        if (Json const* const queries = member(json, "queries")) {
            declarations(*queries, memberPath(json, path, "queries"), "`queries`",
                         [&](std::string const& name, Json const& entry, std::string const& place) {
                             if (std::optional<QueryDecl> decl = query(name, entry, place)) {
                                 scope.queries.push_back(std::move(*decl));
                             }
                         });
        }
        if (Json const* const mutations = member(json, "mutations")) {
            declarations(*mutations, memberPath(json, path, "mutations"), "`mutations`",
                         [&](std::string const& name, Json const& entry, std::string const& place) {
                             if (std::optional<MutationDecl> decl = mutation(name, entry, place)) {
                                 scope.mutations.push_back(std::move(*decl));
                             }
                         });
        }
        if (Json const* const watches = member(json, "watch")) {
            std::string const place = memberPath(json, path, "watch");
            if (!watches->is_array()) {
                refuse(place, "`watch` is a list of watches");
                return;
            }
            std::size_t index = 0;
            for (Json const& entry : watches->get_array()) {
                std::string const entryPath = place + "/" + std::to_string(index++);
                if (!members(entry, entryPath, "a watch", {"on", "source", "do"})) {
                    return;
                }
                Json const* const onJson = member(entry, "on");
                Json const* const run = member(entry, "do");
                if (onJson == nullptr || run == nullptr) {
                    refuse(entryPath, R"(a watch is {"on": e, "do": [...]})");
                    return;
                }
                std::optional<Expr> observed = expression(*onJson, memberPath(entry, entryPath, "on"));
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
            std::string const place = memberPath(json, path, "checks");
            if (!checks->is_array()) {
                refuse(place, "`checks` is a list of checks");
                return;
            }
            std::size_t index = 0;
            for (Json const& entry : checks->get_array()) {
                std::string const entryPath = place + "/" + std::to_string(index++);
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
                if (Json const* const checkId = member(entry, "id")) {
                    decl.id = text(*checkId, memberPath(entry, entryPath, "id"), "`id`").value_or(std::string{});
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

    std::optional<QueryDecl> query(std::string const& name, Json const& entry, std::string const& place) {
        if (!members(entry, place, "a query",
                     {"model", "action", "evaluate", "body", "when", "debounce", "refreshOn", "refreshEvery",
                      "staleTime"})) {
            return std::nullopt;
        }
        QueryDecl decl{.name = name,
                       .path = place,
                       .model = {},
                       .action = {},
                       .evaluate = false,
                       .body = Expr{.path = place},
                       .when = {},
                       .debounce = 0,
                       .refreshOn = {},
                       .refreshEvery = 0,
                       .staleTime = 0};
        Json const* const model = member(entry, "model");
        Json const* const action = member(entry, "action");
        Json const* const evaluate = member(entry, "evaluate");
        if (model == nullptr || (action == nullptr) == (evaluate == nullptr)) {
            refuse(place, "a query names its `model` and one of `action` or `evaluate`");
            return std::nullopt;
        }
        decl.model = text(*model, memberPath(entry, place, "model"), "`model`").value_or(std::string{});
        decl.evaluate = evaluate != nullptr;
        decl.action = text(decl.evaluate ? *evaluate : *action,
                           memberPath(entry, place, decl.evaluate ? "evaluate" : "action"), "the action id")
                          .value_or(std::string{});
        if (Json const* const body = member(entry, "body")) {
            std::optional<Expr> parsed = expression(*body, memberPath(entry, place, "body"));
            if (!parsed) {
                return std::nullopt;
            }
            decl.body = std::move(*parsed);
        } else {
            // No body is an empty request, not an idle query.
            decl.body.form = Expr::Form::Object;
        }
        if (Json const* const when = member(entry, "when")) {
            decl.when = expression(*when, memberPath(entry, place, "when"));
        }
        for (auto [key, field] :
             {std::pair{"debounce", &QueryDecl::debounce}, std::pair{"refreshEvery", &QueryDecl::refreshEvery},
              std::pair{"staleTime", &QueryDecl::staleTime}}) {
            if (Json const* const value = member(entry, key)) {
                decl.*field = integer(*value, memberPath(entry, place, key), "`" + std::string{key} + "`").value_or(0);
            }
        }
        if (Json const* const refreshOn = member(entry, "refreshOn")) {
            decl.refreshOn = names(*refreshOn, memberPath(entry, place, "refreshOn"), "`refreshOn`");
        }
        if (!ok()) {
            return std::nullopt;
        }
        return decl;
    }

    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one block per key of a mutation.
    std::optional<MutationDecl> mutation(std::string const& name, Json const& entry, std::string const& place) {
        if (!members(entry, place, "a mutation",
                     {"model", "action", "body", "concurrency", "serialKey", "invalidates", "patch", "optimistic",
                      "onSuccess", "onError", "offline"})) {
            return std::nullopt;
        }
        MutationDecl decl{.name = name,
                          .path = place,
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
            refuse(place, "a mutation names its `model` and `action`");
            return std::nullopt;
        }
        decl.model = text(*model, memberPath(entry, place, "model"), "`model`").value_or(std::string{});
        decl.action = text(*action, memberPath(entry, place, "action"), "`action`").value_or(std::string{});
        if (Json const* const body = member(entry, "body")) {
            decl.body = expression(*body, memberPath(entry, place, "body"));
        }
        if (Json const* const concurrency = member(entry, "concurrency")) {
            static constexpr std::array<std::pair<std::string_view, MutationDecl::Concurrency>, 4> kPolicies{{
                {"exclusive", MutationDecl::Concurrency::Exclusive},
                {"serial", MutationDecl::Concurrency::Serial},
                {"latest", MutationDecl::Concurrency::Latest},
                {"parallel", MutationDecl::Concurrency::Parallel},
            }};
            // NOLINTNEXTLINE(readability-qualified-auto): an iterator, a pointer only in some standard libraries.
            auto const found = concurrency->is_string()
                                   ? std::ranges::find(kPolicies, concurrency->get_string(),
                                                       &std::pair<std::string_view, MutationDecl::Concurrency>::first)
                                   : kPolicies.end();
            if (found == kPolicies.end()) {
                refuse(memberPath(entry, place, "concurrency"),
                       "`concurrency` is `exclusive`, `serial`, `latest` or `parallel`");
                return std::nullopt;
            }
            decl.concurrency = found->second;
        }
        if (Json const* const serialKey = member(entry, "serialKey")) {
            decl.serialKey = expression(*serialKey, memberPath(entry, place, "serialKey"));
        }
        if (Json const* const invalidates = member(entry, "invalidates")) {
            decl.invalidates = names(*invalidates, memberPath(entry, place, "invalidates"), "`invalidates`");
        }
        if (Json const* const patchJson = member(entry, "patch")) {
            decl.patch = patch(*patchJson, memberPath(entry, place, "patch"));
        }
        if (Json const* const optimistic = member(entry, "optimistic")) {
            decl.optimistic = patch(*optimistic, memberPath(entry, place, "optimistic"));
        }
        if (Json const* const onSuccess = member(entry, "onSuccess")) {
            decl.onSuccess = commands(*onSuccess, memberPath(entry, place, "onSuccess"));
        }
        if (Json const* const onError = member(entry, "onError")) {
            decl.onError = commands(*onError, memberPath(entry, place, "onError"));
        }
        if (Json const* const offline = member(entry, "offline")) {
            if (!offline->is_string() || offline->get_string() != "queue") {
                refuse(memberPath(entry, place, "offline"), "`offline` is `queue`");
                return std::nullopt;
            }
            decl.offlineQueue = true;
        }
        if (!ok()) {
            return std::nullopt;
        }
        return decl;
    }

    std::vector<DocNode> nodes(Json const& json, std::string const& path, std::string_view shape = {}) {
        std::vector<DocNode> out;
        if (json.is_object()) {
            if (std::optional<DocNode> one = node(json, path, shape)) {
                out.push_back(std::move(*one));
            }
            return out;
        }
        if (!json.is_array()) {
            refuse(path, shape.empty() ? "a node list is a list of nodes"
                                       : "`" + std::string{shape} + "` entries are a list");
            return out;
        }
        std::size_t index = 0;
        for (Json const& entry : json.get_array()) {
            std::optional<DocNode> one = node(entry, path + "/" + std::to_string(index++), shape);
            if (!one) {
                return out;
            }
            out.push_back(std::move(*one));
        }
        return out;
    }

    // A node, or with @p shape an entry of that shape.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one branch per key role.
    std::optional<DocNode> node(Json const& json, std::string const& path, std::string_view shape = {}) {
        if (!json.is_object()) {
            refuse(path,
                   shape.empty() ? "a node is an object" : "each `" + std::string{shape} + "` entry is an object");
            return std::nullopt;
        }
        DocNode out;
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
            // NOLINTNEXTLINE(readability-qualified-auto): an iterator, a pointer only in some standard libraries.
            auto const common = std::ranges::find(kCommonKeys, key, &KeySpec::key);
            return common == kCommonKeys.end() ? nullptr : &*common;
        };
        for (auto const& [key, member] : json.get_object()) {
            std::string const place = path + "/" + pointerToken(key);
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
                    if (std::optional<Expr> value = expression(member, place)) {
                        out.props.push_back(NamedExpr{.name = name, .expr = std::move(*value)});
                    }
                    break;
                case KeyRole::Text:
                    if (name == "kind") {
                        break;
                    }
                    if (std::optional<std::string> value = text(member, place, "`" + name + "`")) {
                        out.texts.push_back(NamedText{.name = name, .text = std::move(*value)});
                    }
                    break;
                case KeyRole::Commands:
                    out.events.push_back(NamedCommands{.name = name, .commands = commands(member, place)});
                    break;
                case KeyRole::Nodes:
                    out.slots.push_back(NodeSlot{.name = name, .nodes = nodes(member, place)});
                    break;
                case KeyRole::Entries:
                    out.slots.push_back(NodeSlot{.name = name, .nodes = nodes(member, place, keySpec->shape)});
                    break;
                case KeyRole::Entry:
                    if (std::optional<DocNode> entry = node(member, place, keySpec->shape)) {
                        out.slots.push_back(NodeSlot{.name = name, .nodes = {std::move(*entry)}});
                    }
                    break;
                case KeyRole::ExprMap: {
                    if (!member.is_object()) {
                        refuse(place, "`" + name + "` is an object of expressions");
                        break;
                    }
                    std::vector<NamedExpr> entries;
                    for (auto const& [field, value] : member.get_object()) {
                        if (std::optional<Expr> parsed = expression(value, place + "/" + pointerToken(field))) {
                            entries.push_back(NamedExpr{.name = field, .expr = std::move(*parsed)});
                        }
                    }
                    out.maps.emplace_back(name, std::move(entries));
                    break;
                }
                case KeyRole::EventMap:
                    if (!member.is_object()) {
                        refuse(place, "`on` is an object of command lists");
                        break;
                    }
                    for (auto const& [event, list] : member.get_object()) {
                        out.events.push_back(NamedCommands{
                            .name = "on." + event, .commands = commands(list, place + "/" + pointerToken(event))});
                    }
                    break;
                case KeyRole::Scope: {
                    if (!members(member, place, "a scope",
                                 {"models", "state", "let", "queries", "mutations", "watch", "checks"})) {
                        break;
                    }
                    auto scope = std::make_shared<Scope>();
                    scopeKeys(member, place, *scope);
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
                         [&](std::string const& name, Json const& entry, std::string const& place) {
                             std::optional<std::string> written = text(entry, place, "a param's type");
                             if (!written) {
                                 return;
                             }
                             std::expected<Type, std::string> type = parseType(*written);
                             if (!type) {
                                 refuse(place, type.error());
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
                         [&](std::string const& name, Json const& entry, std::string const& place) {
                             component(name, entry, place, document);
                         });
        }
        if (Json const* const customize = member(json, "customize")) {
            declarations(*customize, memberPath(json, "", "customize"), "`customize`",
                         [&](std::string const& name, Json const& entry, std::string const& place) {
                             point(name, entry, place, document);
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

    void component(std::string const& name, Json const& entry, std::string const& place, Document& document) {
        if (!members(entry, place, "a component", {"qml", "host", "version", "props", "events", "fallback"})) {
            return;
        }
        ComponentDecl decl{
            .name = name, .path = place, .qml = {}, .host = {}, .props = {}, .events = {}, .fallback = {}};
        Json const* const qml = member(entry, "qml");
        Json const* const host = member(entry, "host");
        if ((qml == nullptr) == (host == nullptr)) {
            refuse(place, "a component names one of `qml` or `host`");
            return;
        }
        if (qml != nullptr) {
            decl.qml = text(*qml, memberPath(entry, place, "qml"), "`qml`").value_or(std::string{});
        } else {
            decl.host = text(*host, memberPath(entry, place, "host"), "`host`").value_or(std::string{});
        }
        for (auto [key, into] : {std::pair{"props", &decl.props}, std::pair{"events", &decl.events}}) {
            Json const* const map = member(entry, key);
            if (map == nullptr) {
                continue;
            }
            std::string const mapPath = memberPath(entry, place, key);
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
            decl.fallback = nodes(*fallback, memberPath(entry, place, "fallback"));
        }
        document.components.push_back(std::move(decl));
    }

    void point(std::string const& name, Json const& entry, std::string const& place, Document& document) {
        if (!members(entry, place, "a customization point", {"kind", "of", "options", "type", "default"})) {
            return;
        }
        CustomizePoint decl{
            .name = name, .path = place, .kind = {}, .of = {}, .options = {}, .type = {}, .defaultValue = {}};
        Json const* const kind = member(entry, "kind");
        static constexpr std::array<std::string_view, 7> kKinds{"columns", "choice", "value",  "layout",
                                                                "saved",   "pins",   "arrange"};
        if (kind == nullptr || !kind->is_string() || std::ranges::find(kKinds, kind->get_string()) == kKinds.end()) {
            refuse(memberPath(entry, place, "kind"),
                   "a point's `kind` is `columns`, `choice`, `value`, `layout`, `saved`, `pins` or `arrange`");
            return;
        }
        decl.kind = kind->get_string();
        if (Json const* const target = member(entry, "of")) {
            decl.of = text(*target, memberPath(entry, place, "of"), "`of`").value_or(std::string{});
        }
        if (Json const* const options = member(entry, "options")) {
            decl.options = names(*options, memberPath(entry, place, "options"), "`options`");
        }
        if (Json const* const type = member(entry, "type")) {
            std::expected<Type, std::string> parsed =
                parseType(text(*type, memberPath(entry, place, "type"), "`type`").value_or(std::string{}));
            if (!parsed) {
                refuse(memberPath(entry, place, "type"), parsed.error());
                return;
            }
            decl.type = *parsed;
        }
        if (Json const* const fallback = member(entry, "default")) {
            std::expected<Value, std::string> value =
                decl.type ? decode(*fallback, *decl.type) : decodeUntyped(*fallback);
            if (!value) {
                refuse(memberPath(entry, place, "default"), value.error());
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

/// @brief Why a document is refused at load: the document, the place inside it, and the reason.
struct Refusal {
    /// @brief The screen id of the refused document.
    std::string document;
    /// @brief The JSON pointer of the offending place.
    std::string path;
    /// @brief Why, in a sentence the document's author can act on.
    std::string reason;

    /// @brief Same document, path and reason.
    /// @return Whether the two are equal.
    bool operator==(Refusal const&) const = default;
};

/// @brief What a document is validated against: the model types and actions the server serves, and the bundle's
///        components.
struct Catalog {
    /// @brief A model type.
    struct Model {
        /// @brief The model type id.
        std::string type;
        /// @brief Whether its shared instances are keyed, so an alias may select one by an expression.
        bool keyed = false;
        /// @brief Whether it holds no state, so an alias that names no instance shares its singleton.
        bool stateless = false;
    };

    /// @brief An action.
    struct Action {
        /// @brief The action id.
        std::string id;
        /// @brief The model type that serves it.
        std::string model;
        /// @brief The request's type.
        Type request;
        /// @brief The reply's type.
        Type reply;
        /// @brief The fields of the reply's rows the server calculates; a client never writes them optimistically.
        std::vector<std::string> calculated;
        /// @brief Whether a run sends a credential or returns a session token, so it is never queued offline.
        bool credential = false;
    };

    /// @brief The model types.
    std::vector<Model> models;
    /// @brief The actions.
    std::vector<Action> actions;
    /// @brief The bundle's component names a `custom` node may use besides the document's own.
    std::vector<std::string> components;

    /// @brief A model type, by id.
    /// @param type The model type id.
    /// @return The model, or null when the catalog has none.
    [[nodiscard]] Model const* model(std::string_view type) const noexcept {
        auto const found = std::ranges::find(models, type, &Model::type);
        return found == models.end() ? nullptr : &*found;
    }

    /// @brief An action, by id.
    /// @param actionId The action id.
    /// @return The action, or null when the catalog has none.
    [[nodiscard]] Action const* action(std::string_view actionId) const noexcept {
        auto const found = std::ranges::find(actions, actionId, &Action::id);
        return found == actions.end() ? nullptr : &*found;
    }

    /// @brief Adds an action from the schema the server serves for it: the request's type from the schema, the
    ///        reply's from its `x-reply`, and the calculated fields from the reply rows' `x-computed` and
    ///        `x-readonly` members.
    /// @param actionId The action id.
    /// @param model The model type that serves it.
    /// @param schema The served schema.
    /// @param credential Whether a run sends a credential or returns a session token.
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): glaze's DOM indexes an object by
    // key, and each member read below is checked first.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): an action id and a model type id differ in form.
    void addAction(std::string actionId, std::string model, Json const& schema, bool credential = false) {
        Action action{.id = std::move(actionId),
                      .model = std::move(model),
                      .request = typeFromSchema(schema, schema),
                      .reply = {},
                      .calculated = {},
                      .credential = credential};
        if (schema.is_object() && schema.contains("x-reply")) {
            Json const& reply = schema["x-reply"];
            action.reply = typeFromSchema(reply, reply);
            Json const* row = &reply;
            if (row->is_object() && row->contains("items")) {
                row = &(*row)["items"];
            }
            if (row->is_object() && row->contains("$ref") && (*row)["$ref"].is_string() && reply.contains("$defs")) {
                std::string const name = (*row)["$ref"].get_string().substr(std::string_view{"#/$defs/"}.size());
                if (reply["$defs"].contains(name)) {
                    row = &reply["$defs"][name];
                }
            }
            if (row->is_object() && row->contains("properties") && (*row)["properties"].is_object()) {
                for (auto const& [field, member] : (*row)["properties"].get_object()) {
                    if (member.is_object() && (member.contains("x-computed") || member.contains("x-readonly"))) {
                        action.calculated.push_back(field);
                    }
                }
            }
        }
        actions.push_back(std::move(action));
    }
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
};

namespace detail {

/// @brief The type a value has on its face.
/// @param value The value.
/// @return Its type; a null is an optional `any`.
// NOLINTNEXTLINE(misc-no-recursion): a value nests as deep as its literal does.
[[nodiscard]] inline Type typeOfValue(Value const& value) {
    switch (value.kind()) {
        case Value::Kind::Null:
            return Type{}.withOptional(true);
        case Value::Kind::Bool:
            return Type{Type::Kind::Bool};
        case Value::Kind::Int:
            return Type{Type::Kind::Int};
        case Value::Kind::String:
            return Type{Type::Kind::String};
        case Value::Kind::Decimal:
            return Type{Type::Kind::Decimal};
        case Value::Kind::Quantity:
            return Type{Type::Kind::Quantity};
        case Value::Kind::Timestamp:
            return Type{Type::Kind::Timestamp};
        case Value::Kind::List:
            return Type::list(value.asList()->empty() ? Type{} : typeOfValue(value.asList()->front()));
        case Value::Kind::Record:
        default: {
            std::vector<std::pair<std::string, Type>> fields;
            for (auto const& [name, field] : *value.asRecord()) {
                fields.emplace_back(name, typeOfValue(field));
            }
            return Type::record(std::move(fields));
        }
    }
}

/// @brief The value an optional holds, as a pointer.
/// @tparam T The value type.
/// @param value The optional.
/// @return The value, or null when the optional is empty.
template <typename T>
[[nodiscard]] T const* engaged(std::optional<T> const& value) {
    return value.has_value() ? &*value : nullptr;
}

/// @brief Whether a type is known and of a kind other than @p kinds.
/// @param type The type.
/// @param kinds The kinds it may be.
/// @return Whether it is known to be none of them.
[[nodiscard]] inline bool knownOtherThan(Type const& type, std::initializer_list<Type::Kind> kinds) {
    return type.kind() != Type::Kind::Any && std::ranges::find(kinds, type.kind()) == kinds.end();
}

/// @brief A type as a refusal names it: "a string", "an int", "a record {id: int}".
/// @param type The type.
/// @return The words.
[[nodiscard]] inline std::string described(Type const& type) {
    std::string const text = type.withOptional(false).text();
    return type.kind() == Type::Kind::Record ? "a record " + text : withArticle(text);
}

// The validator's view of one scope: its declarations, the element ids declared in it, and what it binds.
struct Frame {
    Scope const* scope = nullptr;
    std::vector<std::pair<std::string, Type>> names;
    std::vector<std::pair<std::string, std::string>> elements;  // id, kind
    bool row = false;
    Type rowType;
};

// NOLINTBEGIN(misc-no-recursion): a document nests as deep as its JSON does.
class Validator {
public:
    Validator(Document const& document, Catalog const& catalog) : _document{&document}, _catalog{&catalog} {}

    std::vector<Refusal> run() {
        if (_document->vocab.empty()) {
            refuse("", "a screen document names its `vocab`");
        } else if (_document->vocab != kDocumentVocabulary) {
            refuse("/vocab", "the document is written in `" + _document->vocab + "`; this client speaks ui/1");
        }
        if (_document->id.empty()) {
            refuse("", "a screen document names its `id`");
        }
        Frame root{.scope = &_document->scope, .names = {}, .elements = {}, .row = false, .rowType = {}};
        for (auto const& [name, type] : _document->params) {
            root.names.emplace_back(name, type);
        }
        if (_document->view) {
            collectElements(*_document->view, root);
        }
        _frames.push_back(std::move(root));
        scope(_document->scope, false);
        for (auto [expr, flag] : {std::pair{&_document->title, false}, std::pair{&_document->dirty, true},
                                  std::pair{&_document->identity, false}}) {
            if (expr->has_value()) {
                Type const type = check(**expr);
                if (flag) {
                    expectBool(**expr, type, "`dirty`");
                }
            }
        }
        for (NamedCommands const& lifecycle : _document->lifecycle) {
            _inOnClose = lifecycle.name == "onClose";
            commands(lifecycle.commands);
            _inOnClose = false;
        }
        for (std::string const& name : _document->prefetch) {
            if (std::ranges::find(_document->scope.queries, name, &QueryDecl::name) ==
                _document->scope.queries.end()) {
                refuse("/prefetch", "`" + name + "` is not a query of the screen");
            }
        }
        points();
        for (ComponentDecl const& component : _document->components) {
            Frame props{.scope = nullptr, .names = {{"props", Type{}}}, .elements = {}, .row = false, .rowType = {}};
            for (DocNode const& node : component.fallback) {
                collectElements(node, props);
            }
            _frames.push_back(std::move(props));
            for (DocNode const& node : component.fallback) {
                visit(node);
            }
            _frames.pop_back();
        }
        if (_document->view) {
            visit(*_document->view);
        }
        cycles();
        return std::move(_out);
    }

private:
    Document const* _document;
    Catalog const* _catalog;
    std::vector<Refusal> _out;
    std::vector<Frame> _frames;
    std::vector<std::pair<std::string, Type>> _locals;  // comprehension variables and command bindings, innermost last
    bool _quiet = false;
    bool _inCommands = false;
    bool _inOnClose = false;
    bool _inBoundaryError = false;
    LetDecl const* _currentLet = nullptr;
    WatchDecl const* _currentWatch = nullptr;
    std::vector<LetDecl const*> _inferring;
    std::vector<std::pair<LetDecl const*, LetDecl const*>> _letEdges;
    std::vector<WatchDecl const*> _watches;
    std::vector<std::pair<WatchDecl const*, StateDecl const*>> _observes;
    std::vector<std::pair<WatchDecl const*, StateDecl const*>> _writes;

    void refuse(std::string path, std::string reason) {
        if (_quiet) {
            return;
        }
        Refusal refusal{.document = _document->id, .path = std::move(path), .reason = std::move(reason)};
        if (std::ranges::find(_out, refusal) == _out.end()) {
            _out.push_back(std::move(refusal));
        }
    }

    void expectBool(Expr const& expr, Type const& type, std::string const& what) {
        if (knownOtherThan(type, {Type::Kind::Bool})) {
            refuse(expr.path, what + " is a bool, not " + described(type));
        }
    }

    // ---- Resolution, innermost scope first.

    struct Resolved {
        enum class Kind : std::uint8_t { None, Name, State, Let, Element };
        Kind kind = Kind::None;
        Type type;
        StateDecl const* state = nullptr;
        LetDecl const* let = nullptr;
    };

    Resolved resolve(std::string_view head) {
        for (auto& local : std::views::reverse(_locals)) {
            if (local.first == head) {
                return Resolved{.kind = Resolved::Kind::Name, .type = local.second, .state = nullptr, .let = nullptr};
            }
        }
        for (auto& frame : std::views::reverse(_frames)) {
            if (auto const found = std::ranges::find(frame.names, head, &std::pair<std::string, Type>::first);
                found != frame.names.end()) {
                return Resolved{.kind = Resolved::Kind::Name, .type = found->second, .state = nullptr, .let = nullptr};
            }
            if (frame.scope != nullptr) {
                auto const& state = frame.scope->state;
                if (auto const found = std::ranges::find(state, head, &StateDecl::name); found != state.end()) {
                    return Resolved{
                        .kind = Resolved::Kind::State, .type = stateType(*found), .state = &*found, .let = nullptr};
                }
                auto const& lets = frame.scope->lets;
                if (auto const found = std::ranges::find(lets, head, &LetDecl::name); found != lets.end()) {
                    return Resolved{
                        .kind = Resolved::Kind::Let, .type = letType(*found), .state = nullptr, .let = &*found};
                }
            }
            if (std::ranges::find(frame.elements, head, &std::pair<std::string, std::string>::first) !=
                frame.elements.end()) {
                return Resolved{.kind = Resolved::Kind::Element, .type = {}, .state = nullptr, .let = nullptr};
            }
        }
        return {};
    }

    template <typename Decl>
    [[nodiscard]] [[nodiscard]] [[nodiscard]] Decl const* findDecl(std::vector<Decl> Scope::* member,
                                                                   std::string_view name) const {
        for (const auto& frame : std::views::reverse(_frames)) {
            if (frame.scope == nullptr) {
                continue;
            }
            auto const& decls = frame.scope->*member;
            if (auto const found = std::ranges::find(decls, name, &Decl::name); found != decls.end()) {
                return &*found;
            }
        }
        return nullptr;
    }

    [[nodiscard]] std::string const* elementKind(std::string_view elementId) const {
        for (const auto& frame : std::views::reverse(_frames)) {
            auto const found =
                std::ranges::find(frame.elements, elementId, &std::pair<std::string, std::string>::first);
            if (found != frame.elements.end()) {
                return &found->second;
            }
        }
        return nullptr;
    }

    Type stateType(StateDecl const& state) {
        if (state.type) {
            return *state.type;
        }
        return infer(state.init);
    }

    Type letType(LetDecl const& let) {
        if (std::ranges::find(_inferring, &let) != _inferring.end()) {
            return {};
        }
        return infer(let.value);
    }

    Type infer(Expr const& expr) {
        bool const wasQuiet = std::exchange(_quiet, true);
        Type const type = check(expr);
        _quiet = wasQuiet;
        return type;
    }

    [[nodiscard]] Type actionReply(std::string_view action, bool request) const {
        Catalog::Action const* const found = _catalog->action(action);
        if (found == nullptr) {
            return {};
        }
        return request ? found->request : found->reply;
    }

    // A dotted path into @p type, refusing a field a known record lacks.
    Type field(Type type, std::string_view path, Expr const& expr, std::string_view head) {
        std::string walked{head};
        while (!path.empty()) {
            auto const [segment, rest] = splitHead(path);
            if (type.kind() == Type::Kind::Record) {
                std::optional<Type> const next = type.field(segment);
                if (!next) {
                    refuse(expr.path, "`" + walked + "` has no field `" + std::string{segment} + "`");
                    return {};
                }
                type = next->withOptional(next->optional() || type.optional());
            } else {
                type = {};
            }
            walked += "." + std::string{segment};
            path = rest;
        }
        return type;
    }

    // ---- Expressions.

    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one branch per form.
    Type check(Expr const& expr) {
        switch (expr.form) {
            case Expr::Form::Literal:
                return typeOfValue(expr.literal);
            case Expr::Form::Ref: {
                auto const [head, rest] = splitHead(expr.name);
                if (head == "app") {
                    return {};
                }
                Resolved const found = resolve(head);
                if (found.kind == Resolved::Kind::None) {
                    refuse(expr.path, "`" + std::string{head} + "` is not declared in scope");
                    return {};
                }
                if (found.let != nullptr && _currentLet != nullptr && !_quiet) {
                    _letEdges.emplace_back(_currentLet, found.let);
                }
                if (found.state != nullptr && _currentWatch != nullptr && !_quiet) {
                    _observes.emplace_back(_currentWatch, found.state);
                }
                return field(found.type, rest, expr, head);
            }
            case Expr::Form::Event:
                if (!_inCommands) {
                    refuse(expr.path, R"({"ref": "event"} is valid only in a command list)");
                }
                return {};
            case Expr::Form::Row: {
                auto const frame = std::ranges::find_if(_frames.rbegin(), _frames.rend(), &Frame::row);
                if (frame == _frames.rend()) {
                    refuse(expr.path, "`row` is valid only inside a row of a `forEach` or a `table`");
                    return {};
                }
                return field(frame->rowType, expr.name, expr, "row");
            }
            case Expr::Form::Query: {
                QueryDecl const* const query = findDecl(&Scope::queries, expr.name);
                if (query == nullptr) {
                    refuse(expr.path, "`" + expr.name + "` is not a query in scope");
                    return {};
                }
                return actionReply(query->action, query->evaluate);
            }
            case Expr::Form::Result: {
                MutationDecl const* const mutation = findDecl(&Scope::mutations, expr.name);
                if (mutation == nullptr) {
                    refuse(expr.path, "`" + expr.name + "` is not a mutation in scope");
                    return {};
                }
                return actionReply(mutation->action, false).withOptional(true);
            }
            case Expr::Form::Pending:
            case Expr::Form::Error:
            case Expr::Form::ErrorKind:
            case Expr::Form::Can: {
                bool const host = expr.form == Expr::Form::Can && expr.name.starts_with("host:");
                if (!host && findDecl(&Scope::queries, expr.name) == nullptr &&
                    findDecl(&Scope::mutations, expr.name) == nullptr) {
                    refuse(expr.path, "`" + expr.name + "` is not a query or a mutation in scope");
                }
                bool const flag = expr.form == Expr::Form::Pending || expr.form == Expr::Form::Can;
                return flag ? Type{Type::Kind::Bool} : Type{Type::Kind::String, true};
            }
            case Expr::Form::Pref: {
                auto const [head, rest] = splitHead(expr.name);
                auto const point = std::ranges::find(_document->customize, head, &CustomizePoint::name);
                if (point == _document->customize.end()) {
                    refuse(expr.path, "`" + std::string{head} + "` is not a customization point of the screen");
                    return {};
                }
                if (point->kind == "choice") {
                    return Type{Type::Kind::String};
                }
                return field(point->type.value_or(Type{}), rest, expr, head);
            }
            case Expr::Form::Env:
            case Expr::Form::Session: {
                static constexpr std::array<std::string_view, 5> kEnv{"widthClass", "locale", "platform", "online",
                                                                      "backend"};
                static constexpr std::array<std::string_view, 6> kSession{"principal", "userName", "displayName",
                                                                          "userKey",   "tenant",   "authenticated"};
                bool const env = expr.form == Expr::Form::Env;
                bool const known = env ? std::ranges::find(kEnv, expr.name) != kEnv.end()
                                       : std::ranges::find(kSession, expr.name) != kSession.end();
                if (!known) {
                    refuse(expr.path,
                           "`" + expr.name + "` is not " + (env ? "an environment" : "a session") + " attribute");
                    return {};
                }
                bool const flag = expr.name == "online" || expr.name == "authenticated";
                return flag ? Type{Type::Kind::Bool} : Type{Type::Kind::String, true};
            }
            case Expr::Form::Host:
                return {};
            case Expr::Form::Call:
                return call(expr);
            case Expr::Form::Translate:
                for (Expr const& argument : expr.args) {
                    check(argument);
                }
                return Type{Type::Kind::String};
            case Expr::Form::If: {
                expectBool(expr.args.at(0), check(expr.args.at(0)), "`if`'s condition");
                Type const then = check(expr.args.at(1));
                Type const otherwise = check(expr.args.at(2));
                return then.kind() == otherwise.kind() ? then.withOptional(then.optional() || otherwise.optional())
                                                       : Type{};
            }
            case Expr::Form::Object: {
                std::vector<std::pair<std::string, Type>> fields;
                fields.reserve(expr.args.size());
                for (std::size_t index = 0; index < expr.args.size(); ++index) {
                    fields.emplace_back(expr.keys.at(index), check(expr.args.at(index)));
                }
                return Type::record(std::move(fields));
            }
            case Expr::Form::List: {
                Type element;
                for (std::size_t index = 0; index < expr.args.size(); ++index) {
                    Type const each = check(expr.args.at(index));
                    element = index == 0 || each.kind() == element.kind() ? each : Type{};
                }
                return Type::list(element);
            }
            case Expr::Form::Map:
            case Expr::Form::Filter:
            default: {
                Type source = check(expr.args.at(0));
                if (knownOtherThan(source, {Type::Kind::List})) {
                    refuse(expr.args.at(0).path, std::string{expr.form == Expr::Form::Map ? "`map`" : "`filter`"} +
                                                     " takes a list, not " + described(source));
                }
                _locals.emplace_back(expr.name, source.element());
                Type const body = check(expr.args.at(1));
                _locals.pop_back();
                if (expr.form == Expr::Form::Filter) {
                    expectBool(expr.args.at(1), body, "`where`");
                    return source;
                }
                return Type::list(body);
            }
        }
    }

    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one group per family of functions.
    Type call(Expr const& expr) {
        std::vector<Type> types;
        types.reserve(expr.args.size());
        for (Expr const& argument : expr.args) {
            types.push_back(check(argument));
        }
        std::string_view const name = expr.name;
        auto const each = [&](std::initializer_list<Type::Kind> kinds, std::string const& wants) {
            for (std::size_t index = 0; index < types.size(); ++index) {
                if (knownOtherThan(types.at(index), kinds)) {
                    refuse(expr.args.at(index).path,
                           "`" + expr.name + "` takes " + wants + ", not " + described(types.at(index)));
                }
            }
        };
        using K = Type::Kind;
        if (name == "and" || name == "or" || name == "not") {
            each({K::Bool}, "bools");
            return Type{K::Bool, true};
        }
        if (name == "add" || name == "sub" || name == "mul" || name == "div" || name == "neg") {
            each({K::Int, K::Decimal, K::Quantity}, "numbers or quantities");
            bool const quantity =
                std::ranges::any_of(types, [](Type const& type) { return type.kind() == K::Quantity; });
            bool const integral = std::ranges::all_of(types, [](Type const& type) { return type.kind() == K::Int; });
            if (quantity) {
                return Type{K::Quantity, true};
            }
            return Type{integral && name != "div" ? K::Int : K::Decimal, true};
        }
        if (name == "concat" || name == "trim" || name == "length" || name == "contains") {
            each({K::String}, name == "concat" || name == "contains" ? "strings" : "a string");
        }
        static constexpr std::array<std::string_view, 10> kOverLists{"count", "sum", "mean",  "min",      "max",
                                                                     "any",   "all", "first", "includes", "indexOf"};
        if (std::ranges::find(kOverLists, name) != kOverLists.end() && knownOtherThan(types.front(), {K::List})) {
            refuse(expr.args.front().path, "`" + expr.name + "` takes a list, not " + described(types.front()));
        }
        if (name == "eq" || name == "ne" || name == "lt" || name == "le" || name == "gt" || name == "ge" ||
            name == "engaged" || name == "isEmpty" || name == "contains" || name == "includes" || name == "any" ||
            name == "all") {
            return Type{K::Bool, name != "engaged"};
        }
        if (name == "count" || name == "length" || name == "indexOf") {
            return Type{K::Int, true};
        }
        if (name == "first" || name == "min" || name == "max" || name == "sum") {
            return types.front().element().withOptional(true);
        }
        if (name == "mean" || name == "round") {
            return Type{K::Decimal, true};
        }
        if (name == "coalesce") {
            return types.front().kind() == K::Any ? Type{} : types.front().withOptional(false);
        }
        return Type{K::String, true};
    }

    // ---- Commands.

    void commands(CommandList const& list) {
        bool const wasInCommands = std::exchange(_inCommands, true);
        std::size_t const bound = _locals.size();
        for (Command const& command : list) {
            this->command(command);
        }
        _locals.resize(bound);
        _inCommands = wasInCommands;
    }

    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one branch per verb.
    void command(Command const& command) {
        for (NamedExpr const& operand : command.exprs) {
            Type const type = check(operand.expr);
            if (command.verb == "if" && operand.name == "if") {
                expectBool(operand.expr, type, "`if`'s condition");
            }
        }
        std::string_view const verb = command.verb;
        std::string const& target = command.target;
        auto const head = std::string{splitHead(target).first};
        if (verb == "set" || verb == "reset") {
            Resolved const found = resolve(head);
            bool const writable =
                found.kind == Resolved::Kind::State || (verb == "set" && found.kind == Resolved::Kind::Element);
            if (!writable) {
                refuse(command.path,
                       "`" + head + "` is not " + (verb == "set" ? "state or an element" : "state") + " in scope");
            } else if (found.state != nullptr && _currentWatch != nullptr) {
                _writes.emplace_back(_currentWatch, found.state);
            }
        } else if (verb == "run" || verb == "signIn") {
            MutationDecl const* const mutation = findDecl(&Scope::mutations, target);
            if (mutation == nullptr) {
                refuse(command.path, "`" + target + "` is not a mutation in scope");
            } else if (std::string const* const binding = command.text("as")) {
                _locals.emplace_back(*binding, actionReply(mutation->action, false));
            }
        } else if (verb == "refetch") {
            if (findDecl(&Scope::queries, target) == nullptr) {
                refuse(command.path, "`" + target + "` is not a query in scope");
            }
        } else if (verb == "open" || verb == "close") {
            std::string const* const kind = elementKind(target);
            if (!(verb == "close" && target == "self") &&
                (kind == nullptr || (*kind != "dialog" && *kind != "drawer"))) {
                refuse(command.path, "`" + target + "` is not a dialog or a drawer in scope");
            }
        } else if (verb == "focus") {
            if (elementKind(target) == nullptr) {
                refuse(command.path, "`" + target + "` is not an element in scope");
            }
        } else if (verb == "resetPrefs") {
            if (target != "all" &&
                std::ranges::find(_document->customize, target, &CustomizePoint::name) == _document->customize.end()) {
                refuse(command.path, "`" + target + "` is not a customization point of the screen");
            }
        } else if (verb == "upload") {
            std::string const* const mutationName = command.text("to");
            if (mutationName == nullptr || findDecl(&Scope::mutations, *mutationName) == nullptr) {
                refuse(command.path, "`upload` sends to a mutation in scope");
            }
        } else if (verb == "cancelClose" && !_inOnClose) {
            refuse(command.path, "`cancelClose` is valid only in `onClose`");
        } else if (verb == "retry" && !_inBoundaryError) {
            refuse(command.path, "`retry` is valid only inside a boundary's `error`");
        }
        for (NamedCommands const& nested : command.lists) {
            commands(nested.commands);
        }
    }

    // ---- Scopes.

    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one block per declaration kind.
    void scope(Scope const& scope, bool rowScope) {
        std::vector<std::pair<std::string, std::string>> declared;
        auto const name = [&](std::string const& text, std::string const& path) {
            if (std::ranges::find(declared, text, &std::pair<std::string, std::string>::first) != declared.end()) {
                refuse(path, "`" + text + "` is declared twice in this scope");
            }
            declared.emplace_back(text, path);
        };
        for (ModelDecl const& model : scope.models) {
            name(model.name, model.path);
            Catalog::Model const* const type = _catalog->model(model.type);
            if (type == nullptr) {
                refuse(model.path + "/type", "`" + model.type + "` is not a model type of the catalog");
            } else if (model.instance == ModelDecl::Instance::Keyed && !type->keyed) {
                refuse(model.path + "/instance", "`" + model.type + "` is not keyed, so `instance` cannot select one");
            }
            if (rowScope && model.instance == ModelDecl::Instance::Default) {
                refuse(model.path, "a model alias in a row scope names its `instance`");
            }
            if (model.key) {
                check(*model.key);
            }
        }
        for (StateDecl const& state : scope.state) {
            name(state.name, state.path);
            Type const init = check(state.init);
            if (state.type && init.kind() != Type::Kind::Any && !fits(init, *state.type)) {
                refuse(state.init.path, "`init` is " + described(init) + ", not " + described(*state.type));
            }
        }
        for (LetDecl const& let : scope.lets) {
            name(let.name, let.path);
            _currentLet = &let;
            _inferring.push_back(&let);
            check(let.value);
            _inferring.pop_back();
            _currentLet = nullptr;
        }
        for (QueryDecl const& query : scope.queries) {
            name(query.name, query.path);
            Catalog::Action const* const action = modelAction(query.model, query.action, query.path);
            check(query.body);
            if (query.when) {
                expectBool(*query.when, check(*query.when), "`when`");
            }
            for (std::string const& refresh : query.refreshOn) {
                Catalog::Action const* const other = _catalog->action(refresh);
                if (other == nullptr || (action != nullptr && other->model != action->model)) {
                    refuse(query.path + "/refreshOn",
                           "`" + refresh + "` is not an action of " +
                               (action == nullptr ? std::string{"the query's model"} : "`" + action->model + "`"));
                }
            }
        }
        for (MutationDecl const& mutation : scope.mutations) {
            name(mutation.name, mutation.path);
            Catalog::Action const* const action = modelAction(mutation.model, mutation.action, mutation.path);
            for (std::optional<Expr> const* operand : {&mutation.body, &mutation.serialKey}) {
                if (operand->has_value()) {
                    check(**operand);
                }
            }
            for (std::string const& query : mutation.invalidates) {
                if (findDecl(&Scope::queries, query) == nullptr) {
                    refuse(mutation.path + "/invalidates", "`" + query + "` is not a query in scope");
                }
            }
            if (PatchDecl const* const decl = engaged(mutation.patch)) {
                patch(*decl, "result", action == nullptr ? Type{} : action->reply);
            }
            if (PatchDecl const* const decl = engaged(mutation.optimistic)) {
                patch(*decl, "body", action == nullptr ? Type{} : action->request);
                optimistic(*decl);
            }
            if (mutation.offlineQueue && action != nullptr && action->credential) {
                refuse(mutation.path + "/offline", "a mutation that carries a credential is never queued offline");
            }
            commands(mutation.onSuccess);
            commands(mutation.onError);
        }
        for (WatchDecl const& watch : scope.watches) {
            _watches.push_back(&watch);
            _currentWatch = &watch;
            check(watch.on);
            commands(watch.commands);
            _currentWatch = nullptr;
        }
        for (CheckDecl const& item : scope.checks) {
            expectBool(item.assertion, check(item.assertion), "`assert`");
            if (item.message) {
                check(*item.message);
            }
            for (std::string const& target : item.targets) {
                if (elementKind(target) == nullptr) {
                    refuse(item.path + "/targets", "`" + target + "` is not an element in scope");
                }
            }
        }
    }

    Catalog::Action const* modelAction(std::string const& alias, std::string const& actionId,
                                       std::string const& path) {
        ModelDecl const* const model = findDecl(&Scope::models, alias);
        if (model == nullptr) {
            refuse(path + "/model", "`" + alias + "` is not a model alias in scope");
        }
        Catalog::Action const* const action = _catalog->action(actionId);
        if (action == nullptr) {
            refuse(path, "`" + actionId + "` is not an action of the catalog");
            return nullptr;
        }
        if (model != nullptr && action->model != model->type) {
            refuse(path, "`" + actionId + "` is not an action of `" + model->type + "`");
        }
        return action;
    }

    void patch(PatchDecl const& decl, std::string const& binding, Type const& type) {
        if (findDecl(&Scope::queries, decl.query) == nullptr) {
            refuse(decl.path + "/query", "`" + decl.query + "` is not a query in scope");
        }
        _locals.emplace_back(binding, type);
        check(decl.key);
        check(decl.with);
        _locals.pop_back();
    }

    // An optimistic write may not predict a field the server calculates.
    void optimistic(PatchDecl const& decl) {
        QueryDecl const* const query = findDecl(&Scope::queries, decl.query);
        Catalog::Action const* const action = query == nullptr ? nullptr : _catalog->action(query->action);
        if (action == nullptr || decl.with.form != Expr::Form::Object) {
            return;
        }
        for (std::size_t index = 0; index < decl.with.keys.size(); ++index) {
            std::string const& written = decl.with.keys.at(index);
            if (std::ranges::find(action->calculated, written) != action->calculated.end()) {
                refuse(decl.with.args.at(index).path,
                       "`" + written + "` is calculated by the server and cannot be written optimistically");
            }
        }
    }

    // ---- Nodes.

    [[nodiscard]] static bool opensFrame(DocNode const& node) {
        return node.scope != nullptr || node.kind == "forEach" || node.kind == "table";
    }

    // Records the element ids of @p node's subtree in @p frame, stopping at a node that opens a frame of its own.
    static void collectElements(DocNode const& node, Frame& frame) {
        if (std::string const* const elementId = node.text("id")) {
            frame.elements.emplace_back(*elementId, node.kind);
        }
        if (opensFrame(node)) {
            return;
        }
        for (NodeSlot const& slot : node.slots) {
            for (DocNode const& child : slot.nodes) {
                collectElements(child, frame);
            }
        }
    }

    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one block per node concern.
    void visit(DocNode const& node) {
        if (!node.refused.empty()) {
            return;
        }
        static constexpr std::array<std::string_view, 11> kBoolProps{"visible", "enabled",     "readonly", "required",
                                                                     "stale",   "autofocus",   "checked",  "open",
                                                                     "busy",    "dismissible", "multiple"};
        bool const rows = node.kind == "forEach" || node.kind == "table";
        Type rowType;
        for (NamedExpr const& prop : node.props) {
            if (rows && (prop.name == "key")) {
                continue;
            }
            Type const type = check(prop.expr);
            if (std::ranges::find(kBoolProps, prop.name) != kBoolProps.end()) {
                expectBool(prop.expr, type, "`" + prop.name + "`");
            }
            if (rows && prop.name == "rows") {
                if (knownOtherThan(type, {Type::Kind::List})) {
                    refuse(prop.expr.path, "`rows` is a list, not " + described(type));
                }
                rowType = type.element();
            }
        }
        for (auto const& [key, entries] : node.maps) {
            for (NamedExpr const& entry : entries) {
                check(entry.expr);
            }
        }
        if (node.kind == "custom") {
            std::string const* const name = node.text("name");
            bool const declared =
                name != nullptr && (std::ranges::find(_document->components, *name, &ComponentDecl::name) !=
                                        _document->components.end() ||
                                    std::ranges::find(_catalog->components, *name) != _catalog->components.end());
            if (!declared) {
                refuse(node.path,
                       "`" + (name == nullptr ? std::string{} : *name) + "` is not a component of the bundle");
            }
        }
        if (node.kind == "customize") {
            std::string const* const point = node.text("point");
            if (point == nullptr ||
                std::ranges::find(_document->customize, *point, &CustomizePoint::name) == _document->customize.end()) {
                refuse(node.path, "a `customize` node names a customization point of the screen");
            }
        }
        if (std::string const* const action = node.text("action")) {
            std::string const* const model = node.text("model");
            modelAction(model == nullptr ? std::string{} : *model, *action, node.path);
        }
        bool const frame = opensFrame(node);
        if (frame) {
            Frame inner{.scope = node.scope.get(), .names = {}, .elements = {}, .row = rows, .rowType = rowType};
            for (NodeSlot const& slot : node.slots) {
                for (DocNode const& child : slot.nodes) {
                    collectElements(child, inner);
                }
            }
            _frames.push_back(std::move(inner));
            if (node.scope) {
                scope(*node.scope, rows);
            }
        }
        if (Expr const* const key = rows ? node.prop("key") : nullptr) {
            check(*key);
        }
        for (NamedCommands const& event : node.events) {
            commands(event.commands);
        }
        for (NodeSlot const& slot : node.slots) {
            bool const wasInError = _inBoundaryError;
            if (node.kind == "boundary" && slot.name == "error") {
                _inBoundaryError = true;
                _locals.emplace_back("boundary", Type{});
            }
            for (DocNode const& child : slot.nodes) {
                visit(child);
            }
            if (node.kind == "boundary" && slot.name == "error") {
                _locals.pop_back();
            }
            _inBoundaryError = wasInError;
        }
        if (frame) {
            _frames.pop_back();
        }
    }

    void points() {
        for (CustomizePoint const& point : _document->customize) {
            bool const needsOf = point.kind == "columns" || point.kind == "layout" || point.kind == "arrange";
            if (needsOf && !_frames.empty() &&
                std::ranges::find(_frames.front().elements, point.of, &std::pair<std::string, std::string>::first) ==
                    _frames.front().elements.end()) {
                refuse(point.path + "/of", "`" + point.of + "` is not an element of the screen");
            }
            if (point.kind == "choice" && !point.defaultValue.isNull() &&
                (point.defaultValue.asString() == nullptr ||
                 std::ranges::find(point.options, *point.defaultValue.asString()) == point.options.end())) {
                refuse(point.path + "/default", "the default is not one of the point's `options`");
            }
        }
    }

    // ---- Cycles.

    template <typename T>
    static std::vector<T const*> cycleFrom(T const* start, std::vector<std::pair<T const*, T const*>> const& edges) {
        // A depth-first walk that returns the first cycle back to @p start, as the nodes along it.
        std::vector<T const*> path{start};
        std::vector<std::size_t> next{0};
        while (!path.empty()) {
            T const* const place = path.back();
            std::size_t& cursor = next.back();
            bool advanced = false;
            for (; cursor < edges.size(); ++cursor) {
                if (edges.at(cursor).first != place) {
                    continue;
                }
                T const* const target = edges.at(cursor).second;
                ++cursor;
                if (target == start) {
                    path.push_back(target);
                    return path;
                }
                if (std::ranges::find(path, target) == path.end()) {
                    path.push_back(target);
                    next.push_back(0);
                    advanced = true;
                }
                break;
            }
            if (!advanced && cursor >= edges.size()) {
                path.pop_back();
                next.pop_back();
            }
        }
        return {};
    }

    void cycles() {
        letCycles();
        watchCycles();
    }

    void letCycles() {
        std::vector<LetDecl const*> lets;
        for (auto const& edge : _letEdges) {
            if (std::ranges::find(lets, edge.first) == lets.end()) {
                lets.push_back(edge.first);
            }
        }
        std::vector<LetDecl const*> reported;
        for (LetDecl const* const let : lets) {
            if (std::ranges::find(reported, let) != reported.end()) {
                continue;
            }
            std::vector<LetDecl const*> const cycle = cycleFrom(let, _letEdges);
            if (cycle.empty()) {
                continue;
            }
            std::string chain;
            for (LetDecl const* const step : cycle) {
                chain += (chain.empty() ? "" : " → ") + step->name;
                reported.push_back(step);
            }
            refuse(let->path, "the `let` entries form a cycle: " + chain);
        }
    }

    void watchCycles() {
        std::vector<std::pair<WatchDecl const*, WatchDecl const*>> edges;
        for (auto const& [writer, state] : _writes) {
            for (auto const& [observer, observed] : _observes) {
                if (observed == state) {
                    edges.emplace_back(writer, observer);
                }
            }
        }
        std::vector<WatchDecl const*> seen;
        for (WatchDecl const* const watch : _watches) {
            if (std::ranges::find(seen, watch) != seen.end()) {
                continue;
            }
            std::vector<WatchDecl const*> const cycle = cycleFrom(watch, edges);
            if (cycle.empty()) {
                continue;
            }
            std::string chain;
            for (WatchDecl const* const step : cycle) {
                chain += (chain.empty() ? "" : " → ") + step->path;
                seen.push_back(step);
            }
            refuse(watch->path, "the watches form a cycle: " + chain);
        }
    }
};
// NOLINTEND(misc-no-recursion)

}  // namespace detail

/// @brief Validates a document against a catalog, as a client does before mounting it and the server does when it
///        registers it.
///
/// Every refusal is returned, each with the document, the JSON pointer and the reason: a name that does not
/// resolve (innermost scope first), a name declared twice in a scope, a type that does not check where both sides
/// are known, a cycle among `let` entries or among watches, a model type, action or component the catalog lacks,
/// `instance` on a model that is not keyed, a row scope's alias without `instance`, an optimistic write to a
/// field the server calculates, `"offline": "queue"` on a credential mutation, and `cancelClose`, `retry` or
/// `{"ref": "event"}` outside the place it is valid in.
/// @param document The parsed document.
/// @param catalog What the server serves.
/// @return The refusals; empty when the document may mount.
[[nodiscard]] inline std::vector<Refusal> validate(Document const& document, Catalog const& catalog) {
    return detail::Validator{document, catalog}.run();
}

}  // namespace morph::ui

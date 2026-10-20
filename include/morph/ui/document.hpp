// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <format>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "expression.hpp"
#include "value.hpp"

/// @file
/// @brief The UI document (spec 5 §3): its model — declarations, commands, view nodes, components and
///        customization points — and `loadDocument`, which parses a document and runs its load-time validation.
///
/// Specified in `docs/spec/ui/document.md`.

namespace morph::ui {

/// @brief The vocabulary a document declares; the only one this client reads.
inline constexpr std::string_view kVocabulary = "ui/1";

/// @brief What the catalog says about a model type.
struct ModelInfo {
    /// @brief Whether a shared singleton is equivalent to a private instance.
    bool stateless = false;
    /// @brief Whether the model can be shared by an instance key, so an alias may give `instance` an expression.
    bool keyed = false;
};

/// @brief One effect an action declares, as the catalog states it.
struct ActionEffect {
    /// @brief What the effect is: `updates`, `changes`, `changesInstance` or `none`.
    std::string kind;
    /// @brief The wire name of the record the effect names; absent for `changesInstance` and `none`.
    std::optional<std::string> record;

    /// @brief Memberwise equality.
    /// @param lhs One effect.
    /// @param rhs The other.
    /// @return True when kind and record match.
    friend bool operator==(ActionEffect const& lhs, ActionEffect const& rhs) = default;
};

/// @brief A record an action's result holds, as the catalog states it.
struct ActionRecordInfo {
    /// @brief The record's wire name.
    std::string record;
    /// @brief The record's key field; absent for a keyless record.
    std::optional<std::string> keyField;
    /// @brief The JSON paths at which the result holds the record; `""` is the result itself.
    std::vector<std::string> paths;

    /// @brief Memberwise equality.
    /// @param lhs One entry.
    /// @param rhs The other.
    /// @return True when record, key field and paths match.
    friend bool operator==(ActionRecordInfo const& lhs, ActionRecordInfo const& rhs) = default;
};

/// @brief What the catalog says about an action.
struct ActionInfo {
    /// @brief The registered type id of the model the action belongs to.
    std::string modelType;
    /// @brief The fields of the action's reply rows that the server calculates; an `optimistic` write may not name
    ///        them.
    std::vector<std::string> calculatedFields;
    /// @brief The reply fields the server marks `secret`.
    std::vector<std::string> secretFields;
    /// @brief Whether the action sends a credential.
    bool credential = false;
    /// @brief When the action's result is a record, the names of its members that hold a list: what a `patch` or
    ///        `optimistic` of a query of the action may name as its `at`. Empty when the catalog does not say, or the
    ///        result is not a record.
    std::optional<std::vector<std::string>> listFields;
    /// @brief What the action declares it changes. Absent when it declared nothing, which makes it a read; present
    ///        and empty is not produced by the registry.
    std::optional<std::vector<ActionEffect>> effects;
    /// @brief The records the action's result holds, with the paths to each. Empty when it holds none or the
    ///        catalog does not say.
    std::vector<ActionRecordInfo> records;
};

/// @brief The facts load-time validation needs from the application's catalog. Local mode answers them from the
///        model registry in-process, the server at registration, a remote client from the UI service's catalog.
///
/// A lookup left empty skips the rules that need it: `model` the unknown-model and keyed-instance rules; `action`
/// the unknown-action, wrong-model, optimistic and offline rules.
struct Catalog {
    /// @brief Looks up a model type by its registered type id.
    std::function<std::optional<ModelInfo>(std::string_view typeId)> model;
    /// @brief Looks up an action by its id.
    std::function<std::optional<ActionInfo>(std::string_view actionId)> action;
    /// @brief The schema JSON of an action, as `forms::schemaJson<A>()` emits it: what a `form` node builds its form
    ///        from. Local mode answers it from the action registry, a remote client from the server's `schemas`.
    ///        Empty: a `form` node is reported and shown as a muted text.
    std::function<std::optional<std::string>(std::string_view actionId)> schema;
    /// @brief The `v-*` view document whose `v-query` is an action, as `views::viewSchemaJson<V>()` emits it: what a
    ///        `collection` node naming that action builds its collection from. Empty: a `collection` node is
    ///        reported and shown as a muted text.
    std::function<std::optional<std::string>(std::string_view actionId)> view;
    /// @brief The `w-*` wizard document whose first step is an action, as `flows::wizardSchemaJson<W>()` emits it:
    ///        what a `wizard` node naming that action builds its wizard from. Empty: a `wizard` node is reported and
    ///        shown as a muted text.
    std::function<std::optional<std::string>(std::string_view actionId)> wizard;
};

/// @brief A `Catalog::view` or `Catalog::wizard` lookup over documents by action id.
/// @param documents The documents by action id; the lookup keeps its own copy.
/// @return The lookup.
[[nodiscard]] inline std::function<std::optional<std::string>(std::string_view)> documentLookup(
    std::map<std::string, std::string, std::less<>> documents) {
    return [known = std::make_shared<std::map<std::string, std::string, std::less<>> const>(std::move(documents))](
               std::string_view actionId) -> std::optional<std::string> {
        auto const found = known->find(actionId);
        if (found == known->end()) {
            return std::nullopt;
        }
        return found->second;
    };
}

/// @brief Options of `loadDocument`.
struct LoadOptions {
    /// @brief The catalog facts.
    Catalog catalog;
};

/// @brief How a model alias binds its instance (spec 5 §5).
enum class InstanceMode : std::uint8_t {
    Default,   ///< None named: `shared` when the catalog marks the model stateless, `private` otherwise.
    Private,   ///< An instance of the scope's own, released when the scope unmounts.
    Shared,    ///< A stateless model's instance, its queries cached with other screens'; refused for a stateful model.
    Keyed,     ///< The shared instance an expression's key selects.
    Borrowed,  ///< The opener's binding of an alias (`{"from": alias}`), for a screen opened as a dialog.
};

/// @brief A model alias.
struct ModelDecl {
    /// @brief The alias.
    std::string name;
    /// @brief The declaration's JSON Pointer.
    std::string path;
    /// @brief The model's registered type id.
    std::string type;
    /// @brief How the instance binds.
    InstanceMode instance = InstanceMode::Default;
    /// @brief The key expression of a `Keyed` instance.
    ExprPtr key;
    /// @brief The opener's alias of a `Borrowed` instance.
    std::string borrowedFrom;
};

/// @brief A state declaration.
struct StateDecl {
    /// @brief The name.
    std::string name;
    /// @brief The declaration's JSON Pointer.
    std::string path;
    /// @brief The declared type; `any` when the document gives none.
    Type type;
    /// @brief The initial value; `nullptr` starts `null`.
    ExprPtr init;
};

/// @brief A derived value (`let`).
struct LetDecl {
    /// @brief The name.
    std::string name;
    /// @brief The declaration's JSON Pointer.
    std::string path;
    /// @brief The expression.
    ExprPtr value;
};

/// @brief A query (spec 5 §5).
struct QueryDecl {
    /// @brief The name.
    std::string name;
    /// @brief The declaration's JSON Pointer.
    std::string path;
    /// @brief The model alias it goes through.
    std::string model;
    /// @brief The action id it runs, or evaluates.
    std::string action;
    /// @brief Whether it is an `evaluate` of the action (spec 5 §7) rather than a call.
    bool evaluate = false;
    /// @brief The body, which is the query's key; `nullptr` sends an empty object.
    ExprPtr body;
    /// @brief The gate; `nullptr` is always on.
    ExprPtr when;
    /// @brief Milliseconds the key must be stable before a fetch.
    std::int64_t debounce = 0;
    /// @brief Milliseconds between polls; 0 does not poll.
    std::int64_t refreshEvery = 0;
    /// @brief Milliseconds a cached value counts as fresh.
    std::int64_t staleTime = 0;
};

/// @brief What a mutation run does while another is in flight.
enum class Concurrency : std::uint8_t {
    Exclusive,  ///< The run is refused.
    Serial,     ///< Runs queue, one queue per `serialKey`.
    Latest,     ///< Every run is sent; only the newest reply applies.
    Parallel,   ///< Every run is sent; replies apply as they arrive.
};

/// @brief An `optimistic` write: the row a mutation run predicts before its reply.
struct OptimisticDecl {
    /// @brief The query whose cached list it writes.
    std::string query;
    /// @brief The member of the query's record value that holds the list; empty when the value is the list itself.
    std::string at;
    /// @brief The row key to write, read against the run's body bound as `body`.
    ExprPtr key;
    /// @brief The predicted row, read against the run's body bound as `body`.
    ExprPtr with;
};

struct Command;

/// @brief A command list, run as one batch.
using Commands = std::vector<Command>;

/// @brief The commands of `ui/1` (spec 5 §8, spec 6).
enum class CommandKind : std::uint8_t {
    Set,            ///< `{"set": "name.path", "value": e}`
    Reset,          ///< `{"reset": "name"}`
    Run,            ///< `{"run": "m", "body": e?, "as": "x"?, "each": e?}`
    Refetch,        ///< `{"refetch": "q"}`
    Open,           ///< `{"open": "dialog"}`
    Close,          ///< `{"close": "dialog" | "self"}`
    Navigate,       ///< `{"navigate": "screen" | {"route": e}, "params": e, "as": ..., "onResult": [...], ...}`
    Return,         ///< `{"return": e}`
    Focus,          ///< `{"focus": "id"}`
    Host,           ///< `{"host": "name", "args": e?}`
    SwitchBackend,  ///< `{"switchBackend": "name"}`
    ResetPrefs,     ///< `{"resetPrefs": "point" | "all"}`
    SetLocale,      ///< `{"setLocale": "de"}`
    Notify,         ///< `{"notify": e, "role": "success"}`
    Seq,            ///< `{"seq": [...]}`
    All,            ///< `{"all": [...]}`
    If,             ///< `{"if": [cond, [...], [...]]}`
    CancelClose,    ///< `{"cancelClose": true}`
    Retry,          ///< `{"retry": true}`
    SignIn,         ///< `{"signIn": {"from": "m"}}`
    SignOut,        ///< `{"signOut": true}`
    Upload,         ///< `{"upload": e, "to": "m", "as": "x"?, "id": "t"?, "into": "bytes"?, "nameAs"?, "typeAs"?}`
    /// `{"download": e, "save": "dialog", "name": e?}` or `{"download": e, "to": "preview", "as": "url"}`, either
    /// with `"from": "bytes"`, a `name` and a `type`
    Download,
};

/// @brief One command. Which members a kind uses is listed with each member.
struct Command {
    /// @brief The kind.
    CommandKind kind = CommandKind::Set;
    /// @brief The command's JSON Pointer.
    std::string path;
    /// @brief The name it acts on: `set`'s `name.path`; the state of `reset`; the mutation of `run`, `signIn` and
    ///        `upload`; the query of `refetch`; the element of `open`, `close` and `focus`; the screen of
    ///        `navigate`; the name of `host`, `switchBackend`, `setLocale`; the point of `resetPrefs`; the state of
    ///        a `download` to preview.
    std::string target;
    /// @brief The main expression: `set`'s value, `run`'s body, `navigate`'s params, `return`'s value, `host`'s
    ///        args, `notify`'s message, `if`'s condition, the file of `upload` and `download`.
    ExprPtr value;
    /// @brief The second expression: `run`'s `each`, `navigate`'s `route`, `download`'s suggested `name`.
    ExprPtr extra;
    /// @brief A binding name: `run`'s and `upload`'s `as`.
    std::string binding;
    /// @brief A closed option: `navigate`'s `as`, `notify`'s `role`, `download`'s `save` or `to`.
    std::string option;
    /// @brief The transfer id of `upload`, whose progress is an element value.
    std::string transfer;
    /// @brief Whether the file moves inline: `download`'s `"from": "bytes"`, `upload`'s `"into": "bytes"`.
    bool bytes = false;
    /// @brief `download`'s media type of inline bytes.
    ExprPtr mediaType;
    /// @brief `upload`'s body members for an inline file's name and media type; empty for none.
    std::string nameAs;
    /// @brief See `nameAs`.
    std::string typeAs;
    /// @brief Nested commands: `seq` and `all`; `if`'s then branch; `navigate`'s `onResult`.
    Commands commands;
    /// @brief The alternative: `if`'s else branch; `navigate`'s `onCancel`.
    Commands alternative;
};

/// @brief A mutation (spec 5 §5).
struct MutationDecl {
    /// @brief The name.
    std::string name;
    /// @brief The declaration's JSON Pointer.
    std::string path;
    /// @brief The model alias it goes through.
    std::string model;
    /// @brief The action id it runs.
    std::string action;
    /// @brief The body; `nullptr` sends an empty object.
    ExprPtr body;
    /// @brief What a run does while another is in flight.
    Concurrency concurrency = Concurrency::Exclusive;
    /// @brief The queue key of `serial` runs.
    ExprPtr serialKey;
    /// @brief The row a run predicts before its reply.
    std::optional<OptimisticDecl> optimistic;
    /// @brief Commands run after a success.
    Commands onSuccess;
    /// @brief Commands run after a failure.
    Commands onError;
    /// @brief Whether a run that cannot reach the server is queued offline.
    bool offlineQueue = false;
};

/// @brief Which changes a watch reacts to.
enum class WatchSource : std::uint8_t {
    Any,   ///< Every change.
    User,  ///< Only changes caused by the user's own edits.
};

/// @brief A cascade: commands that run when a value changes (spec 5 §6).
struct WatchDecl {
    /// @brief The declaration's JSON Pointer.
    std::string path;
    /// @brief The value watched.
    ExprPtr on;
    /// @brief Which changes run it.
    WatchSource source = WatchSource::Any;
    /// @brief The commands.
    Commands body;
};

/// @brief A validation across elements (spec 5 §6).
struct CheckDecl {
    /// @brief The check's id.
    std::string id;
    /// @brief The declaration's JSON Pointer.
    std::string path;
    /// @brief The condition; `false` fails the check, `null` does not.
    ExprPtr assertion;
    /// @brief The message added to each target's errors.
    ExprPtr message;
    /// @brief The elements the message attaches to.
    std::vector<std::string> targets;
    /// @brief Whether a failure blocks `ready`.
    bool blocking = false;
};

/// @brief The declarations of one scope, in declaration order.
struct ScopeDecl {
    /// @brief Model aliases.
    std::vector<ModelDecl> models;
    /// @brief State.
    std::vector<StateDecl> state;
    /// @brief Derived values.
    std::vector<LetDecl> lets;
    /// @brief Queries.
    std::vector<QueryDecl> queries;
    /// @brief Mutations.
    std::vector<MutationDecl> mutations;
    /// @brief Watches.
    std::vector<WatchDecl> watches;
    /// @brief Checks.
    std::vector<CheckDecl> checks;
};

struct DocNode;

/// @brief A shared, immutable view node.
using DocNodePtr = std::shared_ptr<DocNode const>;

/// @brief A view node of the document (spec 5 §9), or a structured part of one (a table column, a tab, a switch
///        case, a menu item, an `a11y`, `drag`, `drop` or `action` record, a form field overlay).
///
/// Properties are kept by their document keys, so the interpreter reads a kind's properties by name; which keys a
/// kind has, and what shape each takes, is the kind table of `docs/spec/ui/document.md`.
struct DocNode {
    /// @brief The node kind (`text`, `table`, …), or for a part its part kind (`column`, `tab`, `case`, …).
    std::string kind;
    /// @brief The element id; empty when none is given.
    std::string id;
    /// @brief The node's JSON Pointer.
    std::string path;
    /// @brief Expression properties, by key.
    std::map<std::string, ExprPtr, std::less<>> props;
    /// @brief Command lists, by key (`onClick`, …), including a custom node's `on` events as `on.<event>`.
    std::map<std::string, Commands, std::less<>> events;
    /// @brief Child node lists, by key (`children`, `header`, `loading`, `error`, `default`, `cell`, `fallback`).
    std::map<std::string, std::vector<DocNodePtr>, std::less<>> children;
    /// @brief Structured parts, by key (`columns`, `tabs`, `cases`, `items`, `a11y`, `action`, `fields`, …).
    std::map<std::string, std::vector<DocNodePtr>, std::less<>> parts;
    /// @brief Plain string properties, by key (`testId`, `surface`, `name` of a custom node, `model` and `action`
    ///        of a form, `point` of `customize`, `role` of `a11y`, a column's `id`, …).
    std::map<std::string, std::string, std::less<>> names;
    /// @brief Key bindings: chord to commands.
    std::map<std::string, Commands, std::less<>> keys;
    /// @brief Literal flags, by key (`showLabel` of an input).
    std::map<std::string, bool, std::less<>> flags;
    /// @brief The declarations of the scope the node creates (dialog, drawer, switch case, `forEach` or `table`
    ///        row); empty for a node that creates none.
    std::optional<ScopeDecl> scope;

    /// @brief An expression property.
    /// @param key The key.
    /// @return The expression, or `nullptr` when absent.
    [[nodiscard]] ExprPtr prop(std::string_view key) const {
        auto const found = props.find(key);
        return found == props.end() ? nullptr : found->second;
    }

    /// @brief A command list.
    /// @param key The key.
    /// @return The commands, or `nullptr` when absent.
    [[nodiscard]] Commands const* event(std::string_view key) const {
        auto const found = events.find(key);
        return found == events.end() ? nullptr : &found->second;
    }

    /// @brief A child node list.
    /// @param key The key.
    /// @return The nodes; empty when absent.
    [[nodiscard]] std::span<DocNodePtr const> nodes(std::string_view key) const {
        auto const found = children.find(key);
        return found == children.end() ? std::span<DocNodePtr const>{} : std::span<DocNodePtr const>{found->second};
    }

    /// @brief A list of parts.
    /// @param key The key.
    /// @return The parts; empty when absent.
    [[nodiscard]] std::span<DocNodePtr const> partList(std::string_view key) const {
        auto const found = parts.find(key);
        return found == parts.end() ? std::span<DocNodePtr const>{} : std::span<DocNodePtr const>{found->second};
    }

    /// @brief A plain string property.
    /// @param key The key.
    /// @return The string; empty when absent.
    [[nodiscard]] std::string_view name(std::string_view key) const {
        auto const found = names.find(key);
        return found == names.end() ? std::string_view{} : std::string_view{found->second};
    }

    /// @brief A literal flag.
    /// @param key The key.
    /// @return Its value; `false` when absent.
    [[nodiscard]] bool flag(std::string_view key) const {
        auto const found = flags.find(key);
        return found != flags.end() && found->second;
    }
};

/// @brief A custom component declaration (spec 5 §10).
struct Component {
    /// @brief The component's name.
    std::string name;
    /// @brief The declaration's JSON Pointer.
    std::string path;
    /// @brief The content hash of a delivered QML component (`sha256:<hex>`); empty for a host component.
    std::string qml;
    /// @brief The name of a host component; empty for a delivered one.
    std::string host;
    /// @brief The host component's version.
    std::int64_t version = 0;
    /// @brief The props and their prop types (`string`, `bool`, `int`, `exact`, `number`, `json`).
    std::map<std::string, std::string, std::less<>> props;
    /// @brief The events and their payload types.
    std::map<std::string, Type, std::less<>> events;
    /// @brief The subtree of built-in nodes a renderer shows when it cannot load the component.
    DocNodePtr fallback;
};

/// @brief A customization point (spec 5 §12b).
struct CustomizePoint {
    /// @brief The point's name.
    std::string name;
    /// @brief The declaration's JSON Pointer.
    std::string path;
    /// @brief `columns`, `choice`, `value`, `layout`, `saved`, `arrange`, and in the app document `pins`.
    std::string kind;
    /// @brief The element a `columns` or `arrange` point adjusts.
    std::string of;
    /// @brief The options of a `choice`.
    std::vector<std::string> options;
    /// @brief The type of a `value`.
    Type type;
    /// @brief The default choice; `null` when none is declared.
    Value defaultValue;
};

/// @brief What a load-time warning is about. A warning never refuses a document.
enum class LoadWarningKind : std::uint8_t {
    WatchFeedback,  ///< A watch of a query runs a mutation that changes data, on any change of it.
    Uncovered,      ///< A mutation whose action declares nothing, and after which nothing refreshes.
};

/// @brief A finding of load-time validation that does not refuse the document.
struct LoadWarning {
    /// @brief What it is about.
    LoadWarningKind kind = LoadWarningKind::Uncovered;
    /// @brief The JSON Pointer of the declaration it names.
    std::string path;
    /// @brief The finding, in one sentence.
    std::string text;

    /// @brief The warning as one line of text.
    /// @param document The id of the document it was found in; empty leaves it out.
    /// @return `document <id> at <path>: <text>`, leaving out the parts that are empty.
    [[nodiscard]] std::string message(std::string_view document) const {
        std::string out;
        if (!document.empty()) {
            out += "document " + std::string{document} + " ";
        }
        out += "at " + (path.empty() ? std::string{"/"} : path) + ": " + text;
        return out;
    }
};

/// @brief A loaded and validated UI document: a screen, or the app document (id `app`, spec 6 §5).
struct Document {
    /// @brief The vocabulary, `ui/1`.
    std::string vocab;
    /// @brief The document id, such as `lims.results`.
    std::string id;
    /// @brief The title expression.
    ExprPtr title;
    /// @brief The identity expression: equal identities are one screen.
    ExprPtr identity;
    /// @brief The unsaved-changes expression.
    ExprPtr dirty;
    /// @brief The subtitle expression of the screen's head; `nullptr` when none is declared.
    ExprPtr subtitle;
    /// @brief The head's status badges: `headBadge` parts, in order.
    std::vector<DocNodePtr> status;
    /// @brief The head's actions: `actionButton` parts, in order.
    std::vector<DocNodePtr> actions;
    /// @brief The typed params, in declaration order.
    std::vector<std::pair<std::string, Type>> params;
    /// @brief The screen scope's declarations.
    ScopeDecl scope;
    /// @brief The lifecycle command lists by key: `onMount`, `onUnmount`, `onClose`, `onSave`, `onBackendChange`.
    std::map<std::string, Commands, std::less<>> lifecycle;
    /// @brief The screen's key bindings.
    std::map<std::string, Commands, std::less<>> keys;
    /// @brief When errors show: `touched`, `submit` or `always`; empty for the default.
    std::string showErrors;
    /// @brief Queries started before the screen mounts.
    std::vector<std::string> prefetch;
    /// @brief Custom components, by name.
    std::map<std::string, Component, std::less<>> components;
    /// @brief Customization points, by name.
    std::map<std::string, CustomizePoint, std::less<>> customize;
    /// @brief The view; `nullptr` for the app document.
    DocNodePtr view;
    /// @brief What load-time validation found that does not refuse the document, in the order found. Empty when
    ///        the catalog answers no actions.
    std::vector<LoadWarning> warnings;
};

namespace detail {

/// @brief The exception the loader throws inside itself; `loadDocument` turns it into its result.
class DocumentRefused : public std::runtime_error {
public:
    /// @brief Wraps a refusal.
    /// @param refusal The refusal.
    explicit DocumentRefused(Refusal refusal) : std::runtime_error{refusal.reason}, _refusal{std::move(refusal)} {}

    /// @brief The refusal.
    /// @return It.
    [[nodiscard]] Refusal const& refusal() const noexcept { return _refusal; }

private:
    Refusal _refusal;
};

/// @brief Refuses the document.
/// @param path   Where.
/// @param reason Why.
[[noreturn]] inline void refuse(std::string const& path, std::string reason) {
    throw DocumentRefused{Refusal{.document = {}, .path = path, .reason = std::move(reason)}};
}

/// @brief What shape a node key's value takes.
enum class Shape : std::uint8_t {
    Expr,           ///< An expression.
    Condition,      ///< An expression checked as a `bool`.
    Commands,       ///< A command list.
    Nodes,          ///< A node or a list of nodes.
    Text,           ///< A plain string.
    Name,           ///< A plain string that is a name.
    Part,           ///< A record, parsed by the part table of its part kind.
    Parts,          ///< A list of such records.
    PartsOrExpr,    ///< A list of records, or an expression yielding one.
    ExprRecord,     ///< A record of expressions.
    CommandRecord,  ///< A record of command lists.
    Keys,           ///< Key bindings: chord to command list.
    Flag,           ///< A literal `true` or `false`, set once.
};

/// @brief The key is evaluated in the node's own scope (a dialog's, a row's), not the scope around it.
inline constexpr std::uint8_t kInner = 1U;
/// @brief The key is required.
inline constexpr std::uint8_t kRequired = 2U;
/// @brief The key's content is a boundary's `error`, where `boundary.error` and `retry` are valid.
inline constexpr std::uint8_t kBoundaryError = 4U;

/// @brief One key of a kind.
struct KeySpec {
    /// @brief The key.
    std::string_view key;
    /// @brief Its shape.
    Shape shape;
    /// @brief The part kind, for `Part`, `Parts` and `PartsOrExpr`.
    std::string_view part = {};  // NOLINT(readability-redundant-member-init): table rows leave it out
    /// @brief `kInner`, `kRequired`, `kBoundaryError`.
    std::uint8_t flags = 0;
};

/// @brief The node or part creates a scope.
inline constexpr std::uint8_t kScope = 1U;
/// @brief The scope it creates is a row scope, where `row` is valid.
inline constexpr std::uint8_t kRowScope = 2U;
/// @brief It carries the input decorations `readonly`, `required`, `errors` and `stale`.
inline constexpr std::uint8_t kField = 4U;
/// @brief It is a part, not a node: it has no common properties.
inline constexpr std::uint8_t kPart = 8U;
/// @brief A command may write its draft with `set`.
inline constexpr std::uint8_t kWritable = 16U;
/// @brief Any path below its id is an element value (a form's fields).
inline constexpr std::uint8_t kOpenValues = 32U;

/// @brief A node kind or part kind: its keys and its element values.
struct KindSpec {
    /// @brief The kind.
    std::string_view kind;
    /// @brief Its own keys.
    std::span<KeySpec const> keys;
    /// @brief `kScope`, `kRowScope`, `kField`, `kPart`, `kWritable`, `kOpenValues`.
    std::uint8_t flags = 0;
    /// @brief The element values beyond those every element has.
    std::span<std::string_view const> values = {};  // NOLINT(readability-redundant-member-init): rows leave it out
};

// NOLINTBEGIN(modernize-use-designated-initializers): the kind table reads as rows; names on every field would not.
/// @brief The keys every node takes.
inline constexpr std::array<KeySpec, 18> kCommonKeys = {{
    {"id", Shape::Name},
    {"visible", Shape::Condition},
    {"enabled", Shape::Condition},
    {"tooltip", Shape::Expr},
    {"sizing", Shape::Expr},
    {"minWidth", Shape::Expr},
    {"maxWidth", Shape::Expr},
    {"minHeight", Shape::Expr},
    {"maxHeight", Shape::Expr},
    {"alignSelf", Shape::Text},
    {"span", Shape::Expr},
    {"testId", Shape::Text},
    {"surface", Shape::Text},
    {"autofocus", Shape::Expr},
    {"a11y", Shape::Part, "a11y"},
    {"drag", Shape::Part, "drag"},
    {"drop", Shape::Part, "drop"},
    {"keys", Shape::Keys},
}};

/// @brief The input decorations text and the inputs take.
inline constexpr std::array<KeySpec, 4> kFieldKeys = {{
    {"readonly", Shape::Condition},
    {"required", Shape::Condition},
    {"errors", Shape::Expr},
    {"stale", Shape::Condition},
}};

/// @brief The element values every element has.
inline constexpr std::array<std::string_view, 5> kAnyValues = {"visible", "enabled", "valid", "errors", "shownErrors"};
/// @brief The element values of a text, date-time or slider input.
inline constexpr std::array<std::string_view, 5> kInputValues = {"value", "draft", "dirty", "focused", "touched"};
/// @brief The element values of a checkbox.
inline constexpr std::array<std::string_view, 1> kCheckboxValues = {"checked"};
/// @brief The element values of a select.
inline constexpr std::array<std::string_view, 2> kSelectValues = {"selected", "selectedOption"};
/// @brief The element values of a table.
inline constexpr std::array<std::string_view, 10> kTableValues = {
    "selection",   "activeKey",     "sort",    "filters", "viewCount",
    "sourceCount", "selectedCount", "pending", "error",   "hoveredKey"};
/// @brief The element values of a dialog or drawer.
inline constexpr std::array<std::string_view, 3> kDialogValues = {"open", "ready", "submitted"};
/// @brief The element values of a form, beside its fields.
inline constexpr std::array<std::string_view, 3> kFormValues = {"ready", "body", "submitted"};
/// @brief The values of a transfer id.
inline constexpr std::array<std::string_view, 3> kTransferValues = {"progress", "pending", "error"};

/// @brief The keys of `text`.
inline constexpr std::array<KeySpec, 2> kTextKeys = {{{"text", Shape::Expr}, {"role", Shape::Expr}}};
/// @brief The keys of `button`.
inline constexpr std::array<KeySpec, 5> kButtonKeys = {{{"label", Shape::Expr},
                                                        {"icon", Shape::Expr},
                                                        {"emphasis", Shape::Text},
                                                        {"onClick", Shape::Commands},
                                                        {"onActivate", Shape::Commands}}};
/// @brief The keys of `textInput`.
inline constexpr std::array<KeySpec, 9> kTextInputKeys = {{{"value", Shape::Expr},
                                                           {"placeholder", Shape::Expr},
                                                           {"mode", Shape::Expr},
                                                           {"commitOn", Shape::Text},
                                                           {"label", Shape::Expr},
                                                           {"showLabel", Shape::Flag},
                                                           {"onChange", Shape::Commands},
                                                           {"onCommit", Shape::Commands},
                                                           {"onSubmit", Shape::Commands}}};
/// @brief The keys of `checkbox`.
inline constexpr std::array<KeySpec, 4> kCheckboxKeys = {
    {{"label", Shape::Expr}, {"checked", Shape::Expr}, {"onToggle", Shape::Commands}, {"onChange", Shape::Commands}}};
/// @brief The keys of `select`.
inline constexpr std::array<KeySpec, 8> kSelectKeys = {{{"options", Shape::Expr},
                                                        {"selected", Shape::Expr},
                                                        {"style", Shape::Expr},
                                                        {"label", Shape::Expr},
                                                        {"showLabel", Shape::Flag},
                                                        {"onStale", Shape::Text},
                                                        {"onChoose", Shape::Commands},
                                                        {"onChange", Shape::Commands}}};
/// @brief The keys of `menu`.
inline constexpr std::array<KeySpec, 1> kMenuKeys = {{{"items", Shape::Parts, "menuItem"}}};
/// @brief The keys of a menu item part.
inline constexpr std::array<KeySpec, 7> kMenuItemKeys = {{{"label", Shape::Expr},
                                                          {"icon", Shape::Expr},
                                                          {"keys", Shape::Text},
                                                          {"checked", Shape::Expr},
                                                          {"enabled", Shape::Condition},
                                                          {"onClick", Shape::Commands},
                                                          {"items", Shape::Parts, "menuItem"}}};
/// @brief The keys of `column` and `row`.
inline constexpr std::array<KeySpec, 4> kContainerKeys = {
    {{"children", Shape::Nodes}, {"gap", Shape::Expr}, {"align", Shape::Text}, {"justify", Shape::Text}}};
/// @brief The keys of `grid`; `justify` is read only to refuse it.
inline constexpr std::array<KeySpec, 5> kGridKeys = {{{"children", Shape::Nodes},
                                                      {"gap", Shape::Expr},
                                                      {"columns", Shape::Expr},
                                                      {"align", Shape::Text},
                                                      {"justify", Shape::Text}}};
/// @brief The keys of `panel`.
inline constexpr std::array<KeySpec, 5> kPanelKeys = {{{"title", Shape::Expr},
                                                       {"padding", Shape::Expr},
                                                       {"collapsible", Shape::Expr},
                                                       {"collapsed", Shape::Expr},
                                                       {"children", Shape::Nodes}}};
/// @brief The keys of `scroll`.
inline constexpr std::array<KeySpec, 2> kScrollKeys = {{{"axis", Shape::Expr}, {"children", Shape::Nodes}}};
/// @brief The keys of `switch`.
inline constexpr std::array<KeySpec, 3> kSwitchKeys = {
    {{"on", Shape::Expr, {}, kRequired}, {"cases", Shape::Parts, "case"}, {"default", Shape::Nodes}}};
/// @brief The keys of a `switch` case part.
inline constexpr std::array<KeySpec, 2> kCaseKeys = {
    {{"when", Shape::Expr, {}, kRequired}, {"children", Shape::Nodes, {}, kInner}}};
/// @brief The keys of `tabs`.
inline constexpr std::array<KeySpec, 1> kTabsKeys = {{{"tabs", Shape::Parts, "tab"}}};
/// @brief The keys of a `tabs` page part.
inline constexpr std::array<KeySpec, 2> kTabKeys = {{{"label", Shape::Expr}, {"children", Shape::Nodes}}};
/// @brief The keys of `dialog` and `drawer`.
inline constexpr std::array<KeySpec, 9> kDialogKeys = {{{"open", Shape::Condition},
                                                        {"title", Shape::Expr},
                                                        {"showErrors", Shape::Text},
                                                        {"children", Shape::Nodes, {}, kInner},
                                                        {"onDismiss", Shape::Commands},
                                                        {"default", Shape::Commands, {}, kInner},
                                                        {"cancel", Shape::Commands, {}, kInner},
                                                        {"actions", Shape::Parts, "actionButton", kInner},
                                                        {"side", Shape::Expr}}};
/// @brief The keys of `busy`.
inline constexpr std::array<KeySpec, 2> kBusyKeys = {{{"active", Shape::Condition}, {"label", Shape::Expr}}};
/// @brief The keys of `forEach`.
inline constexpr std::array<KeySpec, 5> kForEachKeys = {{{"rows", Shape::Expr, {}, kRequired},
                                                         {"key", Shape::Expr, {}, kInner},
                                                         {"axis", Shape::Expr},
                                                         {"gap", Shape::Expr},
                                                         {"children", Shape::Nodes, {}, kInner}}};
/// @brief The keys of `table`.
inline constexpr std::array<KeySpec, 15> kTableKeys = {{{"rows", Shape::Expr},
                                                        {"key", Shape::Expr, {}, kInner},
                                                        {"model", Shape::Name},
                                                        {"action", Shape::Text},
                                                        {"body", Shape::Expr},
                                                        {"mode", Shape::Expr},
                                                        {"columns", Shape::PartsOrExpr, "column"},
                                                        {"selectionMode", Shape::Expr},
                                                        {"selection", Shape::Expr},
                                                        {"sort", Shape::Expr},
                                                        {"filters", Shape::Expr},
                                                        {"rowTone", Shape::Expr, {}, kInner},
                                                        {"highlightKey", Shape::Expr},
                                                        {"onActivate", Shape::Commands, {}, kInner},
                                                        {"empty", Shape::Nodes}}};
/// @brief The keys of a `table` column part.
inline constexpr std::array<KeySpec, 4> kColumnKeys = {
    {{"id", Shape::Name}, {"label", Shape::Expr}, {"width", Shape::Expr}, {"cell", Shape::Nodes, {}, kInner}}};
/// @brief The keys of `dateTimeInput`.
inline constexpr std::array<KeySpec, 6> kDateTimeKeys = {{{"value", Shape::Expr},
                                                          {"mode", Shape::Expr},
                                                          {"label", Shape::Expr},
                                                          {"showLabel", Shape::Flag},
                                                          {"onChange", Shape::Commands},
                                                          {"onCommit", Shape::Commands}}};
/// @brief The keys of `slider`.
inline constexpr std::array<KeySpec, 7> kSliderKeys = {{{"value", Shape::Expr},
                                                        {"min", Shape::Expr},
                                                        {"max", Shape::Expr},
                                                        {"step", Shape::Expr},
                                                        {"label", Shape::Expr},
                                                        {"onChange", Shape::Commands},
                                                        {"onCommit", Shape::Commands}}};
/// @brief The keys of `filePicker`.
inline constexpr std::array<KeySpec, 5> kFilePickerKeys = {{{"accept", Shape::Expr},
                                                            {"multiple", Shape::Expr},
                                                            {"mode", Shape::Expr},
                                                            {"label", Shape::Expr},
                                                            {"onPick", Shape::Commands}}};
/// @brief The keys of `banner`.
inline constexpr std::array<KeySpec, 5> kBannerKeys = {{{"tone", Shape::Expr},
                                                        {"text", Shape::Expr},
                                                        {"dismissible", Shape::Expr},
                                                        {"action", Shape::Part, "action"},
                                                        {"onDismiss", Shape::Commands}}};
/// @brief The keys of an `action` part.
inline constexpr std::array<KeySpec, 2> kActionKeys = {{{"label", Shape::Expr}, {"onClick", Shape::Commands}}};
/// @brief The keys of an `actionButton` part: a button a screen's head or an overlay's footer shows.
inline constexpr std::array<KeySpec, 7> kActionButtonKeys = {{{"id", Shape::Name, {}, kRequired},
                                                              {"label", Shape::Expr, {}, kRequired},
                                                              {"icon", Shape::Expr},
                                                              {"emphasis", Shape::Text},
                                                              {"enabled", Shape::Condition},
                                                              {"visible", Shape::Condition},
                                                              {"onClick", Shape::Commands}}};
/// @brief The keys of a `headBadge` part: a status badge of a screen's head.
inline constexpr std::array<KeySpec, 4> kHeadBadgeKeys = {
    {{"tone", Shape::Expr}, {"text", Shape::Expr}, {"icon", Shape::Expr}, {"visible", Shape::Condition}}};
/// @brief The keys of `badge`.
inline constexpr std::array<KeySpec, 3> kBadgeKeys = {
    {{"tone", Shape::Expr}, {"text", Shape::Expr}, {"icon", Shape::Expr}}};
/// @brief The keys of `progress`.
inline constexpr std::array<KeySpec, 2> kProgressKeys = {{{"value", Shape::Expr}, {"label", Shape::Expr}}};
/// @brief The keys of `steps`.
inline constexpr std::array<KeySpec, 2> kStepsKeys = {{{"items", Shape::Expr}, {"current", Shape::Expr}}};
/// @brief The keys of `keyValue`.
inline constexpr std::array<KeySpec, 1> kKeyValueKeys = {{{"items", Shape::Expr}}};
/// @brief The keys of `emptyState`.
inline constexpr std::array<KeySpec, 4> kEmptyStateKeys = {
    {{"title", Shape::Expr}, {"text", Shape::Expr}, {"icon", Shape::Expr}, {"action", Shape::Part, "action"}}};
/// @brief The keys of `splitter`.
inline constexpr std::array<KeySpec, 3> kSplitterKeys = {
    {{"orientation", Shape::Expr}, {"sizes", Shape::Expr}, {"children", Shape::Nodes}}};
/// @brief The keys of `collapsible`.
inline constexpr std::array<KeySpec, 5> kCollapsibleKeys = {{{"title", Shape::Expr},
                                                             {"open", Shape::Condition},
                                                             {"header", Shape::Nodes},
                                                             {"children", Shape::Nodes},
                                                             {"onToggle", Shape::Commands}}};
/// @brief The keys of `dropZone`.
inline constexpr std::array<KeySpec, 4> kDropZoneKeys = {
    {{"accept", Shape::Expr}, {"multiple", Shape::Expr}, {"children", Shape::Nodes}, {"onDrop", Shape::Commands}}};
/// @brief The keys of `customize`.
inline constexpr std::array<KeySpec, 1> kCustomizeKeys = {{{"point", Shape::Name, {}, kRequired}}};
/// @brief The keys of `boundary`.
inline constexpr std::array<KeySpec, 3> kBoundaryKeys = {
    {{"loading", Shape::Nodes}, {"error", Shape::Nodes, {}, kBoundaryError}, {"children", Shape::Nodes}}};
/// @brief The keys of `form`.
inline constexpr std::array<KeySpec, 15> kFormKeys = {{{"model", Shape::Name, {}, kRequired},
                                                       {"action", Shape::Text, {}, kRequired},
                                                       {"prefill", Shape::Expr},
                                                       {"key", Shape::Expr},
                                                       {"submit", Shape::Text},
                                                       {"showErrors", Shape::Text},
                                                       {"fields", Shape::ExprRecord, "fields"},
                                                       {"onSubmit", Shape::Commands},
                                                       {"onSuccess", Shape::Commands},
                                                       {"onError", Shape::Commands},
                                                       {"title", Shape::Expr},
                                                       {"submitLabel", Shape::Expr},
                                                       {"showStatus", Shape::Flag},
                                                       {"columns", Shape::Expr},
                                                       {"fieldLayout", Shape::Text}}};
/// @brief The keys of `collection` and `wizard`: the model and action, and the presentation keys that reach every
///        form the view draws.
inline constexpr std::array<KeySpec, 7> kCollectionKeys = {{{"model", Shape::Name, {}, kRequired},
                                                            {"action", Shape::Text, {}, kRequired},
                                                            {"title", Shape::Expr},
                                                            {"submitLabel", Shape::Expr},
                                                            {"showStatus", Shape::Flag},
                                                            {"columns", Shape::Expr},
                                                            {"fieldLayout", Shape::Text}}};
/// @brief The keys of `custom`.
inline constexpr std::array<KeySpec, 3> kCustomKeys = {
    {{"name", Shape::Text, {}, kRequired}, {"props", Shape::ExprRecord}, {"on", Shape::CommandRecord}}};
/// @brief The keys of an `a11y` part.
inline constexpr std::array<KeySpec, 2> kA11yKeys = {{{"name", Shape::Expr}, {"role", Shape::Text}}};
/// @brief The keys of a `drag` part.
inline constexpr std::array<KeySpec, 1> kDragKeys = {{{"payload", Shape::Expr, {}, kRequired}}};
/// @brief The keys of a `drop` part.
inline constexpr std::array<KeySpec, 2> kDropKeys = {{{"accepts", Shape::Expr}, {"onDrop", Shape::Commands}}};

/// @brief The view node kinds of `ui/1` and the part kinds inside them.
inline constexpr std::array<KindSpec, 48> kKinds = {{
    {"text", kTextKeys, kField},
    {"button", kButtonKeys},
    {"textInput", kTextInputKeys, kField | kWritable, kInputValues},
    {"checkbox", kCheckboxKeys, kField | kWritable, kCheckboxValues},
    {"select", kSelectKeys, kField | kWritable, kSelectValues},
    {"menu", kMenuKeys},
    {"column", kContainerKeys},
    {"row", kContainerKeys},
    {"grid", kGridKeys},
    {"spacer", {}},
    {"panel", kPanelKeys},
    {"scroll", kScrollKeys},
    {"switch", kSwitchKeys},
    {"tabs", kTabsKeys},
    {"dialog", kDialogKeys, kScope, kDialogValues},
    {"drawer", kDialogKeys, kScope, kDialogValues},
    {"busy", kBusyKeys},
    {"forEach", kForEachKeys, kScope | kRowScope},
    {"table", kTableKeys, kScope | kRowScope, kTableValues},
    {"dateTimeInput", kDateTimeKeys, kField | kWritable, kInputValues},
    {"slider", kSliderKeys, kField | kWritable, kInputValues},
    {"filePicker", kFilePickerKeys, kField},
    {"banner", kBannerKeys},
    {"badge", kBadgeKeys},
    {"progress", kProgressKeys},
    {"steps", kStepsKeys},
    {"keyValue", kKeyValueKeys},
    {"emptyState", kEmptyStateKeys},
    {"splitter", kSplitterKeys},
    {"collapsible", kCollapsibleKeys},
    {"dropZone", kDropZoneKeys},
    {"customize", kCustomizeKeys},
    {"boundary", kBoundaryKeys},
    {"form", kFormKeys, kOpenValues, kFormValues},
    {"collection", kCollectionKeys},
    {"wizard", kCollectionKeys},
    {"custom", kCustomKeys},
    {"menuItem", kMenuItemKeys, kPart},
    {"case", kCaseKeys, kPart | kScope},
    {"tab", kTabKeys, kPart},
    {"column", kColumnKeys, kPart},
    {"action", kActionKeys, kPart},
    {"a11y", kA11yKeys, kPart},
    {"drag", kDragKeys, kPart},
    {"drop", kDropKeys, kPart},
    {"actionButton", kActionButtonKeys, kPart},
    {"headBadge", kHeadBadgeKeys, kPart},
    {"fields", {}, kPart},
}};
// NOLINTEND(modernize-use-designated-initializers)

/// @brief Looks up a node kind or a part kind.
/// @param kind The kind.
/// @param part Whether a part kind is wanted.
/// @return The spec, or `nullptr`.
[[nodiscard]] inline KindSpec const* findKind(std::string_view kind, bool part) {
    for (KindSpec const& spec : kKinds) {
        if (spec.kind == kind && ((spec.flags & kPart) != 0) == part) {
            return &spec;
        }
    }
    return nullptr;
}

/// @brief Whether a list of names holds one.
/// @param names The list.
/// @param name  The candidate.
/// @return Whether @p name is in @p names.
[[nodiscard]] inline bool listed(std::span<std::string_view const> names, std::string_view name) {
    return std::ranges::find(names, name) != names.end();
}

/// @brief Refuses a must-understand key (`!name`) the client does not know; other unknown keys are ignored.
/// @param object The JSON object.
/// @param path   Its JSON Pointer.
inline void refuseMustUnderstand(Json::object_t const& object, std::string const& path) {
    for (auto const& [key, value] : object) {
        static_cast<void>(value);
        if (key.starts_with('!')) {
            refuse(pointerAppend(path, key), "the must-understand key `" + key + "` is not part of ui/1");
        }
    }
}

/// @brief Parses a key chord, and refuses one the workspace shell reserves.
/// @param chord The chord, such as `Ctrl+Shift+K`.
/// @param path  Its JSON Pointer.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): a chord's grammar, modifiers then one key name.
inline void checkChord(std::string const& chord, std::string const& path) {
    static constexpr std::array<std::string_view, 4> kModifiers = {"Ctrl", "Shift", "Alt", "Meta"};
    static constexpr std::array<std::string_view, 9> kNamedKeys = {"Enter", "Esc",   "Tab",   "Up",    "Down",
                                                                   "Left",  "Right", "Space", "Delete"};
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    for (std::size_t plus = chord.find('+'); plus != std::string::npos; plus = chord.find('+', start)) {
        parts.push_back(std::string_view{chord}.substr(start, plus - start));
        start = plus + 1;
    }
    std::string_view const key = std::string_view{chord}.substr(start);
    std::set<std::string_view> modifiers;
    for (std::string_view const modifier : parts) {
        if (!listed(kModifiers, modifier) || !modifiers.insert(modifier).second) {
            refuse(path, "`" + chord + "` is not a chord: modifiers are Ctrl, Shift, Alt and Meta, once each");
        }
    }
    bool const letterOrDigit =
        key.size() == 1 && ((key.front() >= 'A' && key.front() <= 'Z') || (key.front() >= 'a' && key.front() <= 'z') ||
                            (key.front() >= '0' && key.front() <= '9'));
    bool function = key.size() >= 2 && key.size() <= 3 && key.front() == 'F';
    int number = 0;
    for (char const chr : key.substr(function ? 1 : key.size())) {
        function = function && chr >= '0' && chr <= '9';
        number = (number * 10) + (chr - '0');
    }
    function = function && number >= 1 && number <= 24;
    if (!letterOrDigit && !function && !listed(kNamedKeys, key)) {
        refuse(path, "`" + chord + "` does not end in a key name");
    }
    bool const ctrlOnly = modifiers.size() == 1 && modifiers.contains("Ctrl");
    bool const ctrlShift = modifiers.size() == 2 && modifiers.contains("Ctrl") && modifiers.contains("Shift");
    bool const reserved = (key == "Tab" && (ctrlOnly || ctrlShift)) ||
                          (ctrlOnly && (key == "W" || key == "w" || key == "K" || key == "k"));
    if (reserved) {
        refuse(path, "`" + chord + "` is reserved for the workspace shell");
    }
}

// NOLINTBEGIN(misc-no-recursion): a document is a tree of nodes and command lists, and parsing descends it.
/// @brief Turns the JSON of a document into its model, refusing what does not parse.
class DocumentParser {
public:
    /// @brief Parses the whole document.
    /// @param json The document.
    /// @return The model, not yet checked against its names.
    [[nodiscard]] Document parse(Json const& json) {
        auto const* const object = std::get_if<Json::object_t>(&json.data);
        if (object == nullptr) {
            refuse("", "a document is a JSON object");
        }
        refuseMustUnderstand(*object, "");
        Document doc;
        doc.vocab = text(*object, "vocab", "", true);
        if (doc.vocab != kVocabulary) {
            refuse("/vocab", "the vocabulary `" + doc.vocab + "` is not supported; this client reads ui/1");
        }
        doc.id = text(*object, "id", "", true);
        if (doc.id.empty()) {
            refuse("/id", "a document has a non-empty id");
        }
        doc.title = optionalExpr(*object, "title", "");
        doc.identity = optionalExpr(*object, "identity", "");
        doc.dirty = optionalExpr(*object, "dirty", "");
        doc.subtitle = optionalExpr(*object, "subtitle", "");
        doc.status = topParts(*object, "status", "headBadge");
        doc.actions = topParts(*object, "actions", "actionButton");
        if (doc.id == "app") {
            // The head belongs to a screen's tab; the app document has no view and so no head.
            for (std::string_view const key : {"subtitle", "status", "actions"}) {
                if (member(*object, key) != nullptr) {
                    refuse(pointerAppend("", key),
                           "`" + std::string{key} + "` is a screen's; the app document has none");
                }
            }
        }
        doc.showErrors = showErrors(*object, "");
        if (Json const* const params = member(*object, "params")) {
            for (auto const& [name, type] : objectOf(*params, "/params", "`params` maps names to types")) {
                std::string const path = pointerAppend("/params", name);
                checkName(name, path);
                doc.params.emplace_back(name, typeOf(type, path));
            }
        }
        doc.scope = scope(*object, "");
        for (std::string_view const key : {"onMount", "onUnmount", "onClose", "onSave", "onBackendChange"}) {
            if (Json const* const commands = member(*object, key)) {
                doc.lifecycle.emplace(std::string{key}, commandList(*commands, pointerAppend("", key)));
            }
        }
        if (Json const* const keys = member(*object, "keys")) {
            doc.keys = keyBindings(*keys, "/keys");
        }
        if (Json const* const prefetch = member(*object, "prefetch")) {
            doc.prefetch = nameList(*prefetch, "/prefetch");
        }
        if (Json const* const components = member(*object, "components")) {
            for (auto const& [name, entry] : objectOf(*components, "/components", "`components` is an object")) {
                doc.components.emplace(name, component(name, entry, pointerAppend("/components", name)));
            }
        }
        if (Json const* const points = member(*object, "customize")) {
            for (auto const& [name, entry] : objectOf(*points, "/customize", "`customize` is an object")) {
                doc.customize.emplace(name, point(name, entry, pointerAppend("/customize", name), doc.id == "app"));
            }
        }
        if (Json const* const view = member(*object, "view")) {
            doc.view = node(*view, "/view");
        } else if (doc.id != "app") {
            refuse("", "a screen has a `view`");
        }
        return doc;
    }

private:
    [[nodiscard]] static Json const* member(Json::object_t const& object, std::string_view key) {
        auto const* const found = object.find(key);
        return found == object.end() ? nullptr : &found->second;
    }

    [[nodiscard]] static Json::object_t const& objectOf(Json const& json, std::string const& path, char const* what) {
        auto const* const object = std::get_if<Json::object_t>(&json.data);
        if (object == nullptr) {
            refuse(path, what);
        }
        return *object;
    }

    static void checkName(std::string const& name, std::string const& path) {
        if (!isName(name)) {
            refuse(path, "`" + name + "` is not a name: names match [A-Za-z_][A-Za-z0-9_]*");
        }
    }

    [[nodiscard]] static std::string text(Json::object_t const& object, std::string_view key, std::string const& path,
                                          bool required) {
        Json const* const value = member(object, key);
        std::string const keyPath = pointerAppend(path, key);
        if (value == nullptr) {
            if (required) {
                refuse(path, "`" + std::string{key} + "` is required");
            }
            return {};
        }
        auto const* const string = std::get_if<std::string>(&value->data);
        if (string == nullptr) {
            refuse(keyPath, "`" + std::string{key} + "` is a string");
        }
        return *string;
    }

    [[nodiscard]] static std::string choiceOf(Json::object_t const& object, std::string_view key,
                                              std::string const& path, std::span<std::string_view const> choices) {
        std::string value = text(object, key, path, false);
        if (!value.empty() && !listed(choices, value)) {
            refuse(pointerAppend(path, key), "`" + value + "` is not one of the values of `" + std::string{key} + "`");
        }
        return value;
    }

    [[nodiscard]] static std::string showErrors(Json::object_t const& object, std::string const& path) {
        static constexpr std::array<std::string_view, 3> kPolicies = {"touched", "submit", "always"};
        return choiceOf(object, "showErrors", path, kPolicies);
    }

    [[nodiscard]] static std::int64_t integer(Json::object_t const& object, std::string_view key,
                                              std::string const& path) {
        Json const* const value = member(object, key);
        if (value == nullptr) {
            return 0;
        }
        auto const number = detail::jsonInt(*value);
        if (!number || *number < 0) {
            refuse(pointerAppend(path, key), "`" + std::string{key} + "` is a non-negative integer");
        }
        return *number;
    }

    [[nodiscard]] static bool flag(Json::object_t const& object, std::string_view key, std::string const& path) {
        Json const* const value = member(object, key);
        if (value == nullptr) {
            return false;
        }
        bool const* const set = std::get_if<bool>(&value->data);
        if (set == nullptr) {
            refuse(pointerAppend(path, key), "`" + std::string{key} + "` is a bool");
        }
        return *set;
    }

    [[nodiscard]] static Type typeOf(Json const& json, std::string const& path) {
        auto const* const spelled = std::get_if<std::string>(&json.data);
        if (spelled == nullptr) {
            refuse(path, "a type is written as a string, such as `int?`");
        }
        auto type = parseType(*spelled);
        if (!type) {
            refuse(path, type.error());
        }
        return *type;
    }

    [[nodiscard]] static std::vector<std::string> nameList(Json const& json, std::string const& path) {
        auto const* const array = std::get_if<Json::array_t>(&json.data);
        if (array == nullptr) {
            refuse(path, "a list of names is an array of strings");
        }
        std::vector<std::string> names;
        for (std::size_t index = 0; index < array->size(); ++index) {
            auto const* const name = std::get_if<std::string>(&array->at(index).data);
            if (name == nullptr || name->empty()) {
                refuse(pointerAppend(path, index), "a list of names is an array of strings");
            }
            names.push_back(*name);
        }
        return names;
    }

    [[nodiscard]] static ExprPtr expr(Json const& json, std::string const& path) {
        auto parsed = parseExpression(json, path);
        if (!parsed) {
            throw DocumentRefused{std::move(parsed.error())};
        }
        return std::move(*parsed);
    }

    [[nodiscard]] static ExprPtr optionalExpr(Json::object_t const& object, std::string_view key,
                                              std::string const& path) {
        Json const* const value = member(object, key);
        return value == nullptr ? nullptr : expr(*value, pointerAppend(path, key));
    }

    // A top-level list of parts: the screen head's `status` and `actions`.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the document key, then its part kind, at every call.
    [[nodiscard]] std::vector<DocNodePtr> topParts(Json::object_t const& object, std::string_view key,
                                                   std::string_view kind) {
        std::vector<DocNodePtr> parts;
        Json const* const value = member(object, key);
        if (value == nullptr) {
            return parts;
        }
        std::string const path = pointerAppend("", key);
        auto const* const array = std::get_if<Json::array_t>(&value->data);
        if (array == nullptr) {
            refuse(path, "`" + std::string{key} + "` is an array");
        }
        for (std::size_t index = 0; index < array->size(); ++index) {
            parts.push_back(part(array->at(index), kind, pointerAppend(path, index)));
        }
        return parts;
    }

    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one branch per kind of declaration.
    [[nodiscard]] ScopeDecl scope(Json::object_t const& object, std::string const& path) {
        ScopeDecl decl;
        if (Json const* const models = member(object, "models")) {
            std::string const where = pointerAppend(path, "models");
            for (auto const& [name, entry] : objectOf(*models, where, "`models` maps aliases to models")) {
                decl.models.push_back(model(name, entry, pointerAppend(where, name)));
            }
        }
        if (Json const* const state = member(object, "state")) {
            std::string const where = pointerAppend(path, "state");
            for (auto const& [name, entry] : objectOf(*state, where, "`state` maps names to declarations")) {
                std::string const entryPath = pointerAppend(where, name);
                checkName(name, entryPath);
                auto const& fields = objectOf(entry, entryPath, R"(a state declaration is {"init": e, "type": T})");
                refuseMustUnderstand(fields, entryPath);
                StateDecl stateDecl{.name = name, .path = entryPath, .type = Type::any(), .init = nullptr};
                if (Json const* const type = member(fields, "type")) {
                    stateDecl.type = typeOf(*type, pointerAppend(entryPath, "type"));
                }
                stateDecl.init = optionalExpr(fields, "init", entryPath);
                decl.state.push_back(std::move(stateDecl));
            }
        }
        if (Json const* const lets = member(object, "let")) {
            std::string const where = pointerAppend(path, "let");
            for (auto const& [name, entry] : objectOf(*lets, where, "`let` maps names to expressions")) {
                std::string const entryPath = pointerAppend(where, name);
                checkName(name, entryPath);
                decl.lets.push_back(LetDecl{.name = name, .path = entryPath, .value = expr(entry, entryPath)});
            }
        }
        if (Json const* const queries = member(object, "queries")) {
            std::string const where = pointerAppend(path, "queries");
            for (auto const& [name, entry] : objectOf(*queries, where, "`queries` maps names to queries")) {
                decl.queries.push_back(query(name, entry, pointerAppend(where, name)));
            }
        }
        if (Json const* const mutations = member(object, "mutations")) {
            std::string const where = pointerAppend(path, "mutations");
            for (auto const& [name, entry] : objectOf(*mutations, where, "`mutations` maps names to mutations")) {
                decl.mutations.push_back(mutation(name, entry, pointerAppend(where, name)));
            }
        }
        if (Json const* const watches = member(object, "watch")) {
            std::string const where = pointerAppend(path, "watch");
            auto const* const array = std::get_if<Json::array_t>(&watches->data);
            if (array == nullptr) {
                refuse(where, "`watch` is an array");
            }
            for (std::size_t index = 0; index < array->size(); ++index) {
                decl.watches.push_back(watch(array->at(index), pointerAppend(where, index)));
            }
        }
        if (Json const* const checks = member(object, "checks")) {
            std::string const where = pointerAppend(path, "checks");
            auto const* const array = std::get_if<Json::array_t>(&checks->data);
            if (array == nullptr) {
                refuse(where, "`checks` is an array");
            }
            for (std::size_t index = 0; index < array->size(); ++index) {
                decl.checks.push_back(check(array->at(index), pointerAppend(where, index)));
            }
        }
        return decl;
    }

    [[nodiscard]] static ModelDecl model(std::string const& name, Json const& json, std::string const& path) {
        checkName(name, path);
        auto const& fields = objectOf(json, path, R"(a model alias is {"type": ..., "instance": ...})");
        refuseMustUnderstand(fields, path);
        ModelDecl decl{.name = name,
                       .path = path,
                       .type = text(fields, "type", path, true),
                       .instance = {},
                       .key = nullptr,
                       .borrowedFrom = {}};
        Json const* const instance = member(fields, "instance");
        if (instance == nullptr) {
            return decl;
        }
        std::string const where = pointerAppend(path, "instance");
        auto const* const word = std::get_if<std::string>(&instance->data);
        if (word != nullptr && (*word == "private" || *word == "shared")) {
            decl.instance = *word == "private" ? InstanceMode::Private : InstanceMode::Shared;
            return decl;
        }
        if (auto const* const object = std::get_if<Json::object_t>(&instance->data)) {
            if (Json const* const from = member(*object, "from")) {
                auto const* const alias = std::get_if<std::string>(&from->data);
                if (alias == nullptr || !isName(*alias) || object->size() != 1) {
                    refuse(where, R"(a borrowed instance is {"from": alias})");
                }
                decl.instance = InstanceMode::Borrowed;
                decl.borrowedFrom = *alias;
                return decl;
            }
        }
        decl.instance = InstanceMode::Keyed;
        decl.key = expr(*instance, where);
        return decl;
    }

    /// @brief Refuses @p key of @p fields: a refresh key, which a document does not declare.
    /// @param fields The declaration's fields.
    /// @param path   The declaration's JSON Pointer.
    /// @param key    The refresh key: `invalidates`, `patch` or `refreshOn`.
    static void refuseRefreshKey(Json::object_t const& fields, std::string const& path, std::string_view key) {
        if (member(fields, key) != nullptr) {
            refuse(pointerAppend(path, key),
                   std::format("a document does not declare `{}`: an action's effects say what it changes, and every "
                               "query holding a changed record refreshes; a refresh no action declares is a `refetch` "
                               "command",
                               key));
        }
    }

    [[nodiscard]] static QueryDecl query(std::string const& name, Json const& json, std::string const& path) {
        checkName(name, path);
        auto const& fields = objectOf(json, path, "a query is an object");
        refuseMustUnderstand(fields, path);
        QueryDecl decl;
        decl.name = name;
        decl.path = path;
        decl.model = text(fields, "model", path, true);
        std::string const action = text(fields, "action", path, false);
        std::string const evaluate = text(fields, "evaluate", path, false);
        if (action.empty() == evaluate.empty()) {
            refuse(path, "a query names exactly one of `action` and `evaluate`");
        }
        decl.action = action.empty() ? evaluate : action;
        decl.evaluate = !evaluate.empty();
        decl.body = optionalExpr(fields, "body", path);
        decl.when = optionalExpr(fields, "when", path);
        decl.debounce = integer(fields, "debounce", path);
        decl.refreshEvery = integer(fields, "refreshEvery", path);
        decl.staleTime = integer(fields, "staleTime", path);
        refuseRefreshKey(fields, path, "refreshOn");
        return decl;
    }

    /// @brief Parses an `optimistic` write: `{"query": q, "at"?: m, "key": e, "with": e}`.
    /// @param json The write's JSON.
    /// @param path Its JSON Pointer.
    /// @return The write.
    [[nodiscard]] static OptimisticDecl optimistic(Json const& json, std::string const& path) {
        static constexpr char const* shape = R"(an optimistic write is {"query": q, "at"?: m, "key": e, "with": e})";
        auto const& fields = objectOf(json, path, shape);
        refuseMustUnderstand(fields, path);
        Json const* const key = member(fields, "key");
        Json const* const with = member(fields, "with");
        if (key == nullptr || with == nullptr) {
            refuse(path, shape);
        }
        std::string listMember = text(fields, "at", path, false);
        if (member(fields, "at") != nullptr && listMember.empty()) {
            refuse(pointerAppend(path, "at"), "`at` names the member of the query's record that holds the list");
        }
        return OptimisticDecl{.query = text(fields, "query", path, true),
                              .at = std::move(listMember),
                              .key = expr(*key, pointerAppend(path, "key")),
                              .with = expr(*with, pointerAppend(path, "with"))};
    }

    [[nodiscard]] MutationDecl mutation(std::string const& name, Json const& json, std::string const& path) {
        checkName(name, path);
        auto const& fields = objectOf(json, path, "a mutation is an object");
        refuseMustUnderstand(fields, path);
        MutationDecl decl;
        decl.name = name;
        decl.path = path;
        decl.model = text(fields, "model", path, true);
        decl.action = text(fields, "action", path, true);
        decl.body = optionalExpr(fields, "body", path);
        static constexpr std::array<std::string_view, 4> kModes = {"exclusive", "serial", "latest", "parallel"};
        std::string const mode = choiceOf(fields, "concurrency", path, kModes);
        if (mode == "serial") {
            decl.concurrency = Concurrency::Serial;
        } else if (mode == "latest") {
            decl.concurrency = Concurrency::Latest;
        } else if (mode == "parallel") {
            decl.concurrency = Concurrency::Parallel;
        }
        decl.serialKey = optionalExpr(fields, "serialKey", path);
        if (decl.serialKey && decl.concurrency != Concurrency::Serial) {
            refuse(pointerAppend(path, "serialKey"), "`serialKey` applies to `serial` concurrency only");
        }
        refuseRefreshKey(fields, path, "invalidates");
        refuseRefreshKey(fields, path, "patch");
        if (Json const* const value = member(fields, "optimistic")) {
            decl.optimistic = optimistic(*value, pointerAppend(path, "optimistic"));
        }
        if (Json const* const commands = member(fields, "onSuccess")) {
            decl.onSuccess = commandList(*commands, pointerAppend(path, "onSuccess"));
        }
        if (Json const* const commands = member(fields, "onError")) {
            decl.onError = commandList(*commands, pointerAppend(path, "onError"));
        }
        static constexpr std::array<std::string_view, 1> kOffline = {"queue"};
        decl.offlineQueue = !choiceOf(fields, "offline", path, kOffline).empty();
        return decl;
    }

    [[nodiscard]] WatchDecl watch(Json const& json, std::string const& path) {
        auto const& fields = objectOf(json, path, R"(a watch is {"on": e, "do": [...]})");
        refuseMustUnderstand(fields, path);
        Json const* const watched = member(fields, "on");
        Json const* const body = member(fields, "do");
        if (watched == nullptr || body == nullptr) {
            refuse(path, R"(a watch is {"on": e, "do": [...]})");
        }
        static constexpr std::array<std::string_view, 2> kSources = {"any", "user"};
        return WatchDecl{
            .path = path,
            .on = expr(*watched, pointerAppend(path, "on")),
            .source = choiceOf(fields, "source", path, kSources) == "user" ? WatchSource::User : WatchSource::Any,
            .body = commandList(*body, pointerAppend(path, "do"))};
    }

    [[nodiscard]] static CheckDecl check(Json const& json, std::string const& path) {
        auto const& fields = objectOf(json, path, R"(a check is {"id": ..., "assert": e, ...})");
        refuseMustUnderstand(fields, path);
        Json const* const assertion = member(fields, "assert");
        if (assertion == nullptr) {
            refuse(path, "a check has an `assert`");
        }
        CheckDecl decl;
        decl.id = text(fields, "id", path, false);
        decl.path = path;
        decl.assertion = expr(*assertion, pointerAppend(path, "assert"));
        decl.message = optionalExpr(fields, "message", path);
        if (Json const* const targets = member(fields, "targets")) {
            decl.targets = nameList(*targets, pointerAppend(path, "targets"));
        }
        decl.blocking = flag(fields, "blocking", path);
        return decl;
    }

    struct CommandSpec {
        std::string_view key;
        CommandKind kind;
        std::array<std::string_view, 6> companions;
    };

    // NOLINTBEGIN(modernize-use-designated-initializers): one row per command reads as a table.
    static constexpr std::array<CommandSpec, 23> kCommands = {{
        {"set", CommandKind::Set, {"value"}},
        {"reset", CommandKind::Reset, {}},
        {"run", CommandKind::Run, {"body", "as", "each"}},
        {"refetch", CommandKind::Refetch, {}},
        {"open", CommandKind::Open, {}},
        {"close", CommandKind::Close, {}},
        {"navigate", CommandKind::Navigate, {"params", "as", "onResult", "onCancel"}},
        {"return", CommandKind::Return, {}},
        {"focus", CommandKind::Focus, {}},
        {"host", CommandKind::Host, {"args"}},
        {"switchBackend", CommandKind::SwitchBackend, {}},
        {"resetPrefs", CommandKind::ResetPrefs, {}},
        {"setLocale", CommandKind::SetLocale, {}},
        {"notify", CommandKind::Notify, {"role"}},
        {"seq", CommandKind::Seq, {}},
        {"all", CommandKind::All, {}},
        {"if", CommandKind::If, {}},
        {"cancelClose", CommandKind::CancelClose, {}},
        {"retry", CommandKind::Retry, {}},
        {"signIn", CommandKind::SignIn, {}},
        {"signOut", CommandKind::SignOut, {}},
        {"upload", CommandKind::Upload, {"to", "as", "id", "into", "nameAs", "typeAs"}},
        {"download", CommandKind::Download, {"save", "name", "to", "as", "from", "type"}},
    }};
    // NOLINTEND(modernize-use-designated-initializers)

    [[nodiscard]] Commands commandList(Json const& json, std::string const& path) {
        Commands commands;
        if (auto const* const array = std::get_if<Json::array_t>(&json.data)) {
            for (std::size_t index = 0; index < array->size(); ++index) {
                commands.push_back(command(array->at(index), pointerAppend(path, index)));
            }
            return commands;
        }
        commands.push_back(command(json, path));
        return commands;
    }

    [[nodiscard]] static std::string nameOf(Json const& json, std::string const& path, char const* what,
                                            bool dotted = false) {
        auto const* const name = std::get_if<std::string>(&json.data);
        bool valid = name != nullptr && !name->empty();
        if (valid && dotted) {
            auto const segments = splitPath(*name);
            valid = segments.has_value() && isName(segments->front());
        } else if (valid) {
            valid = isName(*name);
        }
        if (!valid) {
            refuse(path, what);
        }
        return *name;
    }

    [[nodiscard]] static std::string nonEmpty(Json const& json, std::string const& path, char const* what) {
        auto const* const name = std::get_if<std::string>(&json.data);
        if (name == nullptr || name->empty()) {
            refuse(path, what);
        }
        return *name;
    }

    static void requireTrue(Json const& json, std::string const& path, std::string_view key) {
        bool const* const value = std::get_if<bool>(&json.data);
        if (value == nullptr || !*value) {
            refuse(path, "`" + std::string{key} + "` is written `true`");
        }
    }

    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per command of ui/1.
    [[nodiscard]] Command command(Json const& json, std::string const& path) {
        auto const& fields = objectOf(json, path, R"(a command is an object such as {"set": ..., "value": ...})");
        CommandSpec const* chosen = nullptr;
        for (auto const& [key, value] : fields) {
            static_cast<void>(value);
            for (CommandSpec const& spec : kCommands) {
                if (spec.key == key) {
                    if (chosen != nullptr) {
                        refuse(path,
                               "a command has one kind; found `" + std::string{chosen->key} + "` and `" + key + "`");
                    }
                    chosen = &spec;
                }
            }
        }
        if (chosen == nullptr) {
            refuse(path, "not a command of ui/1");
        }
        refuseMustUnderstand(fields, path);
        Command cmd;
        cmd.kind = chosen->kind;
        cmd.path = path;
        Json const& primary = *member(fields, chosen->key);
        std::string const where = pointerAppend(path, chosen->key);
        switch (chosen->kind) {
            case CommandKind::Set: {
                cmd.target = nameOf(primary, where, "`set` names state or an element as `name.path`", true);
                Json const* const value = member(fields, "value");
                if (value == nullptr) {
                    refuse(path, "`set` needs a `value`");
                }
                cmd.value = expr(*value, pointerAppend(path, "value"));
                break;
            }
            case CommandKind::Run:
                cmd.target = nameOf(primary, where, "`run` names a mutation");
                cmd.value = optionalExpr(fields, "body", path);
                cmd.extra = optionalExpr(fields, "each", path);
                if (Json const* const binding = member(fields, "as")) {
                    cmd.binding = nameOf(*binding, pointerAppend(path, "as"), "`as` is a name");
                }
                break;
            case CommandKind::Reset:
            case CommandKind::Refetch:
            case CommandKind::Open:
            case CommandKind::Focus:
                cmd.target = nameOf(primary, where, "the command names a declaration or an element");
                break;
            case CommandKind::Close:
                cmd.target = nameOf(primary, where, "`close` names a dialog, or `self`");
                break;
            case CommandKind::Navigate:
                navigate(cmd, fields, primary, path);
                break;
            case CommandKind::Return:
                cmd.value = expr(primary, where);
                break;
            case CommandKind::Host:
                cmd.target = nonEmpty(primary, where, "`host` names a registered command");
                cmd.value = optionalExpr(fields, "args", path);
                break;
            case CommandKind::SwitchBackend:
            case CommandKind::SetLocale:
                cmd.target = nonEmpty(primary, where, "the command takes a non-empty string");
                break;
            case CommandKind::ResetPrefs:
                cmd.target = nameOf(primary, where, "`resetPrefs` names a customization point, or `all`");
                break;
            case CommandKind::Notify: {
                cmd.value = expr(primary, where);
                static constexpr std::array<std::string_view, 4> kRoles = {"info", "success", "warning", "error"};
                cmd.option = choiceOf(fields, "role", path, kRoles);
                break;
            }
            case CommandKind::Seq:
            case CommandKind::All: {
                if (!std::holds_alternative<Json::array_t>(primary.data)) {
                    refuse(where, "`" + std::string{chosen->key} + "` takes an array of commands");
                }
                cmd.commands = commandList(primary, where);
                break;
            }
            case CommandKind::If: {
                auto const* const array = std::get_if<Json::array_t>(&primary.data);
                if (array == nullptr || array->size() < 2 || array->size() > 3) {
                    refuse(where, "`if` takes [condition, [then...], [else...]]");
                }
                cmd.value = expr(array->at(0), pointerAppend(where, std::size_t{0}));
                cmd.commands = commandList(array->at(1), pointerAppend(where, 1));
                if (array->size() == 3) {
                    cmd.alternative = commandList(array->at(2), pointerAppend(where, 2));
                }
                break;
            }
            case CommandKind::CancelClose:
            case CommandKind::Retry:
            case CommandKind::SignOut:
                requireTrue(primary, where, chosen->key);
                break;
            case CommandKind::SignIn: {
                auto const* const object = std::get_if<Json::object_t>(&primary.data);
                Json const* const from = object == nullptr ? nullptr : member(*object, "from");
                if (from == nullptr) {
                    refuse(where, R"(`signIn` is {"from": mutation})");
                }
                cmd.target = nameOf(*from, pointerAppend(where, "from"), "`signIn` names its mutation");
                break;
            }
            case CommandKind::Upload: {
                cmd.value = expr(primary, where);
                Json const* const destination = member(fields, "to");
                if (destination == nullptr) {
                    refuse(path, "`upload` names its mutation with `to`");
                }
                cmd.target = nameOf(*destination, pointerAppend(path, "to"), "`to` names a mutation");
                if (Json const* const binding = member(fields, "as")) {
                    cmd.binding = nameOf(*binding, pointerAppend(path, "as"), "`as` is a name");
                }
                if (Json const* const transfer = member(fields, "id")) {
                    cmd.transfer = nameOf(*transfer, pointerAppend(path, "id"), "`id` is a name");
                }
                static constexpr std::array<std::string_view, 2> kInto = {"ref", "bytes"};
                cmd.bytes = choiceOf(fields, "into", path, kInto) == "bytes";
                for (auto const& [key, slot] : {std::pair{"nameAs", &cmd.nameAs}, std::pair{"typeAs", &cmd.typeAs}}) {
                    if (Json const* const name = member(fields, key)) {
                        if (!cmd.bytes) {
                            refuse(pointerAppend(path, key),
                                   "`" + std::string{key} +
                                       R"(` names a body member for inline bytes; give `"into": "bytes"`)");
                        }
                        *slot = nameOf(*name, pointerAppend(path, key), "a body member is a name");
                    }
                }
                break;
            }
            case CommandKind::Download:
            default:
                download(cmd, fields, primary, path);
                break;
        }
        return cmd;
    }

    void navigate(Command& cmd, Json::object_t const& fields, Json const& primary, std::string const& path) {
        std::string const where = pointerAppend(path, "navigate");
        if (auto const* const route = std::get_if<Json::object_t>(&primary.data)) {
            Json const* const value = member(*route, "route");
            if (value == nullptr || route->size() != 1) {
                refuse(where, R"(`navigate` names a screen, or is {"route": e})");
            }
            cmd.extra = expr(*value, pointerAppend(where, "route"));
        } else {
            cmd.target = nonEmpty(primary, where, R"(`navigate` names a screen, or is {"route": e})");
        }
        cmd.value = optionalExpr(fields, "params", path);
        static constexpr std::array<std::string_view, 4> kModes = {"tab", "dialog", "drawer", "window"};
        cmd.option = choiceOf(fields, "as", path, kModes);
        if (Json const* const result = member(fields, "onResult")) {
            cmd.commands = commandList(*result, pointerAppend(path, "onResult"));
        }
        if (Json const* const cancel = member(fields, "onCancel")) {
            cmd.alternative = commandList(*cancel, pointerAppend(path, "onCancel"));
        }
    }

    static void download(Command& cmd, Json::object_t const& fields, Json const& primary, std::string const& path) {
        cmd.value = expr(primary, pointerAppend(path, "download"));
        static constexpr std::array<std::string_view, 2> kFrom = {"ref", "bytes"};
        cmd.bytes = choiceOf(fields, "from", path, kFrom) == "bytes";
        if (cmd.bytes) {
            // Inline bytes carry no name or type of their own, so the document gives the name.
            if (member(fields, "name") == nullptr) {
                refuse(path, "a download from bytes names its file with `name`");
            }
            cmd.extra = optionalExpr(fields, "name", path);
            cmd.mediaType = optionalExpr(fields, "type", path);
        } else if (member(fields, "type") != nullptr) {
            refuse(pointerAppend(path, "type"), R"(`type` is the media type of inline bytes; give `"from": "bytes"`)");
        }
        std::string const save = text(fields, "save", path, false);
        std::string const destination = text(fields, "to", path, false);
        if (save == "dialog" && destination.empty()) {
            cmd.option = "dialog";
            cmd.extra = optionalExpr(fields, "name", path);
            return;
        }
        if (destination == "preview" && save.empty()) {
            Json const* const binding = member(fields, "as");
            if (binding == nullptr) {
                refuse(path, "a download to preview binds a state with `as`");
            }
            cmd.option = "preview";
            cmd.target = nameOf(*binding, pointerAppend(path, "as"), "`as` names a state");
            return;
        }
        refuse(path, R"(`download` takes `"save": "dialog"` or `"to": "preview"`)");
    }

    [[nodiscard]] std::map<std::string, Commands, std::less<>> keyBindings(Json const& json, std::string const& path) {
        std::map<std::string, Commands, std::less<>> keys;
        for (auto const& [chord, commands] : objectOf(json, path, "`keys` maps chords to commands")) {
            std::string const where = pointerAppend(path, chord);
            checkChord(chord, where);
            keys.emplace(chord, commandList(commands, where));
        }
        return keys;
    }

    // NOLINTNEXTLINE(readability-function-cognitive-complexity): a component's rules, one after another.
    [[nodiscard]] Component component(std::string const& name, Json const& json, std::string const& path) {
        checkName(name, path);
        auto const& fields = objectOf(json, path, "a component is an object");
        refuseMustUnderstand(fields, path);
        Component decl;
        decl.name = name;
        decl.path = path;
        decl.qml = text(fields, "qml", path, false);
        decl.host = text(fields, "host", path, false);
        if (decl.qml.empty() == decl.host.empty()) {
            refuse(path, "a component names exactly one of `qml` and `host`");
        }
        if (!decl.qml.empty()) {
            bool hash = decl.qml.size() == 71 && decl.qml.starts_with("sha256:");
            for (char const chr : std::string_view{decl.qml}.substr(std::min<std::size_t>(7, decl.qml.size()))) {
                hash = hash && ((chr >= '0' && chr <= '9') || (chr >= 'a' && chr <= 'f'));
            }
            if (!hash) {
                refuse(pointerAppend(path, "qml"), "`qml` is a content hash `sha256:<64 hex digits>`");
            }
        } else {
            Json const* const version = member(fields, "version");
            auto const number = version == nullptr ? std::nullopt : detail::jsonInt(*version);
            if (!number) {
                refuse(path, "a host component has an integer `version`");
            }
            decl.version = *number;
        }
        static constexpr std::array<std::string_view, 6> kPropTypes = {"string", "bool",   "int",
                                                                       "exact",  "number", "json"};
        if (Json const* const props = member(fields, "props")) {
            std::string const where = pointerAppend(path, "props");
            for (auto const& [prop, type] : objectOf(*props, where, "`props` maps names to prop types")) {
                auto const* const spelled = std::get_if<std::string>(&type.data);
                if (spelled == nullptr || !listed(kPropTypes, *spelled)) {
                    refuse(pointerAppend(where, prop), "a prop type is string, bool, int, exact, number or json");
                }
                decl.props.emplace(prop, *spelled);
            }
        }
        if (Json const* const events = member(fields, "events")) {
            std::string const where = pointerAppend(path, "events");
            for (auto const& [event, type] : objectOf(*events, where, "`events` maps names to payload types")) {
                decl.events.emplace(event, typeOf(type, pointerAppend(where, event)));
            }
        }
        Json const* const fallback = member(fields, "fallback");
        if (fallback == nullptr) {
            refuse(path, "a component names a `fallback` of built-in nodes");
        }
        decl.fallback = node(*fallback, pointerAppend(path, "fallback"));
        return decl;
    }

    [[nodiscard]] static CustomizePoint point(std::string const& name, Json const& json, std::string const& path,
                                              bool appDocument) {
        checkName(name, path);
        auto const& fields = objectOf(json, path, "a customization point is an object");
        refuseMustUnderstand(fields, path);
        static constexpr std::array<std::string_view, 7> kPointKinds = {"columns", "choice", "value",  "layout",
                                                                        "saved",   "pins",   "arrange"};
        CustomizePoint decl;
        decl.name = name;
        decl.path = path;
        decl.kind = text(fields, "kind", path, true);
        if (!listed(kPointKinds, decl.kind)) {
            refuse(pointerAppend(path, "kind"), "`" + decl.kind + "` is not a kind of customization point");
        }
        if (decl.kind == "pins" && !appDocument) {
            refuse(pointerAppend(path, "kind"), "a `pins` point belongs to the app document");
        }
        // The container an `arrange` point orders is a screen's, and the app document has no view.
        if (decl.kind == "arrange" && appDocument) {
            refuse(pointerAppend(path, "kind"), "an `arrange` point belongs to the screen whose container it orders");
        }
        decl.of = text(fields, "of", path, decl.kind == "columns" || decl.kind == "arrange");
        Json const* const fallback = member(fields, "default");
        if (decl.kind == "choice") {
            Json const* const options = member(fields, "options");
            if (options == nullptr) {
                refuse(path, "a `choice` point lists its `options`");
            }
            decl.options = nameList(*options, pointerAppend(path, "options"));
            if (fallback != nullptr) {
                auto const* const chosen = std::get_if<std::string>(&fallback->data);
                if (chosen == nullptr || std::ranges::find(decl.options, *chosen) == decl.options.end()) {
                    refuse(pointerAppend(path, "default"), "the default of a `choice` is one of its options");
                }
                decl.defaultValue = *chosen;
            }
            decl.type = Type::of(TypeKind::String);
        } else if (decl.kind == "value") {
            Json const* const type = member(fields, "type");
            if (type == nullptr) {
                refuse(path, "a `value` point declares its `type`");
            }
            decl.type = typeOf(*type, pointerAppend(path, "type"));
            if (fallback != nullptr) {
                auto value = valueFromJson(*fallback, decl.type);
                if (!value) {
                    refuse(pointerAppend(path, "default"),
                           "the default does not have the point's type: " + value.error());
                }
                decl.defaultValue = std::move(*value);
            }
        }
        return decl;
    }

    [[nodiscard]] DocNodePtr node(Json const& json, std::string const& path) {
        auto const* const object = std::get_if<Json::object_t>(&json.data);
        if (object == nullptr) {
            refuse(path, "a node is an object with a `kind`");
        }
        std::string const kind = text(*object, "kind", path, true);
        KindSpec const* const spec = findKind(kind, false);
        if (spec == nullptr) {
            refuse(pointerAppend(path, "kind"), "the node kind `" + kind + "` is not in ui/1");
        }
        auto made = std::make_shared<DocNode>();
        made->kind = kind;
        made->path = path;
        refuseMustUnderstand(*object, path);
        fill(*made, *object, *spec, path);
        return made;
    }

    void fill(DocNode& made, Json::object_t const& object, KindSpec const& spec, std::string const& path) {
        bool const isPart = (spec.flags & kPart) != 0;
        auto apply = [&](KeySpec const& key) {
            Json const* const value = member(object, key.key);
            if (value == nullptr) {
                if ((key.flags & kRequired) != 0) {
                    refuse(path, "`" + std::string{key.key} + "` is required on `" + std::string{spec.kind} + "`");
                }
                return;
            }
            entry(made, key, *value, pointerAppend(path, key.key));
        };
        if (!isPart) {
            for (KeySpec const& key : kCommonKeys) {
                apply(key);
            }
            if ((spec.flags & kField) != 0) {
                for (KeySpec const& key : kFieldKeys) {
                    apply(key);
                }
            }
        }
        for (KeySpec const& key : spec.keys) {
            apply(key);
        }
        if ((spec.flags & kScope) != 0) {
            if (Json const* const declarations = member(object, "scope")) {
                std::string const where = pointerAppend(path, "scope");
                auto const& fields = objectOf(*declarations, where, "`scope` holds a scope's declarations");
                refuseMustUnderstand(fields, where);
                made.scope = scope(fields, where);
            } else {
                made.scope = ScopeDecl{};
            }
        }
        if (made.kind == "fields") {
            for (auto const& [field, overlay] : object) {
                made.parts["fields"].push_back(fieldOverlay(field, overlay, pointerAppend(path, field)));
            }
        }
    }

    // One field overlay of a form's `fields`: expressions by key, and the host `component` that draws the field with
    // its `props`.
    [[nodiscard]] static DocNodePtr fieldOverlay(std::string const& field, Json const& overlay,
                                                 std::string const& where) {
        auto const& settings = objectOf(overlay, where, "a field overlay is an object");
        auto part = std::make_shared<DocNode>();
        part->kind = "field";
        part->id = field;
        part->path = where;
        for (auto const& [key, value] : settings) {
            std::string const keyPath = pointerAppend(where, key);
            if (key == "component") {
                // The host component that draws the field, and the props it is given beside the field's.
                auto const* const name = std::get_if<std::string>(&value.data);
                if (name == nullptr || !isName(*name)) {
                    refuse(keyPath, "`component` is a name");
                }
                part->names.emplace(key, *name);
            } else if (key == "props") {
                auto record = std::make_shared<DocNode>();
                record->kind = "props";
                record->path = keyPath;
                for (auto const& [prop, propValue] : objectOf(value, keyPath, "`props` is a record of expressions")) {
                    record->props.emplace(prop, expr(propValue, pointerAppend(keyPath, prop)));
                }
                part->parts["props"].push_back(std::move(record));
            } else {
                part->props.emplace(key, expr(value, keyPath));
            }
        }
        return part;
    }

    [[nodiscard]] DocNodePtr part(Json const& json, std::string_view kind, std::string const& path) {
        KindSpec const* const spec = findKind(kind, true);
        auto const& object = objectOf(json, path, "a part of a node is an object");
        refuseMustUnderstand(object, path);
        auto made = std::make_shared<DocNode>();
        made->kind = std::string{kind};
        made->path = path;
        fill(*made, object, *spec, path);
        return made;
    }

    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per shape a key takes.
    void entry(DocNode& made, KeySpec const& key, Json const& value, std::string const& path) {
        std::string const name{key.key};
        switch (key.shape) {
            case Shape::Expr:
            case Shape::Condition:
                made.props.emplace(name, expr(value, path));
                break;
            case Shape::Commands:
                made.events.emplace(name, commandList(value, path));
                break;
            case Shape::Nodes: {
                std::vector<DocNodePtr> nodes;
                if (auto const* const array = std::get_if<Json::array_t>(&value.data)) {
                    for (std::size_t index = 0; index < array->size(); ++index) {
                        nodes.push_back(node(array->at(index), pointerAppend(path, index)));
                    }
                } else {
                    nodes.push_back(node(value, path));
                }
                made.children.emplace(name, std::move(nodes));
                break;
            }
            case Shape::Text:
            case Shape::Name: {
                auto const* const string = std::get_if<std::string>(&value.data);
                if (string == nullptr || (key.shape == Shape::Name && !isName(*string))) {
                    refuse(path, "`" + name + "` is " + (key.shape == Shape::Name ? "a name" : "a string"));
                }
                if (name == "id") {
                    made.id = *string;
                } else {
                    made.names.emplace(name, *string);
                }
                break;
            }
            case Shape::Part:
                made.parts[name].push_back(part(value, key.part, path));
                break;
            case Shape::Parts:
            case Shape::PartsOrExpr: {
                auto const* const array = std::get_if<Json::array_t>(&value.data);
                if (array == nullptr && key.shape == Shape::PartsOrExpr) {
                    made.props.emplace(name, expr(value, path));
                    break;
                }
                if (array == nullptr) {
                    refuse(path, "`" + name + "` is an array");
                }
                auto& list = made.parts[name];
                for (std::size_t index = 0; index < array->size(); ++index) {
                    list.push_back(part(array->at(index), key.part, pointerAppend(path, index)));
                }
                break;
            }
            case Shape::ExprRecord: {
                if (!key.part.empty()) {
                    made.parts[name].push_back(part(value, key.part, path));
                    break;
                }
                auto record = std::make_shared<DocNode>();
                record->kind = name;
                record->path = path;
                for (auto const& [field, fieldValue] : objectOf(value, path, "a record of expressions")) {
                    record->props.emplace(field, expr(fieldValue, pointerAppend(path, field)));
                }
                made.parts[name].push_back(std::move(record));
                break;
            }
            case Shape::CommandRecord:
                for (auto const& [event, commands] : objectOf(value, path, "a record of command lists")) {
                    made.events.emplace(std::format("{}.{}", name, event),
                                        commandList(commands, pointerAppend(path, event)));
                }
                break;
            case Shape::Flag: {
                auto const* const flag = std::get_if<bool>(&value.data);
                if (flag == nullptr) {
                    refuse(path, "`" + name + "` is a literal `true` or `false`");
                }
                made.flags.emplace(name, *flag);
                break;
            }
            case Shape::Keys:
            default:
                made.keys = keyBindings(value, path);
                break;
        }
    }
    // NOLINTEND(misc-no-recursion)
};

/// @brief What a name in a scope declares.
enum class NameCategory : std::uint8_t {
    Param,     ///< A screen param.
    Model,     ///< A model alias.
    State,     ///< State.
    Let,       ///< A derived value.
    Query,     ///< A query.
    Mutation,  ///< A mutation.
    Element,   ///< An element id.
    Transfer,  ///< A transfer id of `upload`.
};

struct ScopeInfo;

/// @brief One declared name of a scope.
struct NameEntry {
    /// @brief What it declares.
    NameCategory category = NameCategory::Param;
    /// @brief The declaration's JSON Pointer.
    std::string path;
    /// @brief The scope that declares it.
    ScopeInfo* scope = nullptr;
    /// @brief The type of a param, of typed state, and of state or a `let` once derived.
    Type type;
    /// @brief The model of a `Model` entry.
    ModelDecl const* model = nullptr;
    /// @brief The expression a state or `let` type is derived from; `nullptr` when the type is declared.
    ExprPtr derivedFrom;
    /// @brief The query of a `Query` entry.
    QueryDecl const* query = nullptr;
    /// @brief The mutation of a `Mutation` entry.
    MutationDecl const* mutation = nullptr;
    /// @brief The element of an `Element` entry.
    DocNode const* element = nullptr;
    /// @brief 0 before its type is derived, 1 while it is, 2 after.
    int visit = 0;
};

/// @brief One scope during validation: its names and its parent.
struct ScopeInfo {
    /// @brief The enclosing scope; `nullptr` for a screen or a component fallback.
    ScopeInfo* parent = nullptr;
    /// @brief Whether it is a `forEach` or `table` row scope.
    bool row = false;
    /// @brief Whether it is the screen's own scope.
    bool screen = false;
    /// @brief The names it declares.
    std::map<std::string, NameEntry, std::less<>> names;
};

/// @brief Where an expression or command is checked: its scope and what is valid there.
struct CheckContext {
    /// @brief The scope.
    ScopeInfo* scope = nullptr;
    /// @brief Inside a command list, where `event` is valid.
    bool commands = false;
    /// @brief Inside a boundary's `error`, where `boundary.error` and `retry` are valid.
    bool boundaryError = false;
    /// @brief Inside `onClose`, where `cancelClose` is valid.
    bool onClose = false;
    /// @brief Names bound here: `as` of a sequence step or `each`, `result`, `body`, `props`.
    std::vector<std::pair<std::string, Type>> bindings;
};

class DocumentValidator;

/// @brief Resolves an expression's names against a scope chain and a context.
class ScopeResolver final : public NameResolver {
public:
    /// @brief Resolves for @p validator in @p context.
    /// @param validator The validator, which derives state and `let` types on demand.
    /// @param context   Where the expression is.
    ScopeResolver(DocumentValidator& validator, CheckContext const& context)
        : _validator{validator}, _context{context} {}

    /// @brief Resolves a reference.
    /// @param head The first segment.
    /// @param path The rest.
    /// @return The type, or why it does not resolve.
    [[nodiscard]] std::expected<Type, std::string> ref(std::string_view head,
                                                       std::span<std::string const> path) const override;

    /// @brief Resolves a row field.
    /// @param path The field path.
    /// @return `any` inside a row, or why there is no row.
    [[nodiscard]] std::expected<Type, std::string> row(std::span<std::string const> path) const override;

    /// @brief Resolves a query or mutation name.
    /// @param form The form.
    /// @param name The name.
    /// @return The type, or why it does not resolve.
    [[nodiscard]] std::expected<Type, std::string> control(ExprKind form, std::string_view name) const override;

    /// @brief Resolves a customization point.
    /// @param point The point.
    /// @param path  The rest.
    /// @return The type, or why it does not resolve.
    [[nodiscard]] std::expected<Type, std::string> pref(std::string_view point,
                                                        std::span<std::string const> path) const override;

private:
    DocumentValidator& _validator;
    CheckContext const& _context;
};

/// @brief The type of an element value.
/// @param value The value's name.
/// @return Its type.
[[nodiscard]] inline Type elementValueType(std::string_view value) {
    static constexpr std::array<std::string_view, 10> kBools = {"visible", "enabled", "valid", "dirty", "focused",
                                                                "touched", "checked", "open",  "ready", "submitted"};
    static constexpr std::array<std::string_view, 3> kCounts = {"viewCount", "sourceCount", "selectedCount"};
    if (listed(kBools, value) || value == "pending") {
        return Type::of(TypeKind::Bool);
    }
    if (value == "errors" || value == "shownErrors") {
        return Type::listOf(Type::of(TypeKind::String));
    }
    if (listed(kCounts, value)) {
        return Type::of(TypeKind::Int);
    }
    if (value == "error") {
        return Type::of(TypeKind::String, true);
    }
    return Type::any();
}

/// @brief Whether a value of type @p actual may stand where @p declared is expected.
/// @param declared The expected type.
/// @param actual   The type found.
/// @return Whether they are compatible: equal kinds, `any` on either side, an `int` for a `decimal` or `key`, a
///         `string` for a `key`.
[[nodiscard]] inline bool compatible(Type const& declared, Type const& actual) {
    if (declared.isAny() || actual.isAny() || declared.kind == actual.kind) {
        return true;
    }
    bool const integer = actual.kind == TypeKind::Int;
    return (declared.kind == TypeKind::Decimal && integer) ||
           (declared.kind == TypeKind::Key && (integer || actual.kind == TypeKind::String));
}

// NOLINTBEGIN(misc-no-recursion): a document is a tree of scopes, nodes and command lists, and checking descends it.
/// @brief Checks a parsed document against its own names and the catalog: every load-time rule that needs more
///        than one place in the document.
class DocumentValidator {
public:
    /// @brief Validates @p doc.
    /// @param doc     The parsed document.
    /// @param options The catalog facts.
    DocumentValidator(Document const& doc, LoadOptions const& options) : _doc{&doc}, _options{&options} {}

    /// @brief Runs every rule; throws `DocumentRefused` on the first refusal.
    void run() {
        ScopeInfo& screen = newScope(nullptr, false);
        screen.screen = true;
        for (auto const& [name, type] : _doc->params) {
            NameEntry entry;
            entry.category = NameCategory::Param;
            entry.path = pointerAppend("/params", name);
            entry.type = type;
            add(screen, name, std::move(entry));
        }
        declare(_doc->scope, screen);
        for (auto const& [key, commands] : _doc->lifecycle) {
            static_cast<void>(key);
            collectTransfers(commands, screen);
        }
        for (auto const& [chord, commands] : _doc->keys) {
            static_cast<void>(chord);
            collectTransfers(commands, screen);
        }
        if (_doc->view) {
            collect(*_doc->view, screen);
        }
        for (DocNodePtr const& part : headParts()) {
            collectPart(*part, screen, screen);
        }
        std::vector<ScopeInfo*> componentScopes;
        for (auto const& [name, component] : _doc->components) {
            static_cast<void>(name);
            ScopeInfo& fallbackScope = newScope(nullptr, false);
            collect(*component.fallback, fallbackScope);
            componentScopes.push_back(&fallbackScope);
        }

        CheckContext const root{
            .scope = &screen, .commands = false, .boundaryError = false, .onClose = false, .bindings = {}};
        checkScope(_doc->scope, root);
        if (_doc->title) {
            checkExpr(*_doc->title, root);
        }
        if (_doc->identity) {
            checkExpr(*_doc->identity, root);
        }
        if (_doc->dirty) {
            checkCondition(*_doc->dirty, root, "`dirty`");
        }
        if (_doc->subtitle) {
            checkExpr(*_doc->subtitle, root);
        }
        for (DocNodePtr const& part : headParts()) {
            checkElement(*part, root, true);
        }
        checkActionButtons(_doc->actions, "/actions");
        for (auto const& [key, commands] : _doc->lifecycle) {
            CheckContext context = root;
            context.onClose = key == "onClose";
            checkCommands(commands, context);
        }
        for (auto const& [chord, commands] : _doc->keys) {
            static_cast<void>(chord);
            checkCommands(commands, root);
        }
        checkPrefetch();
        if (_doc->view) {
            checkNode(*_doc->view, root);
        }
        std::size_t index = 0;
        for (auto const& [name, component] : _doc->components) {
            static_cast<void>(name);
            CheckContext context{.scope = componentScopes.at(index++),
                                 .commands = false,
                                 .boundaryError = false,
                                 .onClose = false,
                                 .bindings = {}};
            context.bindings.emplace_back("props", Type::of(TypeKind::Record));
            checkNode(*component.fallback, context);
        }
        checkCustomizePoints();
        checkWatchGraph();
        checkEffects();
    }

    /// @brief What validation found that does not refuse the document.
    /// @return The warnings, in the order found; the validator keeps none.
    [[nodiscard]] std::vector<LoadWarning> takeWarnings() noexcept { return std::move(_warnings); }

    /// @brief Finds a name in a scope chain.
    /// @param scope The innermost scope.
    /// @param name  The name.
    /// @return The entry, or `nullptr`.
    [[nodiscard]] static NameEntry* lookup(ScopeInfo* scope, std::string_view name) {
        for (ScopeInfo* current = scope; current != nullptr; current = current->parent) {
            auto const found = current->names.find(name);
            if (found != current->names.end()) {
                return &found->second;
            }
        }
        return nullptr;
    }

    /// @brief The type of a name whose type is derived from an expression, derived on first use.
    /// @param entry The state or `let` entry.
    /// @param name  Its name, for the cycle's description.
    /// @return The type, or the cycle it closes.
    [[nodiscard]] std::expected<Type, std::string> derivedType(NameEntry& entry, std::string_view name) {
        if (entry.visit == 2 || !entry.derivedFrom) {
            return entry.type;
        }
        if (entry.visit == 1) {
            std::string cycle;
            auto const start = std::ranges::find(_deriving, name);
            for (auto step = start; step != _deriving.end(); ++step) {
                cycle += *step + " → ";
            }
            std::string const graph =
                entry.category == NameCategory::Let ? "the let graph has a cycle: " : "the state inits form a cycle: ";
            return std::unexpected(graph + cycle + std::string{name});
        }
        entry.visit = 1;
        _deriving.emplace_back(name);
        CheckContext const context{
            .scope = entry.scope, .commands = false, .boundaryError = false, .onClose = false, .bindings = {}};
        Type type = checkExpr(*entry.derivedFrom, context);
        _deriving.pop_back();
        entry.visit = 2;
        entry.type = type;
        return type;
    }

    /// @brief The document.
    /// @return It.
    [[nodiscard]] Document const& document() const noexcept { return *_doc; }

private:
    ScopeInfo& newScope(ScopeInfo* parent, bool row) {
        ScopeInfo& made = _scopes.emplace_back();
        made.parent = parent;
        made.row = row;
        return made;
    }

    static void add(ScopeInfo& scope, std::string const& name, NameEntry entry) {
        static constexpr std::array<std::string_view, 6> kReserved = {"event", "app",    "boundary",
                                                                      "props", "result", "body"};
        if (listed(kReserved, name)) {
            refuse(entry.path, "`" + name + "` is reserved and cannot be declared");
        }
        auto const found = scope.names.find(name);
        if (found != scope.names.end()) {
            refuse(entry.path, "`" + name + "` is declared twice in this scope (first at " + found->second.path + ")");
        }
        entry.scope = &scope;
        scope.names.emplace(name, std::move(entry));
    }

    /// Checks a model alias against the catalog's facts, when there is a catalog.
    void checkCatalog(ModelDecl const& model) {
        if (!_options->catalog.model) {
            return;
        }
        auto const info = _options->catalog.model(model.type);
        if (!info) {
            refuse(pointerAppend(model.path, "type"), "the model type `" + model.type + "` is not in the catalog");
        }
        if (model.instance == InstanceMode::Shared && !info->stateless) {
            refuse(pointerAppend(model.path, "instance"),
                   "the model type `" + model.type +
                       "` keeps state, so `shared` names no instance: it needs a key, or `private`");
        }
        if (model.instance == InstanceMode::Keyed && !info->keyed) {
            refuse(pointerAppend(model.path, "instance"),
                   "the model type `" + model.type + "` is not keyed, so its instance is not an expression");
        }
    }

    void declare(ScopeDecl const& decl, ScopeInfo& scope) {
        for (ModelDecl const& model : decl.models) {
            if (scope.row && model.instance == InstanceMode::Default) {
                refuse(model.path, "a model alias in a row scope names its `instance`");
            }
            if (!scope.screen && model.instance == InstanceMode::Borrowed) {
                refuse(model.path, "only a screen's own aliases borrow the opener's instance");
            }
            checkCatalog(model);
            NameEntry entry;
            entry.category = NameCategory::Model;
            entry.path = model.path;
            entry.model = &model;
            add(scope, model.name, std::move(entry));
        }
        for (StateDecl const& state : decl.state) {
            NameEntry entry;
            entry.category = NameCategory::State;
            entry.path = state.path;
            entry.type = state.type;
            entry.derivedFrom = state.type.isAny() ? state.init : nullptr;
            add(scope, state.name, std::move(entry));
        }
        for (LetDecl const& let : decl.lets) {
            NameEntry entry;
            entry.category = NameCategory::Let;
            entry.path = let.path;
            entry.derivedFrom = let.value;
            add(scope, let.name, std::move(entry));
        }
        for (QueryDecl const& query : decl.queries) {
            NameEntry entry;
            entry.category = NameCategory::Query;
            entry.path = query.path;
            entry.query = &query;
            add(scope, query.name, std::move(entry));
            _queries.emplace(query.name, &query);
            _queryScopes.emplace_back(&query, &scope);
        }
        for (MutationDecl const& mutation : decl.mutations) {
            NameEntry entry;
            entry.category = NameCategory::Mutation;
            entry.path = mutation.path;
            entry.mutation = &mutation;
            add(scope, mutation.name, std::move(entry));
            _mutations.emplace_back(&mutation, &scope);
            collectTransfers(mutation.onSuccess, scope);
            collectTransfers(mutation.onError, scope);
        }
        for (WatchDecl const& watch : decl.watches) {
            _watches.emplace_back(&watch, &scope);
            collectTransfers(watch.body, scope);
        }
    }

    void collectTransfers(Commands const& commands, ScopeInfo& scope) {
        for (Command const& cmd : commands) {
            if (cmd.kind == CommandKind::Upload && !cmd.transfer.empty()) {
                NameEntry entry;
                entry.category = NameCategory::Transfer;
                entry.path = pointerAppend(cmd.path, "id");
                add(scope, cmd.transfer, std::move(entry));
            }
            collectTransfers(cmd.commands, scope);
            collectTransfers(cmd.alternative, scope);
        }
    }

    void collect(DocNode const& node, ScopeInfo& scope) { collectIn(node, scope); }

    void collectIn(DocNode const& node, ScopeInfo& scope) {
        KindSpec const* const spec = findKind(node.kind, false);
        if (!node.id.empty()) {
            NameEntry entry;
            entry.category = NameCategory::Element;
            entry.path = pointerAppend(node.path, "id");
            entry.element = &node;
            add(scope, node.id, std::move(entry));
            _elements.emplace(node.id, &node);
        }
        ScopeInfo* inner = &scope;
        if (spec != nullptr && (spec->flags & kScope) != 0) {
            inner = &newScope(&scope, (spec->flags & kRowScope) != 0);
            _scopeOf.emplace(&node, inner);
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access): the parser gives every scope-creating node a scope.
            declare(*node.scope, *inner);
        }
        _innerOf.emplace(&node, inner);
        for (auto const& [key, commands] : node.events) {
            collectTransfers(commands, isInner(spec, key) ? *inner : scope);
        }
        for (auto const& [chord, commands] : node.keys) {
            static_cast<void>(chord);
            collectTransfers(commands, scope);
        }
        for (auto const& [key, nodes] : node.children) {
            for (DocNodePtr const& child : nodes) {
                collectIn(*child, isInner(spec, key) ? *inner : scope);
            }
        }
        for (auto const& [key, parts] : node.parts) {
            for (DocNodePtr const& child : parts) {
                collectPart(*child, isInner(spec, key) ? *inner : scope, *inner);
            }
        }
    }

    void collectPart(DocNode const& part, ScopeInfo& outer, ScopeInfo& ownerInner) {
        KindSpec const* const spec = findKind(part.kind, true);
        ScopeInfo* inner = &ownerInner;
        if (spec != nullptr && (spec->flags & kScope) != 0) {
            inner = &newScope(&outer, false);
            _scopeOf.emplace(&part, inner);
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access): the parser gives every switch case a scope.
            declare(*part.scope, *inner);
        }
        _innerOf.emplace(&part, inner);
        for (auto const& [key, commands] : part.events) {
            collectTransfers(commands, outer);
        }
        for (auto const& [key, nodes] : part.children) {
            for (DocNodePtr const& child : nodes) {
                collectIn(*child, isInner(spec, key) ? *inner : outer);
            }
        }
        for (auto const& [key, parts] : part.parts) {
            static_cast<void>(key);
            for (DocNodePtr const& child : parts) {
                collectPart(*child, outer, ownerInner);
            }
        }
    }

    [[nodiscard]] static bool isInner(KindSpec const* spec, std::string_view key) {
        if (spec == nullptr) {
            return false;
        }
        auto const found = std::ranges::find(spec->keys, key, &KeySpec::key);
        return found != spec->keys.end() && (found->flags & kInner) != 0;
    }

    /// The flags of a child-node key. The parser fills `children` only from a kind's own keys, so the kind and the key
    /// are always found.
    [[nodiscard]] static std::uint8_t keyFlags(KindSpec const& spec, std::string_view key) {
        return std::ranges::find(spec.keys, key, &KeySpec::key)->flags;
    }

    [[nodiscard]] static bool isCondition(KindSpec const* spec, std::string_view key) {
        auto shapeOf = [key](std::span<KeySpec const> keys) {
            auto const found = std::ranges::find(keys, key, &KeySpec::key);
            return found == keys.end() ? Shape::Expr : found->shape;
        };
        return (spec != nullptr && shapeOf(spec->keys) == Shape::Condition) ||
               shapeOf(kCommonKeys) == Shape::Condition || shapeOf(kFieldKeys) == Shape::Condition;
    }

    Type checkExpr(Expr const& expr, CheckContext const& context) {
        ScopeResolver const resolver{*this, context};
        auto checked = checkExpression(expr, resolver);
        if (!checked) {
            throw DocumentRefused{std::move(checked.error())};
        }
        return std::move(*checked);
    }

    void checkCondition(Expr const& expr, CheckContext const& context, std::string_view what) {
        Type const type = checkExpr(expr, context);
        if (!compatible(Type::of(TypeKind::Bool), type)) {
            refuse(expr.path, std::string{what} + " is " + type.name() + "; expected bool");
        }
    }

    [[nodiscard]] static NameEntry& require(CheckContext const& context, std::string const& name,
                                            std::string const& path, std::initializer_list<NameCategory> categories,
                                            std::string_view what) {
        NameEntry* const entry = lookup(context.scope, name);
        if (entry == nullptr || std::ranges::find(categories, entry->category) == categories.end()) {
            refuse(path, "`" + name + "` is not " + std::string{what} + " of this scope");
        }
        return *entry;
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the alias, then the action it runs, as a query reads.
    std::optional<ActionInfo> checkModelAction(CheckContext const& context, std::string const& alias,
                                               std::string const& action, std::string const& path) {
        std::string const modelPath = pointerAppend(path, "model");
        NameEntry const& model = require(context, alias, modelPath, {NameCategory::Model}, "a model alias");
        if (!_options->catalog.action) {
            return std::nullopt;
        }
        auto const info = _options->catalog.action(action);
        if (!info) {
            refuse(path, "the action `" + action + "` is not in the catalog");
        }
        if (info->modelType != model.model->type) {
            refuse(path, "the action `" + action + "` belongs to `" + info->modelType + "`, not `" +
                             model.model->type + "`");
        }
        return info;
    }

    void checkScope(ScopeDecl const& decl, CheckContext const& context) {
        for (ModelDecl const& model : decl.models) {
            if (model.key) {
                checkExpr(*model.key, context);
            }
        }
        for (StateDecl const& state : decl.state) {
            if (!state.init) {
                continue;
            }
            Type const type = checkExpr(*state.init, context);
            bool const literalFits = state.init->kind != ExprKind::Literal || state.type.admits(state.init->literal);
            if (!literalFits || !compatible(state.type, type)) {
                refuse(state.init->path,
                       "the init of `" + state.name + "` does not have its type " + state.type.name());
            }
            // The type is derived now, so a state no expression reads is still checked.
            auto const derived = derivedType(*lookup(context.scope, state.name), state.name);
            static_cast<void>(derived);
        }
        for (LetDecl const& let : decl.lets) {
            auto type = derivedType(*lookup(context.scope, let.name), let.name);
            static_cast<void>(type);
        }
        for (QueryDecl const& query : decl.queries) {
            checkQuery(query, context);
        }
        for (MutationDecl const& mutation : decl.mutations) {
            checkMutation(mutation, context);
        }
        for (WatchDecl const& watch : decl.watches) {
            checkExpr(*watch.on, context);
            checkCommands(watch.body, context);
        }
        for (CheckDecl const& check : decl.checks) {
            checkCondition(*check.assertion, context, "the `assert` of a check");
            if (check.message) {
                checkExpr(*check.message, context);
            }
            for (std::size_t index = 0; index < check.targets.size(); ++index) {
                static_cast<void>(require(context, check.targets.at(index),
                                          pointerAppend(pointerAppend(check.path, "targets"), index),
                                          {NameCategory::Element}, "an element"));
            }
        }
    }

    void checkQuery(QueryDecl const& query, CheckContext const& context) {
        auto const action = checkModelAction(context, query.model, query.action, query.path);
        // An evaluation runs no handler, so only a call can change anything.
        if (action && action->effects && !query.evaluate) {
            refuse(query.path, std::format("a query reads, so its action declares no effects; `{}` declares {}, "
                                           "so it runs as a mutation",
                                           query.action, describeEffects(*action->effects)));
        }
        if (query.body) {
            checkExpr(*query.body, context);
        }
        if (query.when) {
            checkCondition(*query.when, context, "`when`");
        }
    }

    void checkMutation(MutationDecl const& mutation, CheckContext const& context) {
        auto const action = checkModelAction(context, mutation.model, mutation.action, mutation.path);
        if (mutation.body) {
            checkExpr(*mutation.body, context);
        }
        if (mutation.serialKey) {
            CheckContext keyContext = context;
            keyContext.bindings.emplace_back("body", Type::any());
            checkExpr(*mutation.serialKey, keyContext);
        }
        if (mutation.optimistic) {
            OptimisticDecl const& decl = *mutation.optimistic;
            std::string const path = pointerAppend(mutation.path, "optimistic");
            auto const target = _queries.find(decl.query);
            if (target == _queries.end()) {
                refuse(pointerAppend(path, "query"), "`" + decl.query + "` is not a query of this document");
            }
            CheckContext bodyContext = context;
            bodyContext.bindings.emplace_back("body", Type::any());
            checkExpr(*decl.key, bodyContext);
            checkExpr(*decl.with, bodyContext);
            checkPatchAt(decl.query, decl.at, *target->second, path);
            checkOptimistic(decl, *target->second);
        }
        if (mutation.offlineQueue && action) {
            if (action->credential || !action->secretFields.empty()) {
                refuse(pointerAppend(mutation.path, "offline"),
                       "a mutation that sends a credential or returns a secret is never queued offline");
            }
        }
        checkCommands(mutation.onSuccess, context);
        checkCommands(mutation.onError, context);
    }

    // Only a catalog that knows the result is a record can say a patch misses its list; otherwise the interpreter
    // reports the miss when the patch runs.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the query's name, then the member holding the list.
    void checkPatchAt(std::string const& queryName, std::string const& listMember, QueryDecl const& query,
                      std::string const& path) {
        if (!_options->catalog.action) {
            return;
        }
        auto const info = _options->catalog.action(query.action);
        if (!info || !info->listFields) {
            return;
        }
        if (listMember.empty()) {
            refuse(path, std::format("`{}` answers a record, so a patch of `{}` names the member holding its list "
                                     "with `at`",
                                     query.action, queryName));
        }
        if (std::ranges::find(*info->listFields, listMember) == info->listFields->end()) {
            refuse(pointerAppend(path, "at"),
                   std::format("the result of `{}` has no list member `{}`", query.action, listMember));
        }
    }

    void checkOptimistic(OptimisticDecl const& decl, QueryDecl const& query) {
        if (!_options->catalog.action || decl.with->kind != ExprKind::Object) {
            return;
        }
        auto const info = _options->catalog.action(query.action);
        if (!info) {
            return;
        }
        for (auto const& [field, value] : decl.with->fields) {
            if (std::ranges::find(info->calculatedFields, field) != info->calculatedFields.end()) {
                refuse(value->path,
                       std::format("`{}` is calculated by the server and is never written optimistically", field));
            }
        }
    }

    void checkPrefetch() {
        for (std::size_t index = 0; index < _doc->prefetch.size(); ++index) {
            std::string const& name = _doc->prefetch.at(index);
            std::string const path = pointerAppend("/prefetch", index);
            auto const found = std::ranges::find(_doc->scope.queries, name, &QueryDecl::name);
            if (found == _doc->scope.queries.end()) {
                refuse(path, "`" + name + "` is not a query of the screen");
            }
            if (found->body && !readsOnlyParams(*found->body)) {
                refuse(path, "the body of `" + name + "` reads more than the screen's params, so it cannot prefetch");
            }
        }
    }

    [[nodiscard]] bool readsOnlyParams(Expr const& expr) const {
        if (expr.kind == ExprKind::Ref) {
            return std::ranges::find(_doc->params, expr.name, &std::pair<std::string, Type>::first) !=
                   _doc->params.end();
        }
        if (expr.kind != ExprKind::Literal && expr.kind != ExprKind::Call && expr.kind != ExprKind::If &&
            expr.kind != ExprKind::Object && expr.kind != ExprKind::List) {
            return false;
        }
        bool only = true;
        for (ExprPtr const& arg : expr.args) {
            only = only && readsOnlyParams(*arg);
        }
        for (auto const& [name, field] : expr.fields) {
            static_cast<void>(name);
            only = only && readsOnlyParams(*field);
        }
        return only;
    }

    void checkCustomizePoints() {
        std::set<std::string, std::less<>> arranged;
        for (auto const& [name, point] : _doc->customize) {
            static_cast<void>(name);
            if (point.kind == "columns" && _doc->view) {
                auto const found = _elements.find(point.of);
                if (found == _elements.end() || found->second->kind != "table") {
                    refuse(pointerAppend(point.path, "of"),
                           "a `columns` point names a table; `" + point.of + "` is not one");
                }
            }
            if (point.kind == "arrange") {
                checkArranged(point, arranged);
            }
        }
    }

    // The stored order names children by id, so the container is a stack whose every child has one, and one point
    // orders it: two would each claim the order.
    void checkArranged(CustomizePoint const& point, std::set<std::string, std::less<>>& arranged) {
        auto const found = _elements.find(point.of);
        if (found == _elements.end() || (found->second->kind != "column" && found->second->kind != "row") ||
            found->second->nodes("children").empty()) {
            refuse(pointerAppend(point.path, "of"),
                   "an `arrange` point names a column or row of this screen that has children; `" + point.of +
                       "` is not one");
        }
        for (DocNodePtr const& child : found->second->nodes("children")) {
            if (child->id.empty()) {
                refuse(pointerAppend(point.path, "of"),
                       "an `arrange` point orders its container's children by id; a child of `" + point.of +
                           "` has none");
            }
        }
        // An arranged container is drawn as a keyed list, which places its children as a list does.
        std::string const asList =
            "an arranged `" + found->second->kind + "` is drawn as a list, which takes no `align` or `justify`";
        for (std::string_view const key : {"align", "justify"}) {
            if (!found->second->name(key).empty()) {
                refuse(pointerAppend(found->second->path, key), asList);
            }
        }
        if (!arranged.insert(point.of).second) {
            refuse(pointerAppend(point.path, "of"), "`" + point.of + "` is arranged by another point");
        }
    }

    [[nodiscard]] CheckContext innerContext(DocNode const& node, CheckContext const& context) const {
        CheckContext inner = context;
        inner.scope = _innerOf.at(&node);
        return inner;
    }

    void checkNode(DocNode const& node, CheckContext const& context) {
        if (!node.name("alignSelf").empty()) {
            refuse(pointerAppend(node.path, "alignSelf"), std::string{kAlignSelfPlaces});
        }
        checkElement(node, context, false);
    }

    // What `alignSelf` places a node in.
    static constexpr std::string_view kAlignSelfPlaces = "`alignSelf` places a node in its row, column or grid";

    // A child that places itself sits in a row, a column, a grid or a `forEach`; any other parent (a panel, a dialog,
    // a tab page) has no cross axis to place it on.
    static void checkPlacements(DocNode const& parent) {
        bool const places =
            parent.kind == "row" || parent.kind == "column" || parent.kind == "grid" || parent.kind == "forEach";
        for (auto const& [key, nodes] : parent.children) {
            for (DocNodePtr const& child : nodes) {
                if (!child->name("alignSelf").empty() && (!places || key != "children")) {
                    refuse(pointerAppend(child->path, "alignSelf"), std::string{kAlignSelfPlaces});
                }
            }
        }
    }

    void checkElement(DocNode const& node, CheckContext const& outer, bool part) {
        KindSpec const* const spec = findKind(node.kind, part);
        CheckContext const inner = innerContext(node, outer);
        if (auto const found = _scopeOf.find(&node); found != _scopeOf.end()) {
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access): a node or case with a scope of its own has its declarations.
            checkScope(*node.scope, inner);
        }
        for (auto const& [key, expr] : node.props) {
            CheckContext const& context = isInner(spec, key) ? inner : outer;
            if (isCondition(spec, key)) {
                checkCondition(*expr, context, "`" + key + "`");
            } else {
                checkExpr(*expr, context);
            }
        }
        for (auto const& [key, commands] : node.events) {
            if (node.kind == "custom") {
                checkCustomEvent(node, key.substr(3), node.path);
            }
            checkCommands(commands, isInner(spec, key) ? inner : outer);
        }
        for (auto const& [chord, commands] : node.keys) {
            static_cast<void>(chord);
            checkCommands(commands, outer);
        }
        for (auto const& [key, nodes] : node.children) {
            CheckContext context = isInner(spec, key) ? inner : outer;
            context.boundaryError = (keyFlags(*spec, key) & kBoundaryError) != 0 || context.boundaryError;
            for (DocNodePtr const& child : nodes) {
                checkElement(*child, context, false);
            }
        }
        for (auto const& [key, parts] : node.parts) {
            CheckContext const& context = isInner(spec, key) ? inner : outer;
            for (DocNodePtr const& child : parts) {
                checkElement(*child, context, true);
            }
        }
        checkPlacements(node);
        if (!part) {
            checkFlags(node);
            checkLayoutKeys(node);
            checkKindRules(node, outer);
            // An overlay's footer actions; a form's field components.
            checkActionButtons(node.partList("actions"), pointerAppend(node.path, "actions"));
            checkFieldComponents(node);
        }
    }

    void checkCustomEvent(DocNode const& node, std::string const& event, std::string const& path) {
        auto const found = _doc->components.find(node.name("name"));
        if (found != _doc->components.end() && !found->second.events.contains(event)) {
            refuse(pointerAppend(pointerAppend(path, "on"), event),
                   "the component `" + found->second.name + "` declares no event `" + event + "`");
        }
    }

    // A flag that needs a key beside it.
    static void checkFlags(DocNode const& node) {
        if (node.flag("showLabel") && node.prop("label") == nullptr) {
            refuse(pointerAppend(node.path, "showLabel"), "`showLabel` shows the `label`, and the node gives none");
        }
    }

    void checkKindRules(DocNode const& node, CheckContext const& context) {
        if (node.kind == "custom") {
            auto const found = _doc->components.find(node.name("name"));
            if (found == _doc->components.end()) {
                refuse(pointerAppend(node.path, "name"),
                       "`" + std::string{node.name("name")} + "` is not a component of this document");
            }
            for (DocNodePtr const& props : node.partList("props")) {
                for (auto const& [prop, value] : props->props) {
                    if (!found->second.props.contains(prop)) {
                        refuse(value->path, "the component `" + found->first + "` declares no prop `" + prop + "`");
                    }
                }
            }
        } else if (node.kind == "customize" && !_doc->customize.contains(node.name("point"))) {
            refuse(pointerAppend(node.path, "point"),
                   "`" + std::string{node.name("point")} + "` is not a customization point of this document");
        } else if (node.kind == "button") {
            static constexpr std::array<std::string_view, 3> kEmphasis = {"normal", "primary", "danger"};
            checkChoice(node, "emphasis", kEmphasis);
        } else if (node.kind == "grid" && !node.name("justify").empty()) {
            refuse(pointerAppend(node.path, "justify"),
                   "a grid's columns share its width; there is no spare length to justify");
        } else if (node.kind == "textInput") {
            static constexpr std::array<std::string_view, 3> kCommit = {"change", "blur", "enter"};
            checkChoice(node, "commitOn", kCommit);
        } else if (node.kind == "select") {
            static constexpr std::array<std::string_view, 2> kStale = {"clear", "keep"};
            checkChoice(node, "onStale", kStale);
        } else if (node.kind == "dialog" || node.kind == "drawer" || node.kind == "form") {
            static constexpr std::array<std::string_view, 3> kPolicies = {"touched", "submit", "always"};
            checkChoice(node, "showErrors", kPolicies);
            // Absent keeps the schema's own mode, so a misspelt mode would silently do the same.
            static constexpr std::array<std::string_view, 2> kSubmitModes = {"explicit", "auto"};
            checkChoice(node, "submit", kSubmitModes);
        }
        if (node.kind == "form" || node.kind == "collection" || node.kind == "wizard") {
            checkFormColumns(node);
            static constexpr std::array<std::string_view, 2> kFieldLayouts = {"stacked", "inline"};
            checkChoice(node, "fieldLayout", kFieldLayouts);
        }
        static constexpr std::array<std::string_view, 4> kAlign = {"start", "center", "end", "stretch"};
        static constexpr std::array<std::string_view, 4> kJustify = {"start", "center", "end", "between"};
        checkChoice(node, "align", kAlign);
        checkChoice(node, "justify", kJustify);
        checkChoice(node, "alignSelf", kAlign);
        std::string_view const model = node.name("model");
        if (!model.empty()) {
            if (node.name("action").empty()) {
                refuse(node.path, "a `" + node.kind + "` with a `model` names its `action`");
            }
            checkModelAction(context, std::string{model}, std::string{node.name("action")}, node.path);
        } else if (node.kind == "table" && !node.prop("rows")) {
            refuse(node.path, "a table takes `rows`, or a `model` and `action`");
        }
    }

    // Each form field drawn by a host component.
    void checkFieldComponents(DocNode const& form) const {
        if (form.kind != "form") {
            return;
        }
        for (DocNodePtr const& record : form.partList("fields")) {
            for (DocNodePtr const& field : record->partList("fields")) {
                if (!field->name("component").empty()) {
                    checkFieldComponent(*field);
                }
            }
        }
    }

    // A form field drawn by a host component: a declared one with a `value` prop and a `change` event, given only
    // props it declares, and not beside a `widget`, which would choose the control too.
    void checkFieldComponent(DocNode const& field) const {
        std::string_view const name = field.name("component");
        std::string const where = pointerAppend(field.path, "component");
        auto const found = _doc->components.find(name);
        if (found == _doc->components.end()) {
            refuse(where, "`" + std::string{name} + "` is not a component of this document");
        }
        Component const& component = found->second;
        if (!component.props.contains("value") || !component.events.contains("change")) {
            refuse(where, "the component `" + component.name +
                              "` draws a form field, so it declares a prop `value` and an event `change`");
        }
        if (field.prop("widget") != nullptr) {
            refuse(where, "`component` and `widget` both choose the field's control; give one");
        }
        for (DocNodePtr const& props : field.partList("props")) {
            for (auto const& [prop, value] : props->props) {
                if (!component.props.contains(prop)) {
                    refuse(value->path, "the component `" + component.name + "` declares no prop `" + prop + "`");
                }
            }
        }
    }

    // The screen head's parts: its status badges, then its actions.
    [[nodiscard]] std::vector<DocNodePtr> headParts() const {
        std::vector<DocNodePtr> parts = _doc->status;
        parts.insert(parts.end(), _doc->actions.begin(), _doc->actions.end());
        return parts;
    }

    // A list of `actionButton` parts: each id once, and each emphasis one of a button's.
    static void checkActionButtons(std::span<DocNodePtr const> buttons, std::string const& path) {
        static constexpr std::array<std::string_view, 3> kEmphasis = {"normal", "primary", "danger"};
        std::set<std::string_view> ids;
        for (DocNodePtr const& button : buttons) {
            if (!ids.insert(button->id).second) {
                refuse(pointerAppend(button->path, "id"),
                       "the action id `" + button->id + "` is used twice in " + path + "; each action has its own");
            }
            checkChoice(*button, "emphasis", kEmphasis);
        }
    }

    // A literal sizing of one dimension: `"content"`, `{"fixed": n}` (n of 0 or more) or `{"stretch": w}` (w of 1 or
    // more). Answers the fixed amount, if it is one.
    static std::optional<std::int64_t> sizingDimension(Value const& value, std::string const& path) {
        static constexpr std::string_view kForms =
            "`sizing` is `\"content\"`, `{\"fixed\": n}`, `{\"stretch\": w}`, or a record of `width` and `height` "
            "of them";
        if (std::string const* const text = value.asString(); text != nullptr && *text == "content") {
            return std::nullopt;
        }
        ValueRecord const* const record = value.asRecord();
        if (record != nullptr && record->size() == 1) {
            auto const& [form, amount] = *record->begin();
            std::int64_t const* const units = amount.asInt();
            if (form == "fixed" && units != nullptr && *units >= 0) {
                return *units;
            }
            if (form == "stretch" && units != nullptr && *units >= 1) {
                return std::nullopt;
            }
        }
        refuse(path, std::string{kForms});
    }

    // A node's size keys: each bound a literal integer of 0 or more, a minimum no more than its maximum, no bound on
    // an overlay or a custom node; and a literal `sizing` one of the forms the interpreter reads, with a fixed amount
    // inside its dimension's bounds. A sizing that is not literal is read at mount.
    static void checkLayoutKeys(DocNode const& node) {
        static constexpr std::array<std::string_view, 4> kBounds = {"minWidth", "maxWidth", "minHeight", "maxHeight"};
        std::array<std::optional<std::int64_t>, 4> bounds{};
        for (std::size_t index = 0; index < kBounds.size(); ++index) {
            std::string_view const key = kBounds.at(index);
            ExprPtr const bound = node.prop(key);
            if (bound == nullptr) {
                continue;
            }
            if (node.kind == "dialog" || node.kind == "drawer") {
                refuse(bound->path, "an overlay is sized by the frontend, not by its container");
            }
            if (node.kind == "custom") {
                refuse(bound->path,
                       "a custom node carries no common properties, so it takes no `" + std::string{key} + "`");
            }
            std::int64_t const* const units =
                bound->kind == ExprKind::Literal ? bound->literal.asInt() : static_cast<std::int64_t const*>(nullptr);
            if (units == nullptr || *units < 0) {
                refuse(bound->path, "`" + std::string{key} + "` is a literal integer of 0 or more, in backend units");
            }
            bounds.at(index) = *units;
        }
        for (std::size_t axis = 0; axis < 2; ++axis) {
            auto const& minimum = bounds.at(2 * axis);
            auto const& maximum = bounds.at((2 * axis) + 1);
            if (minimum && maximum && *minimum > *maximum) {
                refuse(pointerAppend(node.path, kBounds.at(2 * axis)),
                       "`" + std::string{kBounds.at(2 * axis)} + "` is more than `" +
                           std::string{kBounds.at((2 * axis) + 1)} + "`");
            }
        }
        ExprPtr const sizing = node.prop("sizing");
        if (sizing == nullptr || sizing->kind != ExprKind::Literal) {
            return;
        }
        std::optional<std::int64_t> width;
        std::optional<std::int64_t> height;
        ValueRecord const* const record = sizing->literal.asRecord();
        if (record != nullptr && (record->contains("width") || record->contains("height"))) {
            for (auto const& [dimension, value] : *record) {
                if (dimension == "width") {
                    width = sizingDimension(value, sizing->path);
                } else if (dimension == "height") {
                    height = sizingDimension(value, sizing->path);
                } else {
                    refuse(sizing->path, "a `sizing` record has `width` and `height`; `" + dimension + "` is neither");
                }
            }
        } else {
            width = sizingDimension(sizing->literal, sizing->path);
        }
        for (auto const& [fixed, axis] : {std::pair{width, std::size_t{0}}, std::pair{height, std::size_t{1}}}) {
            auto const& minimum = bounds.at(2 * axis);
            auto const& maximum = bounds.at((2 * axis) + 1);
            if (fixed && (*fixed < minimum.value_or(*fixed) || *fixed > maximum.value_or(*fixed))) {
                refuse(sizing->path, std::string{"a fixed size has no room to bound: the "} +
                                         (axis == 0 ? "width" : "height") + " is fixed outside its bounds");
            }
        }
    }

    // A form's `columns`: a literal integer, 1 to 12, the `x-colspan` convention's full row.
    static void checkFormColumns(DocNode const& node) {
        ExprPtr const columns = node.prop("columns");
        if (columns == nullptr) {
            return;
        }
        std::int64_t const* const count =
            columns->kind == ExprKind::Literal ? columns->literal.asInt() : static_cast<std::int64_t const*>(nullptr);
        if (count == nullptr) {
            refuse(columns->path, "`columns` is a literal integer");
        }
        if (*count < 1 || *count > 12) {
            refuse(columns->path, "`columns` is 1 to 12; a form lays its fields in at least one column");
        }
    }

    static void checkChoice(DocNode const& node, std::string_view key, std::span<std::string_view const> choices) {
        std::string_view const value = node.name(key);
        if (!value.empty() && !listed(choices, value)) {
            refuse(pointerAppend(node.path, key),
                   "`" + std::string{value} + "` is not one of the values of `" + std::string{key} + "`");
        }
    }

    void checkCommands(Commands const& commands, CheckContext const& context, bool sequence = false) {
        CheckContext current = context;
        current.commands = true;
        for (Command const& cmd : commands) {
            checkCommand(cmd, current);
            if (sequence && cmd.kind == CommandKind::Run && !cmd.binding.empty() && !cmd.extra) {
                current.bindings.emplace_back(cmd.binding, Type::any());
            }
        }
    }

    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per command of ui/1.
    void checkCommand(Command const& cmd, CheckContext const& context) {
        std::string const where = cmd.path;
        switch (cmd.kind) {
            case CommandKind::Set:
                checkSet(cmd, context);
                break;
            case CommandKind::Reset: {
                NameEntry const& entry = require(context, cmd.target, where,
                                                 {NameCategory::State, NameCategory::Element}, "state or a dialog");
                if (entry.category == NameCategory::Element && entry.element->kind != "dialog" &&
                    entry.element->kind != "drawer") {
                    refuse(where, "`reset` restores state, or the state of a dialog; `" + cmd.target + "` is a " +
                                      entry.element->kind);
                }
                break;
            }
            case CommandKind::Run: {
                static_cast<void>(require(context, cmd.target, where, {NameCategory::Mutation}, "a mutation"));
                CheckContext body = context;
                if (cmd.extra) {
                    checkExpr(*cmd.extra, context);
                    if (!cmd.binding.empty()) {
                        body.bindings.emplace_back(cmd.binding, Type::any());
                    }
                }
                if (cmd.value) {
                    checkExpr(*cmd.value, body);
                }
                break;
            }
            case CommandKind::Refetch:
                static_cast<void>(require(context, cmd.target, where, {NameCategory::Query}, "a query"));
                break;
            case CommandKind::Open:
            case CommandKind::Close:
                if (cmd.kind == CommandKind::Close && cmd.target == "self") {
                    break;
                }
                checkDialog(cmd, context);
                break;
            case CommandKind::Focus:
                static_cast<void>(require(context, cmd.target, where, {NameCategory::Element}, "an element"));
                break;
            case CommandKind::ResetPrefs:
                if (cmd.target != "all" && !_doc->customize.contains(cmd.target)) {
                    refuse(where, "`" + cmd.target + "` is not a customization point of this document");
                }
                break;
            case CommandKind::If:
                checkCondition(*cmd.value, context, "the condition of `if`");
                checkCommands(cmd.commands, context);
                checkCommands(cmd.alternative, context);
                break;
            case CommandKind::Seq:
                checkCommands(cmd.commands, context, true);
                break;
            case CommandKind::All:
            case CommandKind::Navigate:
                if (cmd.value) {
                    checkExpr(*cmd.value, context);
                }
                if (cmd.extra) {
                    checkExpr(*cmd.extra, context);
                }
                checkCommands(cmd.commands, context);
                checkCommands(cmd.alternative, context);
                break;
            case CommandKind::CancelClose:
                if (!context.onClose) {
                    refuse(where, "`cancelClose` is valid only in `onClose`");
                }
                break;
            case CommandKind::Retry:
                if (!context.boundaryError) {
                    refuse(where, "`retry` is valid only inside a boundary's `error`");
                }
                break;
            case CommandKind::SignIn:
                static_cast<void>(require(context, cmd.target, where, {NameCategory::Mutation}, "a mutation"));
                break;
            case CommandKind::Upload:
                static_cast<void>(require(context, cmd.target, where, {NameCategory::Mutation}, "a mutation"));
                checkExpr(*cmd.value, context);
                break;
            case CommandKind::Download:
                checkExpr(*cmd.value, context);
                if (cmd.extra) {
                    checkExpr(*cmd.extra, context);
                }
                if (cmd.mediaType) {
                    checkExpr(*cmd.mediaType, context);
                }
                if (cmd.option == "preview") {
                    static_cast<void>(require(context, cmd.target, where, {NameCategory::State}, "state"));
                }
                break;
            case CommandKind::Return:
            case CommandKind::Host:
            case CommandKind::Notify:
            case CommandKind::SwitchBackend:
            case CommandKind::SetLocale:
            case CommandKind::SignOut:
            default:
                if (cmd.value) {
                    checkExpr(*cmd.value, context);
                }
                break;
        }
    }

    static void checkDialog(Command const& cmd, CheckContext const& context) {
        NameEntry const& entry = require(context, cmd.target, cmd.path, {NameCategory::Element}, "a dialog");
        if (entry.element->kind != "dialog" && entry.element->kind != "drawer") {
            refuse(cmd.path, "`" + cmd.target + "` is a " + entry.element->kind + ", not a dialog or drawer");
        }
    }

    void checkSet(Command const& cmd, CheckContext const& context) {
        auto const segments = splitPath(cmd.target);
        std::string const& head = segments->front();
        Type const value = checkExpr(*cmd.value, context);
        if (head == "app") {
            return;
        }
        NameEntry& entry =
            require(context, head, cmd.path, {NameCategory::State, NameCategory::Element}, "state or an input");
        if (entry.category == NameCategory::Element) {
            KindSpec const* const spec = findKind(entry.element->kind, false);
            if ((spec->flags & kWritable) == 0) {
                refuse(cmd.path, "`set` writes state or an input's draft; `" + head + "` is a " + entry.element->kind);
            }
            return;
        }
        auto const type = derivedType(entry, head);
        if (type && segments->size() == 1 && !compatible(*type, value)) {
            refuse(cmd.value->path, "`" + head + "` is " + type->name() + "; the value is " + value.name());
        }
    }

    /// The state and elements an expression reads, through the `let` entries it reads.
    void reads(Expr const& expr, ScopeInfo* scope, std::set<NameEntry const*>& out, std::vector<std::string>& locals,
               std::set<NameEntry const*>& seenLets) {
        if (expr.kind == ExprKind::Ref && std::ranges::find(locals, expr.name) == locals.end()) {
            NameEntry const* const entry = lookup(scope, expr.name);
            if (entry != nullptr &&
                (entry->category == NameCategory::State || entry->category == NameCategory::Element)) {
                out.insert(entry);
            } else if (entry != nullptr && entry->category == NameCategory::Let && seenLets.insert(entry).second) {
                std::vector<std::string> inner;
                reads(*entry->derivedFrom, entry->scope, out, inner, seenLets);
            }
        }
        bool const binds = expr.kind == ExprKind::Map || expr.kind == ExprKind::Filter;
        for (std::size_t index = 0; index < expr.args.size(); ++index) {
            if (binds && index == 1) {
                locals.push_back(expr.name);
            }
            reads(*expr.args.at(index), scope, out, locals, seenLets);
            if (binds && index == 1) {
                locals.pop_back();
            }
        }
        for (auto const& [name, field] : expr.fields) {
            static_cast<void>(name);
            reads(*field, scope, out, locals, seenLets);
        }
    }

    /// The state and elements a command list writes.
    static void writes(Commands const& commands, ScopeInfo* scope, std::set<NameEntry const*>& out) {
        for (Command const& cmd : commands) {
            if (cmd.kind == CommandKind::Set || cmd.kind == CommandKind::Reset) {
                auto const segments = splitPath(cmd.target);
                NameEntry const* const entry = lookup(scope, segments->front());
                if (entry != nullptr) {
                    out.insert(entry);
                }
            }
            writes(cmd.commands, scope, out);
            writes(cmd.alternative, scope, out);
        }
    }

    void checkWatchGraph() {
        std::size_t const count = _watches.size();
        std::vector<std::set<NameEntry const*>> observed(count);
        std::vector<std::set<NameEntry const*>> written(count);
        for (std::size_t index = 0; index < count; ++index) {
            auto const& [watch, scope] = _watches.at(index);
            std::vector<std::string> locals;
            std::set<NameEntry const*> seenLets;
            reads(*watch->on, scope, observed.at(index), locals, seenLets);
            writes(watch->body, scope, written.at(index));
        }
        // 0 unvisited, 1 on the current path, 2 finished.
        std::vector<int> state(count, 0);
        std::vector<std::size_t> trail;
        std::function<void(std::size_t)> visit = [&](std::size_t from) {
            state.at(from) = 1;
            trail.push_back(from);
            for (std::size_t to = 0; to < count; ++to) {
                bool edge = false;
                for (NameEntry const* entry : written.at(from)) {
                    edge = edge || observed.at(to).contains(entry);
                }
                if (!edge) {
                    continue;
                }
                if (state.at(to) == 1) {
                    std::string cycle;
                    auto const start = std::ranges::find(trail, to);
                    for (auto step = start; step != trail.end(); ++step) {
                        cycle += _watches.at(*step).first->path + " → ";
                    }
                    refuse(_watches.at(to).first->path,
                           "the watch graph has a cycle: " + cycle + _watches.at(to).first->path);
                }
                if (state.at(to) == 0) {
                    visit(to);
                }
            }
            trail.pop_back();
            state.at(from) = 2;
        };
        for (std::size_t index = 0; index < count; ++index) {
            if (state.at(index) == 0) {
                visit(index);
            }
        }
    }

    // The effect checks below read only what the catalog says about each action. They warn only where the
    // effects certainly cover, or certainly miss, what the document declares, because a warning a document cannot
    // act on would teach its author to ignore the rest.

    /// The effects as a phrase, such as "`changes T_Row`, `none`".
    [[nodiscard]] static std::string describeEffects(std::vector<ActionEffect> const& effects) {
        std::string out;
        for (ActionEffect const& effect : effects) {
            out += (out.empty() ? "`" : ", `") + effect.kind + (effect.record ? " " + *effect.record : "") + "`";
        }
        return out;
    }

    /// Whether an action's effects change anything; `none` alone does not.
    [[nodiscard]] static bool changesData(ActionInfo const& info) {
        return info.effects &&
               std::ranges::any_of(*info.effects, [](ActionEffect const& effect) { return effect.kind != "none"; });
    }

    void warn(LoadWarningKind kind, std::string path, std::string text) {
        _warnings.push_back(LoadWarning{.kind = kind, .path = std::move(path), .text = std::move(text)});
    }

    void checkEffects() {
        if (!_options->catalog.action) {
            return;
        }
        for (auto const& entry : _mutations) {
            checkMutationEffects(*entry.first);
        }
        for (auto const& [watch, scope] : _watches) {
            checkWatchFeedback(*watch, scope);
        }
    }

    void checkMutationEffects(MutationDecl const& mutation) {
        auto const action = _options->catalog.action(mutation.action);
        if (!action) {
            return;
        }
        if (!action->effects && !refetches(mutation.onSuccess)) {
            warn(LoadWarningKind::Uncovered, mutation.path,
                 std::format("nothing refreshes after `{}` succeeds: `{}` declares no effects, and no `refetch` "
                             "follows it; declare what it changes, or `effects::none` when it changes nothing",
                             mutation.name, mutation.action));
        }
    }

    /// Whether a command list refetches a query, which is a refresh the document declares itself.
    [[nodiscard]] static bool refetches(Commands const& commands) {
        return std::ranges::any_of(commands, [](Command const& cmd) {
            return cmd.kind == CommandKind::Refetch || refetches(cmd.commands) || refetches(cmd.alternative);
        });
    }

    // Another client's change refetches the watched query, the watch runs the mutation, and its change reaches the
    // other client, which may do the same. Only `"source": "user"` keeps a refetch from running the watch.
    void checkWatchFeedback(WatchDecl const& watch, ScopeInfo* scope) {
        if (watch.source == WatchSource::User) {
            return;
        }
        std::vector<std::string> locals;
        std::set<NameEntry const*> seenLets;
        auto const query = readQuery(*watch.on, scope, locals, seenLets);
        if (!query) {
            return;
        }
        if (auto const mutation = runsWriter(watch.body, scope)) {
            warn(LoadWarningKind::WatchFeedback, watch.path,
                 std::format("the watch reads the query `{}` and runs `{}`, which changes data: another client's "
                             "change refetches the query and runs it again; give the watch \"source\": \"user\"",
                             *query, *mutation));
        }
    }

    /// The first query whose value an expression reads, directly or through a `let`.
    [[nodiscard]] static std::optional<std::string> readQuery(Expr const& expr, ScopeInfo* scope,
                                                              std::vector<std::string>& locals,
                                                              std::set<NameEntry const*>& seenLets) {
        if (expr.kind == ExprKind::Query) {
            return expr.name;
        }
        if (expr.kind == ExprKind::Ref && std::ranges::find(locals, expr.name) == locals.end()) {
            NameEntry const* const entry = lookup(scope, expr.name);
            if (entry != nullptr && entry->category == NameCategory::Query) {
                return expr.name;
            }
            if (entry != nullptr && entry->category == NameCategory::Let && seenLets.insert(entry).second) {
                std::vector<std::string> inner;
                if (auto found = readQuery(*entry->derivedFrom, entry->scope, inner, seenLets)) {
                    return found;
                }
            }
        }
        bool const binds = expr.kind == ExprKind::Map || expr.kind == ExprKind::Filter;
        for (std::size_t index = 0; index < expr.args.size(); ++index) {
            if (binds && index == 1) {
                locals.push_back(expr.name);
            }
            auto found = readQuery(*expr.args.at(index), scope, locals, seenLets);
            if (binds && index == 1) {
                locals.pop_back();
            }
            if (found) {
                return found;
            }
        }
        for (auto const& [name, field] : expr.fields) {
            static_cast<void>(name);
            if (auto found = readQuery(*field, scope, locals, seenLets)) {
                return found;
            }
        }
        return std::nullopt;
    }

    /// The first mutation a command list runs whose action changes data.
    [[nodiscard]] std::optional<std::string> runsWriter(Commands const& commands, ScopeInfo* scope) const {
        for (Command const& cmd : commands) {
            bool const runs =
                cmd.kind == CommandKind::Run || cmd.kind == CommandKind::Upload || cmd.kind == CommandKind::SignIn;
            NameEntry const* const entry = runs ? lookup(scope, cmd.target) : nullptr;
            if (entry != nullptr && entry->mutation != nullptr) {
                auto const action = _options->catalog.action(entry->mutation->action);
                if (action && changesData(*action)) {
                    return cmd.target;
                }
            }
            if (auto found = runsWriter(cmd.commands, scope)) {
                return found;
            }
            if (auto found = runsWriter(cmd.alternative, scope)) {
                return found;
            }
        }
        return std::nullopt;
    }

    Document const* _doc;
    LoadOptions const* _options;
    std::deque<ScopeInfo> _scopes;
    std::map<DocNode const*, ScopeInfo*> _scopeOf;
    std::map<DocNode const*, ScopeInfo*> _innerOf;
    std::map<std::string, QueryDecl const*, std::less<>> _queries;
    std::map<std::string, DocNode const*, std::less<>> _elements;
    std::vector<std::pair<WatchDecl const*, ScopeInfo*>> _watches;
    std::vector<std::pair<MutationDecl const*, ScopeInfo*>> _mutations;
    std::vector<std::pair<QueryDecl const*, ScopeInfo*>> _queryScopes;
    std::vector<LoadWarning> _warnings;
    std::vector<std::string> _deriving;
    // NOLINTEND(misc-no-recursion)
};

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per kind of name.
inline std::expected<Type, std::string> ScopeResolver::ref(std::string_view head,
                                                           std::span<std::string const> path) const {
    for (auto const& [name, type] : std::views::reverse(_context.bindings)) {
        if (name == head) {
            return typeAlong(type, head, path);
        }
    }
    if (head == "event") {
        if (!_context.commands) {
            return std::unexpected(std::string{"`event` is read only inside a command list"});
        }
        return Type::any();
    }
    if (head == "app") {
        return Type::any();
    }
    if (head == "boundary") {
        if (!_context.boundaryError || path.size() != 1 || path.front() != "error") {
            return std::unexpected(std::string{"`boundary.error` is read only inside a boundary's `error`"});
        }
        return Type::of(TypeKind::String, true);
    }
    NameEntry* const entry = DocumentValidator::lookup(_context.scope, head);
    if (entry == nullptr) {
        return std::unexpected("`" + std::string{head} + "` does not resolve in this scope");
    }
    switch (entry->category) {
        case NameCategory::Param:
            return typeAlong(entry->type, head, path);
        case NameCategory::State:
        case NameCategory::Let: {
            auto type = _validator.derivedType(*entry, head);
            if (!type) {
                return type;
            }
            return typeAlong(*type, head, path);
        }
        case NameCategory::Query:
            return Type::any();
        case NameCategory::Mutation:
            return std::unexpected("`" + std::string{head} +
                                   "` is a mutation; read it with `result`, `pending` or "
                                   "`error`");
        case NameCategory::Model:
            return std::unexpected("`" + std::string{head} + "` is a model alias, not a value");
        case NameCategory::Transfer:
            if (path.empty() || !listed(kTransferValues, path.front())) {
                return std::unexpected("the transfer `" + std::string{head} +
                                       "` is read as `progress`, `pending` or `error`");
            }
            return Type::any();
        case NameCategory::Element:
        default: {
            KindSpec const* const spec = findKind(entry->element->kind, false);
            if (path.empty()) {
                return std::unexpected("the element `" + std::string{head} +
                                       "` is read through one of its values, "
                                       "such as `" +
                                       std::string{head} + ".visible`");
            }
            std::string const& value = path.front();
            bool const open = (spec->flags & kOpenValues) != 0;
            bool const known = listed(kAnyValues, value) || listed(spec->values, value) || open;
            if (!known) {
                return std::unexpected("the " + entry->element->kind + " `" + std::string{head} + "` has no value `" +
                                       value + "`");
            }
            bool const field = open && !listed(kAnyValues, value) && !listed(spec->values, value);
            Type const type = field ? Type::any() : elementValueType(value);
            return typeAlong(type, value, path.subspan(1));
        }
    }
}

inline std::expected<Type, std::string> ScopeResolver::row(std::span<std::string const> path) const {
    static_cast<void>(path);
    for (ScopeInfo const* scope = _context.scope; scope != nullptr; scope = scope->parent) {
        if (scope->row) {
            return Type::any();
        }
    }
    return std::unexpected(std::string{"`row` is read only inside a forEach or table row"});
}

inline std::expected<Type, std::string> ScopeResolver::control(ExprKind form, std::string_view name) const {
    if (form == ExprKind::Can && name.starts_with("host:")) {
        return Type::of(TypeKind::Bool);
    }
    NameEntry const* const entry = DocumentValidator::lookup(_context.scope, name);
    bool const query = entry != nullptr && entry->category == NameCategory::Query;
    bool const mutation = entry != nullptr && entry->category == NameCategory::Mutation;
    if (form == ExprKind::Query && !query) {
        return std::unexpected("`" + std::string{name} + "` is not a query of this scope");
    }
    if (form == ExprKind::Result && !mutation) {
        return std::unexpected("`" + std::string{name} + "` is not a mutation of this scope");
    }
    if (!query && !mutation) {
        return std::unexpected("`" + std::string{name} + "` is not a query or mutation of this scope");
    }
    if (form == ExprKind::Pending || form == ExprKind::Can) {
        return Type::of(TypeKind::Bool);
    }
    if (form == ExprKind::Error || form == ExprKind::ErrorKind) {
        return Type::of(TypeKind::String, true);
    }
    return Type::any();
}

inline std::expected<Type, std::string> ScopeResolver::pref(std::string_view point,
                                                            std::span<std::string const> path) const {
    auto const& points = _validator.document().customize;
    auto const found = points.find(point);
    if (found == points.end()) {
        return std::unexpected("`" + std::string{point} + "` is not a customization point of this document");
    }
    return typeAlong(found->second.type, point, path);
}

}  // namespace detail

/// @brief Parses a UI document and runs its load-time validation (spec 5 §3).
///
/// A document is refused when it does not parse, when a reference does not resolve, when a type does not check,
/// when the `let` or `watch` graph has a cycle, when an expression exceeds 256 nodes, when a node kind, command or
/// function is not in the vocabulary, when a document declares `invalidates`, `patch` or `refreshOn`, when a
/// must-understand key is unknown, or when a rule of spec 5 or spec 6 fails; the list is in
/// `docs/spec/ui/document.md`. The rules that need catalog facts run when @p options supplies them.
/// @param json    The document's JSON.
/// @param options The catalog facts.
/// @return The document, with what validation found that does not refuse it in `Document::warnings`, or the first
///         refusal, naming the document id, the JSON Pointer and the reason.
[[nodiscard]] inline std::expected<Document, Refusal> loadDocument(Json const& json, LoadOptions const& options = {}) {
    std::string documentId;
    if (auto const* const object = std::get_if<Json::object_t>(&json.data)) {
        auto const* const found = object->find("id");
        if (found != object->end()) {
            if (auto const* const text = std::get_if<std::string>(&found->second.data)) {
                documentId = *text;
            }
        }
    }
    try {
        Document doc = detail::DocumentParser{}.parse(json);
        detail::DocumentValidator validator{doc, options};
        validator.run();
        doc.warnings = validator.takeWarnings();
        return doc;
    } catch (detail::DocumentRefused const& refused) {
        Refusal refusal = refused.refusal();
        refusal.document = documentId;
        return std::unexpected(std::move(refusal));
    }
}

/// @brief Parses JSON text as a UI document and runs its load-time validation.
/// @param text    The document's JSON text.
/// @param options The catalog facts.
/// @return The document, or the refusal; text that is not JSON is refused at the root.
[[nodiscard]] inline std::expected<Document, Refusal> loadDocumentText(std::string_view text,
                                                                       LoadOptions const& options = {}) {
    auto json = parseJson(text);
    if (!json) {
        return std::unexpected(Refusal{.document = {}, .path = "", .reason = std::move(json.error())});
    }
    return loadDocument(*json, options);
}

/// @brief The view node kinds of `ui/1`.
/// @return Their `kind` spellings.
[[nodiscard]] inline std::vector<std::string_view> nodeKinds() {
    std::vector<std::string_view> kinds;
    for (detail::KindSpec const& spec : detail::kKinds) {
        if ((spec.flags & detail::kPart) == 0) {
            kinds.push_back(spec.kind);
        }
    }
    return kinds;
}

}  // namespace morph::ui

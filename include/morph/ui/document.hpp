// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <morph/export.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "expression.hpp"
#include "page_layout.hpp"
#include "shell_parts.hpp"
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
    /// @brief The JSON paths at which the result holds the record; `""` is the result itself. Empty when `opaque`.
    std::vector<std::string> paths;
    /// @brief The paths of members that open the entry: while any is set, a row the entry does not hold may come
    ///        to belong to it, so every change of the record marks it.
    std::vector<std::string> open;
    /// @brief The paths of members that make the entry partial: while any is set, an `updates` effect on the
    ///        record marks the entry instead of merging into it.
    std::vector<std::string> partial;
    /// @brief The result holds the record at no path a key can be read at: every change of the record marks the
    ///        entry, which is never merged into, and its reply keeps fractional numbers as decimals.
    bool opaque = false;

    /// @brief Memberwise equality.
    /// @param lhs One entry.
    /// @param rhs The other.
    /// @return True when record, key field, paths, open and partial members and opacity match.
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
[[nodiscard]] MORPH_API std::function<std::optional<std::string>(std::string_view)> documentLookup(
    std::map<std::string, std::string, std::less<>> documents);

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
    /// @brief Whether the document names a type; a declared `any` is a type, and only a state with none takes its
    ///        init's.
    bool typeDeclared = false;
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
    SetContext,     ///< `{"setContext": e}`
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
    /// @brief The second expression: `run`'s `each`, `navigate`'s `route`, `download`'s suggested `name`,
    ///        `focus`'s `row`.
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
    /// @brief The version the element first ships in (`forms::FieldVersion`), as written; empty for the first
    ///        version. Only an element with an `id` carries one.
    std::string since;
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

    /// @brief A literal flag whose absence means something other than `false`.
    /// @param key    The key.
    /// @param absent The value when the key is absent.
    /// @return Its value; @p absent when the key is absent.
    [[nodiscard]] bool flag(std::string_view key, bool absent) const {
        auto const found = flags.find(key);
        return found == flags.end() ? absent : found->second;
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

/// @brief A declaration a `focus` command may reach: an element's node, and how many row keys key it.
struct FocusCandidate {
    /// @brief The declaring node's JSON Pointer.
    std::string path;
    /// @brief How many row scopes enclose the declaration (within its document or component fallback).
    std::size_t rows = 0;
};

/// @brief Where a `focus` command's target resolved: the declarations of that name it may reach. At most one of
///        them is mounted at a time; the command focuses the one that is.
struct FocusTarget {
    /// @brief The declarations, in document order.
    std::vector<FocusCandidate> candidates;
    /// @brief Whether the command gives `row`, whose value replaces the innermost row key of each candidate.
    bool row = false;
};

/// @brief The most steps a guided tour has.
inline constexpr std::size_t kMaxTourSteps = 50;

/// @brief The longest a guided tour's id is, in characters (`[a-z0-9-]`, so bytes too).
inline constexpr std::size_t kMaxTourIdBytes = 64;

/// @brief One step of a guided tour, as the app document declares it (`docs/spec/ui/tours.md`).
struct TourStepDecl {
    /// @brief The step's JSON Pointer, where a refusal about it points.
    std::string path;
    /// @brief The step's title: a text expression in the app scope.
    ExprPtr title;
    /// @brief The step's body: a text expression in the app scope; `nullptr` when none is declared.
    ExprPtr body;
    /// @brief The screen the step opens before it shows; empty for none.
    std::string screen;
    /// @brief The params it opens that screen with: literal values.
    ValueRecord params;
    /// @brief The test id of the part the step rings; empty for a card with nothing ringed.
    std::string target;
    /// @brief The screen whose opening moves the tour on; empty for a step that waits for Next.
    std::string advanceOn;
};

/// @brief A guided tour, as the app document declares it.
struct TourDecl {
    /// @brief The tour's JSON Pointer, where a refusal about it points.
    std::string path;
    /// @brief Its id: `[a-z0-9-]+`, at most `kMaxTourIdBytes`, unique among the app's tours.
    std::string id;
    /// @brief The screen it belongs to; empty for a general tour, offered on every screen.
    std::string screen;
    /// @brief Its title: a text expression in the app scope.
    ExprPtr title;
    /// @brief Its summary: a text expression in the app scope; `nullptr` when none is declared.
    ExprPtr summary;
    /// @brief Its steps, one or more and at most `kMaxTourSteps`.
    std::vector<TourStepDecl> steps;
    /// @brief A runtime tour's revision on the server (`ListedTour::revision`), so a client tells a replaced tour from
    ///        one whose texts changed; 0 for an app-document tour.
    std::uint64_t revision = 0;
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
    /// @brief The title as runs, drawn in place of `title` in a shell's top bar by a client that knows it; `nullptr`
    ///        when none is declared.
    ExprPtr titleSpans;
    /// @brief The eyebrow expression of the screen's head, the line above the title; `nullptr` when none is declared.
    ExprPtr eyebrow;
    /// @brief The eyebrow as runs, drawn in place of `eyebrow` by a client that knows it; `nullptr` when none is
    ///        declared.
    ExprPtr eyebrowSpans;
    /// @brief The subtitle expression of the screen's head; `nullptr` when none is declared.
    ExprPtr subtitle;
    /// @brief The subtitle as runs, drawn in place of `subtitle` by a client that knows it; `nullptr` when none is
    ///        declared.
    ExprPtr subtitleSpans;
    /// @brief Where the head's lead (subtitle and badges) sits: an expression of `below`, `inline` or `end`;
    ///        `nullptr` when none is declared, which is `below`.
    ExprPtr leadPlacement;
    /// @brief The text role of the head's title: an expression of `heading` or `headline`; `nullptr` when none is
    ///        declared, which is `heading`.
    ExprPtr titleRole;
    /// @brief The surface the shell draws the top bar on while the screen is shown: an expression of a surface's
    ///        name; `nullptr` when none is declared.
    ExprPtr headSurface;
    /// @brief The surface the shell draws the page area on while the screen is shown: an expression of a surface's
    ///        name; `nullptr` when none is declared, which is the built-in `page` surface.
    ExprPtr pageSurface;
    /// @brief The app document's principal placement, `sidebar` or `head`; empty for the default, `sidebar`.
    std::string principalPlacement;
    /// @brief The shell's declared parts, the app document's only; their reads are queries of `scope`.
    std::optional<ShellParts> shell;
    /// @brief The head's status badges: `headBadge` parts, in order.
    std::vector<DocNodePtr> status;
    /// @brief The head's actions: `actionButton` parts, in order.
    std::vector<DocNodePtr> actions;
    /// @brief The head's own menu: `headMenuItem` parts, in order.
    std::vector<DocNodePtr> menu;
    /// @brief The accessible name of the head menu's trigger; `nullptr` for the default, "More actions".
    ExprPtr menuLabel;
    /// @brief The head's tools: `headTool` parts, a search field or a select each, in order.
    std::vector<DocNodePtr> tools;
    /// @brief The app document's counts on the navigator's entries: `navigationBadge` parts, in order; empty for a
    ///        screen, which declares none.
    std::vector<DocNodePtr> navigation;
    /// @brief The app document's guided tours, in declaration order; empty for a screen, which declares none.
    std::vector<TourDecl> tours;
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
    /// @brief The app document's date pattern (`formatting.date`), which `mountApp` gives the session; empty keeps
    ///        ISO, and a screen has none.
    std::string datePattern;
    /// @brief The app document's date-and-time pattern (`formatting.dateTime`), as `datePattern`.
    std::string dateTimePattern;
    /// @brief Queries started before the screen mounts.
    std::vector<std::string> prefetch;
    /// @brief Custom components, by name.
    std::map<std::string, Component, std::less<>> components;
    /// @brief Customization points, by name.
    std::map<std::string, CustomizePoint, std::less<>> customize;
    /// @brief The view; `nullptr` for the app document.
    DocNodePtr view;
    /// @brief The static type of each `dateTimeInput`'s `value` that the loader could decide: `String` or
    ///        `Timestamp`, keyed by the node. A node whose value type is `any` or `key` has no entry.
    std::map<DocNode const*, TypeKind> dateValueKinds;
    /// @brief Each `focus` command's resolved target, keyed by the command's JSON Pointer.
    std::map<std::string, FocusTarget, std::less<>> focusTargets;
    /// @brief What load-time validation found that does not refuse the document, in the order found. Empty when
    ///        the catalog answers no actions.
    std::vector<LoadWarning> warnings;
};

namespace detail {

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
/// @brief The scope it creates is a row scope, where `row` is valid. On a part without `kScope`, the part's inner keys
///        are read in a row scope of their own that declares nothing: a chart series' per-point keys.
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

/// @brief Why an action bar takes no `gap`, and a horizontal `forEach` directly in it takes none either.
inline constexpr char const* kActionBarGapRefusal =
    "an action bar's gap is the theme's `actions.gap`; set it there, not on the row";

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

/// @brief Whether a list of names holds one.
/// @param names The list.
/// @param name  The candidate.
/// @return Whether @p name is in @p names.
[[nodiscard]] inline bool listed(std::span<std::string_view const> names, std::string_view name) {
    return std::ranges::find(names, name) != names.end();
}

/// @brief A page node's geometry, from its literal keys; the loader has checked nothing about it yet.
/// @param node A `page` node.
/// @return The geometry.
/// @throws DocumentRefused when a key is not a literal length, or the geometry is not valid.
[[nodiscard]] MORPH_API PageGeometry pageGeometryOf(DocNode const& node);

/// @brief A page region's geometry, from its literal keys.
/// @param node A `pageRegion` node.
/// @param page The sheet it is on.
/// @return The geometry.
/// @throws DocumentRefused when a key is not a literal length, the anchor is unknown, or the region is not valid on
///         the sheet.
[[nodiscard]] MORPH_API RegionGeometry regionGeometryOf(DocNode const& node, PageGeometry const& page);

/// @brief Looks up a node kind or a part kind.
/// @param kind The kind.
/// @param part Whether a part kind is wanted.
/// @return The spec, or `nullptr`.
[[nodiscard]] MORPH_API KindSpec const* findKind(std::string_view kind, bool part);

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
[[nodiscard]] MORPH_API std::expected<Document, Refusal> loadDocument(Json const& json,
                                                                      LoadOptions const& options = {});

/// @brief Parses JSON text as a UI document and runs its load-time validation.
/// @param text    The document's JSON text.
/// @param options The catalog facts.
/// @return The document, or the refusal; text that is not JSON is refused at the root.
[[nodiscard]] MORPH_API std::expected<Document, Refusal> loadDocumentText(std::string_view text,
                                                                          LoadOptions const& options = {});

/// @brief Loads one tour's JSON text, as one entry of the app document's `tours` is read: what a client makes of a
///        runtime tour that `morph.tours.list` answers. Its shape is checked; its texts are parsed, not checked
///        against the app scope, which the server did when the tour was added.
/// @param text The tour's JSON text.
/// @return The tour, or the refusal, its path a pointer into the tour.
[[nodiscard]] MORPH_API std::expected<TourDecl, Refusal> loadTourText(std::string_view text);

/// @brief The view node kinds of `ui/1`.
/// @return Their `kind` spellings.
[[nodiscard]] MORPH_API std::vector<std::string_view> nodeKinds();

/// @brief The part kinds of `ui/1`: the records inside a node or the head of a document (an action, a menu entry, a
///        tool, a column, a series, …).
/// @return Their `kind` spellings.
[[nodiscard]] MORPH_API std::vector<std::string_view> partKinds();

/// @brief The top-level members of an action's request schema that hold a table query: each property marked
///        `x-table` (`table::TableQuery::schemaMarker()`), in name order. A table over the action writes its
///        query into the one member this names.
/// @param schemaJson The schema, as `Catalog::schema` answers it.
/// @return The members' names; empty when the text is not a schema object or no property is marked.
[[nodiscard]] MORPH_API std::vector<std::string> tableQueryMembers(std::string_view schemaJson);

}  // namespace morph::ui

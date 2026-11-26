// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <charconv>
#include <chrono>
#include <core/async/StopToken.hpp>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <expected>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <morph/core/completion.hpp>
#include <morph/export.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/scheduler.hpp>
#include <morph/reactive/scope.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/base64.hpp>
#include <morph/ui/dev_tools.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/expression.hpp>
#include <morph/ui/value.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

/// @file
/// @brief The UI document interpreter: `Interpreter::mount` turns a loaded `Document` into reactive state and a view
///        tree, mounts it through `Mounted` on any `IViewBackend`, and runs its queries, mutations, relations and
///        commands. Model calls go through a `Dispatcher` (the bridge's model-free seams in an application, a
///        scripted fake in a test); what only the application can do goes through an `InterpreterHost`; what
///        screens share — the query cache, the principal's permissions, the formatting facts — is a `Session`.

namespace morph {

namespace bridge {
class Bridge;
enum class BindSharing : std::uint8_t;
}  // namespace bridge

namespace wire {
struct ChangesBody;
struct EventBody;
struct InstanceTopic;
struct WatchBody;
}  // namespace wire

namespace effects {
struct RecordChange;
}  // namespace effects

namespace forms {
class CollectionSession;
class FieldState;
class FieldView;
class FormSession;
class WizardSession;
struct FieldSpec;
struct FormSessionOptions;
struct FormViewOptions;
}  // namespace forms

namespace table {
class ITableStateStore;
struct SaveResult;
struct TableColumn;
struct TableDescription;
struct TableStateKey;
}  // namespace table

}  // namespace morph

namespace morph::ui {

class TableStates;

class Mounted;
class GridController;
class ModelGridSource;
struct GridColumn;
struct GridOptions;
struct ChartRange;

// ---------------------------------------------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------------------------------------------

/// @brief How one model call is sent.
struct CallOptions {
    /// @brief Ask the server to evaluate the action's body instead of executing it (spec 5 §7, `ui-evaluate`).
    bool evaluate = false;
    /// @brief The mutation declares `"offline": "queue"`: a dispatcher with an offline queue holds a call that cannot
    ///        reach the server and settles the completion when the replay settles.
    bool queueOffline = false;
    /// @brief The caller's cancel: a stop request sends the wire's `cancel` for the call where the server takes one,
    ///        and rejects the call with `core::async::OperationCancelled`. A token with no stop state sends nothing.
    ///        An `evaluate` call ignores it, and so does a call a `QueueingDispatcher` replays. Holding a stop token,
    ///        `CallOptions` is not a literal type, and a channel built against it before it had one must recompile.
    ::core::async::StopToken stop;
};

/// @brief What a channel can say about the instance it is bound to.
struct ChannelFacts {
    /// @brief The instance's id on the server; `0` while the bind has not settled.
    std::uint64_t instanceId = 0;
    /// @brief The instance's primary key; empty for one with none.
    std::string primary;
};

/// @brief One bound model alias: the instance a document's alias names, and the calls made through it.
class ModelChannel {
public:
    ModelChannel() = default;
    ModelChannel(ModelChannel const&) = delete;
    ModelChannel& operator=(ModelChannel const&) = delete;
    ModelChannel(ModelChannel&&) = delete;
    ModelChannel& operator=(ModelChannel&&) = delete;
    virtual ~ModelChannel() = default;

    /// @brief Sends one action.
    /// @param actionId The action's registered type id.
    /// @param bodyJson The action's JSON body.
    /// @param options  How to send it.
    /// @return The reply's JSON text, delivered on the runtime's owner.
    [[nodiscard]] virtual async::Completion<std::string> execute(std::string const& actionId,
                                                                 std::string const& bodyJson, CallOptions options) = 0;

    /// @brief The instance it is bound to, as far as it knows.
    /// @return The facts; an empty set for a channel with no instance of its own to report.
    [[nodiscard]] virtual ChannelFacts facts() const { return {}; }
};

/// @brief The interpreter's view of the bridge: binds a model by its type id, privately or to a shared instance.
class Dispatcher {
public:
    Dispatcher() = default;
    Dispatcher(Dispatcher const&) = delete;
    Dispatcher& operator=(Dispatcher const&) = delete;
    Dispatcher(Dispatcher&&) = delete;
    Dispatcher& operator=(Dispatcher&&) = delete;
    virtual ~Dispatcher() = default;

    /// @brief Binds a model.
    /// @param typeId      The model's registered type id.
    /// @param sharing     Private, or shared.
    /// @param instanceKey For a shared binding, the instance key; empty for the model's shared singleton.
    /// @return The bound channel. The caller holds it as long as it makes calls through it.
    [[nodiscard]] virtual std::shared_ptr<ModelChannel> bind(std::string const& typeId, bridge::BindSharing sharing,
                                                             std::string const& instanceKey) = 0;
};

/// @brief Evaluates an action body without running it: the model's type id, the action id and the JSON body in, the
///        query's value out (spec 5 §7). `ui::evaluatorOver` builds one over the UI service's `ui-evaluate`.
using Evaluator = std::function<async::Completion<std::string>(std::string const& typeId, std::string const& actionId,
                                                               std::string const& bodyJson)>;

/// @brief A `Dispatcher` over the bridge's model-free seams: `bindByType`, `executeRaw` and the string-keyed
///        `attach`, through `RawHandler`.
///
/// An evaluation goes through the `Evaluator` it was given, since that envelope belongs to the UI service, not the
/// bridge; without one it is refused with a `std::runtime_error`. It has no offline queue of its own: `queueOffline`
/// is ignored unless a `QueueingDispatcher` wraps it. A shared binding without a key is bound unattached, which the
/// bridge refuses at the first call; the interpreter never binds one, since a `"shared"` alias is a stateless model's
/// and binds privately.
class MORPH_API BridgeDispatcher final : public Dispatcher {
public:
    /// @brief Dispatches through @p bridge, and evaluates through @p evaluator.
    /// @param bridge    The bridge. Borrowed: it must outlive every channel.
    /// @param cbExec    The executor replies are delivered on: the runtime's owner. Borrowed.
    /// @param evaluator What answers an `evaluate` query; empty to refuse them.
    BridgeDispatcher(bridge::Bridge& bridge MORPH_LIFETIMEBOUND, exec::IExecutor* cbExec MORPH_LIFETIMEBOUND,
                     Evaluator evaluator = {})
        : _bridge{&bridge}, _cbExec{cbExec}, _evaluator{std::move(evaluator)} {}

    /// @brief Binds through a `RawHandler`.
    /// @param typeId      The model's registered type id.
    /// @param sharing     Private, or shared.
    /// @param instanceKey For a shared binding, the key to attach to; empty to stay unattached.
    /// @return The channel; destroying it deregisters the binding.
    [[nodiscard]] std::shared_ptr<ModelChannel> bind(std::string const& typeId, bridge::BindSharing sharing,
                                                     std::string const& instanceKey) override;

private:
    class Channel;

    bridge::Bridge* _bridge;
    exec::IExecutor* _cbExec;
    Evaluator _evaluator;
};

// ---------------------------------------------------------------------------------------------------------------
// Host
// ---------------------------------------------------------------------------------------------------------------

/// @brief What `navigate` asks the host to open (spec 6 §16, §20).
struct NavigateRequest {
    /// @brief The screen id; empty when `route` is given.
    std::string screen;
    /// @brief The route, when the command named one instead of a screen.
    Value route;
    /// @brief The params.
    Value params;
    /// @brief `tab`, `dialog`, `drawer` or `window`; empty for the shell's choice.
    std::string as;
    /// @brief Whether the navigation filed a call (the command has `onResult` or `onCancel`), which the next mount of
    ///        the screen with these params adopts. Set by the host that filed it, `DocumentScreenHost`; a host that
    ///        handles such a navigation without mounting that screen settles the call.
    bool call = false;
};

/// @brief What an `upload` command asks the host to send, and the mutation that will reference the file, which is what
///        the server authorises the upload as (spec 6 §18).
struct UploadRequest {
    /// @brief What the user picked or dropped.
    Value file;
    /// @brief The model type of the mutation that will reference the file.
    std::string modelType;
    /// @brief That mutation's action.
    std::string action;
};

/// @brief A file whose bytes a document moves inline: `download` from bytes, `upload` into bytes.
struct InlineFile {
    /// @brief The file's name.
    std::string name;
    /// @brief Its media type; empty when unknown.
    std::string mediaType;
    /// @brief Its bytes.
    std::string bytes;
};

/// @brief The inline cap a session has unless it is given another: 1 MiB.
inline constexpr std::size_t kDefaultInlineFileBytes = std::size_t{1024} * 1024;

/// @brief The largest inline cap a session takes: base64 grows bytes by 4/3, and 64 KiB of an envelope are left for
///        the rest of the body, so the encoded file still fits `wire::kMaxEnvelopeBytes` (8 MiB).
inline constexpr std::size_t kMaxInlineFileBytes = ((std::size_t{8} * 1024 * 1024) - (std::size_t{64} * 1024)) * 3 / 4;

/// @brief What the application answers for the commands a document cannot carry out alone (spec 6). Every default
///        answers "not supported", which the interpreter reports as a diagnostic.
class MORPH_API InterpreterHost {
public:
    InterpreterHost() = default;
    InterpreterHost(InterpreterHost const&) = delete;
    InterpreterHost& operator=(InterpreterHost const&) = delete;
    InterpreterHost(InterpreterHost&&) = delete;
    InterpreterHost& operator=(InterpreterHost&&) = delete;
    virtual ~InterpreterHost() = default;

    // NOLINTBEGIN(performance-unnecessary-value-param,bugprone-easily-swappable-parameters): every default below
    // answers "not supported" and reads nothing; an override keeps the callbacks, so they are taken by value.

    /// @brief Opens another screen.
    /// @param request  What to open.
    /// @param onResult Called with the value a screen opened as a dialog, drawer or window returns.
    /// @param onCancel Called when such a screen closes without a value.
    /// @return Whether the host opened it.
    virtual bool navigate(NavigateRequest const& request, std::function<void(Value)> onResult,
                          std::function<void()> onCancel);

    /// @brief Ends the screen that ran `return` or `close: self`.
    /// @param screen The screen's id.
    /// @param result The value of `return`; `std::nullopt` for `close: self`.
    /// @return Whether the screen was opened as something that can end.
    virtual bool closeSelf(std::string const& screen, std::optional<Value> result);

    /// @brief Runs a command the application registered (`host/1`).
    /// @param name The command.
    /// @param args Its arguments; `null` when none.
    /// @return Whether the command exists.
    virtual bool hostCommand(std::string const& name, Value const& args);

    /// @brief Switches to a backend the application's source lists (`AppSource::switchBackend`, spec 6 §9).
    ///
    /// The switch is asynchronous: the host calls @p done once, on the runtime's owner, when it committed or failed.
    /// A failed switch leaves the old backend in place.
    /// @param name The backend.
    /// @param done Called once with nothing when the switch committed, or with why it failed. Not called when the
    ///        backend is not listed.
    /// @return Whether it is listed.
    virtual bool switchBackend(std::string const& name, std::function<void(std::expected<void, std::string>)> done);

    /// @brief The document reset a customization point, or all of them; the host forgets the stored choice.
    /// @param screen The screen's id.
    /// @param point  The point, or `all`.
    virtual void resetPrefs(std::string const& screen, std::string const& point);

    /// @brief Stores the active locale as a preference. The interpreter has already re-resolved every `t`.
    /// @param locale The BCP 47 locale.
    virtual void setLocale(std::string const& locale) { static_cast<void>(locale); }

    /// @brief Shows a transient message.
    /// @param text The message.
    /// @param role `info`, `success`, `warning` or `error`; empty for the default.
    /// @return Whether it was shown.
    virtual bool notify(std::string const& text, std::string const& role);

    /// @brief Installs the session the reply of a sign-in mutation carries, and reloads the catalog.
    /// @param reply The mutation's reply, secret fields included.
    /// @return Whether the host installed it.
    virtual bool signIn(Value const& reply);

    /// @brief Ends the session. The interpreter has already cleared the session's query cache.
    virtual void signOut() {}

    /// @brief Writes `app.name` with `set` (spec 6 §5).
    /// @param name  The app state's name.
    /// @param value The value.
    /// @return Whether the app document declares it.
    virtual bool setAppValue(std::string const& name, Value const& value);

    /// @brief Sends a file through the server's file side channel (`files/1`).
    /// @param file     What the user picked or dropped.
    /// @param progress Called with the progress, 0 to 1.
    /// @param done     Called once with the file's reference, or the failure's text.
    /// @return Whether the host can send files.
    virtual bool upload(Value const& file, std::function<void(Value)> progress,
                        std::function<void(std::expected<Value, std::string>)> done);

    /// @brief Sends a file through the server's file side channel, knowing the mutation that will reference it. The
    ///        interpreter calls this one; the default calls `upload(file, …)`, so a host that overrides only that
    ///        keeps working.
    /// @param request  The file and the mutation.
    /// @param progress Called with the progress, 0 to 1.
    /// @param done     Called once with the file's reference, or the failure's text.
    /// @return Whether the host can send files.
    virtual bool uploadFor(UploadRequest const& request, std::function<void(Value)> progress,
                           std::function<void(std::expected<Value, std::string>)> done);

    /// @brief Fetches a file by reference to save it, or to a local temporary reference for a viewer.
    /// @param file   The file's reference.
    /// @param option `dialog` to save, `preview` for a local reference.
    /// @param name   The suggested file name; `null` when none.
    /// @param done   Called once with the local reference (`preview`) or `null` (`dialog`), or the failure.
    /// @return Whether the host can fetch files.
    virtual bool download(Value const& file, std::string const& option, Value const& name,
                          std::function<void(std::expected<Value, std::string>)> done);

    /// @brief Reads a file's bytes for `upload` into bytes. A host reads the file's size first and refuses one over
    ///        @p maxBytes without reading it; the interpreter refuses a reply over the cap as well.
    /// @param file     What the user picked or dropped.
    /// @param maxBytes The session's inline cap.
    /// @param done     Called once with the file, or the failure's text.
    /// @return Whether the host can read files.
    virtual bool readFile(Value const& file, std::size_t maxBytes,
                          std::function<void(std::expected<InlineFile, std::string>)> done);

    /// @brief Saves a file a document holds the bytes of, for `download` from bytes.
    /// @param file   The file.
    /// @param option `dialog` to save, `preview` for a local reference.
    /// @param done   Called once with the local reference (`preview`) or `null` (`dialog`), or the failure.
    /// @return Whether the host can save files.
    virtual bool saveFile(InlineFile const& file, std::string const& option,
                          std::function<void(std::expected<Value, std::string>)> done);

    /// @brief The user chose at a customization point through a `customize` editor; the host stores the choice.
    /// @param screen The screen's id.
    /// @param point  The point.
    /// @param value  The choice.
    virtual void preferenceChanged(std::string const& screen, std::string const& point, Value const& value);

    /// @brief A call was refused with error kind `denied`: the permitted set is a snapshot, so the host asks the
    ///        server again and calls `Session::setPermitted`.
    virtual void permissionsStale() {}
    // NOLINTEND(performance-unnecessary-value-param,bugprone-easily-swappable-parameters)
};

// ---------------------------------------------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------------------------------------------

/// @brief Sends a `watch` or `unwatch` request for a session's cache (spec 8 §7): the kind, and the `WatchBody` as
///        JSON. `ui::watcherOver` builds one over a remote source's exchange.
using Watcher = std::function<async::Completion<std::string>(std::string const& kind, std::string const& bodyJson)>;

/// @brief What a `Session` is built with.
struct SessionOptions {
    /// @brief Timers for `debounce`, `refreshEvery` and the cache's eviction. Without one, `debounce` fetches at once,
    ///        `refreshEvery` never ticks and unused entries are kept. Borrowed.
    reactive::Scheduler* scheduler = nullptr;
    /// @brief The clock `staleTime` is measured on; empty uses `std::chrono::steady_clock`.
    std::function<std::chrono::milliseconds()> clock;
    /// @brief How long an entry no mounted query uses is kept (spec 5 §5: five minutes).
    std::chrono::milliseconds keepUnused{std::chrono::minutes{5}};
    /// @brief The catalog: `stateless` decides an alias's default instance; an action's `secretFields` are never
    ///        cached or kept in `result`.
    Catalog catalog;
    /// @brief Maps a failed call to its error kind (`denied`, `offline`, `timeout`, …); empty uses the default,
    ///        which knows the bridge's own failures and calls everything else `error`.
    std::function<std::string(std::exception_ptr const&)> classify;
    /// @brief Development mode: whether this session's screens record what the inspector reads. Off by default.
    DevelopmentTools developmentTools = DevelopmentTools::Off;
    /// @brief Sends the cache's watch changes to the server. Empty: the session still counts what its cache holds,
    ///        and sends nothing, which is right for a local source, whose replies carry every change it sees.
    Watcher watcher;
    /// @brief The most bytes a document moves inline in one file (`download` from bytes, `upload` into bytes, a
    ///        field component writing a base64 member); 1 to `kMaxInlineFileBytes`.
    std::size_t maxInlineFileBytes = kDefaultInlineFileBytes;
    /// @brief How long a model-backed table's visible rows must stay within one page before it fetches a page that
    ///        is not next to one it holds: a scroll that passes over pages fetches none of them. Waited for on
    ///        `scheduler` and measured on `clock` (`std::chrono::steady_clock` without one), so the two must
    ///        agree. Without a scheduler every fetch goes at once.
    std::chrono::milliseconds tableSettle{80};
    /// @brief While a model-backed table's order waits for a level, the least time between two of its refetches
    ///        after change events, waited for on `scheduler` and measured on `clock`: the first goes at once, the
    ///        next no sooner than this after it, whatever the number of events between.
    std::chrono::milliseconds tableAwaitedRefetch{250};
    /// @brief Where every table's persisted state is loaded from and saved to; with none, a table keeps its state
    ///        for as long as it is mounted. `PreferencesTableStateStore` keeps it in the application's preferences
    ///        store.
    std::shared_ptr<table::ITableStateStore> tableState;
    /// @brief How long a table waits for its stored state before its first query; past it the table starts from its
    ///        defaults.
    std::chrono::milliseconds tableStateTimeout{300};
    /// @brief How long a table's arrangement must stay unchanged before its state is saved, waited for on
    ///        `scheduler`; without one every change saves at once.
    std::chrono::milliseconds tableStateDebounce{500};
    /// @brief How long a filter menu waits after the last keystroke of its search, and after the last edit of a
    ///        condition, before it asks for values or sends the filter, waited for on `scheduler`; without one each
    ///        goes at once.
    std::chrono::milliseconds tableFilterDebounce{250};
    /// @brief The most rows a range selection of a model-backed table resolves; a longer range is refused in the
    ///        table's status line and selects nothing.
    std::size_t maxRangeRows = 10'000;
    /// @brief The most rows a table's copy puts on the clipboard; a larger copy is refused in the table's status line.
    std::size_t maxCopyRows = 10'000;
    /// @brief The most values one paste into a grid writes or drops; a larger block is refused in the grid's status
    ///        line and nothing is written.
    std::size_t maxPasteCells = 10'000;
    /// @brief The longest a model-backed table with `retainChanged` holds back the refetches its change events cause
    ///        while an action of this session that changes it runs, waited for on `scheduler`: within it a row the
    ///        action changed never leaves the view and comes back, past it other users' changes are not held up.
    std::chrono::milliseconds retainHold{2000};
    /// @brief The application's id: the `application` of every table-state key.
    std::string application;
};

/// @brief How long a table's status-line message stays, unless the next gesture on the table clears it first.
inline constexpr std::chrono::seconds kStatusMessageHold{6};

class Session;

namespace detail {

/// @brief The table states of a session built with @p options: over `tableState`, debounced by `tableStateDebounce`
///        on `scheduler`. A save that does not end `Saved` is said in the status line of every mounted table whose
///        key it names; a `PreferencesTableStateStore` is given the session's scheduler and `tableStateTimeout` to
///        wait on before it sends a save the server refused as busy again.
/// @param session The session the states belong to; its screens hear a failed save.
/// @param runtime The session's runtime, whose owner the states answer on.
/// @param options The session's options.
/// @return They; `nullptr` without a `tableState`.
[[nodiscard]] MORPH_API std::shared_ptr<TableStates> makeTableStates(Session& session,
                                                                     reactive::Runtime const& runtime,
                                                                     SessionOptions const& options);

/// @brief The text of a failure.
/// @param error The failure.
/// @return `what()`, or `unknown error`.
[[nodiscard]] MORPH_API std::string failureText(std::exception_ptr const& error);

/// @brief The default error kinds: the bridge's own failures, a server's refusal (`denied`), and `error` for the rest.
/// @param error The failure.
/// @return The kind.
[[nodiscard]] MORPH_API std::string defaultErrorKind(std::exception_ptr const& error);

/// @brief A value's JSON text, with record fields in name order: a cache key's canonical body.
/// @param value The value.
/// @return The text.
[[nodiscard]] MORPH_API std::string canonicalJson(Value const& value);

/// @brief Writes @p with over a row: the fields of a record over a record's, otherwise the whole row.
/// @param row  The row.
/// @param with The new content.
/// @return The new row.
[[nodiscard]] MORPH_API Value mergeRow(Value const& row, Value const& with);

/// @brief Why a patch left a query's value as it was.
enum class PatchMiss : std::uint8_t {
    NoList,  ///< The value, or its `at` member, is not a list: the patch can never apply, and is reported.
    NoRow,   ///< No row's `id` equals the key: the row is not loaded, which is not a fault.
};

/// @brief Replaces the row whose `id` equals @p key in a query's list of rows.
/// @param value      The query's value: the list itself, or a record holding it in member @p listMember.
/// @param listMember The record member holding the list; empty when @p value is the list.
/// @param key        The row's id.
/// @param with       The new content, merged over the row.
/// @return The new value, or why it is unchanged.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a list, a key and a row are distinct roles of one type.
[[nodiscard]] MORPH_API std::expected<Value, PatchMiss> patchRows(Value const& value, std::string_view listMember,
                                                                  Value const& key, Value const& with);

/// @brief Removes the named fields from a record, and from each record of a list.
/// @param value  The value.
/// @param fields The field names.
/// @return The value without them.
// NOLINTNEXTLINE(misc-no-recursion): one level per nested list or record of the value.
[[nodiscard]] MORPH_API Value withoutFields(Value const& value, std::vector<std::string> const& fields);

/// @brief One optimistic write still waiting for its reply.
struct OptimisticLayer {
    /// @brief The run that made it.
    std::uint64_t run = 0;
    /// @brief The record member holding the list; empty when the value is the list.
    std::string at;
    /// @brief The row's id.
    Value key;
    /// @brief What was written.
    Value with;
};

/// @brief One entry of the session's query cache (spec 5 §5): one request in flight, one value, one error.
struct MORPH_API CacheEntry {
    /// @brief An empty entry.
    /// @param runtime The runtime its signals belong to.
    explicit CacheEntry(reactive::Runtime& runtime)
        : value{runtime},
          hasValue{runtime, false},
          error{runtime},
          errorKind{runtime},
          refusal{runtime},
          pending{runtime, false} {}

    /// @brief The cache key: model type, instance, action and canonical body.
    std::string key;
    /// @brief The model type.
    std::string typeId;
    /// @brief The action.
    std::string action;
    /// @brief The body's JSON text.
    std::string body;
    /// @brief Whether it is an `evaluate` query.
    bool evaluate = false;
    /// @brief The value shown: the confirmed value with the optimistic layers over it.
    reactive::Signal<Value> value;
    /// @brief Whether a reply has been applied.
    reactive::Signal<bool> hasValue;
    /// @brief The last failure's text; empty when none.
    reactive::Signal<std::string> error;
    /// @brief The last failure's kind; empty when none.
    reactive::Signal<std::string> errorKind;
    /// @brief The last failure's action refusal; `null` when none or when it was not a refusal.
    reactive::Signal<Value> refusal;
    /// @brief Whether a request is in flight.
    reactive::Signal<bool> pending;
    /// @brief The value the server last confirmed.
    Value confirmed;
    /// @brief The optimistic writes still waiting, oldest first.
    std::vector<OptimisticLayer> layers;
    /// @brief The generation of the request in flight; a reply of an older one is dropped.
    std::uint64_t generation = 0;
    /// @brief When the last reply was applied.
    std::optional<std::chrono::milliseconds> fetchedAt;
    /// @brief Marked stale by a `refetch`, a backend switch or a change.
    bool stale = false;
    /// @brief Marked stale while a fetch was in flight: that fetch may have read before the change, so when it
    ///        settles the entry stays stale and fetches once more.
    bool refetchAfter = false;
    /// @brief The mounted queries using it.
    int users = 0;
    /// @brief The eviction timer, while no query uses it.
    reactive::TimerHandle evict;
    /// @brief The fields never cached.
    std::vector<std::string> secretFields;
    /// @brief The alias's instance token the entry was read through.
    std::string instanceToken;
    /// @brief The records the entry's action returns, with the key field and paths of each; empty for an
    ///        `evaluate` entry, which holds none.
    std::vector<ActionRecordInfo> records;

    /// @brief The catalog entry of a record the entry holds.
    /// @param record The record's wire name.
    /// @return It, or `nullptr` when the entry does not hold the record.
    [[nodiscard]] ActionRecordInfo const* holding(std::string_view record) const;
};

/// @brief The rows of a keyed record a value holds, at each of the record's paths.
/// @param value    The value.
/// @param paths    The record's paths.
/// @param keyField The record's key field.
/// @return The rows.
[[nodiscard]] MORPH_API std::vector<Value const*> keyedRowsOf(Value const& value,
                                                              std::vector<std::string> const& paths,
                                                              std::string_view keyField);

/// @brief New rows by the canonical text of their key.
using RowsByKey = std::map<std::string, Value const*, std::less<>>;

/// @brief Merges new rows into a value at each of the record's paths.
/// @param value    The value.
/// @param paths    The record's paths.
/// @param keyField The record's key field.
/// @param rows     The new rows by key.
/// @return The new value, or nothing when no row matched.
[[nodiscard]] MORPH_API std::optional<Value> patchHeldRows(Value const& value, std::vector<std::string> const& paths,
                                                           std::string_view keyField, RowsByKey const& rows);

/// @brief The entries one change marks stale, each once, in the order they were found.
struct MORPH_API Marks {
    /// @brief The entries.
    std::vector<CacheEntry*> entries;

    /// @brief Adds an entry, unless it is already there.
    /// @param entry The entry.
    void add(CacheEntry& entry);
};

class QueryRuntime;
class RtScope;
class ScreenCore;

}  // namespace detail

/// @brief How a mutation's reply was used.
enum class ReplyUse : std::uint8_t {
    Applied,    ///< The reply is the latest: its values may patch the cache.
    Discarded,  ///< A newer run's reply was applied first, so its values are older and only mark.
};

/// @brief What running a host command (`host/1`) did.
struct HostCommandResult {
    /// @brief The outcome.
    enum class Kind : std::uint8_t {
        Ran,      ///< The command ran.
        Missing,  ///< The client has no such command; nothing ran.
        Refused,  ///< The command exists and did not run: its arguments, or a connection that is not verified.
    };
    /// @brief The outcome.
    Kind kind = Kind::Ran;
    /// @brief Why it was refused; empty otherwise.
    std::string reason;
};

/// @brief The client's host commands, as a session runs them (spec 6 §14).
struct HostCommands {
    /// @brief The commands the client can run now; `{"can": "host:name"}` is true for these.
    std::set<std::string, std::less<>> available;
    /// @brief Runs a command with its evaluated `args` (`null` when the document gives none).
    std::function<HostCommandResult(std::string const& name, Value const& args)> run;
};

/// @brief The host names limited to some documents, each with the ids of the documents that may name it. A name not
///        listed is in scope for every document.
using HostScopes = std::map<std::string, std::set<std::string, std::less<>>, std::less<>>;

namespace detail {
struct WatchRegistration;
struct WatchRegistry;
}  // namespace detail

/// @brief A watch of one record that no cache entry holds, made by `Session::watchRecord`.
///
/// A copyable handle over one registration: the record stays in the session's watch set, and the handler keeps
/// hearing its changes, while any copy lives. When the last copy dies the handler is removed and the record is
/// counted out, which sends an `unwatch` when nothing else needs it. A `Completion` hands its value to a handler as
/// `const&`, so a caller that keeps the watch copies the holder there and lets the completion go. A holder that
/// outlives its session releases nothing. A default-constructed holder holds nothing. The last copy must die on the
/// runtime's owner, since releasing the watch changes the session's watch set. A session destroyed while a
/// `watchRecord` waits for its answer drops that completion unsettled.
class MORPH_API WatchHolder {
public:
    /// @brief A holder of nothing.
    WatchHolder() = default;

    /// @brief Whether the holder holds a registration.
    /// @return `true` for a holder `watchRecord` resolved with, and its copies.
    [[nodiscard]] bool holds() const noexcept { return _registration != nullptr; }

private:
    friend class Session;
    explicit WatchHolder(std::shared_ptr<detail::WatchRegistration> registration)
        : _registration{std::move(registration)} {}

    std::shared_ptr<detail::WatchRegistration> _registration;
};

/// @brief A mutation's run as it is sent.
struct RunStarted {
    /// @brief The run's id, unique in the session.
    std::uint64_t run;
    /// @brief The action's id.
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members): the run's context borrows the action name and body of the call for the call's duration, and the object is built and dropped inside it
    std::string const& action;
    /// @brief The body sent.
    Value const& body;
    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
};

/// @brief A mutation's run as it settles.
struct RunSettled {
    /// @brief The run's id, as `RunStarted` gave it.
    std::uint64_t run;
    /// @brief The action's id.
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members): the settled-run view borrows the call's action, body, reply and refusal code, and the object lives only inside the observer callback they outlive
    std::string const& action;
    /// @brief The body sent.
    Value const& body;
    /// @brief Whether the action replied.
    bool succeeded;
    /// @brief The reply; null for a run that failed.
    Value const& reply;
    /// @brief The `ActionRefusal` code of a refused run; empty for none.
    std::string const& refusalCode;
    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
    /// @brief Runs the same mutation again, with the run's bindings and another body.
    std::function<void(Value body)> rerun;
};

/// @brief Whether an action's input names rows of a table as a selection, as one row, or as the row a cell edit
///        changes.
enum class TableInputKind : std::uint8_t {
    Selection,  ///< A `table::RowSelection`: its schema property carries `x-selectionOf`.
    Row,        ///< A `table::RowRef`: its schema property carries `x-rowOf`.
    CellEdit,   ///< A `table::CellEdit` or `table::RowEdit` action: its schema root carries `x-cellEditOf` or
                ///< `x-rowEditOf`, and its member `key` names the row.
};

/// @brief A top-level member of an action's input that names rows of a table.
struct TableInput {
    /// @brief The member's name.
    std::string member;
    /// @brief The table's record, as the keyword names it.
    std::string record;
    /// @brief A selection or one row.
    TableInputKind kind = TableInputKind::Selection;
};

/// @brief The members of an action's input schema that name rows of a table.
/// @param schemaJson The action's input schema.
/// @return Each top-level property carrying `x-selectionOf` or `x-rowOf`, by member name, and the member `key` of an
///         edit action whose root carries `x-cellEditOf` or `x-rowEditOf`; none for a schema that does not read.
[[nodiscard]] MORPH_API std::vector<TableInput> tableInputsOf(std::string_view schemaJson);

/// @brief Hears every mutation run of a session: what a model-backed table marks its rows by, and offers again.
// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions): an interface; observers are not copied through it.
class RunObserver {
public:
    virtual ~RunObserver() = default;
    /// @brief A run was sent.
    /// @param run The run.
    virtual void started(RunStarted const& run) = 0;
    /// @brief A run settled.
    /// @param run The run.
    virtual void settled(RunSettled const& run) = 0;
};

/// @brief What screens of one client session share (spec 5 §5, §12b): the query cache, the mounted queries, the
///        principal's permitted actions, the formatting facts and the client's `env`, `session` and `host` values.
///
/// It belongs to the runtime's owner, outlives every screen mounted on it, and is cleared by `signOut` and on a
/// principal change.
class MORPH_API Session {
public:
    /// @brief Adds an observer of every mutation run; it must be removed before it goes.
    /// @param observer The observer.
    void addRunObserver(RunObserver* observer) { _runObservers.push_back(observer); }

    /// @brief Removes an observer added with `addRunObserver`.
    /// @param observer The observer.
    void removeRunObserver(RunObserver* observer) { std::erase(_runObservers, observer); }

    /// @brief The observers of mutation runs, in the order they were added.
    /// @return A copy, so an observer may remove itself while it is told.
    [[nodiscard]] std::vector<RunObserver*> runObservers() const { return _runObservers; }

    /// @brief The next run id: one counter for the whole session, so two mutations never share one.
    /// @return The id.
    [[nodiscard]] std::uint64_t nextRunId() noexcept { return ++_runIds; }

    /// @brief An empty session.
    /// @param runtime The runtime. Borrowed.
    /// @param options Timers, clock, catalog and the inline cap.
    /// @throws std::invalid_argument when `maxInlineFileBytes` is 0 or above `kMaxInlineFileBytes`.
    explicit Session(reactive::Runtime& runtime MORPH_LIFETIMEBOUND, SessionOptions options = {})
        : _runtime{&runtime},
          _options{checkedOptions(std::move(options))},
          _permitted{runtime},
          _format{runtime, std::make_shared<FormatContext const>()},
          _env{runtime, ValueRecord{{"locale", Value{FormatContext{}.locale}}}},
          _sessionValues{runtime},
          _hostValues{runtime},
          _hostCommandNames{runtime},
          _hostScopesRevision{runtime, 0},
          _contextOffered{runtime, false},
          _tableStates{detail::makeTableStates(*this, runtime, _options)} {}

    Session(Session const&) = delete;
    Session& operator=(Session const&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;
    ~Session();

    /// @brief The runtime.
    /// @return It.
    [[nodiscard]] reactive::Runtime& runtime() const noexcept { return *_runtime; }

    /// @brief The options.
    /// @return They.
    [[nodiscard]] SessionOptions const& options() const noexcept { return _options; }

    /// @brief The table states the session keeps, loads and saves through `SessionOptions::tableState`.
    /// @return They; `nullptr` when the session has no store.
    [[nodiscard]] TableStates* tableStates() const noexcept { return _tableStates.get(); }

    /// @brief Sets the action ids the principal may run, as the catalog reply lists them; `{"can": ...}` follows them.
    /// @param permitted The ids.
    void setPermitted(std::set<std::string, std::less<>> permitted) { _permitted.set(std::move(permitted)); }

    /// @brief Whether the principal may run an action, or the client can run a host command now. A tracked read.
    ///
    /// A `host:` name is answered from the client's own commands (`setHostCommands`), never from the server's
    /// permitted set: the two have different owners, and a catalog refresh must not drop the client's commands.
    /// @param action The action id, or `host:<command>`.
    /// @return Whether it is permitted.
    [[nodiscard]] bool can(std::string_view action) const;

    /// @brief Says whether a scope switcher with at least one option is installed, which is when `{"can": "context"}`
    ///        holds and the `setContext` command can run.
    /// @param offered Whether it is.
    void setContextOffered(bool offered) { _contextOffered.set(offered); }

    /// @brief Installs what runs the `setContext` command: the shell, which checks the value against the switcher's
    ///        options and asks before discarding unsaved work. An empty function removes it.
    /// @param handler It gets the requested value and answers whether it accepted the request.
    void setContextHandler(std::function<bool(Value const&)> handler) { _contextHandler = std::move(handler); }

    /// @brief Asks the installed handler to change the context.
    /// @param value The option value requested.
    /// @return Whether a handler accepted it; `false` with none installed.
    bool requestContext(Value const& value) const { return _contextHandler && _contextHandler(value); }

    /// @brief Sets the scope every screen reads as `{"env": "context"}`.
    /// @param value The chosen option's value; `null` for none.
    void setContext(Value value) { setEnvironmentValue("context", std::move(value)); }

    /// @brief Chooses the number, money and date facts apart from the language. The choice stays through later
    ///        `setLocale` calls, until an empty tag clears it.
    /// @param tag The BCP 47 tag whose facts apply; empty follows the language.
    void setFormatLocale(std::string tag);

    /// @brief The facts a locale scope writes in: the session's with @p tag's row of the locale table (separators,
    ///        sign, currency placement) and the date patterns the application declared, else that row's. The facts
    ///        come from the tag alone, whatever `setFormatLocale` chose. A tracked read of the session's facts.
    /// @param tag The scope's BCP 47 tag.
    /// @return The facts; `locale` is @p tag, and a tag the table does not list writes with `en`'s row.
    [[nodiscard]] std::shared_ptr<FormatContext const> scopedFormatting(std::string_view tag) const;

    /// @brief The session's own facts, shared. A tracked read.
    /// @return They.
    [[nodiscard]] std::shared_ptr<FormatContext const> sharedFormatting() const { return _format.get(); }

    /// @brief Whether a locale scope's tag is one to report: the locale table lists no row for it, and no scope has
    ///        reported it before. The answer is true once per tag for the session's lifetime.
    /// @param tag The tag.
    /// @return Whether to report it now.
    [[nodiscard]] bool firstUnlistedLocale(std::string const& tag);

    /// @brief The tag chosen with `setFormatLocale`.
    /// @return It; empty when the facts follow the language.
    [[nodiscard]] std::string const& formatLocale() const noexcept { return _formatLocale; }

    /// @brief Replaces the formatting facts; every `t` and display function re-evaluates. `{"env": "locale"}` follows
    ///        the facts' locale.
    /// @param format The facts.
    void setFormatting(FormatContext format);

    /// @brief The formatting facts. A tracked read.
    /// @return They.
    [[nodiscard]] FormatContext const& formatting() const { return *_format.get(); }

    /// @brief The formatting facts as a shared handle, for what holds views into them past a later change. A tracked
    ///        read.
    /// @return They.
    [[nodiscard]] std::shared_ptr<FormatContext const> formattingHandle() const { return _format.get(); }

    /// @brief The formatting facts, read without tracking: what a value set once at mount reads.
    /// @return They.
    [[nodiscard]] FormatContext const& formattingNow() const { return *_format.peek(); }

    /// @brief Sets the patterns every date and every date and time is written and read in, keeping the other
    ///        formatting facts. A non-empty pattern is the application's: it stays through every `setLocale`. An empty
    ///        one is not declared: the active locale's pattern applies, and ISO where the locale has none.
    /// @param date     The date pattern, such as `dd-MMM-yyyy`.
    /// @param dateTime The date-and-time pattern, such as `dd-MMM-yyyy HH:mm`.
    void setDatePatterns(std::string date, std::string dateTime);

    /// @brief Changes the active locale and installs its facts from the locale table (`localeFacts`): the separators
    ///        and currency symbols, and the date patterns the application did not declare
    ///        (`setDatePatterns`). The other formatting facts stay.
    /// @param locale The BCP 47 locale.
    void setLocale(std::string locale) { applyFacts(std::move(locale)); }

    /// @brief Replaces the message catalogue, keeping the other formatting facts; every `t` re-resolves.
    /// @param translations The provider, such as `MessageCatalogues::provider()`; empty shows every fallback.
    void setTranslations(render::TranslationProvider translations);

    /// @brief Sets the `env` values (`widthClass`, `platform`, `online`, `backend`). `locale` is the formatting
    ///        facts' locale and is always present: a `locale` in @p values is replaced by it.
    /// @param values The values.
    void setEnvironment(ValueRecord values);

    /// @brief Sets one `env` value and keeps the others, so a client that learns its width class does not drop the
    ///        `backend` a switch set, nor a switch the width class.
    /// @param name The value's name, such as `widthClass`.
    /// @param value The value.
    void setEnvironmentValue(std::string const& name, Value value);

    /// @brief Sets the `session` values (`principal`, `userName`, …).
    /// @param values The values.
    void setSessionValues(ValueRecord values) { _sessionValues.set(std::move(values)); }

    /// @brief Sets the `host` values the application registered.
    /// @param values The values.
    void setHostValues(ValueRecord values) { _hostValues.set(std::move(values)); }

    /// @brief Sets the client's host commands: the names `{"can": "host:name"}` answers for, and what runs them. A
    ///        session without a runner asks `InterpreterHost::hostCommand`.
    /// @param commands The names and the runner.
    void setHostCommands(HostCommands commands);

    /// @brief Limits host names to documents: a name listed is in scope only for the document ids listed with it, and
    ///        a name not listed is in scope everywhere. Out of scope, a value reads as `null`, `{"can": "host:name"}`
    ///        is false and a command is missing, exactly as for a name nobody registered.
    /// @param scopes The names and their documents.
    void setHostScopes(HostScopes scopes) {
        if (scopes == _hostScopes) {
            return;
        }
        _hostScopes = std::move(scopes);
        _hostScopesRevision.set(_hostScopesRevision.peek() + 1);
    }

    /// @brief Whether a document may name a host value or command. A tracked read.
    /// @param name     The host name.
    /// @param document The document's id.
    /// @return False when the name is limited to other documents.
    [[nodiscard]] bool hostNameIn(std::string_view name, std::string_view document) const;

    /// @brief What runs host commands.
    /// @return It; empty when the application set none.
    [[nodiscard]] std::function<HostCommandResult(std::string const&, Value const&)> const& hostCommandRunner()
        const noexcept;

    /// @brief Reads `env`, `session` or `host`. A tracked read.
    /// @param form `Env`, `Session` or `Host`.
    /// @param name The name.
    /// @return The value, or `null`.
    [[nodiscard]] Value client(ExprKind form, std::string_view name) const;

    /// @brief The app document's scope, which `{"ref": "app.name"}` reads and `{"set": "app.name"}` writes; set by
    ///        `Interpreter::mountApp` for as long as the `AppRoot` lives.
    /// @return It, or `nullptr` when no app document is mounted.
    [[nodiscard]] detail::RtScope const* appScope() const noexcept { return _appScope; }

    /// @brief Installs or clears the app document's scope. Called by `AppRoot`, nothing else.
    /// @param scope The scope, or `nullptr`.
    void setAppScope(detail::RtScope const* scope) noexcept { _appScope = scope; }

    /// @brief Drops every cache entry: `signOut` and a principal change (spec 5 §5, "Isolation").
    void clear();

    /// @brief Carries out a committed backend switch (spec 6 §9), in one batch: every mounted screen's
    ///        `onBackendChange` runs first, then every entry is marked stale and the mounted queries refetch, and
    ///        `{"env": "backend"}` becomes @p backend. A query on a private alias therefore never fetches before its
    ///        screen has recovered from the switch.
    /// @param backend The backend now in use.
    void backendChanged(std::string const& backend);

    /// @brief Registers a mounted screen or app document, whose `onBackendChange` a backend switch runs.
    /// @param screen It. Borrowed until `detachScreen`.
    void attachScreen(detail::ScreenCore& screen) { _screens.push_back(&screen); }

    /// @brief Forgets a screen or app document.
    /// @param screen It.
    void detachScreen(detail::ScreenCore& screen) { std::erase(_screens, &screen); }

    /// @brief Tells every screen of the session that a table state's save did not end `Saved`, so each table
    ///        mounted with that key says it in its status line.
    /// @param key    The state's key.
    /// @param result What the store answered.
    void tableSaveFailed(table::TableStateKey const& key, table::SaveResult const& result);

    /// @brief Starts a query's request before its screen mounts, so the screen mounts on a warm cache.
    /// @param channel The alias's channel.
    /// @param token   The alias's instance token (`Screen::instanceToken`).
    /// @param typeId  The model type.
    /// @param action  The action.
    /// @param body    The body.
    void prefetch(std::shared_ptr<ModelChannel> const& channel, std::string const& token, std::string const& typeId,
                  std::string const& action, Value const& body);

    /// @brief The number of entries in the cache.
    /// @return It.
    [[nodiscard]] std::size_t cacheSize() const noexcept { return _entries.size(); }

    /// @brief A fresh instance token for a private alias: what the cache key holds for its instance.
    /// @return It, unique in the session.
    [[nodiscard]] std::string privateToken() { return "private#" + std::to_string(++_privateTokens); }

    /// @brief Whether the newest write to a document source was made by a user event (spec 5 §6, `source: "user"`).
    /// @return It.
    [[nodiscard]] bool lastWriteByUser() const noexcept { return _lastWriteUser; }

    /// @brief Records a write to a document source.
    void noteWrite() noexcept { _lastWriteUser = _userDepth > 0; }

    /// @brief Runs @p body as a user event: what it writes counts as the user's.
    /// @param body The body.
    void asUser(std::function<void()> const& body);

    /// @brief The cache key of a query.
    /// @param token    The alias's instance token.
    /// @param typeId   The model type.
    /// @param action   The action.
    /// @param evaluate Whether it is an `evaluate` query.
    /// @param body     The canonical body.
    /// @return The key.
    [[nodiscard]] static std::string cacheKey(std::string const& token, std::string const& typeId,
                                              std::string const& action, bool evaluate, std::string const& body);

    /// @brief Finds or makes an entry and counts one more user.
    /// @param token    The alias's instance token.
    /// @param typeId   The model type.
    /// @param action   The action.
    /// @param evaluate Whether it is an `evaluate` query.
    /// @param body     The body.
    /// @return The entry.
    std::shared_ptr<detail::CacheEntry> acquire(std::string const& token, std::string const& typeId,
                                                std::string const& action, bool evaluate, Value const& body);

    /// @brief Counts one user less; an entry nobody uses is dropped after `keepUnused`.
    /// @param entry The entry.
    void release(std::shared_ptr<detail::CacheEntry> const& entry);

    /// @brief Sends an entry's request under a new generation; a reply of an older generation is dropped.
    /// @param entry   The entry.
    /// @param channel The channel to send it through; `nullptr` fails the request at once.
    void fetch(std::shared_ptr<detail::CacheEntry> const& entry, std::shared_ptr<ModelChannel> const& channel);

    /// @brief Writes an entry's confirmed value and re-applies its optimistic layers over it.
    /// @param entry     The entry.
    /// @param confirmed The confirmed value.
    void confirm(detail::CacheEntry& entry, Value const& confirmed);

    /// @brief Recomputes what an entry shows from its confirmed value and its layers.
    /// @param entry The entry.
    void showLayers(detail::CacheEntry& entry);

    /// @brief The error kind of a failure.
    /// @param error The failure.
    /// @return The kind.
    [[nodiscard]] std::string classify(std::exception_ptr const& error) const;

    /// @brief The clock.
    /// @return Now.
    [[nodiscard]] std::chrono::milliseconds now() const;

    /// @brief Registers a mounted query, which a reconnect and a backend switch refetch.
    /// @param query The query.
    void attachQuery(detail::QueryRuntime& query) { _mounted.push_back(&query); }

    /// @brief Unregisters a mounted query.
    /// @param query The query.
    void forgetQuery(detail::QueryRuntime& query) { std::erase(_mounted, &query); }

    /// @brief A call of @p actionId on a model of @p typeId succeeded: applies the action's effects to the cache
    ///        (spec 8 §9).
    ///
    /// `updates R` merges the reply's rows of `R` into each entry holding `R`, matched by the record's key field at
    /// the entry's paths, with no fetch; an entry whose fetch is in flight also fetches once more, since that fetch
    /// may have read before the write. `changes R` marks each entry holding `R` stale. `changesInstance` marks each
    /// entry read through the same instance. A marked entry refetches through a mounted query that uses it. A
    /// discarded reply only marks: its `updates` act as `changes`.
    /// @param typeId        The model type.
    /// @param actionId      The action.
    /// @param reply         The reply's value.
    /// @param use           Whether the reply was applied or discarded.
    /// @param instanceToken The instance token of the alias the call went through.
    void actionSucceeded(std::string const& typeId, std::string const& actionId, Value const& reply, ReplyUse use,
                         std::string const& instanceToken);

    /// @brief The connection to the server was made again (spec 8 §7, "Reconnect"): sends the whole watch set with
    ///        `replace: true`, then marks every entry but an `evaluate` one stale. A mounted entry refetches, and one
    ///        whose fetch is in flight fetches once more when that fetch settles, so a change missed while the
    ///        connection was down is read again.
    void reconnected();

    /// @brief Hears every reconnect and `resync`, after the watch set is sent and the cache marked: what a reader
    ///        that holds rows outside the cache and watches no record (a page window over an action that holds none)
    ///        refetches by.
    /// @param handler What hears it, on the runtime's owner.
    /// @return The holder: the handler is called while a copy of it lives.
    [[nodiscard]] std::shared_ptr<void> onReconnected(std::function<void()> handler);

    /// @brief The topics the cache holds now: every record an entry holds, every extra watch, and the keyed shared
    ///        instance of every entry whose action holds no record. What `reconnected` sends.
    /// @return The topics, records and instances each in name order; `replace` is false.
    [[nodiscard]] wire::WatchBody watchSet() const;

    /// @brief Watches a record for something other than the cache, such as a list a client keeps beside it, and
    ///        keeps watching it in every set the session sends: `reconnected` and a `resync` carry it in their
    ///        `replace: true` set, and the cache dropping its last entry on the record does not unwatch it.
    ///
    /// The `watch` goes out at once when there is a watcher. The events arrive through `deliverEvent` like any
    /// other's, for the owner of the extra watch to read.
    ///
    /// A record has one owner: extra watches are not reference-counted, so the first `removeExtraWatch` ends it for
    /// every caller. A refusal of the watch, in the reply or as a failure, is logged at warn.
    /// @param record The record's name; one already watched this way changes nothing.
    void addExtraWatch(std::string const& record);

    /// @brief Stops an extra watch: sends an `unwatch` unless an entry of the cache still holds the record.
    ///
    /// A record has one owner: the watch is not reference-counted, so this ends it whoever else added it.
    /// @param record The record's name; one not watched this way changes nothing.
    void removeExtraWatch(std::string const& record);

    /// @brief Applies a server event to the cache (spec 8 §9).
    ///
    /// A `changes` event marks the entries it names stale, and a `resync` sends the whole watch set again, as
    /// `reconnected` does, and marks every entry but an `evaluate` one;
    /// each marked entry refetches through a mounted query that uses it, and one no query uses refetches when a
    /// query mounts on it. An event of another topic, or a payload that cannot be read, is ignored.
    /// @param event The event.
    void deliverEvent(wire::EventBody const& event);

    /// @brief Applies a `changes` event's payload to the cache, in one reactive batch.
    ///
    /// A record change with keys marks each entry holding the record whose value has a row with one of the keys,
    /// and an entry holding it keyless; a record change without keys marks every entry holding the record. An
    /// instance change marks every entry read through that instance: the keyed shared one its primary key names,
    /// or, with no primary key, the connection's own private ones of that model type.
    /// @param changes The payload.
    void applyChanges(wire::ChangesBody const& changes);

    /// @brief Marks an entry stale. While its fetch is in flight, the entry also refetches once that fetch settles,
    ///        because the fetch may have read before the change.
    /// @param entry The entry.
    static void markStale(detail::CacheEntry& entry);

    /// @brief Runs a read action and settles with its reply's JSON text, making no cache entry and reading none.
    ///
    /// A page window's fetches go through it. The reply is not read into a `Value`: a page's `Number` cells are JSON
    /// numbers with fractions or exponents, which a `Value` cannot hold, so the caller decodes the text against the
    /// table's description (`table::readGenericPage`). No effects are applied: a read has none, and the caller's own
    /// change events reach it as anyone's do.
    /// @param channel  The alias's channel; `nullptr` fails the read at once.
    /// @param actionId The action.
    /// @param body     The body, sent as its canonical JSON.
    /// @param stop     The caller's cancel: passed to the channel as `CallOptions::stop`, so the wire's `cancel` goes
    ///                 out where the server takes one; the read then settles cancelled and its reply is dropped.
    /// @return The reply's JSON text, byte for byte as the channel returned it, delivered on the runtime's owner; or
    ///         the call's failure, or `core::async::OperationCancelled` after a stop.
    [[nodiscard]] async::Completion<std::string> read(std::shared_ptr<ModelChannel> const& channel,
                                                      std::string const& actionId, Value const& body,
                                                      ::core::async::StopToken const& stop = {});

    /// @brief Watches a record without a cache entry, for as long as a copy of the returned holder lives.
    ///
    /// The record is counted in the watch set as an entry's records are, so a count that leaves zero sends a
    /// `watch` at once. @p handler hears, on the runtime's owner, each `changes` event that names the record (after
    /// the cache's entries are marked), and a whole-type change (`keys` absent) on reconnect and on `resync`.
    /// @param record  The record's wire name.
    /// @param handler What hears its changes.
    /// @return The holder, once the server has the watch: when the `watch` this call sent, or the one in flight for
    ///         the record, is answered (whatever the answer, since a refused topic only sends no events); at once
    ///         when the record was already watched and answered, or the session has no watcher.
    [[nodiscard]] async::Completion<WatchHolder> watchRecord(
        std::string const& record, std::function<void(effects::RecordChange const&)> handler);

private:
    // A cap of 0 moves nothing inline, and one above the largest would not fit an envelope once encoded.
    [[nodiscard]] static SessionOptions checkedOptions(SessionOptions options);

    void settleReply(detail::CacheEntry& entry, std::string const& reply);

    /// @brief Refetches an entry through the mounted queries that use it; one fetch even when several do.
    /// @param entry The entry.
    void refetchMounted(detail::CacheEntry const& entry);

    /// @brief Applies a reply's effects to the cache, for `actionSucceeded`.
    /// @param typeId        The model type.
    /// @param actionId      The action.
    /// @param reply         The reply's value.
    /// @param use           Whether the reply was applied or discarded.
    /// @param instanceToken The instance token of the alias the call went through.
    void applyReplyEffects(std::string const& typeId, std::string const& actionId, Value const& reply, ReplyUse use,
                           std::string const& instanceToken);

    /// @brief Counts an entry's topics in or out, queueing a `watch` for a count that leaves zero and an `unwatch`
    ///        for one that returns to it.
    /// @param entry The entry.
    /// @param delta `+1` when the entry is made, `-1` when it is dropped.
    void countTopics(detail::CacheEntry const& entry, int delta);

    /// @brief Counts one record in or out of the watch set, as `countTopics` counts an entry's.
    /// @param record The record.
    /// @param delta  `+1` or `-1`.
    void countRecord(std::string const& record, int delta);

    /// @brief Hands a record change to every watch holder of its record.
    /// @param change The change.
    void notifyWatchers(effects::RecordChange const& change);

    /// @brief The watch registry, made on first use.
    /// @return It.
    detail::WatchRegistry& watchRegistry();

    friend struct detail::WatchRegistration;

    /// @brief Sends the queued watch changes, at most one `watch` and one `unwatch`; drops them when there is no
    ///        watcher.
    void flushWatches();

    /// @brief Sends one watch request, ignoring its reply: a refused topic simply sends no events.
    /// @param kind The kind, `watch` or `unwatch`.
    /// @param body The body.
    void sendWatch(std::string_view kind, wire::WatchBody const& body) const;

    /// @brief Marks every entry but an `evaluate` one stale and refetches the mounted ones, in one batch.
    void markAllStale();

    /// @brief Marks each entry of @p marks stale and refetches it through a mounted query that uses it.
    /// @param marks The entries.
    void markAndRefetch(detail::Marks const& marks);

    /// @brief Applies one `updates` or `changes` effect of a reply: patches the entries holding the record, or marks
    ///        them.
    /// @param effect The effect.
    /// @param record The record it names.
    /// @param info   The action's catalog entry, for the records its reply holds.
    /// @param reply  The reply's value.
    /// @param use    Whether the reply was applied or discarded.
    /// @param marks  Where entries to mark go.
    void applyRecordEffect(ActionEffect const& effect, std::string const& record, ActionInfo const& info,
                           Value const& reply, ReplyUse use, detail::Marks& marks);

    /// @brief Finds the entries a `changes` event's record change names.
    /// @param change The change.
    /// @param marks  Where they go.
    void markRecordChange(effects::RecordChange const& change, detail::Marks& marks);

    /// @brief Finds the entries a `changes` event's instance change names.
    /// @param instance The instance.
    /// @param marks    Where they go.
    void markInstanceChange(wire::InstanceTopic const& instance, detail::Marks& marks);

    void settleFailure(detail::CacheEntry& entry, std::string text, std::string kind, Value refusal = Value{});

    static void detachQuery(detail::QueryRuntime& query);

    reactive::Runtime* _runtime;
    SessionOptions _options;
    reactive::Signal<std::set<std::string, std::less<>>> _permitted;
    std::map<std::string, int, std::less<>> _recordRefs;
    std::shared_ptr<detail::WatchRegistry> _watchRegistry;
    std::vector<std::weak_ptr<std::function<void()>>> _reconnectListeners;
    std::map<std::pair<std::string, std::string>, int> _instanceRefs;
    std::set<std::string, std::less<>> _watchRecords;
    /// The records watched for something other than the cache (`addExtraWatch`).
    std::set<std::string, std::less<>> _extraWatch;
    std::set<std::string, std::less<>> _unwatchRecords;
    std::set<std::pair<std::string, std::string>> _watchInstances;
    // Installs the facts of @p locale's row, or of the chosen format locale's row where one is chosen, and names
    // @p locale as the language.
    void applyFacts(std::string locale);

    std::set<std::pair<std::string, std::string>> _unwatchInstances;
    reactive::Signal<std::shared_ptr<FormatContext const>> _format;
    // What the application declared (`setDatePatterns`) and what the locale installed (`setLocale`), per pattern.
    std::string _declaredDate;
    std::string _declaredDateTime;
    std::string _localeDate;
    std::string _localeDateTime;
    reactive::Signal<ValueRecord> _env;
    reactive::Signal<ValueRecord> _sessionValues;
    reactive::Signal<ValueRecord> _hostValues;
    detail::RtScope const* _appScope = nullptr;
    std::vector<detail::ScreenCore*> _screens;
    reactive::Signal<std::set<std::string, std::less<>>> _hostCommandNames;
    // The scopes are read through `_hostScopesRevision`, which is what a tracked read subscribes to: a signal over the
    // scopes' own type would be one more template instance in every generated inheritance graph of the signal base.
    HostScopes _hostScopes;
    reactive::Signal<std::size_t> _hostScopesRevision;
    reactive::Signal<bool> _contextOffered;
    std::function<bool(Value const&)> _contextHandler;
    std::string _formatLocale;
    std::set<std::string, std::less<>> _unlistedLocales;
    std::function<HostCommandResult(std::string const&, Value const&)> _hostCommandRunner;
    std::map<std::string, std::shared_ptr<detail::CacheEntry>, std::less<>> _entries;
    std::vector<detail::QueryRuntime*> _mounted;
    int _userDepth = 0;
    bool _lastWriteUser = false;
    std::uint64_t _privateTokens = 0;
    std::vector<RunObserver*> _runObservers;
    std::uint64_t _runIds = 0;
    /// After everything a save or load reaches, so it is destroyed first.
    std::shared_ptr<TableStates> _tableStates;
};
// ---------------------------------------------------------------------------------------------------------------
// Runtime structures
// ---------------------------------------------------------------------------------------------------------------

/// @brief A model the opener lends to a screen whose alias says `{"from": alias}` (spec 6 §16).
struct BorrowedModel {
    /// @brief The opener's channel.
    std::shared_ptr<ModelChannel> channel;
    /// @brief The opener's instance token, so the two share cache entries.
    std::string token;
};

class Screen;

class Inspector;

/// @brief One command of a command list the inspector's log shows (spec 6 §21).
struct LoggedCommand {
    /// @brief The command's key: `set`, `run`, `refetch`, …
    std::string kind;
    /// @brief The name it acts on (`Command::target`); empty for a command that names none.
    std::string target;
    /// @brief The command's JSON Pointer in the document.
    std::string path;
};

/// @brief One command list as it ran: the event that ran it, its commands, and the reactive batch they ran in.
struct CommandLogEntry {
    /// @brief The JSON Pointer of the list: the event, lifecycle key or step that holds it.
    std::string event;
    /// @brief Its commands, in order.
    std::vector<LoggedCommand> commands;
    /// @brief The number of the reactive batch it ran in (`reactive::detail::RuntimeCore::currentBatch`); 0 outside
    ///        every batch.
    std::uint64_t batch = 0;
};

/// @brief What `Interpreter::mount` is given beside the document.
struct MountOptions {
    /// @brief The container the screen's root widget is appended to; `nullptr` makes a backend root.
    ContainerWidget* parent = nullptr;
    /// @brief The models the opener lends, by the alias of this screen that borrows them.
    std::map<std::string, BorrowedModel, std::less<>> borrowed;
    /// @brief The reactive scope depth of the screen's own scope.
    std::size_t depth = 0;
    /// @brief The screen this mount replaces, such as one whose document changed; `nullptr` for a fresh mount. Each
    ///        state of the new screen's own scope starts from that screen's value of the state of the same name and
    ///        type, instead of from its init. Borrowed for the call.
    Screen const* keepStateOf = nullptr;
};

class WindowRows;

namespace detail {

/// @brief A name bound for the expressions of one place: a command's `event`, a step's `as`, `result` in a
///        `patch`, `body` in an `optimistic`, `props` in a fallback, `boundary` in a boundary's `error`.
struct Binding {
    /// @brief The name.
    std::string name;
    /// @brief Reads its value; a tracked read for a reactive one.
    std::function<Value()> read;
};

/// @brief The names bound at one place, innermost last.
using Bindings = std::vector<Binding>;

class ScreenCore;
class RtScope;
struct ElementState;
class MutationRuntime;
struct CheckRuntime;
struct BoundaryRuntime;
struct ModelTable;
struct RowsTable;
struct GridTable;
struct TableConfig;

/// @brief One state declaration at run time.
struct StateSlot {
    /// @brief The declaration.
    StateDecl const* decl = nullptr;
    /// @brief The value, made when the init is evaluated.
    std::unique_ptr<reactive::Signal<Value>> value;
    /// @brief The scope that declares it, where its init is evaluated.
    RtScope const* scope = nullptr;
    /// @brief Set while the init is evaluated, so a cyclic read gives `null` instead of recursing.
    bool initializing = false;
};

/// @brief One model alias at run time: the instance it names, and the channel bound to it.
struct ModelAlias {
    /// @brief The declaration.
    ModelDecl const* decl = nullptr;
    /// @brief `Private`, `Shared`, `Keyed` or `Borrowed`, the default resolved through the catalog.
    InstanceMode mode = InstanceMode::Private;
    /// @brief The instance token: what the cache key holds for the instance; empty while unbound (a `null` key).
    reactive::Computed<std::string>* token = nullptr;
    /// @brief The channel of the token it was last bound for.
    std::shared_ptr<ModelChannel> channel;
    /// @brief That token.
    std::string boundToken;
};

/// @brief The values of an `upload` transfer id (spec 6 §18).
struct TransferState {
    /// @brief An idle transfer.
    /// @param runtime The runtime.
    explicit TransferState(reactive::Runtime& runtime) : progress{runtime}, pending{runtime, false}, error{runtime} {}
    /// @brief 0 to 1, or `null`.
    reactive::Signal<Value> progress;
    /// @brief Whether it is running.
    reactive::Signal<bool> pending;
    /// @brief The failure's text, or `null`.
    reactive::Signal<Value> error;
};

/// @brief The field of a table's heading entry that holds the heading row's key; the rest of the entry is the run
///        record its heading reads. A data row with this field would be taken for a heading.
inline constexpr std::string_view kGroupHeadingField = "\x1Fgroup";

/// @brief A table's rows as the table shows them: every row `rows` gave, in the order of the table's sort, and the
///        page of them it shows.
struct TableView {
    /// @brief The rows, sorted: every page.
    std::vector<Value> rows;
    /// @brief With `group`, each row's group: its index in `runs`; empty without.
    std::vector<std::size_t> runOf;
    /// @brief With `group`, each group's heading entry: the run record of the whole group, in source order, and its
    ///        heading key in `kGroupHeadingField`.
    std::vector<Value> runs;
    /// @brief The position in `rows` of the page's first row.
    std::size_t first = 0;
    /// @brief The position in `rows` past the page's last row.
    std::size_t last = 0;
    /// @brief The page shown, 0-based, clamped to the last page.
    std::int64_t page = 0;
    /// @brief How many pages there are; at least 1.
    std::int64_t pageCount = 1;
    /// @brief Equal when the rows are.
    /// @param other The other view.
    /// @return Whether they are equal.
    [[nodiscard]] bool operator==(TableView const& other) const = default;
};

/// @brief An element's live values (spec 5 §6).
struct MORPH_API ElementState {
    /// @brief The element's state before any user action.
    /// @param runtime The runtime.
    explicit ElementState(reactive::Runtime& runtime);

    /// @brief Destroys the element's state, and the sessions it owns.
    ~ElementState();
    ElementState(ElementState const&) = delete;
    ElementState& operator=(ElementState const&) = delete;
    ElementState(ElementState&&) = delete;
    ElementState& operator=(ElementState&&) = delete;

    /// @brief The node.
    DocNode const* node = nullptr;
    /// @brief The scope that declares it.
    RtScope* scope = nullptr;
    /// @brief What the user is editing: a text input's text, a checkbox's state, a select's key, a slider's number,
    ///        a date-time input's timestamp.
    reactive::Signal<Value> draft;
    /// @brief The committed value of an input whose value no expression binds.
    reactive::Signal<Value> committed;
    /// @brief Whether the user changed the draft since it last followed the bound value.
    reactive::Signal<bool> edited;
    /// @brief Whether it has been committed at least once (Enter or focus leaving after an edit).
    reactive::Signal<bool> touched;
    /// @brief Whether it has keyboard focus. The view tree reports no focus, so it stays false.
    reactive::Signal<bool> focused;
    /// @brief A dialog's or drawer's `open`, set by `open` and `close`.
    reactive::Signal<bool> open;
    /// @brief A dialog's or drawer's `submitted`.
    reactive::Signal<bool> submitted;
    /// @brief A table's selected keys.
    reactive::Signal<Value> selection;
    /// @brief A table's own sort, a list of keys, while no `sort` key binds one.
    reactive::Signal<Value> sort;
    /// @brief A table's last activated key.
    reactive::Signal<Value> activeKey;
    /// @brief Bumped when a check targeting it comes or goes.
    reactive::Signal<int> checksVersion;
    /// @brief A dialog's or drawer's scope while its content is mounted.
    reactive::Signal<RtScope*> inner;
    /// @brief The checks targeting it.
    std::vector<CheckRuntime*> checks;
    /// @brief The value an expression binds (`value`, `checked`, `selected`, a table's `selection`); `nullptr` when
    ///        the element is uncontrolled.
    reactive::Computed<Value>* bound = nullptr;
    /// @brief What is wrong now, as a list of strings: `required` unmet, the node's `errors`, the reply's field
    ///        errors and the failing checks' messages.
    reactive::Computed<Value>* errors = nullptr;
    /// @brief A table's rows or a select's options, for its counts and `selectedOption`.
    reactive::Computed<Value>* rows = nullptr;
    /// @brief A table's bound `sort`; `nullptr` while the table holds its own.
    reactive::Computed<Value>* sortBound = nullptr;
    /// @brief A table's rows as it shows them, in its engine's order, and the page of them it shows: what every
    ///        reader of the table's element values reads; `nullptr` for a table without `rows`.
    reactive::Computed<TableView>* view = nullptr;
    /// @brief The rows a table's mount reads: a table over `rows` its `EngineRows`, a model-backed table the rows
    ///        over its window that tell the element of each change and gesture, a collection its engine's rows that
    ///        tell its table of each change; null for any other element.
    std::shared_ptr<TableRows> tableRows;
    /// @brief A model-backed table's window over the server's pages; null for any other element.
    std::shared_ptr<WindowRows> tableWindow;
    /// @brief A `form`'s session (spec 2), made when the element is wired; null when its schema is unknown.
    std::unique_ptr<forms::FormSession> form;
    /// @brief A `collection`'s session (spec 2), made when the element is wired; null when its view document is
    ///        unknown or does not build.
    std::unique_ptr<forms::CollectionSession> collection;
    /// @brief A `wizard`'s session (spec 2), made when the element is wired; null when its wizard document or a
    ///        step's schema is unknown or does not build.
    std::unique_ptr<forms::WizardSession> wizard;
    /// @brief A model-backed table's rows, description, persisted state and status line, made when the element is
    ///        wired; null for any other element. It outlives every mount of the table.
    std::unique_ptr<ModelTable> modelTable;
    /// @brief A table over `rows`: its source, engine and page, its persisted state and its status line, made when
    ///        the element is wired; a collection's table: its engine over the session's source, its configuring,
    ///        persisted state and status line. Null for any other element. It outlives every mount of the table.
    std::unique_ptr<RowsTable> rowsTable;
    /// @brief A grid of a form (a table with `form` and `field`) or a fixed grid (a table with `layoutRows`): its grid
    ///        and its status line, made when the table is first lowered; null for any other element. It outlives every
    ///        mount of the table.
    std::unique_ptr<GridTable> gridTable;
    /// @brief A form's fields drawn as grids through a field overlay's `grid` part, by the field's name: each an
    /// element
    ///        of its own, whose node is the part, holding its grid. Empty for any other element.
    std::map<std::string, std::unique_ptr<ElementState>, std::less<>> fieldGrids;
};

/// @brief One check at run time.
struct MORPH_API CheckRuntime {
    /// @brief The declaration.
    CheckDecl const* decl = nullptr;
    /// @brief Whether `assert` is `false`.
    reactive::Computed<bool>* failing = nullptr;
    /// @brief The message, as text.
    reactive::Computed<std::string>* message = nullptr;
    /// @brief The elements it targets.
    std::vector<ElementState*> targets;

    CheckRuntime() = default;
    CheckRuntime(CheckRuntime const&) = delete;
    CheckRuntime& operator=(CheckRuntime const&) = delete;
    CheckRuntime(CheckRuntime&&) = delete;
    CheckRuntime& operator=(CheckRuntime&&) = delete;

    /// @brief Leaves its targets.
    ~CheckRuntime();
};

/// @brief One declared name of a run-time scope.
struct NameRef {
    /// @brief What it declares.
    NameCategory category = NameCategory::Param;
    /// @brief A param's value.
    Value param;
    /// @brief The state.
    StateSlot* state = nullptr;
    /// @brief The `let`.
    reactive::Computed<Value>* let = nullptr;
    /// @brief The `let`'s expression, for the boundary's ownership walk.
    ExprPtr letExpr;
    /// @brief The query.
    QueryRuntime* query = nullptr;
    /// @brief The mutation.
    MutationRuntime* mutation = nullptr;
    /// @brief The model alias.
    ModelAlias* model = nullptr;
    /// @brief The element.
    ElementState* element = nullptr;
    /// @brief The transfer.
    TransferState* transfer = nullptr;
};

/// @brief A boundary at run time: the queries it owns (spec 5 §9).
struct BoundaryRuntime {
    /// @brief The queries read inside its children.
    std::vector<QueryRuntime*> queries;
};

/// @brief One document scope at run time: the screen, a dialog's or drawer's content, a switch case or a row.
///
/// It lives while anything that reads it does: the view nodes lowered in it hold it by `shared_ptr`, a reply or
/// a timer holds it weakly. It belongs to a `ScreenCore`, which outlives it.
class MORPH_API RtScope : public std::enable_shared_from_this<RtScope> {
public:
    /// @brief An empty scope.
    /// @param screen    The screen.
    /// @param enclosing The enclosing scope; `nullptr` for the screen and a component fallback.
    /// @param current   The row of a row scope; `nullptr` otherwise.
    /// @param depth     The reactive scope depth of what it owns.
    RtScope(ScreenCore& screen, std::shared_ptr<RtScope> enclosing, reactive::Signal<Value> const* current,
            std::size_t depth);

    RtScope(RtScope const&) = delete;
    RtScope& operator=(RtScope const&) = delete;
    RtScope(RtScope&&) = delete;
    RtScope& operator=(RtScope&&) = delete;

    /// @brief Releases what it owns, and tells the dialog it belongs to.
    ~RtScope();

    /// @brief Resolves a name, innermost scope first.
    /// @param name The name.
    /// @return The declaration, or `nullptr`.
    [[nodiscard]] NameRef const* find(std::string_view name) const;

    /// @brief The innermost row, from this scope outwards.
    /// @return The row signal, or `nullptr`.
    [[nodiscard]] reactive::Signal<Value> const* innermostRow() const;

    /// @brief The innermost scope that decides when errors show and is submitted: a dialog's, or the screen's.
    /// @return The scope.
    [[nodiscard]] RtScope const& policyScope() const;

    /// @brief Whether every element of the scope is valid and no blocking check of it fails. A tracked read.
    /// @return It.
    [[nodiscard]] bool ready() const;

    /// @brief The screen.
    ScreenCore& core;
    /// @brief The enclosing scope.
    std::shared_ptr<RtScope> parent;
    /// @brief The row of a row scope.
    reactive::Signal<Value> const* row = nullptr;
    /// @brief The canonical key of a row scope's row; empty for any other scope.
    std::string rowKey;
    /// @brief For a row scope whose row signal is set to other rows (a pooled table cell): the canonical key of the
    ///        row it holds now, read in place of `rowKey`; empty for any other scope.
    std::function<std::string()> rowKeyNow;
    /// @brief For a component fallback's scope, the focus key of the `custom` node it is lowered for, so one
    ///        fallback lowered for two uses keys its elements apart; empty for any other scope.
    std::string usePrefix;
    /// @brief The dialog or drawer whose content it is.
    ElementState* owner = nullptr;
    /// @brief `touched`, `submit` or `always`, for a scope that decides it.
    std::string showErrors;
    /// @brief `submitted` of a scope that decides when errors show.
    reactive::Signal<bool>* submitted = nullptr;
    /// @brief Its names.
    std::map<std::string, NameRef, std::less<>> names;
    /// @brief Its state.
    std::vector<std::unique_ptr<StateSlot>> states;
    /// @brief Its elements, in document order.
    std::vector<std::unique_ptr<ElementState>> elements;
    /// @brief Its transfers.
    std::vector<std::unique_ptr<TransferState>> transfers;
    /// @brief Its model aliases.
    std::vector<std::unique_ptr<ModelAlias>> models;
    /// @brief Its queries.
    std::vector<std::unique_ptr<QueryRuntime>> queries;
    /// @brief Its mutations.
    std::vector<std::unique_ptr<MutationRuntime>> mutations;
    /// @brief Its checks.
    std::vector<std::unique_ptr<CheckRuntime>> checks;
    /// @brief Called with a field a row expression read that the row's record does not hold; set on a server-mode
    ///        row scope, whose record holds only the columns fetched. Called from inside a binding's evaluation, so
    ///        it writes no signal.
    std::function<void(std::string_view field)> const* onMissingRowField = nullptr;
    /// @brief For a locale scope's scope, the formatting facts it writes in; `nullptr` for any other scope.
    reactive::Computed<std::shared_ptr<FormatContext const>>* localeFormat = nullptr;
    /// @brief Whether the scope declares nothing: the elements of its nodes are the enclosing scope's.
    bool transparent = false;
    /// @brief For the scope of a component's fallback, whose names are isolated from the use site's: the use site's
    ///        scope, which still decides the formatting.
    std::shared_ptr<RtScope> formatSource;

    /// @brief The formatting facts of this place: the innermost enclosing locale scope's, else the session's. A
    ///        tracked read.
    /// @return They. The reference is into the current facts and holds until the facts are next recomputed; a caller
    ///         keeps it only while it lowers or evaluates, when nothing writes the session's formatting or a scope's
    ///         tag.
    [[nodiscard]] FormatContext const& formatting() const;

    /// @brief The reactive nodes it owns; declared last, so they go first.
    reactive::Scope nodes;
};

/// @brief The `Environment` the evaluator reads through: a scope chain and the names bound at one place.
class MORPH_API ScopeEnv final : public Environment {
public:
    /// @brief Reads @p scope.
    /// @param scope    The scope.
    /// @param bindings The names bound here; may be `nullptr`.
    /// @param row      A row to read instead of the scope's (a table's `key`, evaluated before its row scope exists).
    ScopeEnv(RtScope const& scope, Bindings const* bindings, Value const* row = nullptr)
        : _scope{&scope}, _bindings{bindings}, _row{row} {}

    /// @brief Reads a name: a binding, then the scope chain.
    /// @param head The name.
    /// @param path The path after it.
    /// @return The value.
    [[nodiscard]] Value ref(std::string_view head, std::span<std::string const> path) const override;

    /// @brief Reads the innermost row.
    /// @param path The path.
    /// @return The value.
    [[nodiscard]] Value row(std::span<std::string const> path) const override;

    /// @brief Reads a query or mutation form.
    /// @param form The form.
    /// @param name The query or mutation.
    /// @return The value.
    [[nodiscard]] Value control(ExprKind form, std::string_view name) const override;

    /// @brief Reads a customization choice.
    /// @param point The point.
    /// @param path  The path.
    /// @return The value.
    [[nodiscard]] Value pref(std::string_view point, std::span<std::string const> path) const override;

    /// @brief Reads `env`, `session` or `host`.
    /// @param form The form.
    /// @param name The name.
    /// @return The value.
    [[nodiscard]] Value client(ExprKind form, std::string_view name) const override;

    /// @brief The session's formatting facts.
    /// @return They.
    [[nodiscard]] FormatContext const& formatting() const override;

    /// @brief The session's formatting facts, which typed text is read in whatever locale scope encloses it.
    /// @return They.
    [[nodiscard]] FormatContext const& entryFormatting() const override;

    /// @brief Reports to the screen's diagnostics.
    /// @param path   The expression's path.
    /// @param reason What went wrong.
    void report(std::string_view path, std::string_view reason) const override;

private:
    RtScope const* _scope;
    Bindings const* _bindings;
    Value const* _row;
};

/// @brief Evaluates an expression in a scope.
/// @param scope    The scope.
/// @param bindings The names bound here; may be `nullptr`.
/// @param expr     The expression; `nullptr` evaluates to `null`.
/// @return The value.
[[nodiscard]] MORPH_API Value evalIn(RtScope const& scope, Bindings const* bindings, ExprPtr const& expr);

/// @brief Whether a value counts as given for `required`: not `null`, not an empty string or list.
/// @param value The value.
/// @return It.
[[nodiscard]] MORPH_API bool engaged(Value const& value);

/// @brief A value as text for a widget: a string as itself, a number in the locale, a timestamp in ISO 8601,
///        `null` as nothing, anything else as JSON.
/// @param value  The value.
/// @param format The formatting facts.
/// @return The text.
[[nodiscard]] MORPH_API std::string textOf(Value const& value, FormatContext const& format);

/// @brief A view key as a value.
/// @param key The key.
/// @return An `int` or a string.
[[nodiscard]] MORPH_API Value valueOfKey(Key const& key);

/// @brief A query at run time (spec 5 §5): a key over the scope, an entry of the session's cache, latest wins.
class MORPH_API QueryRuntime {
public:
    /// @brief A query of @p scope, not yet started.
    /// @param decl  The declaration.
    /// @param scope The scope.
    /// @param alias The model alias it goes through.
    QueryRuntime(QueryDecl const& decl, RtScope& scope, ModelAlias* alias);

    QueryRuntime(QueryRuntime const&) = delete;
    QueryRuntime& operator=(QueryRuntime const&) = delete;
    QueryRuntime(QueryRuntime&&) = delete;
    QueryRuntime& operator=(QueryRuntime&&) = delete;

    /// @brief Stops its timers and releases its entry.
    ~QueryRuntime();

    /// @brief Starts it: makes its key and the Effect that follows it, and its refresh timer.
    void start();

    /// @brief The value: the entry's, kept from the previous key while the new one has none. A tracked read.
    /// @return It.
    [[nodiscard]] Value value() const;

    /// @brief Whether a request is in flight or waiting out its debounce. A tracked read.
    /// @return It.
    [[nodiscard]] bool pending() const;

    /// @brief The failure's text, `null` when none. A tracked read.
    /// @return It.
    [[nodiscard]] Value error() const;

    /// @brief The failure's kind, `null` when none. A tracked read.
    /// @return It.
    [[nodiscard]] Value errorKind() const;

    /// @brief The failure's action refusal, `null` when none or when the failure was not a refusal. A tracked read.
    /// @return It.
    [[nodiscard]] Value refusal() const;

    /// @brief Whether the query has a value or a failure for its current key, or is idle. A tracked read, for a
    ///        boundary.
    /// @return It.
    [[nodiscard]] bool settled() const;

    /// @brief Refetches the current key now, ending a debounce wait.
    void refetch();

    /// @brief Fetches the key its body has now, inside the batch that changed the body: switches to that key's entry
    ///        when it differs from the current one, and fetches the current one otherwise. A backend switch uses it,
    ///        so a body `onBackendChange` changed is fetched once, with what the recovery set, and the old key is
    ///        not fetched first.
    void refetchCurrent();

    /// @brief Marks its entry stale and refetches it.
    void invalidate();

    /// @brief Forgets its entry without releasing it: the session dropped the cache.
    void detach();

    /// @brief The current entry. A tracked read.
    /// @return It, or `nullptr` while idle.
    [[nodiscard]] std::shared_ptr<CacheEntry> const& entry() const { return _entry.get(); }

    /// @brief The declaration.
    /// @return It.
    [[nodiscard]] QueryDecl const& decl() const noexcept { return *_decl; }

    /// @brief The model type.
    /// @return It.
    [[nodiscard]] std::string const& typeId() const noexcept { return _typeId; }

    /// @brief The channel its alias is bound to now.
    /// @return It, or `nullptr` while the alias is unbound.
    [[nodiscard]] std::shared_ptr<ModelChannel> channel() const;

private:
    /// @brief What a key evaluates to: the cache entry it names and the body, or idle.
    struct Target {
        bool idle = true;
        std::string token;
        Value body;
        std::string bodyText;
        bool operator==(Target const& other) const;
    };

    void switchTo(Target const& target);

    QueryDecl const* _decl;
    std::string _typeId;
    RtScope* _scope;
    ModelAlias* _alias;
    reactive::Signal<std::shared_ptr<CacheEntry>> _entry;
    reactive::Signal<Value> _carried;
    reactive::Signal<bool> _waiting;
    Target _target;
    Target _pendingTarget;
    // The query's key, owned by its scope's nodes; set once `start` made it.
    reactive::Computed<Target> const* _key = nullptr;
    reactive::TimerHandle _debounce;
    reactive::TimerHandle _refresh;
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
};

/// @brief One run of a mutation, as a command sees it: whether it succeeded, and the reply.
using RunDone = std::function<void(bool succeeded, Value const& reply)>;

/// @brief A mutation at run time (spec 5 §5): concurrency, optimistic writes and the reply's effects.
class MORPH_API MutationRuntime {
public:
    /// @brief A mutation of @p scope.
    /// @param decl  The declaration.
    /// @param scope The scope.
    /// @param alias The model alias it goes through.
    MutationRuntime(MutationDecl const& decl, RtScope& scope, ModelAlias* alias);

    MutationRuntime(MutationRuntime const&) = delete;
    MutationRuntime& operator=(MutationRuntime const&) = delete;
    MutationRuntime(MutationRuntime&&) = delete;
    MutationRuntime& operator=(MutationRuntime&&) = delete;

    /// @brief Drops the replies still to come.
    ~MutationRuntime();

    /// @brief Runs it, queues the run or refuses it, as its concurrency says.
    /// @param body     The body.
    /// @param bindings The names bound where the run was started, for `onSuccess` and `onError`.
    /// @param done     Called once the run settles, or at once when it is refused.
    void run(Value body, std::shared_ptr<Bindings const> bindings, RunDone done);

    /// @brief Whether a run is in flight or queued. A tracked read.
    /// @return It.
    [[nodiscard]] bool pending() const { return _inFlight.get() > 0; }

    /// @brief The last applied result. A tracked read.
    /// @return It.
    [[nodiscard]] Value result() const { return _result.get(); }

    /// @brief The last applied failure's text, or `null`. A tracked read.
    /// @return It.
    [[nodiscard]] Value error() const { return _error.get(); }

    /// @brief The last applied failure's kind, or `null`. A tracked read.
    /// @return It.
    [[nodiscard]] Value errorKind() const { return _errorKind.get(); }

    /// @brief The last applied failure's action refusal, or `null`. A tracked read.
    /// @return It.
    [[nodiscard]] Value refusal() const { return _refusal.get(); }

    /// @brief The declaration.
    /// @return It.
    [[nodiscard]] MutationDecl const& decl() const noexcept { return *_decl; }

    /// @brief The model type.
    /// @return It.
    [[nodiscard]] std::string const& typeId() const noexcept { return _typeId; }

private:
    struct Run {
        std::uint64_t id = 0;
        /// The id observers know the run by: `Session::nextRunId`, unique across mutations.
        std::uint64_t sessionRun = 0;
        Value body;
        std::shared_ptr<Bindings const> bindings;
        RunDone done;
        std::string serialKey;
        std::vector<std::shared_ptr<CacheEntry>> layered;
    };

    void send(std::shared_ptr<Run> const& run);
    void settle(std::shared_ptr<Run> const& run, std::optional<Value> reply, std::exception_ptr const& error);
    void next(std::string const& serialKey);
    void applyOptimistic(Run& run);
    void dropLayers(Run& run);

    MutationDecl const* _decl;
    RtScope* _scope;
    ModelAlias* _alias;
    std::string _typeId;
    ScreenCore* _core;
    reactive::Signal<int> _inFlight;
    reactive::Signal<Value> _result;
    reactive::Signal<Value> _error;
    reactive::Signal<Value> _errorKind;
    reactive::Signal<Value> _refusal;
    std::uint64_t _runs = 0;
    std::uint64_t _latest = 0;
    std::map<std::string, std::deque<std::shared_ptr<Run>>, std::less<>> _queues;
    std::set<std::string, std::less<>> _busy;
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
};

}  // namespace detail

/// @brief What an `Interpreter` is built with.
struct InterpreterOptions {
    /// @brief The application's side of the commands; `nullptr` answers every one "not supported". Borrowed.
    InterpreterHost* host = nullptr;
    /// @brief Called with each diagnostic as it is reported: a runtime expression error, a command the host does not
    ///        support, a node kind this interpreter does not lower.
    std::function<void(Refusal const&)> diagnostics;
};

/// @brief The kind of a tool of a screen's head.
enum class HeadToolKind : std::uint8_t {
    Search,  ///< A search field: one line of text.
    Select,  ///< A choice of one option.
};

namespace detail {

/// @brief Where a command list runs: its scope, the names bound there, and what it may answer.
struct CommandCtx {
    /// @brief The scope.
    std::shared_ptr<RtScope> scope;
    /// @brief The names bound here; a `seq` step's `as` adds to them.
    std::shared_ptr<Bindings> bindings;
    /// @brief Set by `cancelClose`.
    std::shared_ptr<bool> cancelClose;
    /// @brief The innermost boundary, for `retry`.
    std::shared_ptr<BoundaryRuntime> boundary;
};

/// @brief Called once when a command finishes: whether it succeeded.
using Done = std::function<void(bool)>;

/// @brief Where a node is lowered: its scope, the names bound there and the boundary that owns its queries.
struct LowerCtx {
    /// @brief The scope.
    std::shared_ptr<RtScope> scope;
    /// @brief The names bound here.
    std::shared_ptr<Bindings const> bindings;
    /// @brief The innermost boundary.
    std::shared_ptr<BoundaryRuntime> boundary;
    /// @brief The spacing step of the vertical list whose node is being lowered; 0 where the node sits in no such
    ///        list. Only a `switch` reads it, so its cases space their nodes as the list around the switch does.
    int listGap = 0;
};

/// @brief An `actionButton` part, lowered in the scope of the list it belongs to: what a screen's head shows and
///        an overlay's footer draws as buttons.
struct ActionModel {
    /// @brief The action's id, unique in its list.
    std::string id;
    /// @brief The part's JSON Pointer, where a diagnostic about it is reported.
    std::string path;
    /// @brief The label.
    Prop<std::string> label;
    /// @brief The name of an icon; empty for none.
    Prop<std::string> icon;
    /// @brief Whether the button draws its icon alone, the label being its accessible name and tooltip.
    Prop<bool> iconOnly = false;
    /// @brief How much it stands out.
    ButtonEmphasis emphasis = ButtonEmphasis::Normal;
    /// @brief Whether it can be run.
    Prop<bool> enabled = true;
    /// @brief Whether it is shown.
    Prop<bool> visible = true;
    /// @brief Runs its `onClick` as a user event, in one batch; empty when it declares none.
    Action run;
};

/// @brief A list of lowered `actionButton` parts, in document order.
using ActionModels = std::vector<ActionModel>;

/// @brief A `headBadge` part, lowered in the screen scope.
struct BadgeModel {
    /// @brief The badge's text.
    Prop<std::string> text;
    /// @brief Its tone.
    Prop<Tone> tone = Tone::Neutral;
    /// @brief The name of an icon; empty for none.
    Prop<std::string> icon;
    /// @brief Whether it is shown.
    Prop<bool> visible = true;
};

/// @brief A `headMenuItem` part, lowered in the screen scope: the entry as a menu item, with the id
///        `ScreenHead::choose` takes and its submenu's entries beside it.
struct HeadMenuModel {
    /// @brief The entry's id; empty for a separator or a submenu.
    std::string id;
    /// @brief The part's JSON Pointer, where a diagnostic about it is reported.
    std::string path;
    /// @brief The entry, its own `items` empty.
    MenuItem item;
    /// @brief The submenu's entries; empty for an entry that is chosen.
    std::vector<HeadMenuModel> items;
};

/// @brief A `headTool` part, lowered in the screen scope.
struct HeadToolModel {
    /// @brief The tool's id, unique among the head's tools.
    std::string id;
    /// @brief The part's JSON Pointer, where a diagnostic about it is reported.
    std::string path;
    /// @brief A search field or a select.
    HeadToolKind kind = HeadToolKind::Search;
    /// @brief The accessible name.
    Prop<std::string> label;
    /// @brief A search's text.
    Prop<std::string> value;
    /// @brief A search's placeholder.
    Prop<std::string> placeholder;
    /// @brief A select's options.
    Prop<std::vector<SelectOption>> options;
    /// @brief A select's chosen key, or none.
    Prop<std::optional<Key>> selected;
    /// @brief The width in units; 0 for the content's.
    int width = 0;
    /// @brief Whether it can be used.
    Prop<bool> enabled = true;
    /// @brief Whether it is shown.
    Prop<bool> visible = true;
    /// @brief Runs a search's `onChange` or a select's `onChoose` with `event` the edit, as a user event.
    std::function<void(Value)> edit;
    /// @brief Runs a search's `onSubmit` with `event` its text, as a user event; empty for a select.
    std::function<void(Value)> submit;
};

/// @brief How many command lists the inspector's log keeps.
inline constexpr std::size_t kCommandLogSize = 256;

/// @brief How often, in lowered nodes, the inspector's node list drops the nodes whose scope is gone.
inline constexpr std::size_t kPruneLoweredAt = 256;

/// @brief The key a command is written with.
/// @param kind The command's kind.
/// @return Its key, such as `refetch`.
[[nodiscard]] MORPH_API std::string commandKey(CommandKind kind);

/// @brief What the horizontal positions of a chart's points have been, across every series.
struct ChartAxisKinds {
    /// @brief A position was a number.
    bool numbers = false;
    /// @brief A position was a date or an instant.
    bool instants = false;
};

/// @brief One reference line of a chart as the lowering reads it, before its default label is known.
struct ChartRawLine {
    /// @brief Where it is drawn, in the chart's unit.
    math::Rational value;
    /// @brief Its label; none takes the value at the chart's places.
    std::optional<std::string> label;
    /// @brief Its colour.
    Tone tone = Tone::Neutral;
    /// @brief Its stroke.
    ChartLineStyle style = ChartLineStyle::Solid;
};

/// @brief One point of a chart's series as the lowering reads it, before the range is known.
struct ChartRawPoint {
    /// @brief The element's position in the series' list.
    std::size_t index = 0;
    /// @brief Its `x`: a number, or an instant's milliseconds since the epoch; none without `x`.
    std::optional<math::Rational> x;
    /// @brief Its value in the chart's unit; none is a gap.
    std::optional<math::Rational> y;
    /// @brief Its label on the horizontal axis.
    std::string xLabel;
    /// @brief The tone that marks it.
    std::optional<Tone> tone;
    /// @brief The text beside it.
    std::string note;
};

/// @brief One mounted screen's interpreter: its document, diagnostics, slots and preferences, and the code that
///        declares scopes, lowers nodes and runs commands.
class MORPH_API ScreenCore {
public:
    /// @brief The core of a screen about to mount.
    /// @param doc        The document.
    /// @param owner   The session.
    /// @param calls   The dispatcher.
    /// @param options The interpreter's options.
    /// @param mount   The mount's options.
    ScreenCore(std::shared_ptr<Document const> doc, Session& owner, Dispatcher& calls,
               InterpreterOptions const& options, MountOptions const& mount);

    ScreenCore(ScreenCore const&) = delete;
    ScreenCore& operator=(ScreenCore const&) = delete;
    ScreenCore(ScreenCore&&) = delete;
    ScreenCore& operator=(ScreenCore&&) = delete;
    /// @brief Takes the screen off the session, so a later backend switch does not run its `onBackendChange`.
    ~ScreenCore() { session.detachScreen(*this); }

    /// @brief Runs a lifecycle list in the document's own scope, as a screen's or app document's own command list.
    /// @param key The lifecycle key, such as `onBackendChange`.
    void runLifecycleOf(std::string_view key);

    /// @brief The document.
    /// @return It.
    [[nodiscard]] Document const& doc() const noexcept { return *_doc; }

    /// @brief The runtime.
    /// @return It.
    [[nodiscard]] reactive::Runtime& runtime() const noexcept { return session.runtime(); }

    /// @brief Reports a diagnostic once per path.
    /// @param path   Where.
    /// @param reason What.
    void report(std::string_view path, std::string_view reason);

    /// @brief The next slot id: the interpreter numbers every bound property of the document.
    /// @return It.
    [[nodiscard]] SlotId nextSlot() noexcept { return _slots++; }

    // --- Declaring a scope -------------------------------------------------------------------------------------

    /// @brief Declares a scope's names and starts its reactive parts.
    /// @param scope    The scope.
    /// @param decl     Its declarations; `nullptr` when it declares none.
    /// @param content  The nodes whose elements belong to it.
    /// @param commands The command lists whose transfers belong to it.
    void declare(RtScope& scope, ScopeDecl const* decl, std::vector<DocNode const*> const& content,
                 std::vector<Commands const*> const& commands);

    /// @brief Makes a state's signal from its init, once.
    /// @param scope The scope that declares it.
    /// @param slot  The state.
    void initState(RtScope const& scope, StateSlot& slot) const;

    /// @brief Reads an element value. A tracked read.
    /// @param element The element.
    /// @param name    The value's name.
    /// @return The value.
    [[nodiscard]] Value elementValue(ElementState const& element, std::string_view name) const;
    /// @brief Whether @p element is a group with an element inside it the user touched: a group has no value of its
    ///        own to touch, so its errors show under the `touched` policy once any of its inputs is.
    /// @param element The element.
    /// @return True when it is such a group.
    [[nodiscard]] static bool touchedWithin(ElementState const& element);

    /// @brief The current value of an input, checkbox or select: what `required` tests.
    /// @param element The element.
    /// @return The value.
    [[nodiscard]] Value inputValue(ElementState const& element) const;

    /// @brief Shows @p text as the table's status-line message for `kStatusMessageHold`, on the session's scheduler
    ///        (without one, until the table's next gesture); an empty @p text clears the message and notifies
    ///        nothing. Under `status: false` a non-empty text goes to `InterpreterHost::notify` as a warning instead.
    /// @param table The table's element; one that is neither a model-backed table nor one over `rows` is left alone.
    /// @param text  The message, or empty.
    void tableMessage(ElementState& table, std::string text);
    /// @brief Shows @p text as a model-backed table's status-line message, as the overload above does, with the
    ///        choices it offers after it; choosing one clears the message and runs what it was given with. The
    ///        choices go with the message: when it is cleared or replaced, or its hold ends. Under `status: false`
    ///        the text goes to the host and no choice is offered.
    /// @param table   The table's element; for one that is not a model-backed table the choices are not offered.
    /// @param text    The message; not empty.
    /// @param entries Each choice, by its id and label, with what choosing it runs.
    void tableMessage(ElementState& table, std::string text,
                      std::vector<std::pair<StatusEntry, std::function<void()>>> entries);

    /// @brief Says a table state's failed save in the status line of every table of the screen mounted with its key.
    /// @param key    The state's key.
    /// @param result What the store answered.
    void tableSaveFailed(table::TableStateKey const& key, table::SaveResult const& result);

    /// @brief How many entries a model-backed table's index of row scopes holds, for tests: one per realized row
    ///        whose scope `onActivate` can run in.
    /// @param table The table's element.
    /// @return The entries; 0 for an element that is not a model-backed table.
    [[nodiscard]] static std::size_t tableRowScopes(ElementState const& table);

    /// @brief Shows a model-backed table's state at the `columns` and `saved` points that name it: a `columns` point
    ///        reads the column layout (`table::ColumnLayoutValue` as a record), a `saved` point the saved view's name.
    /// @param table The table's element.
    void tablePoints(ElementState& table);

    /// @brief Writes a choice for a `columns` or `saved` point into the state of the model-backed table it names,
    ///        which saves it as any change of the table's arrangement is saved.
    /// @param point The point.
    /// @param value The choice: a layout record for `columns`, a name or `null` for `saved`.
    /// @return Nothing when the point names no mounted model-backed table; otherwise whether the choice was taken
    ///         (a value that does not read as the point's part is refused).
    [[nodiscard]] std::optional<bool> tablePoint(CustomizePoint const& point, Value const& value);

    // --- Commands ----------------------------------------------------------------------------------------------

    /// @brief Runs a command list: each command in order in one batch; done when every command in it is.
    /// @param commands The list.
    /// @param ctx      Where it runs.
    /// @param done     Called once, with whether every command succeeded; may be empty.
    void runList(Commands const& commands, CommandCtx const& ctx, Done done);

    /// @brief Runs one command.
    /// @param command The command.
    /// @param ctx     Where it runs.
    /// @param done    Called once, with whether it succeeded.
    void runOne(Command const& command, CommandCtx const& ctx, Done done);

    /// @brief Runs a command list for an event of the user's.
    /// @param commands The list; `nullptr` runs nothing.
    /// @param ctx      Where it was lowered.
    /// @param event    The event's payload.
    void runEvent(Commands const* commands, LowerCtx const& ctx, Value event);

    /// @brief Lowers a list of `actionButton` parts into reactive models: label, icon, enabled and visible bound in
    ///        @p ctx's scope, and a run function that runs the part's `onClick` there as a user event.
    /// @param parts The parts, in order.
    /// @param ctx   The scope the list belongs to.
    /// @return The models, in the parts' order.
    [[nodiscard]] ActionModels lowerActionParts(std::span<DocNodePtr const> parts, LowerCtx const& ctx);

    /// @brief Lowers a screen head's `menu` in the scope @p ctx names.
    /// @param parts The `headMenuItem` parts.
    /// @param ctx   The scope.
    /// @return The entries, in document order, submenus included.
    [[nodiscard]] std::vector<HeadMenuModel> lowerHeadMenu(std::span<DocNodePtr const> parts, LowerCtx const& ctx);

    /// @brief Lowers a screen head's `tools` in the scope @p ctx names.
    /// @param parts The `headTool` parts.
    /// @param ctx   The scope.
    /// @return The tools, in document order.
    [[nodiscard]] std::vector<HeadToolModel> lowerHeadTools(std::span<DocNodePtr const> parts, LowerCtx const& ctx);

    /// @brief An overlay's actions as the action bar its footer shows: a button per action, test id
    ///        `action_<id>`, shown and enabled as the action is.
    /// @param actions The lowered actions, in order.
    /// @return The `ActionBar` node.
    [[nodiscard]] static Node actionBarOf(ActionModels actions);

    /// @brief Lowers a screen head's `headBadge` parts, bound in @p ctx's scope.
    /// @param parts The parts, in order.
    /// @param ctx   The screen scope.
    /// @return The badges, in the parts' order.
    [[nodiscard]] std::vector<BadgeModel> lowerBadgeParts(std::span<DocNodePtr const> parts, LowerCtx const& ctx);

    // --- Lowering ----------------------------------------------------------------------------------------------

    /// @brief Lowers a document node to a view node.
    /// @param node The node.
    /// @param ctx  Where.
    /// @return The view node.
    [[nodiscard]] Node lower(DocNode const& node, LowerCtx const& ctx);

    /// @brief Lowers a node list into one node: the node itself, or a column of them.
    /// @param nodes The nodes.
    /// @param ctx   Where.
    /// @param gap   The spacing step between the nodes of a list of several; a single node is lowered unwrapped.
    /// @return The node.
    // NOLINTNEXTLINE(misc-no-recursion): one level per nested node of the document.
    [[nodiscard]] Node lowerAll(std::span<DocNodePtr const> nodes, LowerCtx const& ctx, int gap = 0);

    /// @brief The size request of one child of a holder.
    /// @param node The child.
    /// @param own  The names the holder's own scope declares.
    /// @param ctx  Where the holder is lowered.
    /// @return The child's hints; content size where its sizing reads a name in @p own.
    [[nodiscard]] LayoutHints nodeHints(DocNode const& node, std::set<std::string, std::less<>> const& own,
                                        LowerCtx const& ctx);

    /// @brief Reports each child of an overlay's or a case's content that sizes itself by a share, once at @p path,
    /// and
    ///        makes it fill the holder instead.
    /// @param children The content, lowered.
    /// @param path     Where the report points.
    void fillHolder(std::vector<Node>& children, std::string const& path);
    /// @brief The size request of a holder of @p content: the largest stretch of its children on each axis.
    ///
    /// A literal sizing is read as written and a computed one is evaluated once in the enclosing scope. A computed
    /// sizing that reads a name the holder's own scope declares cannot be evaluated before the holder mounts; it is
    /// reported and counts as content.
    /// @param content The holder's document children.
    /// @param decl    The declarations of the holder's own scope; `nullptr` for none.
    /// @param ctx     Where the holder is lowered.
    /// @return The hints; bounds and `alignSelf` stay unset, because they belong to a child.
    [[nodiscard]] LayoutHints holderHints(std::span<DocNode const* const> content, ScopeDecl const* decl,
                                          LowerCtx const& ctx);

    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): the screen's shared state, which its
    // scopes and runtimes read and write directly.
    /// @brief The session.
    Session& session;
    /// @brief The dispatcher.
    Dispatcher& dispatcher;
    /// @brief The host; may be `nullptr`.
    InterpreterHost* host;
    /// @brief The diagnostic callback.
    std::function<void(Refusal const&)> onDiagnostic;
    /// @brief Moves focus to a mounted element by its id; empty until the screen is mounted, and again once it is
    ///        unmounting.
    std::function<bool(std::string_view)> focusElement;
    /// @brief Puts text on the clipboard through the backend's `IViewBackend::copyText`, returning whether it did;
    ///        empty until the screen is mounted, and again once it is unmounting.
    std::function<bool(std::string_view)> clipboard;
    /// @brief Reads the clipboard's text through the backend's `IViewBackend::pasteText`; empty until the screen is
    ///        mounted, and again once it is unmounting.
    std::function<std::optional<std::string>()> pasteboard;
    /// @brief The models the opener lends.
    std::map<std::string, BorrowedModel, std::less<>> borrowed;
    /// @brief The screen scope's reactive depth.
    std::size_t depth;
    /// @brief The diagnostics reported, in order.
    std::vector<Refusal> diagnostics;
    /// @brief The paths and reasons already reported.
    std::set<std::string, std::less<>> reported;
    /// The sheet whose regions are being lowered, while they are.
    std::optional<PageGeometry> pageBeingLowered;
    /// @brief The customization choices, by point.
    reactive::Signal<ValueRecord> prefs;
    /// @brief The points' defaults.
    ValueRecord prefDefaults;
    /// @brief The screen's `submitted`.
    reactive::Signal<bool> submitted;
    /// @brief The open dialogs and drawers, the mutation runs and the transfers in flight, across every scope of the
    ///        screen: what a close waits for (spec 6 §3).
    reactive::Signal<int> pendingWork;
    /// @brief Whether this screen records what the inspector reads: its session's `developmentTools`, read once.
    bool const recording;

    /// @brief Counts pending work in or out.
    /// @param delta What started (positive) or ended (negative).
    void addPending(int delta) { pendingWork.set(pendingWork.peek() + delta); }
    /// @brief The document's own scope, once mounted.
    std::weak_ptr<RtScope> rootScope;
    /// @brief A state of the screen this one replaces: its declared type's name and its value.
    struct KeptState {
        /// @brief The declared type's name.
        std::string type;
        /// @brief The value.
        Value value;
    };
    /// @brief The states of the own scope of the screen this one replaces, by name (`MountOptions::keepStateOf`).
    std::map<std::string, KeptState, std::less<>> keptStates;
    /// @brief A node as it was lowered, which the inspector lists while its scope lives.
    struct LoweredNode {
        /// @brief The node.
        DocNode const* node = nullptr;
        /// @brief The scope it was lowered in.
        std::weak_ptr<RtScope> scope;
        /// @brief The names bound where it was lowered.
        std::weak_ptr<Bindings const> bindings;
    };
    /// @brief Every node lowered, in lowering order; an entry whose scope is gone is pruned as more are added.
    std::vector<LoweredNode> loweredNodes;
    /// @brief The screen's live scopes, in creation order.
    std::vector<RtScope*> scopes;
    /// @brief The elements of the screen's tables that keep a persisted state, model-backed or over `rows`, while
    ///        each lives: what a failed save of a table state is said in.
    std::vector<ElementState*> modelTables;
    /// @brief The latest command lists run, oldest first, at most `kCommandLogSize`.
    std::deque<CommandLogEntry> commandLog;
    /// @brief Set while the inspector evaluates, so that an expression error it meets is not reported.
    bool inspecting = false;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

    /// @brief The value a state of the screen's own scope keeps from the screen it replaces: that screen's value of
    ///        the state of the same name, when its declared type is the same and admits it.
    /// @param scope The scope that declares the state.
    /// @param decl  The declaration.
    /// @return The value, or nothing for a state that starts from its init.
    [[nodiscard]] std::optional<Value> keptState(RtScope const& scope, StateDecl const& decl) const;

    /// @brief What a form session submits through; the same type as `forms::Submitter`.
    using FormSubmitter =
        std::function<async::Completion<std::string>(std::string_view actionType, std::string bodyJson)>;

private:
    void wireElement(ElementState& element);
    void wireErrors(ElementState& element);
    /// A table over `rows`: its rows in its engine's view, each grouped row's run and each run's heading entry, and
    /// the page of them it shows, by its `pageSize` and `page`. Nothing while its stored state is read.
    TableView tableView(RowsTable const& table);
    /// The rows, source, engine and page of a table over `rows`, read in the table's scope with @p bindings; with
    /// @p persisted, its stored state is loaded and its changes saved, and it is the element's own.
    std::unique_ptr<RowsTable> makeRowsTable(ElementState& element, std::shared_ptr<Bindings const> bindings,
                                             bool persisted);
    /// Brings a table over `rows` to its sort: the columns its keys read are in use, and the engine orders by them,
    /// within the groups first when it has any. A sort that is not a list of its sortable columns is reported.
    void sortRowsTable(RowsTable& table, Value const& sort);
    /// What a table over `rows` does when its engine's view changed: reports an invalid cell of a sort column once,
    /// and lets the rows through once the stored order is in place.
    void rowsTableChanged(RowsTable& table);
    /// Shows @p text in the status line of a table over `rows`, as `tableMessage` does for a table's element.
    void rowsTableMessage(RowsTable& rows, std::string text) const;
    /// Applies a table over `rows`' stored state once its load completed, or reports why it could not.
    void storedRowsState(RowsTable& table, std::optional<std::string> const& json, std::string const& failure);
    void collect(DocNode const& node, RtScope& scope, std::vector<Commands const*>& commands);
    /// The readers every kind's lowering shares, bound to one node.
    class KindKit;
    /// A column or a row. Its own function, so a deep document stacks only what this kind needs per level.
    Node lowerStack(DocNode const& node, LowerCtx const& ctx, ElementState* element);
    /// A grid. Its own function, so a deep document stacks only what this kind needs per level.
    Node lowerGrid(DocNode const& node, LowerCtx const& ctx, ElementState* element);
    /// A panel. Its own function, so a deep document stacks only what this kind needs per level.
    Node lowerPanel(DocNode const& node, LowerCtx const& ctx, ElementState* element);
    /// A splitter. Its own function, so a deep document stacks only what this kind needs per level.
    Node lowerSplitter(DocNode const& node, LowerCtx const& ctx, ElementState* element);
    /// A collapsible section. Its own function, so a deep document stacks only what this kind needs per level.
    Node lowerCollapsible(DocNode const& node, LowerCtx const& ctx, ElementState* element);
    /// Reports the `gap` of each `forEach` directly in an action bar whose computed `axis` is horizontal: such a
    /// `forEach` is part of the bar's row and takes the theme's action gap. A literal axis is refused at load.
    void reportActionBarGaps(DocNode const& bar, LowerCtx const& ctx);
    /// A drop zone. Its own function, so a deep document stacks only what this kind needs per level.
    Node lowerDropZone(DocNode const& node, LowerCtx const& ctx, ElementState* element);
    /// A page, its regions lowered with its geometry in hand.
    Node lowerPage(DocNode const& node, LowerCtx const& ctx, ElementState* element);
    /// A page region; only a page lowers one.
    Node lowerPageRegion(DocNode const& node, LowerCtx const& ctx, ElementState* element);
    /// A locale scope: a holder column over a scope that writes in another locale.
    Node lowerLocaleScope(DocNode const& node, LowerCtx const& ctx);
    Node lowerKind(DocNode const& node, LowerCtx const& ctx, ElementState* element);
    /// The leaf-like kinds, from `text` to `menu`; empty for any other kind. They are lowered apart from the
    /// containers so that a deep document does not stack their locals once per level: `lowerKind` recurses into
    /// its children, and in an unoptimised build a frame holds every branch's locals at once.
    std::optional<Node> lowerWidget(DocNode const& node, LowerCtx const& ctx, ElementState* element);
    /// The display kinds, from `banner` to `emptyState`, apart from the containers for the same reason as
    /// `lowerWidget`; empty for any other kind.
    std::optional<Node> lowerDisplay(DocNode const& node, LowerCtx const& ctx, ElementState* element);
    Node lowerDialog(DocNode const& node, LowerCtx const& ctx, ElementState* element);
    Node lowerSwitch(DocNode const& node, LowerCtx const& ctx);
    Node lowerRows(DocNode const& node, LowerCtx const& ctx, ElementState* element);
    /// A model-backed table: a list of at most one table, keyed by its description's revision, over its rows.
    Node lowerModelTable(DocNode const& node, LowerCtx const& ctx, ElementState* element);
    /// A grid of a form: a table over a form node's collection, its rows the field's and its cells its members.
    Node lowerFormGrid(DocNode const& node, LowerCtx const& ctx, ElementState* element);
    /// The table of a grid of a form, kept in @p element: a `table` node's with `form` and `field`, or a field
    /// overlay's `grid` part's.
    Node formGridTable(DocNode const& part, LowerCtx const& ctx, ElementState& element, forms::FormSession& form,
                       forms::FieldState& array);
    /// The table node of the grid @p element keeps, made anew by each lowering.
    Node gridTableNode(DocNode const& part, LowerCtx const& ctx, ElementState& element);
    /// The rows a grid with summaries or a group is mounted over: its summary decorator, each data row's cell as the
    /// grid draws it, each summary row's cells its part's expressions, and each run's heading.
    TableSource summarySource(DocNode const& part, LowerCtx const& ctx, GridTable& held,
                              std::vector<TableColumn> const& columns);
    /// A summary cell's value: the expression the summary row's part gives the column, read with `rows` and `column`;
    /// none when the part gives the column none.
    std::optional<Value> summaryValue(DocNode const& table, Value const& record, std::string const& column,
                                      LowerCtx const& ctx, ValueList rows);
    /// A summary row's own parts: the summary flag, and its part's `label` as the row's name.
    TableRowLook summaryLook(DocNode const& table, LowerCtx const& ctx, Value const& record);
    /// A grid's summary cell text: its value over the data rows in its scope, at the column's member's places.
    std::string gridSummaryText(DocNode const& part, LowerCtx const& ctx, GridTable& held, Value const& record,
                                std::string const& column);
    /// What a grid's part (a `table` with a `form`, a field overlay's `grid`) gives beyond its table: its columns and
    /// its options.
    void gridParts(DocNode const& part, LowerCtx const& ctx, forms::FieldState const& array,
                   std::vector<GridColumn>& columns, GridOptions& options);
    /// What any grid's part gives of its walk and its activation.
    void gridOptions(DocNode const& part, LowerCtx const& ctx, GridOptions& options);
    /// A fixed grid: a table whose rows are its `layoutRows`, each cell a field of the form `form` names, an
    /// expression or a text, kept in @p element as a grid of a form is.
    Node lowerFixedGrid(DocNode const& node, LowerCtx const& ctx, ElementState* element);
    /// The table a model-backed table shows for the description its rows hold, or the status line alone without one.
    Node modelTableOf(DocNode const& node, LowerCtx const& ctx, ElementState& element);
    /// A model-backed table with a column its description edits, as a grid over its cells: the grid's controller, its
    /// rows handed to @p source; null for a table with no editable column.
    std::shared_ptr<GridController> editableModelTable(DocNode const& node, LowerCtx const& ctx, ElementState& element,
                                                       table::TableDescription const& description,
                                                       std::vector<table::TableColumn> const& shown,
                                                       TableSource& source, TableOptions const& options,
                                                       std::shared_ptr<std::weak_ptr<ModelGridSource>> const& drafts);
    /// Makes a model-backed table's rows, its description query and its persisted state's load.
    void wireModelTable(ElementState& element);
    /// Takes the description its query holds now: reports the filters a new revision drops, then hands it to the rows.
    void describedTable(ElementState& element, Value const& described);
    /// Applies the filter a typed list action's table held while its kinds waited for the first reply: a stored
    /// one fitted to those kinds, each filter it drops reported; another set, or reported as refused.
    void inferredTable(ElementState& element);
    /// Applies a model-backed table's stored state once its load completed, or reports why it could not.
    void storedTableState(ElementState& element, std::optional<std::string> const& json, std::string const& failure);
    /// A user's gesture on a model-backed table: the stored state no longer replaces the arrangement, and the
    /// status-line message goes.
    void tableGesture(ElementState& element);
    /// Wires a table's header, menus and arrangement to its configuring: sort gestures held to the table's bound,
    /// layout gestures to what it allows, each refusal shown in its status line and each change run through its
    /// commands; and its copy, of the rows `TableConfig::gather` gives. @p keep is held by every handler, for a table
    /// its mount owns. Returns the column chooser and the
    /// sort dialog its menu opens, to mount beside the table; null for a table with no menus.
    Node configureTable(TableConfig& config, LowerCtx const& ctx, TableOptions& options,
                        std::shared_ptr<void const> const& keep);
    /// Takes the columns a model-backed table's layout arranges from the description its rows hold now.
    void modelLayoutColumns(ElementState& element) const;
    /// A grouped table's heading row: its `group`'s `heading`, lowered as one cell in a row scope of its own whose
    /// row is the group's run record and whose key is `key`, the row's canonical key, as a data row's scope has.
    TableRowLook headingRow(DocNode const& node, LowerCtx const& ctx, reactive::Signal<Value> const& row,
                            std::string key);
    /// A `rowTone` value as a tone: `neutral`, `info`, `ok`, `warn`, `err`, or `null` for none. Another value is
    /// reported, at @p path, while @p noted is still false, and shows no tone.
    std::optional<Tone> rowToneOf(Value const& value, std::string const& path, bool& noted);
    /// A `rowSpans` value as spans: a list of `{column, rows}` records, `column` a column's id from @p columns and
    /// `rows` an integer from 1; `null` and `[]` are none. A record of another shape is dropped and reported once
    /// through @p noted.
    std::vector<RowSpan> rowSpansOf(Value const& value, std::vector<std::string> const& columns,
                                    std::string const& path, bool& noted);
    Node lowerBoundary(DocNode const& node, LowerCtx const& ctx);
    /// A group: a column of its legend (a text carrying the mark), its children, its description and its shown
    /// errors, named and announced as a group.
    Node lowerGroup(DocNode const& node, LowerCtx const& ctx, ElementState* element, std::vector<Node> children);
    Node lowerCustom(DocNode const& node, LowerCtx const& ctx);
    Node lowerChart(DocNode const& node, LowerCtx const& ctx, ElementState* element);
    /// Everything a chart draws, read from its node in @p scope; the one tracked read of the chart's binding.
    ChartData chartData(DocNode const& node, RtScope const& scope, Bindings const* bindings);
    /// One series' points, read exactly: each value in @p unit, the chart's, which the first value fixes.
    std::vector<detail::ChartRawPoint> chartPoints(DocNode const& series, RtScope const& scope,
                                                   Bindings const* bindings, std::optional<std::string>& unit,
                                                   detail::ChartAxisKinds& kinds);
    /// A chart value as an exact magnitude in @p unit, which the first value read fixes; a value that is not a number
    /// or does not convert is reported at @p path and read as none.
    std::optional<math::Rational> chartMagnitude(Value const& value, std::string const& path,
                                                 std::optional<std::string>& unit);
    /// A chart tone: one of the five names, or `null` for none; another value is reported at @p path.
    std::optional<Tone> chartToneOf(Value const& value, std::string const& path);
    /// A point's `x` as a number or an instant's milliseconds, noting its kind in @p kinds; none for `null`, and for
    /// another value, which is reported at @p path.
    std::optional<math::Rational> chartPosition(Value const& value, std::string const& path,
                                                detail::ChartAxisKinds& kinds);
    /// A chart's reference lines, read exactly in @p unit; a line whose value is `null` or not a number is left out.
    std::vector<detail::ChartRawLine> chartLines(DocNode const& node, RtScope const& scope, Bindings const* bindings,
                                                 std::optional<std::string>& unit);
    /// @p range with `yMin` and `yMax` in place of its computed ends; ends that leave it empty are reported, and
    /// @p range is kept.
    ChartRange chartFixedRange(DocNode const& node, RtScope const& scope, Bindings const* bindings,
                               std::optional<std::string>& unit, ChartRange const& range);
    /// The drawn series: each point at its own `x` when @p positioned, else at its index, clipped to @p range.
    static void chartSeries(ChartData& data, DocNode const& node, RtScope const& scope, Bindings const* bindings,
                            std::vector<std::vector<detail::ChartRawPoint>>& series, bool positioned,
                            ChartRange const& range);
    /// A form field's control drawn by the host component its overlay names, with the built-in control as the
    /// fallback.
    Node fieldComponent(forms::FieldView const& view, DocNode const& overlay, LowerCtx const& ctx);
    /// A custom node's prop of a declared type over @p read, converted as `lowerCustom` converts a document's.
    CustomProp componentProp(std::string const& prop, std::string const& type, std::function<Value()> read);
    /// A field component's `change`: the payload read as the `value` prop's type and written as the user's edit, or
    /// reported when it does not decode, does not fit the field, or is over the inline cap for a base64 member.
    void changeField(forms::FieldState& state, forms::FieldSpec const& spec, std::string const& path,
                     std::string_view valueType, std::string const& payload);
    Node lowerCustomize(DocNode const& node, LowerCtx const& ctx);
    [[nodiscard]] std::string arrangedBy(std::string const& elementId) const;
    Node lowerArranged(DocNode const& node, LowerCtx const& ctx, ElementState* element, std::string const& point,
                       Axis axis, int gap);
    Node lowerArrangeEditor(DocNode const& node, LowerCtx const& ctx, std::string const& point,
                            CustomizePoint const& spec, std::function<void(Value)> const& choose);
    void wireForm(ElementState& element);
    /// A form's rounds of derived values over its alias: its evaluation and its `derive` action.
    void deriveForm(ElementState& element, ModelAlias* alias);
    Node lowerForm(DocNode const& node, LowerCtx const& ctx, ElementState* element);
    [[nodiscard]] FormSubmitter aliasCalls(ModelAlias* alias);
    [[nodiscard]] FormSubmitter aliasSubmits(ModelAlias* alias, std::string read = {});
    [[nodiscard]] forms::FormSessionOptions engineOptions() const;
    void followFormatting(ElementState& element, RtScope& scope);
    void wireComposite(ElementState& element);
    Node lowerComposite(DocNode const& node, LowerCtx const& ctx, ElementState* element);
    /// A collection's table, kept in its element as a table over `rows` is: an engine over the session's source,
    /// the configuring of every column, and its state persisted under the collection's action.
    void wireCollectionTable(ElementState& element, std::string modelType);
    /// The rows of a collection, lowered as a table on the engine: sorted, filtered, arranged and copied as any other
    /// table, with @p buttons, when set, drawing each row's trailing cell for its key text.
    Node lowerCollectionRows(ElementState& element, LowerCtx const& ctx,
                             std::function<Node(std::string const&)> const& buttons);
    /// Applies a collection's stored state once its load completed, or reports why it could not.
    void storedCollectionState(RowsTable& table, std::optional<std::string> const& json, std::string const& failure);
    void presentForm(DocNode const& node, LowerCtx const& ctx, forms::FormViewOptions& options);
    Node lowerTabs(DocNode const& node, LowerCtx const& ctx, ElementState* element);
    Prop<std::size_t> controlledTabSelection(DocNode const& node, LowerCtx const& ctx, ExprPtr const& selected,
                                             std::shared_ptr<std::vector<std::string>> const& ids);
    std::shared_ptr<RtScope> rowScope(DocNode const& node, LowerCtx const& ctx, reactive::Signal<Value> const& row,
                                      std::vector<DocNode const*> const& content, std::string key);
    bool focusTarget(Command const& command, RtScope const& scope, Bindings const* bindings);
    void commonOf(DocNode const& node, LowerCtx const& ctx, ElementState* element, Common& common);
    void layoutOf(DocNode const& node, LowerCtx const& ctx, LayoutHints& layout);
    void fieldOf(DocNode const& node, LowerCtx const& ctx, ElementState* element, FieldState& field);
    std::vector<MenuItem> menuItems(std::span<DocNodePtr const> parts, LowerCtx const& ctx);
    MenuItem menuItemOf(DocNode const& part, LowerCtx const& ctx);
    void ownQueries(DocNode const& node, RtScope const& scope, BoundaryRuntime& boundary,
                    std::set<std::string, std::less<>>& seen);
    void ownQueriesOf(ExprPtr const& expr, RtScope const& scope, BoundaryRuntime& boundary,
                      std::set<std::string, std::less<>>& seen);
    void runMutation(Command const& command, CommandCtx const& ctx, Done done) const;
    void runSet(Command const& command, CommandCtx const& ctx, Done const& done);
    void runSetApp(Command const& command, std::span<std::string const> rest, Value value, Done const& done);
    void writeSlot(StateSlot& slot, std::span<std::string const> within, Value value, Command const& command,
                   Done const& done);
    void runReset(Command const& command, CommandCtx const& ctx, Done const& done);
    void runHostCommand(Command const& command, CommandCtx const& ctx, Done const& done);
    void runTransfer(Command const& command, CommandCtx const& ctx, Done done);
    void runDownload(Command const& command, CommandCtx const& ctx, Value const& file, Done done);
    // A download's inline bytes, decoded and under the cap, with its name and type; nothing, reported, otherwise.
    std::optional<InlineFile> inlineDownload(Command const& command, CommandCtx const& ctx, Value const& file,
                                             Value const& name);
    void runUpload(Command const& command, CommandCtx const& ctx, Value const& file, Done done);
    bool unsupported(Command const& command, std::string_view what);

    /// @brief A bound property: a constant for a literal, a numbered slot otherwise.
    template <typename T, typename Convert>
    Prop<T> bindProp(LowerCtx const& ctx, ExprPtr const& expr, T fallback, Convert convert) {
        if (!expr) {
            return Prop<T>{std::move(fallback)};
        }
        if (expr->kind == ExprKind::Literal) {
            return Prop<T>{convert(expr->literal, ctx.scope->formatting())};
        }
        return Prop<T>{ui::slot(nextSlot(), [scope = ctx.scope, bindings = ctx.bindings, expr, convert]() -> T {
            return convert(evalIn(*scope, bindings.get(), expr), scope->formatting());
        })};
    }

    /// @brief A text's role: a literal one as the loader checked it; a computed one evaluated in its slot, where a
    ///        name of no role is reported once for its path and drawn `Normal`.
    Prop<TextRole> roleProp(LowerCtx const& ctx, ExprPtr const& expr);
    /// A key-value item: its caption, its value or spans, and its tone, each problem reported once at @p path.
    KeyValueItem keyValueItemOf(Value const& item, std::string const& path, RtScope const& scope);
    /// A text's or a banner's `spans`, reported once at its path when it drops a run.
    Prop<std::vector<TextSpan>> spansProp(LowerCtx const& ctx, ExprPtr const& expr);

    /// @brief A node's surface: the name of a theme surface, or empty for the default. A computed one is evaluated in
    ///        its slot; `null` is no surface, and any other value that is not a string is reported for the node's
    ///        `surface` path and drawn as no surface.
    Prop<std::string> surfaceOf(DocNode const& node, LowerCtx const& ctx);

    /// @brief A property read through @p read, as a numbered slot.
    template <typename T, typename Read>
    Prop<T> slotProp(Read read) {
        return Prop<T>{ui::slot(nextSlot(), std::move(read))};
    }

    /// @brief Evaluates an expression once, untracked: a property the view tree sets once.
    [[nodiscard]] Value once(LowerCtx const& ctx, ExprPtr const& expr) const;

    /// @brief Keeps a command list in the inspector's log.
    void logList(Commands const& commands);

    std::shared_ptr<Document const> _doc;
    SlotId _slots = 1;
    /// @brief Expires with the screen, so a reply that arrives after it is gone reports nothing.
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
};

}  // namespace detail

/// @brief A status badge of a screen's head, as its handle answers it.
struct HeadBadge {
    /// @brief The badge's text.
    std::string text;
    /// @brief Its tone.
    Tone tone = Tone::Neutral;
    /// @brief The name of an icon; empty for none.
    std::string icon;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(HeadBadge const&) const = default;
};

/// @brief Where a screen's head puts its lead, the subtitle and the badges.
enum class LeadPlacement : std::uint8_t {
    /// @brief Under the title.
    Below,
    /// @brief On the title's line, right after the title.
    Inline,
    /// @brief At the trailing edge of the head's first line, after the tools and before the actions.
    End,
};

/// @brief Where a shell shows who is signed in.
enum class PrincipalPlacement : std::uint8_t {
    /// @brief At the sidebar's foot.
    Sidebar,
    /// @brief In the top bar, at its trailing end.
    Head,
};

/// @brief A count the app document puts on a navigator entry (its `navigation`), as `AppRoot::navigationBadges`
///        answers it.
struct NavigationBadge {
    /// @brief The screen id of the entry it goes on.
    std::string screen;
    /// @brief The badge's text.
    std::string text;
    /// @brief Its tone.
    Tone tone = Tone::Neutral;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(NavigationBadge const&) const = default;
};

/// @brief One step of a guided tour, its texts evaluated now, as `AppRoot::tours` answers it.
struct TourStep {
    /// @brief The step's title.
    std::string title;
    /// @brief Its body; empty for none.
    std::string body;
    /// @brief The screen it opens before it shows; empty for none.
    std::string screen;
    /// @brief The params it opens that screen with.
    ValueRecord params;
    /// @brief The test id it rings; empty for a card with nothing ringed.
    std::string target;
    /// @brief The screen whose opening moves the tour on; empty for a step that waits for Next.
    std::string advanceOn;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(TourStep const&) const = default;
};

/// @brief A guided tour, its texts evaluated now, as `AppRoot::tours` answers it.
struct Tour {
    /// @brief Its id.
    std::string id;
    /// @brief The screen it belongs to; empty for a general tour.
    std::string screen;
    /// @brief Its title.
    std::string title;
    /// @brief Its summary; empty for none.
    std::string summary;
    /// @brief Its steps, in order.
    std::vector<TourStep> steps;
    /// @brief A runtime tour's revision on the server; 0 for an app-document tour.
    std::uint64_t revision = 0;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(Tour const&) const = default;
};

/// @brief An action of a screen's head, as its handle answers it.
struct HeadAction {
    /// @brief The action's id, what `ScreenHead::run` takes.
    std::string id;
    /// @brief The label.
    std::string label;
    /// @brief The name of an icon; empty for none.
    std::string icon;
    /// @brief Whether the button draws its icon alone, its label being its accessible name and tooltip.
    bool iconOnly = false;
    /// @brief How much it stands out.
    ButtonEmphasis emphasis = ButtonEmphasis::Normal;
    /// @brief Whether it can be run now.
    bool enabled = true;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(HeadAction const&) const = default;
};

/// @brief An entry of a screen head's own menu, as its handle answers it: a menu entry with the id
///        `ScreenHead::choose` takes.
struct HeadMenuEntry {
    /// @brief The entry's id, what `ScreenHead::choose` takes; empty for a separator or a submenu.
    std::string id;
    /// @brief The caption.
    std::string label;
    /// @brief The name of an icon; empty for none.
    std::string icon;
    /// @brief The chord shown beside the label; empty for none.
    std::string keys;
    /// @brief Engaged: a checkable entry with this mark; `nullopt`: not checkable.
    std::optional<bool> checked;
    /// @brief Whether it can be chosen, or its submenu opened.
    bool enabled = true;
    /// @brief The submenu's visible entries; empty for an entry that is chosen.
    std::vector<HeadMenuEntry> items;
    /// @brief Whether it is a line between entries.
    bool separator = false;
    /// @brief A muted line of help with the entry; empty for none.
    std::string description;

    /// @brief Memberwise equality, submenus included.
    /// @return Whether every member matches.
    bool operator==(HeadMenuEntry const&) const = default;
};

/// @brief A tool of a screen's head, as its handle answers it: a search field or a select the shell draws.
struct HeadTool {
    /// @brief The tool's id, what `ScreenHead::edit` and `submit` take.
    std::string id;
    /// @brief A search field or a select.
    HeadToolKind kind = HeadToolKind::Search;
    /// @brief The accessible name; the shell draws no caption.
    std::string label;
    /// @brief A search's text.
    std::string value;
    /// @brief A search's placeholder.
    std::string placeholder;
    /// @brief A select's options.
    std::vector<SelectOption> options;
    /// @brief A select's chosen key, or none.
    std::optional<Key> selected;
    /// @brief The width in units; 0 for the content's.
    int width = 0;
    /// @brief Whether it can be used now.
    bool enabled = true;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(HeadTool const&) const = default;
};

namespace testing {
/// @brief What a test reads of a `Screen` that its interface does not show; the tests define it.
struct ScreenProbe;
}  // namespace testing

/// @brief A mounted screen: its scopes, its view mounted through `Mounted`, and what its host asks of it.
///
/// Neither copyable nor movable. It belongs to the runtime's owner; the runtime, the session, the dispatcher and
/// the backend outlive it. Destroying it runs `onUnmount`, unmounts the view and releases every scope.
class MORPH_API Screen {
public:
    Screen(Screen const&) = delete;
    Screen& operator=(Screen const&) = delete;
    Screen(Screen&&) = delete;
    Screen& operator=(Screen&&) = delete;

    /// @brief Runs `onUnmount`, then unmounts.
    ~Screen();

    /// @brief The root widget.
    /// @return It, valid as long as the screen.
    [[nodiscard]] Widget& root() const;

    /// @brief The document's id.
    /// @return It.
    [[nodiscard]] std::string const& id() const noexcept { return _core->doc().id; }

    /// @brief Evaluates an expression in the screen's scope: what a host reads (`title`, `identity`), and what a
    ///        test inspects.
    /// @param expression The expression's JSON.
    /// @return The value; `null` for an expression that does not parse.
    [[nodiscard]] Value read(Json const& expression) const;

    /// @brief Evaluates the document's `title`.
    /// @return It, or `null`.
    [[nodiscard]] Value title() const { return detail::evalIn(*_scope, nullptr, _core->doc().title); }

    /// @brief Evaluates the document's `identity`.
    /// @return It, or `null`.
    [[nodiscard]] Value identity() const { return detail::evalIn(*_scope, nullptr, _core->doc().identity); }

    /// @brief One model alias of the screen's own scope, with an instance.
    struct BoundAlias {
        /// @brief Its declaration.
        ModelDecl const* decl = nullptr;
        /// @brief The instance it is bound to.
        ChannelFacts facts;
    };

    /// @brief The aliases of the screen's own scope that hold an instance now. One still binding is left out, and so
    ///        is one of a nested scope, which is mounted per row or per opening and so names no one instance.
    /// @return Them, in declaration order.
    [[nodiscard]] std::vector<BoundAlias> boundAliases() const;

    /// @brief Whether the screen's shown root lays itself at the page's edges (`pageEdge`). A tracked read: it starts
    ///        at the view's root, and while the node is a `switch` it moves to the case shown now (the default when
    ///        none matches) if that holds exactly one node, so a width- or state-dependent root is seen through.
    ///        A case that declares a scope of its own is not entered, since its names are not the screen's.
    /// @return The shown root's flag; `false` for a root of any other shape.
    [[nodiscard]] bool pageEdge() const;

    /// @brief Whether the shown root's first child continues the head's block (`headJoined`). A tracked read that
    ///        sees through `switch`es as `pageEdge` does.
    /// @return The shown root's flag; `false` for a root of any other shape.
    [[nodiscard]] bool headJoined() const;

    /// @brief Where the head's lead sits: the document's `leadPlacement`, read now. A tracked read. A value that
    ///        names none of the three places is reported once at its path and taken as `below`.
    /// @return The place; `Below` when the document declares none.
    [[nodiscard]] LeadPlacement leadPlacement() const;

    /// @brief The text role of the head's title: the document's `titleRole`, read now. A tracked read. A value that
    ///        names neither role is reported once at its path and taken as `heading`.
    /// @return `Heading`, or `Headline` where the document asks for it; `Heading` when it declares none.
    [[nodiscard]] TextRole titleRole() const;

    /// @brief The name of the surface the shell draws the top bar on while this screen is shown: the document's
    ///        `headSurface`, read now. A tracked read.
    /// @return The name; empty for none.
    [[nodiscard]] std::string headSurface() const;

    /// @brief The name of the surface the shell draws the page area on while this screen is shown: the document's
    ///        `pageSurface`, read now. A tracked read.
    /// @return The name; empty for none, which is the built-in `page` surface.
    [[nodiscard]] std::string pageSurface() const;

    /// @brief Evaluates the document's `titleSpans` into runs; a run the head cannot read is dropped and reported.
    /// @return The runs; none when the document declares none.
    [[nodiscard]] std::vector<TextSpan> titleSpans() const;

    /// @brief Evaluates the document's `eyebrow`, the head's line above the title.
    /// @return It, or `null`.
    [[nodiscard]] Value eyebrow() const { return detail::evalIn(*_scope, nullptr, _core->doc().eyebrow); }

    /// @brief Evaluates the document's `eyebrowSpans` into runs; a run the head cannot read is dropped and reported.
    /// @return The runs; none when the document declares none.
    [[nodiscard]] std::vector<TextSpan> eyebrowSpans() const;

    /// @brief Evaluates the document's `subtitle`, the head's line under the title.
    /// @return It, or `null`.
    [[nodiscard]] Value subtitle() const { return detail::evalIn(*_scope, nullptr, _core->doc().subtitle); }

    /// @brief Evaluates the document's `subtitleSpans` into runs; a run the head cannot read is dropped and reported.
    /// @return The runs; none when the document declares none.
    [[nodiscard]] std::vector<TextSpan> subtitleSpans() const;

    /// @brief The head's status badges that are visible now, in order. A tracked read.
    /// @return They.
    [[nodiscard]] std::vector<HeadBadge> status() const;

    /// @brief The head's actions that are visible now, in order, each with whether it is enabled. A tracked read.
    /// @return They.
    [[nodiscard]] std::vector<HeadAction> actions() const;

    /// @brief Runs a head action's `onClick` as a user event in the screen scope, in one batch.
    /// @param actionId The action's id.
    /// @return Whether it ran: `false`, with a diagnostic, for an id the head does not hold, or holds hidden or
    ///         disabled.
    bool runAction(std::string_view actionId);

    /// @brief The head menu's visible entries, in order, each with whether it is enabled. A separator that hidden
    ///        entries leave first, last or next to another is left out, as a menu's mount leaves it out. A tracked
    ///        read.
    /// @return They; empty when the document declares no `menu`.
    [[nodiscard]] std::vector<HeadMenuEntry> menu() const { return shownHeadMenu(_headMenu); }

    /// @brief Evaluates the document's `menuLabel`, the accessible name of the head menu's trigger.
    /// @return It, or "More actions" when the document declares none.
    [[nodiscard]] std::string menuLabel() const;

    /// @brief Runs a head menu entry's `onClick` as a user event in the screen scope, in one batch.
    /// @param entryId The entry's id.
    /// @return Whether it ran: `false`, with a diagnostic, for an id the menu does not hold, or holds hidden or
    ///         disabled (the entry itself or a submenu it is in).
    bool chooseMenuEntry(std::string_view entryId);

    /// @brief The head's tools that are visible now, in order, each with its value and whether it is enabled. A
    ///        tracked read.
    /// @return They; empty when the document declares no `tools`.
    [[nodiscard]] std::vector<HeadTool> tools() const;

    /// @brief Reports a user's edit of a head tool: runs a search's `onChange` with `event` the text, or a select's
    ///        `onChoose` with `event` the chosen key, as a user event in the screen scope, in one batch.
    /// @param toolId The tool's id.
    /// @param value  A search's text, or a select's key.
    /// @return Whether it ran: `false`, with a diagnostic, for an id the head does not hold, or holds hidden or
    ///         disabled, and for a search given anything but a string.
    bool editTool(std::string_view toolId, Value const& value);

    /// @brief Reports a user's submit of a head search: runs its `onSubmit` with `event` its text, as a user event in
    ///        the screen scope, in one batch.
    /// @param toolId The tool's id.
    /// @return Whether it ran: `false`, with a diagnostic, for an id the head does not hold, or holds hidden or
    ///         disabled, and for a select.
    bool submitTool(std::string_view toolId);

    /// @brief Evaluates the document's `dirty`.
    /// @return Whether it is `true`.
    [[nodiscard]] bool dirty() const;

    /// @brief Whether the screen has work a close waits for: an open dialog or drawer, a mutation run, or an upload or
    ///        download in flight, in any of its scopes (spec 6 §3). A tracked read, so an Effect that reads it runs
    ///        again when it changes.
    /// @return It.
    [[nodiscard]] bool pending() const { return _core->pendingWork.get() > 0; }

    /// @brief The reactive runtime the screen runs on.
    /// @return It.
    [[nodiscard]] reactive::Runtime& runtime() const noexcept { return _core->runtime(); }

    /// @brief Runs `onClose`, as the host does before it closes the screen.
    /// @return False when `onClose` ran `cancelClose`: the screen stays open.
    bool requestClose();

    /// @brief Runs `onClose`, and hands back the flag its `cancelClose` sets.
    ///
    /// The flag is shared with every command list `onClose` starts, such as the `onResult` of a confirm dialog it
    /// opens, so it can still be set after this returns; a host reads it once the screen is no longer `pending()`.
    /// @return The flag, or `nullptr` when the document declares no `onClose`.
    [[nodiscard]] std::shared_ptr<bool const> beginClose();

    /// @brief Whether the document declares a lifecycle command list.
    /// @param key The key: `onClose`, `onSave`, …
    /// @return It.
    [[nodiscard]] bool declares(std::string_view key) const { return lifecycleList(key) != nullptr; }

    /// @brief Runs a lifecycle command list: `onSave`, `onBackendChange`, or any other the document declares.
    /// @param key The key.
    void runLifecycle(std::string_view key);

    /// @brief The diagnostics reported so far, each path and reason once.
    /// @return They.
    [[nodiscard]] std::vector<Refusal> const& diagnostics() const noexcept { return _core->diagnostics; }

    /// @brief Sets the user's choice at a customization point, as the preferences store loaded or the user made
    ///        it. A choice that does not fit the point is ignored (spec 5 §12b).
    /// @param point The point.
    /// @param value The choice.
    /// @return Whether it fits and was taken.
    bool setPreference(std::string const& point, Value value);

    /// @brief The screen's document, whose `customize` block names the points a preferences store loads.
    /// @return It.
    [[nodiscard]] Document const& document() const noexcept { return _core->doc(); }

    /// @brief What an alias of this screen lends to a screen it opens that borrows it.
    /// @param alias The alias.
    /// @return Its channel and token; an empty channel when it has none.
    [[nodiscard]] BorrowedModel lend(std::string_view alias) const;

private:
    friend class Interpreter;
    friend class Inspector;
    friend struct testing::ScreenProbe;

    // Out of line: an exported class emits its inline members in every module, and this one destroys a
    // `std::unique_ptr<Mounted>` on a throw, which needs `Mounted` complete.
    Screen();

    /// Evaluates a head expression of runs; a run the head cannot read is dropped and reported.
    [[nodiscard]] std::vector<TextSpan> headSpans(ExprPtr const& expr) const;

    /// The view node the screen shows as its root: the view's root, or, while it is a `switch`, the case shown now
    /// (the default when none matches) if that holds exactly one node. Null for a case that declares a scope of its
    /// own, whose names are not the screen's, and for a shape that is no single node. A tracked read.
    [[nodiscard]] DocNode const* shownRoot() const;

    /// The states of the screen's own scope that have a value, by name: what a screen that replaces it keeps.
    [[nodiscard]] std::map<std::string, detail::ScreenCore::KeptState, std::less<>> ownState() const;

    [[nodiscard]] Commands const* lifecycleList(std::string_view key) const;

    /// Runs a command list as the screen's own (not a user's); returns whether it ran `cancelClose`.
    bool runCommands(Commands const& commands) { return *runCommandsFlag(commands); }

    /// Runs a command list as the screen's own; returns the `cancelClose` flag it and what it starts share.
    std::shared_ptr<bool const> runCommandsFlag(Commands const& commands);

    [[nodiscard]] static bool fits(CustomizePoint const& point, Value const& value);

    [[nodiscard]] static bool listed(std::span<std::string const> options, std::string const& text);

    // A head tool by id that is visible and enabled; null, with a diagnostic, for any other.
    [[nodiscard]] detail::HeadToolModel const* usableTool(std::string_view toolId) const;

    // A head menu entry by id, with whether it and every submenu it is in are visible and enabled.
    struct FoundEntry {
        detail::HeadMenuModel const* model = nullptr;
        bool visible = true;
        bool enabled = true;
    };

    // NOLINTNEXTLINE(misc-no-recursion): a submenu is a menu one level down.
    [[nodiscard]] static FoundEntry findHeadEntry(std::vector<detail::HeadMenuModel> const& level,
                                                  std::string_view entryId, FoundEntry const& outer);

    // One level of the head menu as it shows now: hidden entries left out, and a separator they leave first, last or
    // next to another with them.
    // NOLINTNEXTLINE(misc-no-recursion): a submenu is a menu one level down.
    [[nodiscard]] static std::vector<HeadMenuEntry> shownHeadMenu(std::vector<detail::HeadMenuModel> const& level);

    std::unique_ptr<detail::ScreenCore> _core;
    std::shared_ptr<detail::RtScope> _scope;
    std::unique_ptr<Mounted> _mounted;
    // The head's parts, bound in the screen scope.
    std::vector<detail::BadgeModel> _headStatus;
    detail::ActionModels _headActions;
    std::vector<detail::HeadMenuModel> _headMenu;
    std::vector<detail::HeadToolModel> _headTools;
};

/// @brief The app document mounted as the root scope every screen of the session shares (spec 6 §5).
///
/// Its state, lets, queries and mutations live once for the session. A screen reads them as `{"ref": "app.name"}`,
/// a binding on one updates, and `{"set": "app.name", …}` writes the app document's state. A query of the app
/// document runs once however many screens read it. Destroying the root runs its `onUnmount`, takes its scope off the
/// session and releases its model instances; a host does that before it clears the session on sign-out.
class MORPH_API AppRoot {
public:
    AppRoot(AppRoot const&) = delete;
    AppRoot& operator=(AppRoot const&) = delete;
    AppRoot(AppRoot&&) = delete;
    AppRoot& operator=(AppRoot&&) = delete;

    /// @brief Runs `onUnmount`, takes the scope off the session, and releases everything.
    ~AppRoot();

    /// @brief The app document's `navigation` badges that are visible now, in order, each with the screen whose
    ///        navigator entry it goes on, and following the app state they read.
    /// @return They, valid as long as the root; empty when the document declares no `navigation`.
    [[nodiscard]] reactive::Computed<std::vector<NavigationBadge>> const& navigationBadges() const;

    /// @brief The app document's guided tours in declaration order, then the runtime tours `setRuntimeTours` gave,
    ///        each text evaluated in the app scope and again when what it reads changes, a change of locale included.
    /// @return They, valid as long as the root; empty when the document declares none.
    [[nodiscard]] reactive::Computed<std::vector<Tour>> const& tours() const;

    /// @brief Gives the runtime tours, as the server listed them: `tours()` lists them after the app document's, in
    ///        this order, leaving out one whose id an app-document tour has.
    /// @param tours The tours, loaded with `loadTourText`.
    void setRuntimeTours(std::vector<TourDecl> tours);

    /// @brief Where a shell shows who is signed in: the document's `principalPlacement`.
    /// @return The place; `Sidebar` when the document declares none.
    [[nodiscard]] PrincipalPlacement principalPlacement() const noexcept;

    /// @brief Evaluates an expression in the app document's scope.
    /// @param expression The expression's JSON.
    /// @return Its value; `null` for an expression that does not parse.
    [[nodiscard]] Value read(Json const& expression) const;

    /// @brief What a query of the app document holds now.
    struct QueryRead {
        /// @brief Its value; `null` before the first reply.
        Value value;
        /// @brief Whether a request is in flight.
        bool pending = false;
        /// @brief Whether the last request failed.
        bool failed = false;
        /// @brief Whether the app document declares the query.
        bool declared = false;
        /// @brief Whether the server refused the request as not permitted.
        bool denied = false;
    };

    /// @brief Reads a query of the app document. A tracked read: an effect that calls it follows the query.
    /// @param name The query.
    /// @return Its state; `declared` is false for a name the document does not declare as a query.
    [[nodiscard]] QueryRead query(std::string_view name) const;

    /// @brief Writes a state of the app document, as a `set` of `app.name` does.
    /// @param name  The state.
    /// @param value The value.
    /// @return Whether the document declares the state.
    bool setState(std::string_view name, Value value);

    /// @brief Runs a mutation of the app document, as a `run` command does.
    /// @param name The mutation.
    /// @param body The body it is run with.
    /// @param done Called once the run settles, with whether it succeeded; empty for none.
    /// @return Whether the document declares the mutation.
    bool runMutation(std::string_view name, Value body, std::function<void(bool succeeded)> done = {});

    /// @brief The diagnostics reported so far.
    /// @return They.
    [[nodiscard]] std::vector<Refusal> const& diagnostics() const noexcept { return _core->diagnostics; }

private:
    friend class Interpreter;

    AppRoot() = default;

    void runCommands(Commands const& commands);

    // A navigation badge, lowered in the app scope, with the screen of its entry.
    struct PlacedBadge {
        std::string screen;
        detail::BadgeModel badge;
    };

    std::unique_ptr<detail::ScreenCore> _core;
    std::shared_ptr<detail::RtScope> _scope;
    std::vector<PlacedBadge> _navigation;
    std::unique_ptr<reactive::Computed<std::vector<NavigationBadge>>> _navigationBadges;
    std::unique_ptr<reactive::Signal<std::shared_ptr<std::vector<TourDecl> const>>> _runtimeTours;
    std::unique_ptr<reactive::Computed<std::vector<Tour>>> _tours;
};

/// @brief Mounts UI documents (spec 5): one per client process or window, over a session and a dispatcher.
class MORPH_API Interpreter {
public:
    /// @brief An interpreter.
    /// @param session    The session the screens share. Borrowed.
    /// @param dispatcher Where model calls go. Borrowed.
    /// @param options    The host and the diagnostic callback.
    Interpreter(Session& session MORPH_LIFETIMEBOUND, Dispatcher& dispatcher MORPH_LIFETIMEBOUND,
                InterpreterOptions options = {})
        : _session{&session}, _dispatcher{&dispatcher}, _options{std::move(options)} {}

    /// @brief Mounts a screen.
    ///
    /// The document has loaded (`loadDocument`); what mounting adds is what only the opener knows: the params, and
    /// the models a `{"from": alias}` instance borrows. A param that is missing and not optional, a param of the
    /// wrong type, a borrowed model the opener did not lend and a document without a view are refused.
    /// @param doc     The document.
    /// @param params  The params, by name; names the document does not declare are ignored.
    /// @param backend The backend to mount on. Borrowed: it outlives the screen.
    /// @param options Where to mount, and the models lent.
    /// @return The screen, or the refusal.
    [[nodiscard]] std::expected<std::unique_ptr<Screen>, Refusal> mount(std::shared_ptr<Document const> doc,
                                                                        ValueRecord const& params,
                                                                        IViewBackend& backend,
                                                                        MountOptions const& options = {});

    /// @brief Mounts the app document as the session's root scope (spec 6 §5).
    ///
    /// Refused: a document whose id is not `app`, and a second app document while one is mounted on the session.
    /// @param doc The app document, loaded.
    /// @return The root, or the refusal. Keep it as long as the session's screens may read `app.*`.
    [[nodiscard]] std::expected<std::unique_ptr<AppRoot>, Refusal> mountApp(std::shared_ptr<Document const> doc);

private:
    Session* _session;
    Dispatcher* _dispatcher;
    InterpreterOptions _options;
};

namespace detail {

/// @brief The text of an instance key, as the bridge's string-keyed attach takes it.
/// @param key The key's value.
/// @return The text; empty for `null`.
[[nodiscard]] MORPH_API std::string instanceKeyText(Value const& key);

/// @brief A JSON text as a document value; `null` when it is not JSON or holds what a value cannot (a fraction).
/// @param text The JSON text.
/// @return The value.
[[nodiscard]] MORPH_API Value jsonTextValue(std::string_view text);

/// @brief A byte count as a person reads it: `512 bytes`, `2.4 KiB`, `1 MiB`.
/// @param bytes The count.
/// @return The text.
[[nodiscard]] MORPH_API std::string sizeText(std::size_t bytes);

/// @brief A document's inline file content as bytes: base64 text, or a list of integers 0 to 255.
/// @param value The value.
/// @return The bytes, or why the value is neither.
[[nodiscard]] MORPH_API std::expected<std::string, std::string> inlineBytes(Value const& value);

/// @brief A value as a custom component's text prop (spec 5 §10): an `int` or `exact` as plain decimal text, so no
///        id or amount loses precision; `json` as its JSON text; `string` as the formatted text.
/// @param value  The value.
/// @param type   The prop type the component declares.
/// @param format The formatting facts, for `string`.
/// @return The text.
[[nodiscard]] MORPH_API std::string componentText(Value const& value, std::string_view type,
                                                  FormatContext const& format);

/// @brief A value as a custom component's `number` prop: the one kind that crosses as a double.
/// @param value The value.
/// @return It; zero for anything that is not a number.
[[nodiscard]] MORPH_API double componentNumber(Value const& value);

/// @brief A custom component's event payload as a value: `i:<int64>` an `int`, `s:<text>` a `string`, anything else
///        JSON text read as the event's declared type, and text that is none of those a `string`.
/// @param payload The payload, as the backend reports it.
/// @param type    The payload type the component declares; null for an undeclared event.
/// @return The value.
[[nodiscard]] MORPH_API Value componentPayload(std::string const& payload, Type const* type);

/// @brief A value as an integer: an `int`, or a decimal truncated toward zero.
/// @param value    The value.
/// @param fallback What anything else gives.
/// @return It.
[[nodiscard]] MORPH_API std::int64_t intOf(Value const& value, std::int64_t fallback);

/// @brief A tone by its exact name, the strict reading of a value's evaluation: a table row's and a key-value item's
///        tone, which unlike `toneOf` takes no alias, so a misspelt name can be reported.
/// @param value `neutral`, `info`, `ok`, `warn` or `err`; `null` is `Neutral`.
/// @return The tone; `nullopt` for anything else.
[[nodiscard]] MORPH_API std::optional<Tone> toneNamed(Value const& value);

/// @brief A span list's runs: each element a record `{text, strong?, mono?, tone?}`, or a plain run of any other value
///        but a list or `null`. A run's text is shown as `textOf` shows any value, so a computed number is a run of
///        its digits, and a flag is set only by `true`.
/// @param value   The list; `null` is no runs.
/// @param format  How a run's value is written.
/// @param dropped Set when the value is not a list, or an element is a list, `null`, or a record without `text`;
///                such an element is left out.
/// @param badTone Set, when given, when a run's `tone` is none of the tone names; the run is kept, untoned.
/// @return The runs, in order.
[[nodiscard]] MORPH_API std::vector<TextSpan> spansOf(Value const& value, FormatContext const& format, bool& dropped,
                                                      bool* badTone = nullptr);

/// @brief A sizing by its document form: `"content"`, `{"fixed": n}`, `{"stretch": w}` or `{"percent": n}`.
/// @param value The value.
/// @return The sizing.
[[nodiscard]] MORPH_API Sizing sizingOf(Value const& value);

/// @brief Whether a value is a sizing of one dimension: `"content"`, `{"fixed": n}`, `{"stretch": w}` or
///        `{"percent": n}` with n from 1 to 100.
/// @param value The value.
/// @return Whether `sizingOf` reads it as written rather than as content by default.
[[nodiscard]] MORPH_API bool isSizing(Value const& value);

/// @brief A component's `change` payload as the wire value of the field it draws, read as the `value` prop's
///        declared type.
/// @param payload The payload, as the backend reports it.
/// @param type    The `value` prop's type.
/// @return The wire value, or why the payload is not one.
[[nodiscard]] MORPH_API std::expected<glz::generic_u64, std::string> changeWire(std::string const& payload,
                                                                                std::string_view type);

}  // namespace detail

}  // namespace morph::ui

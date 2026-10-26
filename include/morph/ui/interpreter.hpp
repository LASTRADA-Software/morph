// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <charconv>
#include <chrono>
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

#include "../core/backend.hpp"
#include "../core/bridge.hpp"
#include "../core/completion.hpp"
#include "../core/wire.hpp"
#include "../forms/engine/collection.hpp"
#include "../forms/engine/collection_view.hpp"
#include "../forms/engine/field_model.hpp"
#include "../forms/engine/form_session.hpp"
#include "../forms/engine/form_view.hpp"
#include "../forms/engine/wizard.hpp"
#include "../forms/engine/wizard_view.hpp"
#include "../reactive/runtime.hpp"
#include "../reactive/scheduler.hpp"
#include "../reactive/scope.hpp"
#include "../reactive/signal.hpp"
#include "../ui/backend.hpp"
#include "dev_tools.hpp"
#include "document.hpp"
#include "expression.hpp"
#include "message.hpp"
#include "mount.hpp"
#include "value.hpp"
#include "view.hpp"

/// @file
/// @brief The UI document interpreter: `Interpreter::mount` turns a loaded `Document` into reactive state and a view
///        tree, mounts it through `Mounted` on any `IViewBackend`, and runs its queries, mutations, relations and
///        commands. Model calls go through a `Dispatcher` (the bridge's model-free seams in an application, a
///        scripted fake in a test); what only the application can do goes through an `InterpreterHost`; what
///        screens share — the query cache, the principal's permissions, the formatting facts — is a `Session`.

namespace morph::ui {

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
class BridgeDispatcher final : public Dispatcher {
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
                                                     std::string const& instanceKey) override {
        return std::make_shared<Channel>(*_bridge, _cbExec, _evaluator, typeId, sharing, instanceKey);
    }

private:
    class Channel final : public ModelChannel {
    public:
        // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the type id, then the key, as `bind` takes them.
        Channel(bridge::Bridge& bridge, exec::IExecutor* cbExec, Evaluator evaluator, std::string const& typeId,
                bridge::BindSharing sharing, std::string const& instanceKey)
            : _cbExec{cbExec},
              _evaluator{std::move(evaluator)},
              _typeId{typeId},
              _handler{bridge, cbExec, typeId, sharing, instanceKey} {}

        [[nodiscard]] async::Completion<std::string> execute(std::string const& actionId, std::string const& bodyJson,
                                                             CallOptions options) override {
            if (options.evaluate) {
                if (_evaluator) {
                    return _evaluator(_typeId, actionId, bodyJson);
                }
                auto [refused, promise] = async::Completion<std::string>::makeSettleable(_cbExec);
                promise.reject(std::make_exception_ptr(std::runtime_error{"evaluate needs the UI service"}));
                return std::move(refused);
            }
            return _handler.execute(actionId, bodyJson);
        }

    private:
        exec::IExecutor* _cbExec;
        Evaluator _evaluator;
        std::string _typeId;
        bridge::RawHandler _handler;
    };

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
///        the rest of the body, so the encoded file still fits `wire::kMaxEnvelopeBytes`.
inline constexpr std::size_t kMaxInlineFileBytes = (wire::kMaxEnvelopeBytes - (std::size_t{64} * 1024)) * 3 / 4;

/// @brief What the application answers for the commands a document cannot carry out alone (spec 6). Every default
///        answers "not supported", which the interpreter reports as a diagnostic.
class InterpreterHost {
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
                          std::function<void()> onCancel) {
        static_cast<void>(request);
        static_cast<void>(onResult);
        static_cast<void>(onCancel);
        return false;
    }

    /// @brief Ends the screen that ran `return` or `close: self`.
    /// @param screen The screen's id.
    /// @param result The value of `return`; `std::nullopt` for `close: self`.
    /// @return Whether the screen was opened as something that can end.
    virtual bool closeSelf(std::string const& screen, std::optional<Value> result) {
        static_cast<void>(screen);
        static_cast<void>(result);
        return false;
    }

    /// @brief Runs a command the application registered (`host/1`).
    /// @param name The command.
    /// @param args Its arguments; `null` when none.
    /// @return Whether the command exists.
    virtual bool hostCommand(std::string const& name, Value const& args) {
        static_cast<void>(name);
        static_cast<void>(args);
        return false;
    }

    /// @brief Switches to a backend the application's source lists (`AppSource::switchBackend`, spec 6 §9).
    ///
    /// The switch is asynchronous: the host calls @p done once, on the runtime's owner, when it committed or failed.
    /// A failed switch leaves the old backend in place.
    /// @param name The backend.
    /// @param done Called once with nothing when the switch committed, or with why it failed. Not called when the
    ///        backend is not listed.
    /// @return Whether it is listed.
    virtual bool switchBackend(std::string const& name, std::function<void(std::expected<void, std::string>)> done) {
        static_cast<void>(name);
        static_cast<void>(done);
        return false;
    }

    /// @brief The document reset a customization point, or all of them; the host forgets the stored choice.
    /// @param screen The screen's id.
    /// @param point  The point, or `all`.
    virtual void resetPrefs(std::string const& screen, std::string const& point) {
        static_cast<void>(screen);
        static_cast<void>(point);
    }

    /// @brief Stores the active locale as a preference. The interpreter has already re-resolved every `t`.
    /// @param locale The BCP 47 locale.
    virtual void setLocale(std::string const& locale) { static_cast<void>(locale); }

    /// @brief Shows a transient message.
    /// @param text The message.
    /// @param role `info`, `success`, `warning` or `error`; empty for the default.
    /// @return Whether it was shown.
    virtual bool notify(std::string const& text, std::string const& role) {
        static_cast<void>(text);
        static_cast<void>(role);
        return false;
    }

    /// @brief Installs the session the reply of a sign-in mutation carries, and reloads the catalog.
    /// @param reply The mutation's reply, secret fields included.
    /// @return Whether the host installed it.
    virtual bool signIn(Value const& reply) {
        static_cast<void>(reply);
        return false;
    }

    /// @brief Ends the session. The interpreter has already cleared the session's query cache.
    virtual void signOut() {}

    /// @brief Writes `app.name` with `set` (spec 6 §5).
    /// @param name  The app state's name.
    /// @param value The value.
    /// @return Whether the app document declares it.
    virtual bool setAppValue(std::string const& name, Value const& value) {
        static_cast<void>(name);
        static_cast<void>(value);
        return false;
    }

    /// @brief Sends a file through the server's file side channel (`files/1`).
    /// @param file     What the user picked or dropped.
    /// @param progress Called with the progress, 0 to 1.
    /// @param done     Called once with the file's reference, or the failure's text.
    /// @return Whether the host can send files.
    virtual bool upload(Value const& file, std::function<void(Value)> progress,
                        std::function<void(std::expected<Value, std::string>)> done) {
        static_cast<void>(file);
        static_cast<void>(progress);
        static_cast<void>(done);
        return false;
    }

    /// @brief Sends a file through the server's file side channel, knowing the mutation that will reference it. The
    ///        interpreter calls this one; the default calls `upload(file, …)`, so a host that overrides only that
    ///        keeps working.
    /// @param request  The file and the mutation.
    /// @param progress Called with the progress, 0 to 1.
    /// @param done     Called once with the file's reference, or the failure's text.
    /// @return Whether the host can send files.
    virtual bool uploadFor(UploadRequest const& request, std::function<void(Value)> progress,
                           std::function<void(std::expected<Value, std::string>)> done) {
        return upload(request.file, std::move(progress), std::move(done));
    }

    /// @brief Fetches a file by reference to save it, or to a local temporary reference for a viewer.
    /// @param file   The file's reference.
    /// @param option `dialog` to save, `preview` for a local reference.
    /// @param name   The suggested file name; `null` when none.
    /// @param done   Called once with the local reference (`preview`) or `null` (`dialog`), or the failure.
    /// @return Whether the host can fetch files.
    virtual bool download(Value const& file, std::string const& option, Value const& name,
                          std::function<void(std::expected<Value, std::string>)> done) {
        static_cast<void>(file);
        static_cast<void>(option);
        static_cast<void>(name);
        static_cast<void>(done);
        return false;
    }

    /// @brief Reads a file's bytes for `upload` into bytes. A host reads the file's size first and refuses one over
    ///        @p maxBytes without reading it; the interpreter refuses a reply over the cap as well.
    /// @param file     What the user picked or dropped.
    /// @param maxBytes The session's inline cap.
    /// @param done     Called once with the file, or the failure's text.
    /// @return Whether the host can read files.
    virtual bool readFile(Value const& file, std::size_t maxBytes,
                          std::function<void(std::expected<InlineFile, std::string>)> done) {
        static_cast<void>(file);
        static_cast<void>(maxBytes);
        static_cast<void>(done);
        return false;
    }

    /// @brief Saves a file a document holds the bytes of, for `download` from bytes.
    /// @param file   The file.
    /// @param option `dialog` to save, `preview` for a local reference.
    /// @param done   Called once with the local reference (`preview`) or `null` (`dialog`), or the failure.
    /// @return Whether the host can save files.
    virtual bool saveFile(InlineFile const& file, std::string const& option,
                          std::function<void(std::expected<Value, std::string>)> done) {
        static_cast<void>(file);
        static_cast<void>(option);
        static_cast<void>(done);
        return false;
    }

    /// @brief The user chose at a customization point through a `customize` editor; the host stores the choice.
    /// @param screen The screen's id.
    /// @param point  The point.
    /// @param value  The choice.
    virtual void preferenceChanged(std::string const& screen, std::string const& point, Value const& value) {
        static_cast<void>(screen);
        static_cast<void>(point);
        static_cast<void>(value);
    }

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
};

namespace detail {

/// @brief The text of a failure.
/// @param error The failure.
/// @return `what()`, or `unknown error`.
[[nodiscard]] inline std::string failureText(std::exception_ptr const& error) {
    try {
        std::rethrow_exception(error);
    } catch (std::exception const& failure) {
        return failure.what();
    } catch (...) {
        return "unknown error";
    }
}

/// @brief The default error kinds: the bridge's own failures, and `error` for the rest.
/// @param error The failure.
/// @return The kind.
[[nodiscard]] inline std::string defaultErrorKind(std::exception_ptr const& error) {
    try {
        std::rethrow_exception(error);
    } catch (backend::DisconnectedError const&) {
        return "offline";
    } catch (backend::TimeoutError const&) {
        return "timeout";
    } catch (backend::ClientTimeoutError const&) {
        return "timeout";
    } catch (backend::BackendChangedError const&) {
        return "backendChanged";
    } catch (...) {
        return "error";
    }
}

/// @brief A value's JSON text, with record fields in name order: a cache key's canonical body.
/// @param value The value.
/// @return The text.
[[nodiscard]] inline std::string canonicalJson(Value const& value) {
    return glz::write_json(valueToJson(value)).value_or(std::string{});
}

/// @brief A value with the field at @p path replaced, creating records along the way.
/// @param base  The value written into.
/// @param path  The field path.
/// @param value The new value at the path.
/// @return The new value.
// NOLINTNEXTLINE(misc-no-recursion): one level per path segment
[[nodiscard]] inline Value withPath(Value const& base, std::span<std::string const> path, Value value) {
    if (path.empty()) {
        return value;
    }
    ValueRecord fields;
    if (ValueRecord const* const record = base.asRecord()) {
        fields = *record;
    }
    Value const inner = base.field(path.front()) != nullptr ? *base.field(path.front()) : Value{};
    fields.insert_or_assign(path.front(), withPath(inner, path.subspan(1), std::move(value)));
    return Value{std::move(fields)};
}

/// @brief A row's identity for `patch` and `optimistic`: its `id` field.
/// @param row The row.
/// @return The `id` field, or `null`.
[[nodiscard]] inline Value rowId(Value const& row) {
    Value const* const found = row.field("id");
    return found == nullptr ? Value{} : *found;
}

/// @brief Writes @p with over a row: the fields of a record over a record's, otherwise the whole row.
/// @param row  The row.
/// @param with The new content.
/// @return The new row.
[[nodiscard]] inline Value mergeRow(Value const& row, Value const& with) {
    ValueRecord const* const fields = row.asRecord();
    ValueRecord const* const over = with.asRecord();
    if (fields == nullptr || over == nullptr) {
        return with;
    }
    ValueRecord merged = *fields;
    for (auto const& [name, field] : *over) {
        merged.insert_or_assign(name, field);
    }
    return Value{std::move(merged)};
}

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
// NOLINTBEGIN(bugprone-easily-swappable-parameters): a list, a key and a row are distinct roles of one type.
[[nodiscard]] inline std::expected<Value, PatchMiss> patchRows(Value const& value, std::string_view listMember,
                                                               Value const& key, Value const& with) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    ValueRecord const* const record = listMember.empty() ? nullptr : value.asRecord();
    if (!listMember.empty() && (record == nullptr || !record->contains(listMember))) {
        return std::unexpected(PatchMiss::NoList);
    }
    Value const* const rows = record == nullptr ? &value : &record->find(listMember)->second;
    ValueList const* const items = rows->asList();
    if (items == nullptr) {
        return std::unexpected(PatchMiss::NoList);
    }
    if (key.isNull()) {
        return std::unexpected(PatchMiss::NoRow);
    }
    ValueList out = *items;
    bool found = false;
    for (Value& row : out) {
        if (rowId(row) == key) {
            row = mergeRow(row, with);
            found = true;
        }
    }
    if (!found) {
        return std::unexpected(PatchMiss::NoRow);
    }
    if (record == nullptr) {
        return Value{std::move(out)};
    }
    ValueRecord patched = *record;
    patched.insert_or_assign(std::string{listMember}, Value{std::move(out)});
    return Value{std::move(patched)};
}

/// @brief What a patch that found no list reports.
/// @param query      The query it patches.
/// @param listMember The record member it names; empty when none.
/// @return The diagnostic's reason.
[[nodiscard]] inline std::string patchMissReason(std::string_view query, std::string_view listMember) {
    if (listMember.empty()) {
        return "the value of `" + std::string{query} +
               "` is not a list, so the patch changes nothing; `at` names the record member that holds the list";
    }
    return "the value of `" + std::string{query} + "` has no list at `" + std::string{listMember} +
           "`, so the patch changes nothing";
}

/// @brief Removes the named fields from a record, and from each record of a list.
/// @param value  The value.
/// @param fields The field names.
/// @return The value without them.
// NOLINTNEXTLINE(misc-no-recursion): one level per nested list or record of the value.
[[nodiscard]] inline Value withoutFields(Value const& value, std::vector<std::string> const& fields) {
    if (fields.empty()) {
        return value;
    }
    if (ValueRecord const* const record = value.asRecord()) {
        ValueRecord kept = *record;
        for (std::string const& field : fields) {
            kept.erase(field);
        }
        return Value{std::move(kept)};
    }
    if (ValueList const* const items = value.asList()) {
        ValueList kept;
        kept.reserve(items->size());
        for (Value const& item : *items) {
            kept.push_back(withoutFields(item, fields));
        }
        return Value{std::move(kept)};
    }
    return value;
}

/// @brief The members of a catalog record path: `"header.rows"` is `header`, then `rows`; `""` is none.
/// @param path The path.
/// @return Its members in order.
[[nodiscard]] inline std::vector<std::string> pathMembers(std::string_view path) {
    std::vector<std::string> members;
    for (auto const part : std::views::split(path, '.')) {
        if (!part.empty()) {
            members.emplace_back(part.begin(), part.end());
        }
    }
    return members;
}

/// @brief The rows of a keyed record that a value holds at a path.
///
/// A list on the way is looked through, as the result walker looks through a `std::vector`. At the path's end a
/// record with the key field is a row, and a record without it is a map whose values are the rows.
/// @param value    The value.
/// @param path     The members still to follow.
/// @param keyField The record's key field.
/// @param out      Where the rows go.
// NOLINTNEXTLINE(misc-no-recursion): one level per path member or nested list.
inline void keyedRowsAt(Value const& value, std::span<std::string const> path, std::string_view keyField,
                        std::vector<Value const*>& out) {
    if (ValueList const* const items = value.asList()) {
        for (Value const& item : *items) {
            keyedRowsAt(item, path, keyField, out);
        }
        return;
    }
    ValueRecord const* const record = value.asRecord();
    if (record == nullptr) {
        return;
    }
    if (!path.empty()) {
        if (auto const found = record->find(path.front()); found != record->end()) {
            keyedRowsAt(found->second, path.subspan(1), keyField, out);
        }
        return;
    }
    if (record->contains(keyField)) {
        out.push_back(&value);
        return;
    }
    for (auto const& [name, item] : *record) {
        static_cast<void>(name);
        if (ValueRecord const* const row = item.asRecord(); row != nullptr && row->contains(keyField)) {
            out.push_back(&item);
        }
    }
}

/// @brief New rows by the canonical text of their key.
using RowsByKey = std::map<std::string, Value const*, std::less<>>;

/// @brief Merges new rows over the rows with the same key that a value holds at a path.
/// @param value    The value.
/// @param path     The members still to follow.
/// @param keyField The record's key field.
/// @param rows     The new rows.
/// @return The new value, or nothing when no row matched.
// NOLINTNEXTLINE(misc-no-recursion): one level per path member or nested list.
[[nodiscard]] inline std::optional<Value> patchKeyedRows(Value const& value, std::span<std::string const> path,
                                                         std::string_view keyField, RowsByKey const& rows) {
    if (ValueList const* const items = value.asList()) {
        ValueList out = *items;
        bool changed = false;
        for (Value& item : out) {
            if (auto patched = patchKeyedRows(item, path, keyField, rows)) {
                item = std::move(*patched);
                changed = true;
            }
        }
        return changed ? std::optional<Value>{Value{std::move(out)}} : std::nullopt;
    }
    ValueRecord const* const record = value.asRecord();
    if (record == nullptr) {
        return std::nullopt;
    }
    if (!path.empty()) {
        auto const found = record->find(path.front());
        if (found == record->end()) {
            return std::nullopt;
        }
        auto patched = patchKeyedRows(found->second, path.subspan(1), keyField, rows);
        if (!patched) {
            return std::nullopt;
        }
        ValueRecord out = *record;
        out.insert_or_assign(path.front(), std::move(*patched));
        return Value{std::move(out)};
    }
    if (auto const key = record->find(keyField); key != record->end()) {
        auto const row = rows.find(canonicalJson(key->second));
        return row == rows.end() ? std::nullopt : std::optional<Value>{mergeRow(value, *row->second)};
    }
    ValueRecord out = *record;
    bool changed = false;
    for (auto& [name, item] : out) {
        static_cast<void>(name);
        if (ValueRecord const* const row = item.asRecord(); row != nullptr && row->contains(keyField)) {
            if (auto patched = patchKeyedRows(item, {}, keyField, rows)) {
                item = std::move(*patched);
                changed = true;
            }
        }
    }
    return changed ? std::optional<Value>{Value{std::move(out)}} : std::nullopt;
}

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
struct CacheEntry {
    /// @brief An empty entry.
    /// @param runtime The runtime its signals belong to.
    explicit CacheEntry(reactive::Runtime& runtime)
        : value{runtime}, hasValue{runtime, false}, error{runtime}, errorKind{runtime}, pending{runtime, false} {}

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
    [[nodiscard]] ActionRecordInfo const* holding(std::string_view record) const {
        auto const found = std::ranges::find(records, record, &ActionRecordInfo::record);
        return found == records.end() ? nullptr : &*found;
    }
};

/// @brief The rows of a keyed record a value holds, at each of the record's paths.
/// @param value    The value.
/// @param paths    The record's paths.
/// @param keyField The record's key field.
/// @return The rows.
[[nodiscard]] inline std::vector<Value const*> keyedRowsOf(Value const& value, std::vector<std::string> const& paths,
                                                           std::string_view keyField) {
    std::vector<Value const*> rows;
    for (std::string const& path : paths) {
        std::vector<std::string> const members = pathMembers(path);
        keyedRowsAt(value, members, keyField, rows);
    }
    return rows;
}

/// @brief Merges new rows into a value at each of the record's paths.
/// @param value    The value.
/// @param paths    The record's paths.
/// @param keyField The record's key field.
/// @param rows     The new rows by key.
/// @return The new value, or nothing when no row matched.
[[nodiscard]] inline std::optional<Value> patchHeldRows(Value const& value, std::vector<std::string> const& paths,
                                                        std::string_view keyField, RowsByKey const& rows) {
    std::optional<Value> patched;
    for (std::string const& path : paths) {
        std::vector<std::string> const members = pathMembers(path);
        auto next = patched ? patchKeyedRows(*patched, members, keyField, rows)
                            : patchKeyedRows(value, members, keyField, rows);
        if (next) {
            patched = std::move(next);
        }
    }
    return patched;
}

/// @brief The entries one change marks stale, each once, in the order they were found.
struct Marks {
    /// @brief The entries.
    std::vector<CacheEntry*> entries;

    /// @brief Adds an entry, unless it is already there.
    /// @param entry The entry.
    void add(CacheEntry& entry) {
        if (std::ranges::find(entries, &entry) == entries.end()) {
            entries.push_back(&entry);
        }
    }
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

/// @brief What screens of one client session share (spec 5 §5, §12b): the query cache, the mounted queries, the
///        principal's permitted actions, the formatting facts and the client's `env`, `session` and `host` values.
///
/// It belongs to the runtime's owner, outlives every screen mounted on it, and is cleared by `signOut` and on a
/// principal change.
class Session {
public:
    /// @brief An empty session.
    /// @param runtime The runtime. Borrowed.
    /// @param options Timers, clock, catalog and the inline cap.
    /// @throws std::invalid_argument when `maxInlineFileBytes` is 0 or above `kMaxInlineFileBytes`.
    explicit Session(reactive::Runtime& runtime MORPH_LIFETIMEBOUND, SessionOptions options = {})
        : _runtime{&runtime},
          _options{checkedOptions(std::move(options))},
          _permitted{runtime},
          _format{runtime, std::make_shared<FormatContext const>()},
          _env{runtime},
          _sessionValues{runtime},
          _hostValues{runtime},
          _hostCommandNames{runtime} {}

    Session(Session const&) = delete;
    Session& operator=(Session const&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;
    ~Session() = default;

    /// @brief The runtime.
    /// @return It.
    [[nodiscard]] reactive::Runtime& runtime() const noexcept { return *_runtime; }

    /// @brief The options.
    /// @return They.
    [[nodiscard]] SessionOptions const& options() const noexcept { return _options; }

    /// @brief Sets the action ids the principal may run, as the catalog reply lists them; `{"can": ...}` follows them.
    /// @param permitted The ids.
    void setPermitted(std::set<std::string, std::less<>> permitted) { _permitted.set(std::move(permitted)); }

    /// @brief Whether the principal may run an action, or the client can run a host command now. A tracked read.
    ///
    /// A `host:` name is answered from the client's own commands (`setHostCommands`), never from the server's
    /// permitted set: the two have different owners, and a catalog refresh must not drop the client's commands.
    /// @param action The action id, or `host:<command>`.
    /// @return Whether it is permitted.
    [[nodiscard]] bool can(std::string_view action) const {
        static constexpr std::string_view kHostPrefix = "host:";
        if (action.starts_with(kHostPrefix)) {
            return _hostCommandNames.get().contains(action.substr(kHostPrefix.size()));
        }
        return _permitted.get().contains(action);
    }

    /// @brief Replaces the formatting facts; every `t` and display function re-evaluates.
    /// @param format The facts.
    void setFormatting(FormatContext format) { _format.set(std::make_shared<FormatContext const>(std::move(format))); }

    /// @brief The formatting facts. A tracked read.
    /// @return They.
    [[nodiscard]] FormatContext const& formatting() const { return *_format.get(); }

    /// @brief Changes the active locale, keeping the other formatting facts.
    /// @param locale The BCP 47 locale.
    void setLocale(std::string locale) {
        FormatContext next = *_format.peek();
        next.locale = std::move(locale);
        setFormatting(std::move(next));
    }

    /// @brief Replaces the message catalogue, keeping the other formatting facts; every `t` re-resolves.
    /// @param translations The provider, such as `MessageCatalogues::provider()`; empty shows every fallback.
    void setTranslations(render::TranslationProvider translations) {
        FormatContext next = *_format.peek();
        next.translations = std::move(translations);
        setFormatting(std::move(next));
    }

    /// @brief Sets the `env` values (`widthClass`, `locale`, `platform`, `online`, `backend`).
    /// @param values The values.
    void setEnvironment(ValueRecord values) { _env.set(std::move(values)); }

    /// @brief Sets one `env` value and keeps the others, so a client that learns its width class does not drop the
    ///        `backend` a switch set, nor a switch the width class.
    /// @param name The value's name, such as `widthClass`.
    /// @param value The value.
    void setEnvironmentValue(std::string const& name, Value value) {
        ValueRecord env = _env.peek();
        env.insert_or_assign(name, std::move(value));
        _env.set(std::move(env));
    }

    /// @brief Sets the `session` values (`principal`, `userName`, …).
    /// @param values The values.
    void setSessionValues(ValueRecord values) { _sessionValues.set(std::move(values)); }

    /// @brief Sets the `host` values the application registered.
    /// @param values The values.
    void setHostValues(ValueRecord values) { _hostValues.set(std::move(values)); }

    /// @brief Sets the client's host commands: the names `{"can": "host:name"}` answers for, and what runs them. A
    ///        session without a runner asks `InterpreterHost::hostCommand`.
    /// @param commands The names and the runner.
    void setHostCommands(HostCommands commands) {
        _hostCommandRunner = std::move(commands.run);
        _hostCommandNames.set(std::move(commands.available));
    }

    /// @brief What runs host commands.
    /// @return It; empty when the application set none.
    [[nodiscard]] std::function<HostCommandResult(std::string const&, Value const&)> const& hostCommandRunner()
        const noexcept {
        return _hostCommandRunner;
    }

    /// @brief Reads `env`, `session` or `host`. A tracked read.
    /// @param form `Env`, `Session` or `Host`.
    /// @param name The name.
    /// @return The value, or `null`.
    [[nodiscard]] Value client(ExprKind form, std::string_view name) const {
        ValueRecord const& values = [&]() -> ValueRecord const& {
            if (form == ExprKind::Env) {
                return _env.get();
            }
            return form == ExprKind::Session ? _sessionValues.get() : _hostValues.get();
        }();
        auto const found = values.find(name);
        return found == values.end() ? Value{} : found->second;
    }

    /// @brief The app document's scope, which `{"ref": "app.name"}` reads and `{"set": "app.name"}` writes; set by
    ///        `Interpreter::mountApp` for as long as the `AppRoot` lives.
    /// @return It, or `nullptr` when no app document is mounted.
    [[nodiscard]] detail::RtScope const* appScope() const noexcept { return _appScope; }

    /// @brief Installs or clears the app document's scope. Called by `AppRoot`, nothing else.
    /// @param scope The scope, or `nullptr`.
    void setAppScope(detail::RtScope const* scope) noexcept { _appScope = scope; }

    /// @brief Drops every cache entry: `signOut` and a principal change (spec 5 §5, "Isolation").
    void clear() {
        _runtime->batch([this] {
            for (auto const& [key, entry] : _entries) {
                static_cast<void>(key);
                ++entry->generation;
                entry->evict.cancel();
                countTopics(*entry, -1);
            }
            _entries.clear();
            for (detail::QueryRuntime* const query : _mounted) {
                detachQuery(*query);
            }
        });
    }

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

    /// @brief Starts a query's request before its screen mounts, so the screen mounts on a warm cache.
    /// @param channel The alias's channel.
    /// @param token   The alias's instance token (`Screen::instanceToken`).
    /// @param typeId  The model type.
    /// @param action  The action.
    /// @param body    The body.
    void prefetch(std::shared_ptr<ModelChannel> const& channel, std::string const& token, std::string const& typeId,
                  std::string const& action, Value const& body) {
        auto entry = acquire(token, typeId, action, false, body);
        if (!entry->pending.peek() && (!entry->hasValue.peek() || entry->stale)) {
            fetch(entry, channel);
        }
        release(entry);
    }

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
    void asUser(std::function<void()> const& body) {
        ++_userDepth;
        try {
            body();
        } catch (...) {
            --_userDepth;
            throw;
        }
        --_userDepth;
    }

    /// @brief The cache key of a query.
    /// @param token    The alias's instance token.
    /// @param typeId   The model type.
    /// @param action   The action.
    /// @param evaluate Whether it is an `evaluate` query.
    /// @param body     The canonical body.
    /// @return The key.
    [[nodiscard]] static std::string cacheKey(std::string const& token, std::string const& typeId,
                                              std::string const& action, bool evaluate, std::string const& body) {
        return typeId + '\x1f' + token + '\x1f' + (evaluate ? "evaluate:" : "") + action + '\x1f' + body;
    }

    /// @brief Finds or makes an entry and counts one more user.
    /// @param token    The alias's instance token.
    /// @param typeId   The model type.
    /// @param action   The action.
    /// @param evaluate Whether it is an `evaluate` query.
    /// @param body     The body.
    /// @return The entry.
    std::shared_ptr<detail::CacheEntry> acquire(std::string const& token, std::string const& typeId,
                                                std::string const& action, bool evaluate, Value const& body) {
        std::string const text = detail::canonicalJson(body);
        std::string key = cacheKey(token, typeId, action, evaluate, text);
        auto found = _entries.find(key);
        if (found == _entries.end()) {
            auto entry = std::make_shared<detail::CacheEntry>(*_runtime);
            entry->key = key;
            entry->typeId = typeId;
            entry->action = action;
            entry->body = text;
            entry->evaluate = evaluate;
            entry->instanceToken = token;
            if (_options.catalog.action) {
                if (auto const info = _options.catalog.action(action)) {
                    entry->secretFields = info->secretFields;
                    if (!evaluate) {
                        entry->records = info->records;
                    }
                }
            }
            countTopics(*entry, +1);
            found = _entries.emplace(std::move(key), std::move(entry)).first;
        }
        ++found->second->users;
        found->second->evict.cancel();
        return found->second;
    }

    /// @brief Counts one user less; an entry nobody uses is dropped after `keepUnused`.
    /// @param entry The entry.
    void release(std::shared_ptr<detail::CacheEntry> const& entry) {
        if (--entry->users > 0 || _options.scheduler == nullptr) {
            return;
        }
        std::weak_ptr<detail::CacheEntry> const weak = entry;
        entry->evict = _options.scheduler->after(_options.keepUnused, [this, weak] {
            auto const held = weak.lock();
            if (held && held->users == 0) {
                countTopics(*held, -1);
                _entries.erase(held->key);
                flushWatches();
            }
        });
    }

    /// @brief Sends an entry's request under a new generation; a reply of an older generation is dropped.
    /// @param entry   The entry.
    /// @param channel The channel to send it through; `nullptr` fails the request at once.
    void fetch(std::shared_ptr<detail::CacheEntry> const& entry, std::shared_ptr<ModelChannel> const& channel) {
        // The watch goes out before the read, on the same connection, so a change committed after the read is
        // seen as an event.
        flushWatches();
        std::uint64_t const generation = ++entry->generation;
        if (channel == nullptr) {
            settleFailure(*entry, "the model alias has no instance", "error");
            return;
        }
        entry->pending.set(true);
        std::weak_ptr<detail::CacheEntry> const weak = entry;
        try {
            auto completion = channel->execute(entry->action, entry->body, CallOptions{.evaluate = entry->evaluate});
            completion
                .then([this, weak, generation](std::string const& reply) {
                    auto const held = weak.lock();
                    if (held && held->generation == generation) {
                        settleReply(*held, reply);
                    }
                })
                .onError([this, weak, generation](std::exception_ptr const& error) {
                    auto const held = weak.lock();
                    if (held && held->generation == generation) {
                        settleFailure(*held, detail::failureText(error), classify(error));
                    }
                });
        } catch (...) {
            std::exception_ptr const error = std::current_exception();
            settleFailure(*entry, detail::failureText(error), classify(error));
        }
    }

    /// @brief Writes an entry's confirmed value and re-applies its optimistic layers over it.
    /// @param entry     The entry.
    /// @param confirmed The confirmed value.
    void confirm(detail::CacheEntry& entry, Value const& confirmed) {
        entry.confirmed = detail::withoutFields(confirmed, entry.secretFields);
        showLayers(entry);
    }

    /// @brief Recomputes what an entry shows from its confirmed value and its layers.
    /// @param entry The entry.
    void showLayers(detail::CacheEntry& entry) {
        Value shown = entry.confirmed;
        for (detail::OptimisticLayer const& layer : entry.layers) {
            if (auto patched = detail::patchRows(shown, layer.at, layer.key, layer.with)) {
                shown = std::move(*patched);
            }
        }
        noteWrite();
        entry.value.set(std::move(shown));
    }

    /// @brief The error kind of a failure.
    /// @param error The failure.
    /// @return The kind.
    [[nodiscard]] std::string classify(std::exception_ptr const& error) const {
        return _options.classify ? _options.classify(error) : detail::defaultErrorKind(error);
    }

    /// @brief The clock.
    /// @return Now.
    [[nodiscard]] std::chrono::milliseconds now() const {
        if (_options.clock) {
            return _options.clock();
        }
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch());
    }

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

    /// @brief The topics the cache holds now: every record an entry holds, and the keyed shared instance of every
    ///        entry whose action holds no record. What `reconnected` sends.
    /// @return The topics, records and instances each in name order; `replace` is false.
    [[nodiscard]] wire::WatchBody watchSet() const;

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
    static void markStale(detail::CacheEntry& entry) {
        entry.stale = true;
        if (entry.pending.peek()) {
            entry.refetchAfter = true;
        }
    }

private:
    // A cap of 0 moves nothing inline, and one above the largest would not fit an envelope once encoded.
    [[nodiscard]] static SessionOptions checkedOptions(SessionOptions options) {
        if (options.maxInlineFileBytes == 0 || options.maxInlineFileBytes > kMaxInlineFileBytes) {
            throw std::invalid_argument{"morph::ui::Session: maxInlineFileBytes is " +
                                        std::to_string(options.maxInlineFileBytes) + "; it is 1 to " +
                                        std::to_string(kMaxInlineFileBytes) + " (kMaxInlineFileBytes)"};
        }
        return options;
    }

    void settleReply(detail::CacheEntry& entry, std::string const& reply) {
        auto json = parseJson(reply);
        auto value = json ? valueFromJson(*json) : std::unexpected(json.error());
        if (!value) {
            settleFailure(entry, "the reply is not a document value: " + value.error(), "error");
            return;
        }
        _runtime->batch([&] {
            confirm(entry, *value);
            entry.hasValue.set(true);
            entry.error.set({});
            entry.errorKind.set({});
            entry.pending.set(false);
            entry.fetchedAt = now();
            entry.stale = entry.refetchAfter;
            if (entry.refetchAfter) {
                entry.refetchAfter = false;
                refetchMounted(entry);
            }
        });
    }

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

    void settleFailure(detail::CacheEntry& entry, std::string text, std::string kind) {
        _runtime->batch([&] {
            noteWrite();
            entry.error.set(std::move(text));
            entry.errorKind.set(std::move(kind));
            entry.pending.set(false);
        });
    }

    static void detachQuery(detail::QueryRuntime& query);

    reactive::Runtime* _runtime;
    SessionOptions _options;
    reactive::Signal<std::set<std::string, std::less<>>> _permitted;
    std::map<std::string, int, std::less<>> _recordRefs;
    std::map<std::pair<std::string, std::string>, int> _instanceRefs;
    std::set<std::string, std::less<>> _watchRecords;
    std::set<std::string, std::less<>> _unwatchRecords;
    std::set<std::pair<std::string, std::string>> _watchInstances;
    std::set<std::pair<std::string, std::string>> _unwatchInstances;
    reactive::Signal<std::shared_ptr<FormatContext const>> _format;
    reactive::Signal<ValueRecord> _env;
    reactive::Signal<ValueRecord> _sessionValues;
    reactive::Signal<ValueRecord> _hostValues;
    detail::RtScope const* _appScope = nullptr;
    std::vector<detail::ScreenCore*> _screens;
    reactive::Signal<std::set<std::string, std::less<>>> _hostCommandNames;
    std::function<HostCommandResult(std::string const&, Value const&)> _hostCommandRunner;
    std::map<std::string, std::shared_ptr<detail::CacheEntry>, std::less<>> _entries;
    std::vector<detail::QueryRuntime*> _mounted;
    int _userDepth = 0;
    bool _lastWriteUser = false;
    std::uint64_t _privateTokens = 0;
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
struct ElementState {
    /// @brief The element's state before any user action.
    /// @param runtime The runtime.
    explicit ElementState(reactive::Runtime& runtime)
        : draft{runtime},
          committed{runtime},
          edited{runtime, false},
          touched{runtime, false},
          focused{runtime, false},
          open{runtime, false},
          submitted{runtime, false},
          selection{runtime, Value{ValueList{}}},
          sort{runtime, Value{ValueList{}}},
          activeKey{runtime},
          checksVersion{runtime, 0},
          inner{runtime, nullptr} {}

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
    /// @brief A table's rows as it shows them, made beside `rows` so that every reader of the table's element
    ///        values, the table's own lowering included, sees one order; `nullptr` for a table without `rows`.
    reactive::Computed<TableView>* view = nullptr;
    /// @brief A `form`'s session (spec 2), made when the element is wired; null when its schema is unknown.
    std::unique_ptr<forms::FormSession> form;
    /// @brief A `collection`'s session (spec 2), made when the element is wired; null when its view document is
    ///        unknown or does not build.
    std::unique_ptr<forms::CollectionSession> collection;
    /// @brief A `wizard`'s session (spec 2), made when the element is wired; null when its wizard document or a
    ///        step's schema is unknown or does not build.
    std::unique_ptr<forms::WizardSession> wizard;
};

/// @brief One check at run time.
struct CheckRuntime {
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
    ~CheckRuntime() {
        for (ElementState* const target : targets) {
            std::erase(target->checks, this);
            target->checksVersion.set(target->checksVersion.peek() + 1);
        }
    }
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
class RtScope : public std::enable_shared_from_this<RtScope> {
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
    [[nodiscard]] NameRef const* find(std::string_view name) const {
        for (RtScope const* scope = this; scope != nullptr; scope = scope->parent.get()) {
            auto const found = scope->names.find(name);
            if (found != scope->names.end()) {
                return &found->second;
            }
        }
        return nullptr;
    }

    /// @brief The innermost row, from this scope outwards.
    /// @return The row signal, or `nullptr`.
    [[nodiscard]] reactive::Signal<Value> const* innermostRow() const {
        for (RtScope const* scope = this; scope != nullptr; scope = scope->parent.get()) {
            if (scope->row != nullptr) {
                return scope->row;
            }
        }
        return nullptr;
    }

    /// @brief The innermost scope that decides when errors show and is submitted: a dialog's, or the screen's.
    /// @return The scope.
    [[nodiscard]] RtScope const& policyScope() const {
        RtScope const* scope = this;
        while (scope->submitted == nullptr && scope->parent != nullptr) {
            scope = scope->parent.get();
        }
        return *scope;
    }

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
    /// @brief The reactive nodes it owns; declared last, so they go first.
    reactive::Scope nodes;
};

/// @brief The `Environment` the evaluator reads through: a scope chain and the names bound at one place.
class ScopeEnv final : public Environment {
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
    [[nodiscard]] Value row(std::span<std::string const> path) const override {
        if (_row != nullptr) {
            return walk(*_row, path);
        }
        reactive::Signal<Value> const* const current = _scope->innermostRow();
        return current == nullptr ? Value{} : walk(current->get(), path);
    }

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
[[nodiscard]] inline Value evalIn(RtScope const& scope, Bindings const* bindings, ExprPtr const& expr) {
    if (!expr) {
        return Value{};
    }
    ScopeEnv const env{scope, bindings};
    return evaluate(*expr, env);
}

/// @brief Whether a value counts as given for `required`: not `null`, not an empty string or list.
/// @param value The value.
/// @return It.
[[nodiscard]] inline bool engaged(Value const& value) {
    if (value.isNull()) {
        return false;
    }
    if (std::string const* const text = value.asString()) {
        return !text->empty();
    }
    if (ValueList const* const items = value.asList()) {
        return !items->empty();
    }
    return true;
}

/// @brief A value as text for a widget: a string as itself, a number in the locale, a timestamp in ISO 8601,
///        `null` as nothing, anything else as JSON.
/// @param value  The value.
/// @param format The formatting facts.
/// @return The text.
[[nodiscard]] inline std::string textOf(Value const& value, FormatContext const& format) {
    if (value.isNull()) {
        return {};
    }
    if (std::string const* const text = value.asString()) {
        return *text;
    }
    if (bool const* const flag = value.asBool()) {
        return *flag ? "true" : "false";
    }
    // A number that cannot be rounded at its own places shows as its exact JSON rather than as a clamped value.
    if (Quantity const* const quantity = value.asQuantity()) {
        auto number = formatNumber(value, format.numeric);
        return number ? *number + " " + unitDisplay(quantity->unit, format.units) : canonicalJson(value);
    }
    if (auto number = formatNumber(value, format.numeric)) {
        return *std::move(number);
    }
    if (time::DateTime const* const instant = value.asTimestamp()) {
        return instant->toIso8601();
    }
    return canonicalJson(value);
}

/// @brief A value as a view key: an `int` or a string as itself, anything else as its JSON text.
/// @param value The value.
/// @return The key.
[[nodiscard]] inline Key keyOf(Value const& value) {
    if (std::int64_t const* const number = value.asInt()) {
        return Key{*number};
    }
    if (std::string const* const text = value.asString()) {
        return Key{*text};
    }
    return Key{canonicalJson(value)};
}

/// @brief A view key as a value.
/// @param key The key.
/// @return An `int` or a string.
[[nodiscard]] inline Value valueOfKey(Key const& key) {
    if (std::int64_t const* const number = std::get_if<std::int64_t>(&key)) {
        return Value{*number};
    }
    return Value{std::get<std::string>(key)};
}

/// @brief A list of strings out of a value: a string, a list of strings, or nothing.
/// @param value The value.
/// @return The strings.
[[nodiscard]] inline std::vector<std::string> stringsOf(Value const& value) {
    std::vector<std::string> out;
    if (std::string const* const text = value.asString()) {
        out.push_back(*text);
    } else if (ValueList const* const items = value.asList()) {
        for (Value const& item : *items) {
            if (std::string const* const element = item.asString()) {
                out.push_back(*element);
            }
        }
    }
    return out;
}

/// @brief A list of strings as a value.
/// @param texts The strings.
/// @return The list.
[[nodiscard]] inline Value listOfStrings(std::vector<std::string> const& texts) {
    ValueList items;
    items.reserve(texts.size());
    for (std::string const& text : texts) {
        items.emplace_back(text);
    }
    return Value{std::move(items)};
}

/// @brief A query at run time (spec 5 §5): a key over the scope, an entry of the session's cache, latest wins.
class QueryRuntime {
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
    void invalidate() {
        if (auto const& current = _entry.peek()) {
            current->stale = true;
        }
        refetch();
    }

    /// @brief Forgets its entry without releasing it: the session dropped the cache.
    void detach() {
        _entry.set(nullptr);
        _carried.set(Value{});
        _target = {};
    }

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
        bool operator==(Target const& other) const {
            return idle == other.idle && token == other.token && bodyText == other.bodyText;
        }
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
class MutationRuntime {
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

    /// @brief The declaration.
    /// @return It.
    [[nodiscard]] MutationDecl const& decl() const noexcept { return *_decl; }

    /// @brief The model type.
    /// @return It.
    [[nodiscard]] std::string const& typeId() const noexcept { return _typeId; }

private:
    struct Run {
        std::uint64_t id = 0;
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

/// @brief A command context without its scope: what a callback the host keeps may hold, so that the host never
///        keeps a scope alive past its screen. The callback holds the scope weakly and puts it back.
/// @param ctx The context.
/// @return The context, its scope dropped.
[[nodiscard]] inline CommandCtx detached(CommandCtx const& ctx) {
    CommandCtx kept = ctx;
    kept.scope = nullptr;
    return kept;
}

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

/// @brief A kind's key spec, from the kind table.
/// @param kind The kind spec.
/// @param key  The key.
/// @return The spec, or `nullptr` when the kind has no such key of its own.
[[nodiscard]] inline KeySpec const* keySpec(KindSpec const& kind, std::string_view key) {
    for (KeySpec const& spec : kind.keys) {
        if (spec.key == key) {
            return &spec;
        }
    }
    return nullptr;
}

/// @brief Whether a node kind keeps per-element state even without an id: the inputs, dialogs, tables and the forms
///        engine's screens.
/// @param kind The kind.
/// @return It.
[[nodiscard]] inline bool statefulKind(std::string_view kind) {
    static constexpr std::array<std::string_view, 12> kStateful = {
        "textInput", "checkbox", "select", "dateTimeInput", "slider",     "filePicker",
        "dialog",    "drawer",   "table",  "form",          "collection", "wizard"};
    return listed(kStateful, kind);
}

/// @brief How many command lists the inspector's log keeps.
inline constexpr std::size_t kCommandLogSize = 256;

/// @brief How often, in lowered nodes, the inspector's node list drops the nodes whose scope is gone.
inline constexpr std::size_t kPruneLoweredAt = 256;

/// @brief The key a command is written with.
/// @param kind The command's kind.
/// @return Its key, such as `refetch`.
[[nodiscard]] inline std::string commandKey(CommandKind kind) {
    switch (kind) {
        case CommandKind::Set:
            return "set";
        case CommandKind::Reset:
            return "reset";
        case CommandKind::Run:
            return "run";
        case CommandKind::Refetch:
            return "refetch";
        case CommandKind::Open:
            return "open";
        case CommandKind::Close:
            return "close";
        case CommandKind::Navigate:
            return "navigate";
        case CommandKind::Return:
            return "return";
        case CommandKind::Focus:
            return "focus";
        case CommandKind::Host:
            return "host";
        case CommandKind::SwitchBackend:
            return "switchBackend";
        case CommandKind::ResetPrefs:
            return "resetPrefs";
        case CommandKind::SetLocale:
            return "setLocale";
        case CommandKind::Notify:
            return "notify";
        case CommandKind::Seq:
            return "seq";
        case CommandKind::All:
            return "all";
        case CommandKind::If:
            return "if";
        case CommandKind::CancelClose:
            return "cancelClose";
        case CommandKind::Retry:
            return "retry";
        case CommandKind::SignIn:
            return "signIn";
        case CommandKind::SignOut:
            return "signOut";
        case CommandKind::Upload:
            return "upload";
        case CommandKind::Download:
            return "download";
        default:
            return "unknown";
    }
}

/// @brief One mounted screen's interpreter: its document, diagnostics, slots and preferences, and the code that
///        declares scopes, lowers nodes and runs commands.
class ScreenCore {
public:
    /// @brief The core of a screen about to mount.
    /// @param doc        The document.
    /// @param owner   The session.
    /// @param calls   The dispatcher.
    /// @param options The interpreter's options.
    /// @param mount   The mount's options.
    ScreenCore(std::shared_ptr<Document const> doc, Session& owner, Dispatcher& calls,
               InterpreterOptions const& options, MountOptions const& mount)
        : session{owner},
          dispatcher{calls},
          host{options.host},
          onDiagnostic{options.diagnostics},
          borrowed{mount.borrowed},
          depth{mount.depth},
          prefs{session.runtime()},
          submitted{session.runtime(), false},
          pendingWork{session.runtime(), 0},
          recording{owner.options().developmentTools == DevelopmentTools::On},
          _doc{std::move(doc)} {
        ValueRecord defaults;
        for (auto const& [name, point] : _doc->customize) {
            defaults.insert_or_assign(name, point.defaultValue);
        }
        prefDefaults = defaults;
        prefs.set(std::move(defaults));
        session.attachScreen(*this);
    }

    ScreenCore(ScreenCore const&) = delete;
    ScreenCore& operator=(ScreenCore const&) = delete;
    ScreenCore(ScreenCore&&) = delete;
    ScreenCore& operator=(ScreenCore&&) = delete;
    /// @brief Takes the screen off the session, so a later backend switch does not run its `onBackendChange`.
    ~ScreenCore() { session.detachScreen(*this); }

    /// @brief Runs a lifecycle list in the document's own scope, as a screen's or app document's own command list.
    /// @param key The lifecycle key, such as `onBackendChange`.
    void runLifecycleOf(std::string_view key) {
        auto const found = _doc->lifecycle.find(key);
        auto const scope = rootScope.lock();
        if (found == _doc->lifecycle.end() || !scope) {
            return;
        }
        CommandCtx const ctx{.scope = scope,
                             .bindings = std::make_shared<Bindings>(),
                             .cancelClose = std::make_shared<bool>(false),
                             .boundary = nullptr};
        runList(found->second, ctx, {});
    }

    /// @brief The document.
    /// @return It.
    [[nodiscard]] Document const& doc() const noexcept { return *_doc; }

    /// @brief The runtime.
    /// @return It.
    [[nodiscard]] reactive::Runtime& runtime() const noexcept { return session.runtime(); }

    /// @brief Reports a diagnostic once per path.
    /// @param path   Where.
    /// @param reason What.
    void report(std::string_view path, std::string_view reason) {
        if (inspecting) {
            return;
        }
        std::string key = std::string{path} + '\x1f' + std::string{reason};
        if (!reported.insert(std::move(key)).second) {
            return;
        }
        Refusal const refusal{.document = _doc->id, .path = std::string{path}, .reason = std::string{reason}};
        diagnostics.push_back(refusal);
        if (onDiagnostic) {
            onDiagnostic(refusal);
        }
    }

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
    void initState(RtScope const& scope, StateSlot& slot) const {
        if (slot.value || slot.initializing) {
            return;
        }
        if (std::optional<Value> keptValue = keptState(scope, *slot.decl)) {
            slot.value = std::make_unique<reactive::Signal<Value>>(runtime(), std::move(*keptValue));
            return;
        }
        slot.initializing = true;
        Value init = runtime().untracked([&] { return evalIn(scope, nullptr, slot.decl->init); });
        slot.value = std::make_unique<reactive::Signal<Value>>(runtime(), std::move(init));
        slot.initializing = false;
    }

    /// @brief Reads an element value. A tracked read.
    /// @param element The element.
    /// @param name    The value's name.
    /// @return The value.
    [[nodiscard]] Value elementValue(ElementState const& element, std::string_view name) const;

    /// @brief The current value of an input, checkbox or select: what `required` tests.
    /// @param element The element.
    /// @return The value.
    [[nodiscard]] Value inputValue(ElementState const& element) const;

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
    void runEvent(Commands const* commands, LowerCtx const& ctx, Value event) {
        if (commands == nullptr || commands->empty()) {
            return;
        }
        auto bindings = std::make_shared<Bindings>(*ctx.bindings);
        bindings->push_back(Binding{.name = "event", .read = [event = std::move(event)] { return event; }});
        CommandCtx const command{.scope = ctx.scope,
                                 .bindings = std::move(bindings),
                                 .cancelClose = std::make_shared<bool>(false),
                                 .boundary = ctx.boundary};
        session.asUser([&] { runtime().batch([&] { runList(*commands, command, {}); }); });
    }

    /// @brief Lowers a list of `actionButton` parts into reactive models: label, icon, enabled and visible bound in
    ///        @p ctx's scope, and a run function that runs the part's `onClick` there as a user event.
    /// @param parts The parts, in order.
    /// @param ctx   The scope the list belongs to.
    /// @return The models, in the parts' order.
    [[nodiscard]] ActionModels lowerActionParts(std::span<DocNodePtr const> parts, LowerCtx const& ctx);

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
    /// @return The node.
    // NOLINTNEXTLINE(misc-no-recursion): one level per nested node of the document.
    [[nodiscard]] Node lowerAll(std::span<DocNodePtr const> nodes, LowerCtx const& ctx) {
        if (nodes.size() == 1) {
            return lower(*nodes.front(), ctx);
        }
        Column column;
        for (DocNodePtr const& child : nodes) {
            column.children.push_back(lower(*child, ctx));
        }
        return ui::column(std::move(column));
    }

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
    /// @brief The models the opener lends.
    std::map<std::string, BorrowedModel, std::less<>> borrowed;
    /// @brief The screen scope's reactive depth.
    std::size_t depth;
    /// @brief The diagnostics reported, in order.
    std::vector<Refusal> diagnostics;
    /// @brief The paths and reasons already reported.
    std::set<std::string, std::less<>> reported;
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
    [[nodiscard]] std::optional<Value> keptState(RtScope const& scope, StateDecl const& decl) const {
        if (keptStates.empty() || rootScope.lock().get() != &scope) {
            return std::nullopt;
        }
        auto const found = keptStates.find(decl.name);
        if (found == keptStates.end() || found->second.type != decl.type.name() ||
            !decl.type.admits(found->second.value)) {
            return std::nullopt;
        }
        return found->second.value;
    }

private:
    void wireElement(ElementState& element);
    void wireErrors(ElementState& element);
    /// A table's rows in the order of its sort, read in the table's scope with @p bindings. A sort that is not a
    /// list of the table's sortable columns, or whose values cannot be ordered, is reported and leaves the source
    /// order.
    void sortedRows(ElementState const& element, Bindings const* bindings, TableView& view);
    /// A grouped table's runs of its source rows by its `group`'s `by`: each row's run index and each run's heading
    /// entry, into @p view.
    static void groupRows(ElementState const& element, Bindings const* bindings, TableView& view);
    /// A table's sorted rows and the page of them it shows, by its `pageSize` and `page`.
    TableView tableView(ElementState const& element, Bindings const* bindings);
    void collect(DocNode const& node, RtScope& scope, std::vector<Commands const*>& commands);
    Node lowerKind(DocNode const& node, LowerCtx const& ctx, ElementState* element);
    Node lowerDialog(DocNode const& node, LowerCtx const& ctx, ElementState* element);
    Node lowerSwitch(DocNode const& node, LowerCtx const& ctx);
    Node lowerRows(DocNode const& node, LowerCtx const& ctx, ElementState* element);
    /// A grouped table's heading row: its `group`'s `heading`, lowered as one cell in a row scope of its own whose
    /// row is the group's run record and whose key is `key`, the row's canonical key, as a data row's scope has.
    TableRowView headingRow(DocNode const& node, LowerCtx const& ctx, reactive::Signal<Value> const& row,
                            std::string key);
    /// A `rowTone` value as a tone: `neutral`, `info`, `ok`, `warn`, `err`, or `null` for none. Another value is
    /// reported, at @p path, while @p noted is still false, and shows no tone.
    std::optional<Tone> rowToneOf(Value const& value, std::string const& path, bool& noted);
    Node lowerBoundary(DocNode const& node, LowerCtx const& ctx);
    Node lowerCustom(DocNode const& node, LowerCtx const& ctx);
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
    Node lowerForm(DocNode const& node, LowerCtx const& ctx, ElementState* element);
    [[nodiscard]] forms::Submitter aliasCalls(ModelAlias* alias);
    [[nodiscard]] forms::Submitter aliasSubmits(ModelAlias* alias, std::string read = {});
    [[nodiscard]] forms::FormSessionOptions engineOptions() const;
    void wireComposite(ElementState& element);
    Node lowerComposite(DocNode const& node, LowerCtx const& ctx, ElementState* element);
    void presentForm(DocNode const& node, LowerCtx const& ctx, forms::FormViewOptions& options);
    Node lowerTabs(DocNode const& node, LowerCtx const& ctx, ElementState* element);
    std::shared_ptr<RtScope> rowScope(DocNode const& node, LowerCtx const& ctx, reactive::Signal<Value> const& row,
                                      std::vector<DocNode const*> const& content, std::string key);
    bool focusTarget(Command const& command, RtScope const& scope, Bindings const* bindings);
    void commonOf(DocNode const& node, LowerCtx const& ctx, ElementState* element, Common& common);
    void layoutOf(DocNode const& node, LowerCtx const& ctx, LayoutHints& layout);
    void fieldOf(DocNode const& node, LowerCtx const& ctx, ElementState* element, FieldState& field);
    std::vector<MenuItem> menuItems(std::span<DocNodePtr const> parts, LowerCtx const& ctx);
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
    bool unsupported(Command const& command, std::string_view what) {
        report(command.path, std::string{what} + " is not supported by the host");
        return false;
    }

    /// @brief A bound property: a constant for a literal, a numbered slot otherwise.
    template <typename T, typename Convert>
    Prop<T> bindProp(LowerCtx const& ctx, ExprPtr const& expr, T fallback, Convert convert) {
        if (!expr) {
            return Prop<T>{std::move(fallback)};
        }
        if (expr->kind == ExprKind::Literal) {
            return Prop<T>{convert(expr->literal, session.formatting())};
        }
        return Prop<T>{ui::slot(
            nextSlot(), [scope = ctx.scope, bindings = ctx.bindings, expr, convert, session = &session]() -> T {
                return convert(evalIn(*scope, bindings.get(), expr), session->formatting());
            })};
    }

    /// @brief A text's role: a literal one as the loader checked it; a computed one evaluated in its slot, where a
    ///        name of no role is reported once for its path and drawn `Normal`.
    Prop<TextRole> roleProp(LowerCtx const& ctx, ExprPtr const& expr);

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
    [[nodiscard]] Value once(LowerCtx const& ctx, ExprPtr const& expr) const {
        return runtime().untracked([&] { return evalIn(*ctx.scope, ctx.bindings.get(), expr); });
    }

    /// @brief Keeps a command list in the inspector's log.
    void logList(Commands const& commands) {
        CommandLogEntry entry{.event = {}, .commands = {}, .batch = runtime().core()->currentBatch()};
        std::string_view const first = commands.front().path;
        entry.event = std::string{first.substr(0, first.rfind('/') == std::string_view::npos ? 0 : first.rfind('/'))};
        for (Command const& command : commands) {
            entry.commands.push_back(
                LoggedCommand{.kind = commandKey(command.kind), .target = command.target, .path = command.path});
        }
        if (commandLog.size() == kCommandLogSize) {
            commandLog.pop_front();
        }
        commandLog.push_back(std::move(entry));
    }

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

/// @brief An action of a screen's head, as its handle answers it.
struct HeadAction {
    /// @brief The action's id, what `ScreenHead::run` takes.
    std::string id;
    /// @brief The label.
    std::string label;
    /// @brief The name of an icon; empty for none.
    std::string icon;
    /// @brief How much it stands out.
    ButtonEmphasis emphasis = ButtonEmphasis::Normal;
    /// @brief Whether it can be run now.
    bool enabled = true;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(HeadAction const&) const = default;
};

/// @brief A mounted screen: its scopes, its view mounted through `Mounted`, and what its host asks of it.
///
/// Neither copyable nor movable. It belongs to the runtime's owner; the runtime, the session, the dispatcher and
/// the backend outlive it. Destroying it runs `onUnmount`, unmounts the view and releases every scope.
class Screen {
public:
    Screen(Screen const&) = delete;
    Screen& operator=(Screen const&) = delete;
    Screen(Screen&&) = delete;
    Screen& operator=(Screen&&) = delete;

    /// @brief Runs `onUnmount`, then unmounts.
    ~Screen() {
        try {
            if (Commands const* const commands = lifecycleList("onUnmount")) {
                runCommands(*commands);
            }
        } catch (...) {  // NOLINT(bugprone-empty-catch): a destructor has no one to report to.
        }
        _core->focusElement = {};
        _mounted.reset();
        _headActions.clear();
        _headStatus.clear();
        _scope.reset();
    }

    /// @brief The root widget.
    /// @return It, valid as long as the screen.
    [[nodiscard]] Widget& root() const {
        if (!_mounted) {
            throw std::logic_error{"the screen is not mounted"};
        }
        return _mounted->root();
    }

    /// @brief The document's id.
    /// @return It.
    [[nodiscard]] std::string const& id() const noexcept { return _core->doc().id; }

    /// @brief Evaluates an expression in the screen's scope: what a host reads (`title`, `identity`), and what a
    ///        test inspects.
    /// @param expression The expression's JSON.
    /// @return The value; `null` for an expression that does not parse.
    [[nodiscard]] Value read(Json const& expression) const {
        auto parsed = parseExpression(expression);
        if (!parsed) {
            return Value{};
        }
        return detail::evalIn(*_scope, nullptr, *parsed);
    }

    /// @brief Evaluates the document's `title`.
    /// @return It, or `null`.
    [[nodiscard]] Value title() const { return detail::evalIn(*_scope, nullptr, _core->doc().title); }

    /// @brief Evaluates the document's `identity`.
    /// @return It, or `null`.
    [[nodiscard]] Value identity() const { return detail::evalIn(*_scope, nullptr, _core->doc().identity); }

    /// @brief Evaluates the document's `subtitle`, the head's line under the title.
    /// @return It, or `null`.
    [[nodiscard]] Value subtitle() const { return detail::evalIn(*_scope, nullptr, _core->doc().subtitle); }

    /// @brief The head's status badges that are visible now, in order. A tracked read.
    /// @return They.
    [[nodiscard]] std::vector<HeadBadge> status() const {
        std::vector<HeadBadge> shown;
        for (detail::BadgeModel const& badge : _headStatus) {
            if (badge.visible.evaluate()) {
                shown.push_back(HeadBadge{
                    .text = badge.text.evaluate(), .tone = badge.tone.evaluate(), .icon = badge.icon.evaluate()});
            }
        }
        return shown;
    }

    /// @brief The head's actions that are visible now, in order, each with whether it is enabled. A tracked read.
    /// @return They.
    [[nodiscard]] std::vector<HeadAction> actions() const {
        std::vector<HeadAction> shown;
        for (detail::ActionModel const& action : _headActions) {
            if (action.visible.evaluate()) {
                shown.push_back(HeadAction{.id = action.id,
                                           .label = action.label.evaluate(),
                                           .icon = action.icon.evaluate(),
                                           .emphasis = action.emphasis,
                                           .enabled = action.enabled.evaluate()});
            }
        }
        return shown;
    }

    /// @brief Runs a head action's `onClick` as a user event in the screen scope, in one batch.
    /// @param actionId The action's id.
    /// @return Whether it ran: `false`, with a diagnostic, for an id the head does not hold, or holds hidden or
    ///         disabled.
    bool runAction(std::string_view actionId) {
        auto const found = std::ranges::find(_headActions, actionId, &detail::ActionModel::id);
        if (found == _headActions.end()) {
            _core->report("/actions", "head action " + std::string{actionId} + " is unknown");
            return false;
        }
        auto const [visible, enabled] =
            runtime().untracked([&] { return std::pair{found->visible.evaluate(), found->enabled.evaluate()}; });
        if (!visible || !enabled) {
            _core->report(found->path, "head action " + found->id + (visible ? " is disabled" : " is hidden"));
            return false;
        }
        if (found->run) {
            found->run();
        }
        return true;
    }

    /// @brief Evaluates the document's `dirty`.
    /// @return Whether it is `true`.
    [[nodiscard]] bool dirty() const {
        Value const value = detail::evalIn(*_scope, nullptr, _core->doc().dirty);
        bool const* const flag = value.asBool();
        return flag != nullptr && *flag;
    }

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
    bool requestClose() {
        std::shared_ptr<bool const> const cancelled = beginClose();
        return cancelled == nullptr || !*cancelled;
    }

    /// @brief Runs `onClose`, and hands back the flag its `cancelClose` sets.
    ///
    /// The flag is shared with every command list `onClose` starts, such as the `onResult` of a confirm dialog it
    /// opens, so it can still be set after this returns; a host reads it once the screen is no longer `pending()`.
    /// @return The flag, or `nullptr` when the document declares no `onClose`.
    [[nodiscard]] std::shared_ptr<bool const> beginClose() {
        Commands const* const commands = lifecycleList("onClose");
        if (commands == nullptr) {
            return nullptr;
        }
        return runCommandsFlag(*commands);
    }

    /// @brief Whether the document declares a lifecycle command list.
    /// @param key The key: `onClose`, `onSave`, …
    /// @return It.
    [[nodiscard]] bool declares(std::string_view key) const { return lifecycleList(key) != nullptr; }

    /// @brief Runs a lifecycle command list: `onSave`, `onBackendChange`, or any other the document declares.
    /// @param key The key.
    void runLifecycle(std::string_view key) {
        if (Commands const* const commands = lifecycleList(key)) {
            runCommands(*commands);
        }
    }

    /// @brief The diagnostics reported so far, each path and reason once.
    /// @return They.
    [[nodiscard]] std::vector<Refusal> const& diagnostics() const noexcept { return _core->diagnostics; }

    /// @brief Sets the user's choice at a customization point, as the preferences store loaded or the user made
    ///        it. A choice that does not fit the point is ignored (spec 5 §12b).
    /// @param point The point.
    /// @param value The choice.
    /// @return Whether it fits and was taken.
    bool setPreference(std::string const& point, Value value) {
        auto const found = _core->doc().customize.find(point);
        if (found == _core->doc().customize.end() || !fits(found->second, value)) {
            return false;
        }
        ValueRecord prefs = _core->prefs.peek();
        prefs.insert_or_assign(point, std::move(value));
        _core->prefs.set(std::move(prefs));
        return true;
    }

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

    Screen() = default;

    /// The states of the screen's own scope that have a value, by name: what a screen that replaces it keeps.
    [[nodiscard]] std::map<std::string, detail::ScreenCore::KeptState, std::less<>> ownState() const {
        std::map<std::string, detail::ScreenCore::KeptState, std::less<>> states;
        for (auto const& slot : _scope->states) {
            if (slot->value) {
                states.insert_or_assign(
                    slot->decl->name,
                    detail::ScreenCore::KeptState{.type = slot->decl->type.name(), .value = slot->value->peek()});
            }
        }
        return states;
    }

    [[nodiscard]] Commands const* lifecycleList(std::string_view key) const {
        auto const found = _core->doc().lifecycle.find(key);
        return found == _core->doc().lifecycle.end() ? nullptr : &found->second;
    }

    /// Runs a command list as the screen's own (not a user's); returns whether it ran `cancelClose`.
    bool runCommands(Commands const& commands) { return *runCommandsFlag(commands); }

    /// Runs a command list as the screen's own; returns the `cancelClose` flag it and what it starts share.
    std::shared_ptr<bool const> runCommandsFlag(Commands const& commands) {
        detail::CommandCtx const ctx{.scope = _scope,
                                     .bindings = std::make_shared<detail::Bindings>(),
                                     .cancelClose = std::make_shared<bool>(false),
                                     .boundary = nullptr};
        _core->runtime().batch([&] { _core->runList(commands, ctx, {}); });
        return ctx.cancelClose;
    }

    [[nodiscard]] static bool fits(CustomizePoint const& point, Value const& value) {
        if (point.kind == "choice") {
            std::string const* const text = value.asString();
            return text != nullptr && listed(std::span<std::string const>{point.options}, *text);
        }
        if (point.kind == "value") {
            return point.type.admits(value);
        }
        return true;
    }

    [[nodiscard]] static bool listed(std::span<std::string const> options, std::string const& text) {
        return std::ranges::find(options, text) != options.end();
    }

    std::unique_ptr<detail::ScreenCore> _core;
    std::shared_ptr<detail::RtScope> _scope;
    std::optional<Mounted> _mounted;
    // The head's parts, bound in the screen scope.
    std::vector<detail::BadgeModel> _headStatus;
    detail::ActionModels _headActions;
};

/// @brief The app document mounted as the root scope every screen of the session shares (spec 6 §5).
///
/// Its state, lets, queries and mutations live once for the session. A screen reads them as `{"ref": "app.name"}`,
/// a binding on one updates, and `{"set": "app.name", …}` writes the app document's state. A query of the app
/// document runs once however many screens read it. Destroying the root runs its `onUnmount`, takes its scope off the
/// session and releases its model instances; a host does that before it clears the session on sign-out.
class AppRoot {
public:
    AppRoot(AppRoot const&) = delete;
    AppRoot& operator=(AppRoot const&) = delete;
    AppRoot(AppRoot&&) = delete;
    AppRoot& operator=(AppRoot&&) = delete;

    /// @brief Runs `onUnmount`, takes the scope off the session, and releases everything.
    ~AppRoot() {
        try {
            if (auto const found = _core->doc().lifecycle.find("onUnmount"); found != _core->doc().lifecycle.end()) {
                runCommands(found->second);
            }
        } catch (...) {  // NOLINT(bugprone-empty-catch): a destructor has no one to report to.
        }
        _core->session.setAppScope(nullptr);
        _scope.reset();
    }

    /// @brief Evaluates an expression in the app document's scope.
    /// @param expression The expression's JSON.
    /// @return Its value; `null` for an expression that does not parse.
    [[nodiscard]] Value read(Json const& expression) const {
        auto parsed = parseExpression(expression);
        if (!parsed) {
            return Value{};
        }
        return detail::evalIn(*_scope, nullptr, *parsed);
    }

    /// @brief The diagnostics reported so far.
    /// @return They.
    [[nodiscard]] std::vector<Refusal> const& diagnostics() const noexcept { return _core->diagnostics; }

private:
    friend class Interpreter;

    AppRoot() = default;

    void runCommands(Commands const& commands) {
        detail::CommandCtx const ctx{.scope = _scope,
                                     .bindings = std::make_shared<detail::Bindings>(),
                                     .cancelClose = std::make_shared<bool>(false),
                                     .boundary = nullptr};
        _core->runtime().batch([&] { _core->runList(commands, ctx, {}); });
    }

    std::unique_ptr<detail::ScreenCore> _core;
    std::shared_ptr<detail::RtScope> _scope;
};

/// @brief Mounts UI documents (spec 5): one per client process or window, over a session and a dispatcher.
class Interpreter {
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

// ---------------------------------------------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------------------------------------------

inline void Session::backendChanged(std::string const& backend) {
    _runtime->batch([this, &backend] {
        std::vector<detail::ScreenCore*> const screens = _screens;
        for (detail::ScreenCore* const screen : screens) {
            if (std::ranges::find(_screens, screen) != _screens.end()) {
                screen->runLifecycleOf("onBackendChange");
            }
        }
        ValueRecord env = _env.peek();
        env.insert_or_assign("backend", Value{backend});
        _env.set(std::move(env));
        for (auto const& [key, entry] : _entries) {
            static_cast<void>(key);
            markStale(*entry);
        }
        std::vector<detail::QueryRuntime*> const mounted = _mounted;
        for (detail::QueryRuntime* const query : mounted) {
            query->refetchCurrent();
        }
    });
}

inline void Session::refetchMounted(detail::CacheEntry const& entry) {
    std::vector<detail::QueryRuntime*> const mounted = _mounted;
    for (detail::QueryRuntime* const query : mounted) {
        if (_runtime->untracked([query] { return query->entry(); }).get() == &entry) {
            query->refetchCurrent();
        }
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a model type and an action id, as every caller names them.
inline void Session::applyReplyEffects(std::string const& typeId, std::string const& actionId, Value const& reply,
                                       ReplyUse use, std::string const& instanceToken) {
    if (!_options.catalog.action) {
        return;
    }
    std::optional<ActionInfo> const info = _options.catalog.action(actionId);
    if (!info || !info->effects) {
        return;
    }
    detail::Marks marks;
    for (ActionEffect const& effect : *info->effects) {
        if (effect.kind == "changesInstance") {
            for (auto const& [key, entry] : _entries) {
                static_cast<void>(key);
                if (!entry->evaluate && entry->typeId == typeId && entry->instanceToken == instanceToken) {
                    marks.add(*entry);
                }
            }
        } else if (effect.record && (effect.kind == "updates" || effect.kind == "changes")) {
            applyRecordEffect(effect, *effect.record, *info, reply, use, marks);
        }
    }
    markAndRefetch(marks);
}

inline void Session::applyRecordEffect(ActionEffect const& effect, std::string const& record, ActionInfo const& info,
                                       Value const& reply, ReplyUse use, detail::Marks& marks) {
    // The reply's rows of the record, by key; none when the reply does not hold it keyed, or must only mark.
    auto const own = std::ranges::find(info.records, record, &ActionRecordInfo::record);
    std::optional<std::string> const ownKey =
        effect.kind == "updates" && use == ReplyUse::Applied && own != info.records.end() ? own->keyField
                                                                                          : std::nullopt;
    detail::RowsByKey rows;
    if (ownKey) {
        for (Value const* const row : detail::keyedRowsOf(reply, own->paths, *ownKey)) {
            rows.insert_or_assign(detail::canonicalJson(*row->field(*ownKey)), row);
        }
    }
    for (auto const& [key, entry] : _entries) {
        static_cast<void>(key);
        ActionRecordInfo const* const held = entry->holding(record);
        if (held == nullptr) {
            continue;
        }
        if (!ownKey || !held->keyField) {
            marks.add(*entry);
            continue;
        }
        if (auto patched = detail::patchHeldRows(entry->confirmed, held->paths, *held->keyField, rows)) {
            confirm(*entry, *patched);
        }
        // A fetch in flight may have read before the write, and would settle over the patch.
        if (entry->pending.peek()) {
            markStale(*entry);
        }
    }
}

inline void Session::markAndRefetch(detail::Marks const& marks) {
    for (detail::CacheEntry* const entry : marks.entries) {
        markStale(*entry);
    }
    for (detail::CacheEntry const* const entry : marks.entries) {
        refetchMounted(*entry);
    }
}

inline void Session::countTopics(detail::CacheEntry const& entry, int delta) {
    if (entry.evaluate) {
        return;
    }
    // One topic: its count, and the queues a change of the count from or to zero goes to.
    auto const count = [delta](auto& refs, auto const& topic, auto& watches, auto& unwatches) {
        int& refsOf = refs[topic];
        refsOf += delta;
        if (delta > 0 && refsOf == 1 && unwatches.erase(topic) == 0) {
            watches.insert(topic);
        } else if (refsOf <= 0) {
            refs.erase(topic);
            if (watches.erase(topic) == 0) {
                unwatches.insert(topic);
            }
        }
    };
    if (!entry.records.empty()) {
        for (ActionRecordInfo const& record : entry.records) {
            count(_recordRefs, record.record, _watchRecords, _unwatchRecords);
        }
        return;
    }
    constexpr std::string_view kShared = "shared:";
    if (entry.instanceToken.starts_with(kShared)) {
        count(_instanceRefs, std::pair{entry.typeId, entry.instanceToken.substr(kShared.size())}, _watchInstances,
              _unwatchInstances);
    }
}

inline void Session::sendWatch(std::string_view kind, wire::WatchBody const& body) const {
    auto completion = _options.watcher(std::string{kind}, glz::write_json(body).value_or(std::string{}));
    completion.then([](std::string const&) {}).onError([](std::exception_ptr const&) {});
}

inline void Session::flushWatches() {
    if (!_options.watcher) {
        _watchRecords.clear();
        _unwatchRecords.clear();
        _watchInstances.clear();
        _unwatchInstances.clear();
        return;
    }
    auto const take = [](auto& records, auto& instances) {
        wire::WatchBody body;
        body.records.assign(records.begin(), records.end());
        for (auto const& [modelType, primary] : instances) {
            body.instances.push_back(wire::InstanceTopic{.modelType = modelType, .primary = primary, .instance = 0});
        }
        records.clear();
        instances.clear();
        return body;
    };
    if (!_watchRecords.empty() || !_watchInstances.empty()) {
        sendWatch(wire::kWatchKind, take(_watchRecords, _watchInstances));
    }
    if (!_unwatchRecords.empty() || !_unwatchInstances.empty()) {
        sendWatch(wire::kUnwatchKind, take(_unwatchRecords, _unwatchInstances));
    }
}

inline wire::WatchBody Session::watchSet() const {
    wire::WatchBody body;
    for (auto const& [record, refs] : _recordRefs) {
        static_cast<void>(refs);
        body.records.push_back(record);
    }
    for (auto const& [topic, refs] : _instanceRefs) {
        static_cast<void>(refs);
        body.instances.push_back(
            wire::InstanceTopic{.modelType = topic.first, .primary = topic.second, .instance = 0});
    }
    return body;
}

inline void Session::markAllStale() {
    _runtime->batch([this] {
        detail::Marks marks;
        for (auto const& [key, entry] : _entries) {
            static_cast<void>(key);
            if (!entry->evaluate) {
                marks.add(*entry);
            }
        }
        markAndRefetch(marks);
    });
}

inline void Session::reconnected() {
    // The whole set replaces what the server holds, so the queued changes are superseded.
    _watchRecords.clear();
    _unwatchRecords.clear();
    _watchInstances.clear();
    _unwatchInstances.clear();
    if (_options.watcher) {
        wire::WatchBody body = watchSet();
        body.replace = true;
        sendWatch(wire::kWatchKind, body);
    }
    markAllStale();
}

inline void Session::deliverEvent(wire::EventBody const& event) {
    if (event.topic == wire::kResyncTopic) {
        reconnected();
        return;
    }
    if (event.topic != wire::kChangesTopic) {
        return;
    }
    if (auto changes = wire::readChangesBody(event.payload.str)) {
        applyChanges(*changes);
    }
}

inline void Session::applyChanges(wire::ChangesBody const& changes) {
    _runtime->batch([&] {
        detail::Marks marks;
        for (effects::RecordChange const& change : changes.records) {
            markRecordChange(change, marks);
        }
        for (wire::InstanceTopic const& instance : changes.instances) {
            markInstanceChange(instance, marks);
        }
        markAndRefetch(marks);
    });
}

inline void Session::markRecordChange(effects::RecordChange const& change, detail::Marks& marks) {
    // The keys' canonical text, so a key the server wrote is compared with the rows' own.
    std::set<std::string, std::less<>> keys;
    for (glz::raw_json const& key : change.keys.value_or(std::vector<glz::raw_json>{})) {
        auto json = parseJson(key.str);
        auto value = json ? valueFromJson(*json) : std::unexpected(json.error());
        if (value) {
            keys.insert(detail::canonicalJson(*value));
        }
    }
    for (auto const& [name, entry] : _entries) {
        static_cast<void>(name);
        ActionRecordInfo const* const held = entry->holding(change.record);
        if (held == nullptr) {
            continue;
        }
        // An entry still waiting for its first value may come to hold a key; keyless rows cannot be told apart.
        if (!change.keys || !held->keyField || !entry->hasValue.peek()) {
            marks.add(*entry);
            continue;
        }
        std::string const& keyField = *held->keyField;
        bool const holdsKey = std::ranges::any_of(
            detail::keyedRowsOf(entry->confirmed, held->paths, keyField),
            [&](Value const* row) { return keys.contains(detail::canonicalJson(*row->field(keyField))); });
        if (holdsKey) {
            marks.add(*entry);
        }
    }
}

inline void Session::markInstanceChange(wire::InstanceTopic const& instance, detail::Marks& marks) {
    std::string const shared = "shared:" + instance.primary;
    for (auto const& [name, entry] : _entries) {
        static_cast<void>(name);
        if (entry->evaluate || entry->typeId != instance.modelType) {
            continue;
        }
        // Without a primary key, the instance is one of the connection's own private ones.
        bool const same =
            instance.primary.empty() ? !entry->instanceToken.starts_with("shared") : entry->instanceToken == shared;
        if (same) {
            marks.add(*entry);
        }
    }
}

inline void Session::actionSucceeded(std::string const& typeId, std::string const& actionId, Value const& reply,
                                     ReplyUse use, std::string const& instanceToken) {
    _runtime->batch([&] { applyReplyEffects(typeId, actionId, reply, use, instanceToken); });
}

inline void Session::detachQuery(detail::QueryRuntime& query) { query.detach(); }

namespace detail {

/// @brief The text of an instance key, as the bridge's string-keyed attach takes it.
/// @param key The key's value.
/// @return The text; empty for `null`.
[[nodiscard]] inline std::string instanceKeyText(Value const& key) {
    if (key.isNull()) {
        return {};
    }
    if (std::int64_t const* const number = key.asInt()) {
        return std::to_string(*number);
    }
    if (std::string const* const text = key.asString()) {
        return *text;
    }
    return canonicalJson(key);
}

/// @brief The channel an alias is bound to for its current token, binding it when the token changed.
/// @param alias The alias.
/// @param core  The screen.
/// @return The channel, or `nullptr` while the alias is unbound.
[[nodiscard]] inline std::shared_ptr<ModelChannel> channelOf(ModelAlias& alias, ScreenCore& core) {
    std::string const token = core.runtime().untracked([&] { return alias.token->get(); });
    if (token.empty()) {
        return nullptr;
    }
    if (alias.channel && alias.boundToken == token) {
        return alias.channel;
    }
    std::string const& type = alias.decl->type;
    switch (alias.mode) {
        case InstanceMode::Borrowed: {
            auto const found = core.borrowed.find(alias.decl->name);
            alias.channel = found == core.borrowed.end() ? nullptr : found->second.channel;
            break;
        }
        case InstanceMode::Shared:
            // `shared` is for a stateless model, whose instances are interchangeable: its calls go to an instance of
            // the alias's own, and what it shares with other screens is the query cache, through the token.
            alias.channel = core.dispatcher.bind(type, bridge::BindSharing::Private, {});
            break;
        case InstanceMode::Keyed:
            alias.channel = core.dispatcher.bind(type, bridge::BindSharing::Shared, token.substr(7));
            break;
        case InstanceMode::Private:
        case InstanceMode::Default:
        default:
            alias.channel = core.dispatcher.bind(type, bridge::BindSharing::Private, {});
            break;
    }
    alias.boundToken = token;
    return alias.channel;
}

inline RtScope::RtScope(ScreenCore& screen, std::shared_ptr<RtScope> enclosing, reactive::Signal<Value> const* current,
                        std::size_t depth)
    : core{screen}, parent{std::move(enclosing)}, row{current}, nodes{screen.runtime(), depth} {
    if (core.recording) {
        core.scopes.push_back(this);
    }
}

inline RtScope::~RtScope() {
    if (core.recording) {
        std::erase(core.scopes, this);
    }
    if (owner != nullptr) {
        core.addPending(-1);
        if (owner->inner.peek() == this) {
            owner->inner.set(nullptr);
        }
    }
}

// NOLINTNEXTLINE(misc-no-recursion): an element's value can read a nested scope's readiness, one level per nesting.
inline bool RtScope::ready() const {
    for (auto const& element : elements) {
        Value const shown = core.elementValue(*element, "visible");
        bool const* const visible = shown.asBool();
        ValueList const* const errors = element->errors->get().asList();
        if ((visible == nullptr || *visible) && errors != nullptr && !errors->empty()) {
            return false;
        }
    }
    for (auto const& check : checks) {
        if (check->decl->blocking && check->failing->get()) {
            return false;
        }
    }
    return true;
}

// NOLINTNEXTLINE(misc-no-recursion): an `app.` name is read in the root scope with its head consumed, once per segment.
inline Value ScopeEnv::ref(std::string_view head, std::span<std::string const> path) const {
    if (head == "app") {
        RtScope const* const app = _scope->core.session.appScope();
        if (app == nullptr || path.empty()) {
            return Value{};
        }
        return ScopeEnv{*app, nullptr}.ref(path.front(), path.subspan(1));
    }
    if (_bindings != nullptr) {
        for (Binding const& bound : std::views::reverse(*_bindings)) {
            if (bound.name == head) {
                return walk(bound.read(), path);
            }
        }
    }
    NameRef const* const name = _scope->find(head);
    if (name == nullptr) {
        return Value{};
    }
    switch (name->category) {
        case NameCategory::Param:
            return walk(name->param, path);
        case NameCategory::State:
            _scope->core.initState(*name->state->scope, *name->state);
            // State inits are acyclic (`loadDocument` checks it), so the value is made by now.
            return walk(name->state->value->get(), path);
        case NameCategory::Let:
            return walk(name->let->get(), path);
        case NameCategory::Query:
            return walk(name->query->value(), path);
        case NameCategory::Element:
            if (path.empty()) {
                return Value{};
            }
            return walk(_scope->core.elementValue(*name->element, path.front()), path.subspan(1));
        case NameCategory::Transfer: {
            if (path.empty()) {
                return Value{};
            }
            TransferState const& transfer = *name->transfer;
            Value value;
            if (path.front() == "progress") {
                value = transfer.progress.get();
            } else if (path.front() == "pending") {
                value = Value{transfer.pending.get()};
            } else if (path.front() == "error") {
                value = transfer.error.get();
            }
            return walk(value, path.subspan(1));
        }
        case NameCategory::Model:
        case NameCategory::Mutation:
        default:
            return Value{};
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per control form and kind of name.
inline Value ScopeEnv::control(ExprKind form, std::string_view name) const {
    Session const& session = _scope->core.session;
    NameRef const* const found = _scope->find(name);
    if (form == ExprKind::Can) {
        if (found != nullptr && found->category == NameCategory::Query) {
            return session.can(found->query->decl().action);
        }
        if (found != nullptr && found->category == NameCategory::Mutation) {
            return session.can(found->mutation->decl().action);
        }
        return session.can(name);
    }
    if (found != nullptr && found->category == NameCategory::Query) {
        QueryRuntime const& query = *found->query;
        if (form == ExprKind::Query) {
            return query.value();
        }
        if (form == ExprKind::Pending) {
            return query.pending();
        }
        if (form == ExprKind::Error) {
            return query.error();
        }
        return form == ExprKind::ErrorKind ? query.errorKind() : Value{};
    }
    if (found != nullptr && found->category == NameCategory::Mutation) {
        MutationRuntime const& mutation = *found->mutation;
        if (form == ExprKind::Result) {
            return mutation.result();
        }
        if (form == ExprKind::Pending) {
            return mutation.pending();
        }
        if (form == ExprKind::Error) {
            return mutation.error();
        }
        return form == ExprKind::ErrorKind ? mutation.errorKind() : Value{};
    }
    return Value{};
}

inline Value ScopeEnv::pref(std::string_view point, std::span<std::string const> path) const {
    ValueRecord const& prefs = _scope->core.prefs.get();
    auto const found = prefs.find(point);
    return found == prefs.end() ? Value{} : walk(found->second, path);
}

inline Value ScopeEnv::client(ExprKind form, std::string_view name) const {
    return _scope->core.session.client(form, name);
}

inline FormatContext const& ScopeEnv::formatting() const { return _scope->core.session.formatting(); }

inline void ScopeEnv::report(std::string_view path, std::string_view reason) const {
    _scope->core.report(path, reason);
}

// --- Queries ---------------------------------------------------------------------------------------------------

inline QueryRuntime::QueryRuntime(QueryDecl const& decl, RtScope& scope, ModelAlias* alias)
    : _decl{&decl},
      _typeId{alias->decl->type},
      _scope{&scope},
      _alias{alias},
      _entry{scope.core.runtime()},
      _carried{scope.core.runtime()},
      _waiting{scope.core.runtime(), false} {
    scope.core.session.attachQuery(*this);
}

inline QueryRuntime::~QueryRuntime() {
    _debounce.cancel();
    _refresh.cancel();
    _scope->core.session.forgetQuery(*this);
    if (auto const& current = _entry.peek()) {
        _scope->core.session.release(current);
    }
}

inline std::shared_ptr<ModelChannel> QueryRuntime::channel() const { return channelOf(*_alias, _scope->core); }

// NOLINTNEXTLINE(readability-function-cognitive-complexity): the query's key, its fetch and its refresh triggers.
inline void QueryRuntime::start() {
    RtScope& scope = *_scope;
    auto& key = scope.nodes.computed([this]() -> Target {
        Target target;
        if (_decl->when) {
            Value const when = evalIn(*_scope, nullptr, _decl->when);
            bool const* const flag = when.asBool();
            if (flag == nullptr || !*flag) {
                return target;
            }
        }
        std::string token = _alias->token->get();
        if (token.empty()) {
            return target;
        }
        Value body = _decl->body ? evalIn(*_scope, nullptr, _decl->body) : Value{ValueRecord{}};
        if (body.isNull()) {
            return target;
        }
        target.idle = false;
        target.token = std::move(token);
        target.bodyText = canonicalJson(body);
        target.body = std::move(body);
        return target;
    });
    _key = &key;
    reactive::Scheduler* const scheduler = scope.core.session.options().scheduler;
    std::weak_ptr<bool> const alive = _alive;
    scope.nodes.effect([this, &key, scheduler, alive, first = true]() mutable {
        Target target = key.get();
        _scope->core.runtime().untracked([&] {
            if (first || target.idle || _decl->debounce <= 0 || scheduler == nullptr) {
                first = false;
                _debounce.cancel();
                _waiting.set(false);
                switchTo(target);
                return;
            }
            _pendingTarget = std::move(target);
            _waiting.set(true);
            _debounce = scheduler->after(std::chrono::milliseconds{_decl->debounce}, [this, alive] {
                if (alive.expired()) {
                    return;
                }
                _scope->core.runtime().batch([this] {
                    _waiting.set(false);
                    switchTo(_pendingTarget);
                });
            });
        });
    });
    if (_decl->refreshEvery > 0 && scheduler != nullptr) {
        _refresh = scheduler->every(std::chrono::milliseconds{_decl->refreshEvery}, [this, alive] {
            if (alive.expired()) {
                return;
            }
            auto const current = _entry.peek();
            if (!current || _waiting.peek() || current->pending.peek()) {
                return;
            }
            _scope->core.runtime().batch([&] { _scope->core.session.fetch(current, channel()); });
        });
    }
}

inline void QueryRuntime::switchTo(Target const& target) {
    Session& session = _scope->core.session;
    std::shared_ptr<CacheEntry> const old = _entry.peek();
    if (old && target == _target) {
        return;
    }
    Value shown = old && old->hasValue.peek() ? old->value.peek() : _carried.peek();
    _target = target;
    if (target.idle) {
        _carried.set(Value{});
        _entry.set(nullptr);
        if (old) {
            session.release(old);
        }
        return;
    }
    auto fresh = session.acquire(target.token, _typeId, _decl->action, _decl->evaluate, target.body);
    _carried.set(std::move(shown));
    _entry.set(fresh);
    if (old) {
        session.release(old);
    }
    bool const young = fresh->fetchedAt.has_value() &&
                       session.now() - *fresh->fetchedAt < std::chrono::milliseconds{_decl->staleTime};
    if (!fresh->pending.peek() && (!fresh->hasValue.peek() || fresh->stale || !young)) {
        session.fetch(fresh, channel());
    }
}

inline void QueryRuntime::refetch() {
    _scope->core.runtime().batch([this] {
        bool switched = false;
        if (_waiting.peek()) {
            _debounce.cancel();
            _waiting.set(false);
            switchTo(_pendingTarget);
            switched = true;
        }
        auto const current = _entry.peek();
        if (current && !(switched && current->pending.peek())) {
            _scope->core.session.fetch(current, channel());
        }
    });
}

inline void QueryRuntime::refetchCurrent() {
    if (_key == nullptr) {
        refetch();
        return;
    }
    _scope->core.runtime().batch([this] {
        _debounce.cancel();
        _waiting.set(false);
        Target const target = _key->peek();
        if (!(_entry.peek() && target == _target)) {
            switchTo(target);
            return;
        }
        if (auto const current = _entry.peek(); current && !current->pending.peek()) {
            _scope->core.session.fetch(current, channel());
        }
    });
}

inline Value QueryRuntime::value() const {
    auto const& current = _entry.get();
    if (current) {
        if (current->hasValue.get()) {
            return current->value.get();
        }
        if (!current->error.get().empty()) {
            return Value{};
        }
    }
    return _carried.get();
}

inline bool QueryRuntime::pending() const {
    if (_waiting.get()) {
        return true;
    }
    auto const& current = _entry.get();
    return current && current->pending.get();
}

inline Value QueryRuntime::error() const {
    auto const& current = _entry.get();
    if (!current || current->error.get().empty()) {
        return Value{};
    }
    return Value{current->error.get()};
}

inline Value QueryRuntime::errorKind() const {
    auto const& current = _entry.get();
    if (!current || current->errorKind.get().empty()) {
        return Value{};
    }
    return Value{current->errorKind.get()};
}

inline bool QueryRuntime::settled() const {
    auto const& current = _entry.get();
    return !current || current->hasValue.get() || !current->error.get().empty();
}

// --- Mutations -------------------------------------------------------------------------------------------------

inline MutationRuntime::MutationRuntime(MutationDecl const& decl, RtScope& scope, ModelAlias* alias)
    : _decl{&decl},
      _scope{&scope},
      _alias{alias},
      _typeId{alias->decl->type},
      _core{&scope.core},
      _inFlight{scope.core.runtime(), 0},
      _result{scope.core.runtime()},
      _error{scope.core.runtime()},
      _errorKind{scope.core.runtime()} {}

inline MutationRuntime::~MutationRuntime() {
    *_alive = false;
    // A run whose reply will now never be read is no longer pending.
    if (int const running = _inFlight.peek(); running > 0) {
        _core->addPending(-running);
    }
}

// NOLINTBEGIN(misc-no-recursion): a mutation's commands can run another mutation, and a serial queue sends its next
// run as the last one settles.
inline void MutationRuntime::run(Value body, std::shared_ptr<Bindings const> bindings, RunDone done) {
    auto run = std::make_shared<Run>();
    run->body = std::move(body);
    run->bindings = std::move(bindings);
    run->done = std::move(done);
    if (_decl->concurrency == Concurrency::Exclusive && _inFlight.peek() > 0) {
        run->done(false, Value{});
        return;
    }
    _inFlight.set(_inFlight.peek() + 1);
    _core->addPending(1);
    if (_decl->concurrency == Concurrency::Serial) {
        // The key reads the run's body, so a run keys the same whichever command started it.
        Bindings withBody = *run->bindings;
        withBody.push_back(Binding{.name = "body", .read = [body = run->body] { return body; }});
        Value const key = evalIn(*_scope, &withBody, _decl->serialKey);
        run->serialKey = key.isNull() ? std::string{} : canonicalJson(key);
        if (_busy.contains(run->serialKey)) {
            _queues[run->serialKey].push_back(run);
            return;
        }
        _busy.insert(run->serialKey);
    }
    send(run);
}

inline void MutationRuntime::send(std::shared_ptr<Run> const& run) {
    run->id = ++_runs;
    _latest = run->id;
    applyOptimistic(*run);
    std::shared_ptr<ModelChannel> const channel = channelOf(*_alias, _scope->core);
    if (channel == nullptr) {
        settle(run, std::nullopt, std::make_exception_ptr(std::runtime_error{"the model alias has no instance"}));
        return;
    }
    std::weak_ptr<bool> const alive = _alive;
    try {
        auto completion = channel->execute(_decl->action, canonicalJson(run->body),
                                           CallOptions{.queueOffline = _decl->offlineQueue});
        completion
            .then([this, alive, run](std::string const& reply) {
                if (alive.expired()) {
                    return;
                }
                auto json = parseJson(reply);
                auto value = json ? valueFromJson(*json) : std::unexpected(json.error());
                if (!value) {
                    settle(run, std::nullopt,
                           std::make_exception_ptr(
                               std::runtime_error{"the reply is not a document value: " + value.error()}));
                    return;
                }
                settle(run, std::move(*value), nullptr);
            })
            .onError([this, alive, run](std::exception_ptr const& error) {
                if (!alive.expired()) {
                    settle(run, std::nullopt, error);
                }
            });
    } catch (...) {
        settle(run, std::nullopt, std::current_exception());
    }
}

inline void MutationRuntime::applyOptimistic(Run& run) {
    if (!_decl->optimistic) {
        return;
    }
    NameRef const* const target = _scope->find(_decl->optimistic->query);
    if (target == nullptr || target->category != NameCategory::Query) {
        return;
    }
    auto entry = _scope->core.runtime().untracked([&] { return target->query->entry(); });
    if (!entry) {
        return;
    }
    Bindings bindings = *run.bindings;
    bindings.push_back(Binding{.name = "body", .read = [body = run.body] { return body; }});
    Value key = _scope->core.runtime().untracked([&] { return evalIn(*_scope, &bindings, _decl->optimistic->key); });
    Value with = _scope->core.runtime().untracked([&] { return evalIn(*_scope, &bindings, _decl->optimistic->with); });
    // Before a value has arrived there is no list to miss.
    if (entry->hasValue.peek() &&
        patchRows(entry->confirmed, _decl->optimistic->at, key, with).error_or(PatchMiss::NoRow) ==
            PatchMiss::NoList) {
        _scope->core.report(pointerAppend(_decl->path, "optimistic"),
                            patchMissReason(_decl->optimistic->query, _decl->optimistic->at));
    }
    entry->layers.push_back(
        OptimisticLayer{.run = run.id, .at = _decl->optimistic->at, .key = std::move(key), .with = std::move(with)});
    _scope->core.session.showLayers(*entry);
    run.layered.push_back(std::move(entry));
}

inline void MutationRuntime::dropLayers(Run& run) {
    for (auto const& entry : run.layered) {
        std::erase_if(entry->layers, [&](OptimisticLayer const& layer) { return layer.run == run.id; });
        _scope->core.session.showLayers(*entry);
    }
    run.layered.clear();
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one branch per outcome a reply can have.
inline void MutationRuntime::settle(std::shared_ptr<Run> const& run, std::optional<Value> reply,
                                    std::exception_ptr const& error) {
    ScreenCore& core = _scope->core;
    Session& session = core.session;
    bool const applied = _decl->concurrency != Concurrency::Latest || run->id == _latest;
    std::shared_ptr<RtScope> const scope = _scope->shared_from_this();
    auto commandCtx = [&](Bindings extra) {
        return CommandCtx{.scope = scope,
                          .bindings = std::make_shared<Bindings>(std::move(extra)),
                          .cancelClose = std::make_shared<bool>(false),
                          .boundary = nullptr};
    };
    Value delivered;
    bool succeeded = false;
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one branch per outcome a reply can have.
    core.runtime().batch([&] {
        _inFlight.set(_inFlight.peek() - 1);
        _core->addPending(-1);
        if (reply) {
            succeeded = true;
            // The run's optimistic writes are confirmed by the reply: they become part of the confirmed rows.
            for (auto const& entry : run->layered) {
                Value confirmed = entry->confirmed;
                for (OptimisticLayer const& layer : entry->layers) {
                    if (layer.run == run->id) {
                        if (auto patched = patchRows(confirmed, layer.at, layer.key, layer.with)) {
                            confirmed = std::move(*patched);
                        }
                    }
                }
                std::erase_if(entry->layers, [&](OptimisticLayer const& layer) { return layer.run == run->id; });
                session.confirm(*entry, confirmed);
            }
            run->layered.clear();
            std::vector<std::string> secret;
            if (session.options().catalog.action) {
                if (auto const info = session.options().catalog.action(_decl->action)) {
                    secret = info->secretFields;
                }
            }
            delivered = withoutFields(*reply, secret);
            std::string const token =
                _alias == nullptr ? std::string{} : core.runtime().untracked([&] { return _alias->token->get(); });
            if (!applied) {
                session.actionSucceeded(_typeId, _decl->action, *reply, ReplyUse::Discarded, token);
                return;
            }
            _result.set(*reply);
            _error.set(Value{});
            _errorKind.set(Value{});
            session.actionSucceeded(_typeId, _decl->action, *reply, ReplyUse::Applied, token);
            core.runList(_decl->onSuccess, commandCtx(*run->bindings), {});
            if (!secret.empty()) {
                _result.set(delivered);
            }
            return;
        }
        dropLayers(*run);
        std::string const kind = session.classify(error);
        if (applied) {
            _error.set(Value{failureText(error)});
            _errorKind.set(Value{kind});
            core.runList(_decl->onError, commandCtx(*run->bindings), {});
        }
        if (kind == "denied" && core.host != nullptr) {
            core.host->permissionsStale();
        }
    });
    if (_decl->concurrency == Concurrency::Serial) {
        next(run->serialKey);
    }
    run->done(succeeded, delivered);
}

inline void MutationRuntime::next(std::string const& serialKey) {
    auto const found = _queues.find(serialKey);
    if (found == _queues.end() || found->second.empty()) {
        _queues.erase(serialKey);
        _busy.erase(serialKey);
        return;
    }
    std::shared_ptr<Run> const queued = found->second.front();
    found->second.pop_front();
    _scope->core.runtime().batch([&] { send(queued); });
}
// NOLINTEND(misc-no-recursion)

}  // namespace detail

namespace detail {

// --- Declaring scopes ------------------------------------------------------------------------------------------

/// @brief The element a node of a scope keeps its state in.
/// @param scope The scope that declares it.
/// @param node  The node.
/// @return The element, or `nullptr` for a node that keeps none.
[[nodiscard]] inline ElementState* elementFor(RtScope const& scope, DocNode const& node) {
    for (auto const& element : scope.elements) {
        if (element->node == &node) {
            return element.get();
        }
    }
    return nullptr;
}

/// @brief The transfers of the `upload` commands in a list, nested lists included.
/// @param commands The list.
/// @param out      Where the transfer ids go.
// NOLINTNEXTLINE(misc-no-recursion): one level per nested command list
inline void transfersOf(Commands const& commands, std::vector<std::string>& out) {
    for (Command const& command : commands) {
        if (command.kind == CommandKind::Upload && !command.transfer.empty()) {
            out.push_back(command.transfer);
        }
        transfersOf(command.commands, out);
        transfersOf(command.alternative, out);
    }
}

// NOLINTBEGIN(misc-no-recursion): a document is a tree of nodes and parts, and collecting descends it.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per kind of node and part.
inline void ScreenCore::collect(DocNode const& node, RtScope& scope, std::vector<Commands const*>& commands) {
    KindSpec const* const spec = findKind(node.kind, false);
    if (!node.id.empty() || statefulKind(node.kind)) {
        auto element = std::make_unique<ElementState>(runtime());
        element->node = &node;
        element->scope = &scope;
        if (!node.id.empty()) {
            NameRef name;
            name.category = NameCategory::Element;
            name.element = element.get();
            scope.names.insert_or_assign(node.id, name);
        }
        wireErrors(*element);
        scope.elements.push_back(std::move(element));
    }
    bool const createsScope = spec != nullptr && (spec->flags & kScope) != 0;
    for (auto const& [key, list] : node.events) {
        KeySpec const* const keyed = spec == nullptr ? nullptr : keySpec(*spec, key);
        if (!createsScope || keyed == nullptr || (keyed->flags & kInner) == 0) {
            commands.push_back(&list);
        }
    }
    for (auto const& [chord, list] : node.keys) {
        static_cast<void>(chord);
        commands.push_back(&list);
    }
    for (auto const& [key, nodes] : node.children) {
        KeySpec const* const keyed = spec == nullptr ? nullptr : keySpec(*spec, key);
        if (createsScope && keyed != nullptr && (keyed->flags & kInner) != 0) {
            continue;
        }
        for (DocNodePtr const& child : nodes) {
            collect(*child, scope, commands);
        }
    }
    for (auto const& [key, parts] : node.parts) {
        static_cast<void>(key);
        for (DocNodePtr const& part : parts) {
            KindSpec const* const partSpec = findKind(part->kind, true);
            if (partSpec != nullptr && (partSpec->flags & kScope) != 0) {
                continue;
            }
            for (auto const& [event, list] : part->events) {
                static_cast<void>(event);
                commands.push_back(&list);
            }
            for (auto const& [childKey, nodes] : part->children) {
                KeySpec const* const keyed = partSpec == nullptr ? nullptr : keySpec(*partSpec, childKey);
                if (keyed != nullptr && (keyed->flags & kInner) != 0) {
                    continue;
                }
                for (DocNodePtr const& child : nodes) {
                    collect(*child, scope, commands);
                }
            }
        }
    }
}
// NOLINTEND(misc-no-recursion)

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one section per kind of declaration.
inline void ScreenCore::declare(RtScope& scope, ScopeDecl const* decl, std::vector<DocNode const*> const& content,
                                std::vector<Commands const*> const& commands) {
    std::vector<Commands const*> lists = commands;
    for (DocNode const* const node : content) {
        collect(*node, scope, lists);
    }
    std::vector<std::string> transferIds;
    for (Commands const* const list : lists) {
        transfersOf(*list, transferIds);
    }
    for (std::string const& transferId : transferIds) {
        if (scope.names.contains(transferId)) {
            continue;
        }
        scope.transfers.push_back(std::make_unique<TransferState>(runtime()));
        NameRef name;
        name.category = NameCategory::Transfer;
        name.transfer = scope.transfers.back().get();
        scope.names.insert_or_assign(transferId, name);
    }
    if (decl != nullptr) {
        for (ModelDecl const& model : decl->models) {
            auto alias = std::make_unique<ModelAlias>();
            alias->decl = &model;
            alias->mode = model.instance;
            if (alias->mode == InstanceMode::Default) {
                std::optional<ModelInfo> const info =
                    session.options().catalog.model ? session.options().catalog.model(model.type) : std::nullopt;
                alias->mode = info && info->stateless ? InstanceMode::Shared : InstanceMode::Private;
            }
            ModelAlias* const raw = alias.get();
            switch (raw->mode) {
                case InstanceMode::Keyed:
                    raw->token = &scope.nodes.computed([&scope, raw]() -> std::string {
                        std::string const key = instanceKeyText(evalIn(scope, nullptr, raw->decl->key));
                        return key.empty() ? std::string{} : "shared:" + key;
                    });
                    break;
                case InstanceMode::Shared:
                    raw->token = &scope.nodes.computed([] { return std::string{"shared"}; });
                    break;
                case InstanceMode::Borrowed: {
                    auto const found = borrowed.find(model.name);
                    std::string token = found == borrowed.end() ? std::string{} : found->second.token;
                    raw->token = &scope.nodes.computed([token = std::move(token)] { return token; });
                    break;
                }
                case InstanceMode::Private:
                case InstanceMode::Default:
                default: {
                    std::string token = session.privateToken();
                    raw->token = &scope.nodes.computed([token = std::move(token)] { return token; });
                    break;
                }
            }
            NameRef name;
            name.category = NameCategory::Model;
            name.model = raw;
            scope.names.insert_or_assign(model.name, name);
            scope.models.push_back(std::move(alias));
        }
        for (LetDecl const& let : decl->lets) {
            NameRef name;
            name.category = NameCategory::Let;
            name.letExpr = let.value;
            name.let = &scope.nodes.computed([&scope, expr = let.value] { return evalIn(scope, nullptr, expr); });
            scope.names.insert_or_assign(let.name, name);
        }
        for (StateDecl const& state : decl->state) {
            auto slot = std::make_unique<StateSlot>();
            slot->decl = &state;
            slot->scope = &scope;
            NameRef name;
            name.category = NameCategory::State;
            name.state = slot.get();
            scope.names.insert_or_assign(state.name, name);
            scope.states.push_back(std::move(slot));
        }
        auto aliasOf = [&scope](std::string const& model) -> ModelAlias* {
            NameRef const* const found = scope.find(model);
            return found != nullptr && found->category == NameCategory::Model ? found->model : nullptr;
        };
        for (QueryDecl const& query : decl->queries) {
            scope.queries.push_back(std::make_unique<QueryRuntime>(query, scope, aliasOf(query.model)));
            NameRef name;
            name.category = NameCategory::Query;
            name.query = scope.queries.back().get();
            scope.names.insert_or_assign(query.name, name);
        }
        for (MutationDecl const& mutation : decl->mutations) {
            scope.mutations.push_back(std::make_unique<MutationRuntime>(mutation, scope, aliasOf(mutation.model)));
            NameRef name;
            name.category = NameCategory::Mutation;
            name.mutation = scope.mutations.back().get();
            scope.names.insert_or_assign(mutation.name, name);
        }
    }
    for (auto const& slot : scope.states) {
        initState(scope, *slot);
    }
    for (auto const& element : scope.elements) {
        wireElement(*element);
    }
    if (decl == nullptr) {
        return;
    }
    for (CheckDecl const& check : decl->checks) {
        auto made = std::make_unique<CheckRuntime>();
        made->decl = &check;
        made->failing = &scope.nodes.computed([&scope, expr = check.assertion] {
            Value const asserted = evalIn(scope, nullptr, expr);
            bool const* const flag = asserted.asBool();
            return flag != nullptr && !*flag;
        });
        made->message = &scope.nodes.computed([&scope, expr = check.message, session = &session] {
            return textOf(evalIn(scope, nullptr, expr), session->formatting());
        });
        for (std::string const& target : check.targets) {
            NameRef const* const found = scope.find(target);
            if (found != nullptr && found->category == NameCategory::Element) {
                found->element->checks.push_back(made.get());
                made->targets.push_back(found->element);
                found->element->checksVersion.set(found->element->checksVersion.peek() + 1);
            }
        }
        scope.checks.push_back(std::move(made));
    }
    for (auto const& query : scope.queries) {
        query->start();
    }
    for (WatchDecl const& watch : decl->watches) {
        auto& source = scope.nodes.computed([&scope, expr = watch.on] { return evalIn(scope, nullptr, expr); });
        scope.nodes.effect([this, &scope, &source, watched = &watch, first = true]() mutable {
            static_cast<void>(source.get());
            if (first) {
                first = false;
                return;
            }
            bool const user = session.lastWriteByUser();
            if (watched->source == WatchSource::User && !user) {
                return;
            }
            runtime().untracked([&] {
                CommandCtx const ctx{.scope = scope.shared_from_this(),
                                     .bindings = std::make_shared<Bindings>(),
                                     .cancelClose = std::make_shared<bool>(false),
                                     .boundary = nullptr};
                if (user) {
                    session.asUser([&] { runList(watched->body, ctx, {}); });
                } else {
                    runList(watched->body, ctx, {});
                }
            });
        });
    }
}

/// @brief What a date input reads from a value: the instant it shows, and whether the value names one at all.
struct DateReading {
    /// @brief The instant shown; empty for an empty field.
    std::optional<time::DateTime> instant;
    /// @brief Whether the value names a date, an instant or nothing; `false` for text that names no date.
    bool readable = true;
};

/// @brief Reads what a date input shows for @p value, in a display zone @p offsetMinutes from UTC.
///
/// The value is read as the date functions read it (`readDateValue`): a timestamp is its instant; a `yyyy-MM-dd`
/// string is midnight of that date in the display zone; a string `DateTime::fromIso8601` reads is its instant; `null`
/// and `""` are empty. Any other value is empty and unreadable.
/// @param value         The value.
/// @param offsetMinutes The display zone's offset from UTC, in minutes.
/// @return The instant, or nothing, and whether the value was readable.
[[nodiscard]] inline DateReading dateInstantOf(Value const& value, std::int32_t offsetMinutes) {
    DateValue const reading = readDateValue(value);
    switch (reading.form) {
        case DateForm::Absent:
            return {};
        case DateForm::Date:
            return {.instant =
                        time::DateTime{std::chrono::sys_days{reading.date} - std::chrono::minutes{offsetMinutes}}};
        case DateForm::Timestamp:
        case DateForm::InstantText:
            return {.instant = reading.instant};
        case DateForm::Unreadable:
        default:
            return {.instant = std::nullopt, .readable = false};
    }
}

/// @brief A date input's date text for @p instant in a display zone @p offsetMinutes from UTC.
/// @param instant       The instant.
/// @param offsetMinutes The display zone's offset from UTC, in minutes.
/// @return `yyyy-MM-dd`.
[[nodiscard]] inline std::string dateTextOf(time::DateTime const& instant, std::int32_t offsetMinutes) {
    auto const local = instant.value + std::chrono::minutes{offsetMinutes};
    std::chrono::year_month_day const date{std::chrono::floor<std::chrono::days>(local)};
    return std::format("{:04}-{:02}-{:02}", static_cast<int>(date.year()), static_cast<unsigned>(date.month()),
                       static_cast<unsigned>(date.day()));
}

/// @brief Whether two values show the same in an input: by text for a text input, by instant for a date input, by
///        value otherwise.
/// @param kind   The input's kind.
/// @param lhs    One value.
/// @param rhs    The other.
/// @param format The formatting facts.
/// @return It.
[[nodiscard]] inline bool sameShown(std::string_view kind, Value const& lhs, Value const& rhs,
                                    FormatContext const& format) {
    if (kind == "textInput" || kind == "filePicker") {
        return textOf(lhs, format) == textOf(rhs, format);
    }
    if (kind == "dateTimeInput") {
        return dateInstantOf(lhs, format.utcOffsetMinutes).instant ==
               dateInstantOf(rhs, format.utcOffsetMinutes).instant;
    }
    return lhs == rhs;
}

/// @brief The key of a select option: a record's `key`, `id` or `value` field, or the option itself.
/// @param option The option.
/// @return The key.
[[nodiscard]] inline Value optionKey(Value const& option) {
    if (option.asRecord() == nullptr) {
        return option;
    }
    for (std::string_view const field : {"key", "id", "value"}) {
        if (Value const* const found = option.field(field)) {
            return *found;
        }
    }
    return Value{};
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one rule per source of an error.
inline void ScreenCore::wireErrors(ElementState& element) {
    RtScope& scope = *element.scope;
    element.errors = &scope.nodes.computed([this, &element]() -> Value {
        static_cast<void>(element.checksVersion.get());
        DocNode const& field = *element.node;
        RtScope const& owner = *element.scope;
        std::vector<std::string> errors;
        if (ExprPtr const required = field.prop("required")) {
            Value const requiredNow = evalIn(owner, nullptr, required);
            bool const* const flag = requiredNow.asBool();
            if (flag != nullptr && *flag && !engaged(inputValue(element))) {
                errors.emplace_back("Required");
            }
        }
        for (std::string& text : stringsOf(evalIn(owner, nullptr, field.prop("errors")))) {
            errors.push_back(std::move(text));
        }
        if (!field.id.empty()) {
            std::vector<std::string> const path{"errors", field.id};
            for (auto const& query : owner.queries) {
                for (std::string& text : stringsOf(walk(query->value(), path))) {
                    errors.push_back(std::move(text));
                }
            }
            for (auto const& mutation : owner.mutations) {
                for (std::string& text : stringsOf(walk(mutation->result(), path))) {
                    errors.push_back(std::move(text));
                }
            }
        }
        for (CheckRuntime const* const check : element.checks) {
            if (check->failing->get()) {
                errors.push_back(check->message->get());
            }
        }
        return listOfStrings(errors);
    });
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per kind of element.
inline void ScreenCore::wireElement(ElementState& element) {
    DocNode const& node = *element.node;
    RtScope& scope = *element.scope;
    std::string_view const kind = node.kind;
    if (kind == "form") {
        wireForm(element);
        return;
    }
    if (kind == "collection" || kind == "wizard") {
        wireComposite(element);
        return;
    }
    bool const input = kind == "textInput" || kind == "checkbox" || kind == "select" || kind == "dateTimeInput" ||
                       kind == "slider" || kind == "filePicker";
    if (input) {
        std::string_view key = "value";
        if (kind == "checkbox") {
            key = "checked";
        } else if (kind == "select") {
            key = "selected";
        }
        ExprPtr const expr = kind == "filePicker" ? nullptr : node.prop(key);
        bool const controlled = expr && (expr->kind != ExprKind::Literal || !expr->literal.isNull());
        if (controlled) {
            element.bound = &scope.nodes.computed([&scope, expr] { return evalIn(scope, nullptr, expr); });
            element.draft.set(runtime().untracked([&] { return element.bound->get(); }));
            scope.nodes.effect([this, &element, kind] {
                Value const bound = element.bound->get();
                runtime().untracked([&] {
                    if (!element.edited.peek()) {
                        element.draft.set(bound);
                    } else if (sameShown(kind, element.draft.peek(), bound, session.formatting())) {
                        element.edited.set(false);
                    }
                });
            });
        } else if (kind == "checkbox") {
            element.draft.set(false);
        }
    }
    if (kind == "select") {
        element.rows =
            &scope.nodes.computed([&scope, expr = node.prop("options")] { return evalIn(scope, nullptr, expr); });
        if (element.bound == nullptr && node.name("onStale") == "clear") {
            scope.nodes.effect([this, &element] {
                Value const options = element.rows->get();
                runtime().untracked([&] {
                    Value const selected = element.draft.peek();
                    if (selected.isNull()) {
                        return;
                    }
                    ValueList const* const items = options.asList();
                    bool const kept = items != nullptr && std::ranges::any_of(*items, [&](Value const& option) {
                                          return optionKey(option) == selected;
                                      });
                    if (!kept) {
                        session.noteWrite();
                        element.draft.set(Value{});
                    }
                });
            });
        }
    }
    if (kind == "table" && node.prop("rows")) {
        element.rows =
            &scope.nodes.computed([&scope, expr = node.prop("rows")] { return evalIn(scope, nullptr, expr); });
        // A bound selection is controlled, as a select's `selected` is: any expression but a literal `null` binds it.
        ExprPtr const selection = node.prop("selection");
        if (selection && (selection->kind != ExprKind::Literal || !selection->literal.isNull())) {
            element.bound = &scope.nodes.computed([&scope, selection] { return evalIn(scope, nullptr, selection); });
        }
        // A bound sort is controlled as the selection is.
        ExprPtr const sort = node.prop("sort");
        if (sort && (sort->kind != ExprKind::Literal || !sort->literal.isNull())) {
            element.sortBound = &scope.nodes.computed([&scope, sort] { return evalIn(scope, nullptr, sort); });
        }
        element.view = &scope.nodes.computed([this, &element] { return tableView(element, nullptr); });
    }
}

/// @brief A table's sort as its element value reads it: the bound `sort` when the key binds one, else the table's
///        own.
/// @param element The table's element.
/// @return The sort. Tracked.
[[nodiscard]] inline Value tableSort(ElementState const& element) {
    return element.sortBound != nullptr ? element.sortBound->get() : element.sort.get();
}

/// @brief A table's page values: `viewCount` (the rows it orders, every page), `page`, `pageCount`, and
///        `pageFirst` and `pageLast`, the 1-based positions of the page's first and last rows, both 0 when there are
///        none. A table that does not page reads page 0 of 1, over every row.
/// @param element The table's element.
/// @param name    The value's name.
/// @return The value. Tracked.
[[nodiscard]] inline Value tablePageValue(ElementState const& element, std::string_view name) {
    if (element.view == nullptr) {
        return name == "pageCount" ? std::int64_t{1} : std::int64_t{0};
    }
    TableView const& view = element.view->get();
    if (name == "viewCount") {
        return static_cast<std::int64_t>(view.rows.size());
    }
    if (name == "page") {
        return view.page;
    }
    if (name == "pageCount") {
        return view.pageCount;
    }
    bool const none = view.first == view.last;
    if (name == "pageFirst") {
        return none ? std::int64_t{0} : static_cast<std::int64_t>(view.first + 1);
    }
    return static_cast<std::int64_t>(view.last);
}

/// @brief The selected keys of the rows a table holds on pages other than the one shown, in their order: what an
///        `Extend` report keeps, since the backend holds the shown page alone.
/// @param view      The table's rows and page.
/// @param selection The selection, a list of keys.
/// @param rowKey    A row's key.
/// @return The keys.
[[nodiscard]] inline ValueList selectedOffPage(TableView const& view, Value const& selection,
                                               std::function<Key(Value const&)> const& rowKey) {
    std::unordered_set<Key> onPage;
    std::unordered_set<Key> offPage;
    for (std::size_t index = 0; index < view.rows.size(); ++index) {
        (index >= view.first && index < view.last ? onPage : offPage).insert(rowKey(view.rows.at(index)));
    }
    ValueList kept;
    if (ValueList const* const selected = selection.asList()) {
        for (Value const& item : *selected) {
            Key const key = keyOf(item);
            if (offPage.contains(key) && !onPage.contains(key)) {
                kept.push_back(item);
            }
        }
    }
    return kept;
}

/// @brief The sortable column part of a table with the id @p columnId.
/// @param table    The table.
/// @param columnId The column's id.
/// @return The part, or `nullptr` when no sortable column has that id.
[[nodiscard]] inline DocNode const* sortableColumn(DocNode const& table, std::string_view columnId) {
    for (DocNodePtr const& column : table.partList("columns")) {
        if (column->id == columnId && column->flag("sortable")) {
            return column.get();
        }
    }
    return nullptr;
}

/// @brief One key of a table's sort: the column part that orders the rows, and whether it is descending.
struct SortKey {
    /// @brief The sortable column.
    DocNode const* column = nullptr;
    /// @brief Whether the key sorts descending.
    bool descending = false;
};

/// @brief A table's sort read as its keys: a list of `{"column", "direction"}` records naming sortable columns.
/// @param table The table.
/// @param sort  The sort; `null` is the source order, as `[]` is.
/// @return The keys, most significant first; `nullopt` when @p sort is not such a list.
[[nodiscard]] inline std::optional<std::vector<SortKey>> sortKeysOf(DocNode const& table, Value const& sort) {
    std::vector<SortKey> keys;
    if (sort.isNull()) {
        return keys;
    }
    ValueList const* const items = sort.asList();
    if (items == nullptr) {
        return std::nullopt;
    }
    for (Value const& item : *items) {
        Value const* const column = item.field("column");
        Value const* const direction = item.field("direction");
        std::string const* const columnId = column == nullptr ? nullptr : column->asString();
        std::string const* const way = direction == nullptr ? nullptr : direction->asString();
        DocNode const* const part = columnId == nullptr ? nullptr : sortableColumn(table, *columnId);
        if (part == nullptr || way == nullptr || (*way != "asc" && *way != "desc")) {
            return std::nullopt;
        }
        keys.push_back(SortKey{.column = part, .descending = *way == "desc"});
    }
    return keys;
}

inline void ScreenCore::groupRows(ElementState const& element, Bindings const* bindings, TableView& view) {
    auto const parts = element.node->partList("group");
    if (parts.empty()) {
        return;
    }
    ExprPtr const groupBy = parts.front()->prop("by");
    RtScope const& scope = *element.scope;
    ScopeEnv const tableEnv{scope, bindings};
    Evaluator runs{tableEnv};
    // A heading's key is its run's key and its ordinal among the runs with an equal key, since adjacent grouping
    // can give two runs one key (samples A, B, A); the ordinal stays put across a sort, which moves no run.
    std::map<std::string, std::size_t, std::less<>> ordinals;
    Value key;
    ValueList run;
    auto const close = [&] {
        std::string const json = canonicalJson(key);
        Value const record = runRecord(key, std::exchange(run, {}));
        ValueRecord entry = *record.asRecord();
        entry.insert_or_assign(std::string{kGroupHeadingField},
                               Value{"\x1Fgroup:" + json + ":" + std::to_string(ordinals[json]++)});
        view.runs.emplace_back(std::move(entry));
    };
    for (Value const& row : view.rows) {
        Value value = evaluate(*groupBy, ScopeEnv{scope, bindings, &row});
        if (!run.empty() && !runs.sameRun(key, value)) {
            close();
        }
        if (run.empty()) {
            key = std::move(value);
        }
        run.push_back(row);
        view.runOf.push_back(view.runs.size());
    }
    if (!run.empty()) {
        close();
    }
}

inline void ScreenCore::sortedRows(ElementState const& element, Bindings const* bindings, TableView& view) {
    DocNode const& node = *element.node;
    RtScope const& scope = *element.scope;
    Value const source = bindings == nullptr ? element.rows->get() : evalIn(scope, bindings, node.prop("rows"));
    ValueList const* const items = source.asList();
    if (items == nullptr) {
        return;
    }
    view.rows.assign(items->begin(), items->end());
    groupRows(element, bindings, view);
    Value const sort = tableSort(element);
    std::optional<std::vector<SortKey>> const keys = sortKeysOf(node, sort);
    std::string const sortPath = pointerAppend(node.path, "sort");
    if (!keys) {
        report(sortPath, "sort is " + canonicalJson(sort) +
                             R"(; a table's sort is a list of {"column": <a sortable column's id>, "direction": "asc" or "desc"})");
        return;
    }
    if (keys->empty()) {
        return;
    }
    // Each row's keys are read once, then the rows are ordered by them: a change to the sort, the rows, or anything
    // a `sortBy` reads sorts again. A grouped table orders by the group first, so every group keeps its place in
    // the source order and its rows are sorted within it.
    bool const grouped = !view.runOf.empty();
    std::vector<std::vector<Value>> rowKeys;
    rowKeys.reserve(view.rows.size());
    for (std::size_t index = 0; index < view.rows.size(); ++index) {
        Value const& row = view.rows.at(index);
        ScopeEnv const env{scope, bindings, &row};
        std::vector<Value> values;
        values.reserve(keys->size() + 1);
        if (grouped) {
            values.emplace_back(static_cast<std::int64_t>(view.runOf.at(index)));
        }
        for (SortKey const& key : *keys) {
            if (ExprPtr const sortBy = key.column->prop("sortBy")) {
                values.push_back(evaluate(*sortBy, env));
            } else {
                Value const* const field = row.field(key.column->id);
                values.push_back(field == nullptr ? Value{} : *field);
            }
        }
        rowKeys.push_back(std::move(values));
    }
    std::vector<bool> descending;
    descending.reserve(keys->size() + 1);
    if (grouped) {
        descending.push_back(false);
    }
    for (SortKey const& key : *keys) {
        descending.push_back(key.descending);
    }
    ScopeEnv const env{scope, bindings};
    std::optional<std::vector<std::size_t>> const order = Evaluator{env}.sortRows(rowKeys, descending);
    if (!order) {
        report(sortPath, "the rows' values for the sort are not of one comparable kind; the rows keep their order");
        return;
    }
    std::vector<Value> sorted;
    std::vector<std::size_t> runOf;
    sorted.reserve(view.rows.size());
    for (std::size_t const index : *order) {
        sorted.push_back(view.rows.at(index));
        if (grouped) {
            runOf.push_back(view.runOf.at(index));
        }
    }
    view.rows = std::move(sorted);
    view.runOf = std::move(runOf);
}

inline TableView ScreenCore::tableView(ElementState const& element, Bindings const* bindings) {
    DocNode const& node = *element.node;
    RtScope const& scope = *element.scope;
    TableView view;
    sortedRows(element, bindings, view);
    view.last = view.rows.size();
    ExprPtr const sizeExpr = node.prop("pageSize");
    if (!sizeExpr) {
        return view;
    }
    // A `pageSize` of `null` does not page; one that is no positive integer is reported and does not page either.
    Value const size = evalIn(scope, bindings, sizeExpr);
    std::int64_t const* const perPage = size.asInt();
    if (size.isNull() || perPage == nullptr || *perPage < 1) {
        if (!size.isNull()) {
            report(pointerAppend(node.path, "pageSize"),
                   "pageSize is " + canonicalJson(size) + "; a page holds a positive whole number of rows");
        }
        return view;
    }
    // A `page` of `null` is the first; one that is negative or not a whole number is reported and shows the first.
    // One past the last is the last: a filter that shrinks the rows under the user is normal, not an error.
    std::int64_t asked = 0;
    if (ExprPtr const pageExpr = node.prop("page")) {
        Value const page = evalIn(scope, bindings, pageExpr);
        std::int64_t const* const index = page.asInt();
        if (index != nullptr && *index >= 0) {
            asked = *index;
        } else if (!page.isNull()) {
            report(pointerAppend(node.path, "page"),
                   "page is " + canonicalJson(page) + "; a page is a whole number from 0");
        }
    }
    auto const total = static_cast<std::int64_t>(view.rows.size());
    view.pageCount = std::max<std::int64_t>(1, (total + *perPage - 1) / *perPage);
    view.page = std::min(asked, view.pageCount - 1);
    view.first = static_cast<std::size_t>(std::min(total, view.page * *perPage));
    view.last = static_cast<std::size_t>(std::min(total, (view.page + 1) * *perPage));
    return view;
}

/// @brief A table's selection as its element value reads it: the bound `selection` when the key binds one, else
///        the table's own. A bound value that is not a list (`null` while state starts empty) selects nothing.
/// @param element The table's element.
/// @return A list of keys. Tracked.
[[nodiscard]] inline Value tableSelection(ElementState const& element) {
    if (element.bound == nullptr) {
        return element.selection.get();
    }
    Value const bound = element.bound->get();
    return bound.asList() != nullptr ? bound : Value{ValueList{}};
}

/// @brief A JSON text as a document value; `null` when it is not JSON or holds what a value cannot (a fraction).
/// @param text The JSON text.
/// @return The value.
[[nodiscard]] inline Value jsonTextValue(std::string_view text) {
    auto const json = parseJson(text);
    if (!json) {
        return Value{};
    }
    auto value = valueFromJson(*json);
    return value ? *std::move(value) : Value{};
}

/// @brief One field of a form as a record: `value` (the encoded value, `null` while blank or invalid), `draft` (the
///        text), `valid`, `required`, `visible`, `readonly` and `errors`. Tracked.
/// @param field The field.
/// @return The record.
[[nodiscard]] inline Value formFieldValue(forms::FieldState const& field) {
    ValueRecord record;
    forms::Encoding const& encoding = field.encoded().get();
    Value value;
    if (encoding.has_value() && encoding->has_value()) {
        value = (*encoding)->value.isNull() ? jsonTextValue((*encoding)->json) : (*encoding)->value;
    }
    std::optional<forms::FieldError> const& error = field.error().get();
    record.insert_or_assign("value", std::move(value));
    record.insert_or_assign("draft", Value{field.spec().kind == forms::FieldKind::Object ||
                                                   field.spec().kind == forms::FieldKind::ObjectArray
                                               ? std::string{}
                                               : field.draft()});
    record.insert_or_assign("valid", Value{!error.has_value()});
    record.insert_or_assign("required", Value{field.required().get()});
    record.insert_or_assign("visible", Value{field.visible().get()});
    record.insert_or_assign("readonly", Value{field.readonly().get()});
    record.insert_or_assign("errors",
                            error.has_value() ? Value{ValueList{Value{error->message}}} : Value{ValueList{}});
    return Value{std::move(record)};
}

/// @brief A form's element values (spec 5 §6): `ready`, `body` (the record the form would submit, `null` while not
///        ready), `submitted` (whether a submission was sent), `pending`, and each field by its wire name. Tracked.
/// @param element The form's element.
/// @param name    The value's name.
/// @return The value; `null` for a name the form does not have, or a form without a session.
[[nodiscard]] inline Value formValue(ElementState const& element, std::string_view name) {
    forms::FormSession const* const form = element.form.get();
    if (form == nullptr) {
        return Value{};
    }
    if (name == "ready") {
        return form->ready();
    }
    if (name == "body") {
        std::optional<std::string> const body = form->body();
        return body.has_value() ? jsonTextValue(*body) : Value{};
    }
    if (name == "submitted") {
        return form->submitAttempts() > 0;
    }
    if (name == "pending") {
        return form->pending();
    }
    for (auto const& field : form->fields()) {
        if (field->spec().name == name) {
            return formFieldValue(*field);
        }
    }
    return Value{};
}

inline Value ScreenCore::inputValue(ElementState const& element) const {
    std::string_view const kind = element.node->kind;
    if (kind == "checkbox") {
        return elementValue(element, "checked");
    }
    if (kind == "select") {
        return elementValue(element, "selected");
    }
    return elementValue(element, "value");
}

// NOLINTNEXTLINE(misc-no-recursion,readability-function-cognitive-complexity): one case per element value.
inline Value ScreenCore::elementValue(ElementState const& element, std::string_view name) const {
    DocNode const& node = *element.node;
    RtScope const& scope = *element.scope;
    std::string_view const kind = node.kind;
    auto flagOf = [&](std::string_view key, bool fallback) -> Value {
        ExprPtr const expr = node.prop(key);
        if (!expr) {
            return fallback;
        }
        Value const value = evalIn(scope, nullptr, expr);
        bool const* const flag = value.asBool();
        return flag != nullptr && *flag;
    };
    auto errors = [&]() -> Value { return element.errors->get(); };
    auto count = [](Value const& list) -> Value {
        ValueList const* const items = list.asList();
        return static_cast<std::int64_t>(items == nullptr ? 0 : items->size());
    };
    if (name == "visible" || name == "enabled") {
        return flagOf(name, true);
    }
    if (name == "errors") {
        return errors();
    }
    if (name == "valid") {
        Value const current = errors();
        ValueList const* const items = current.asList();
        return items == nullptr || items->empty();
    }
    if (name == "shownErrors") {
        RtScope const& policy = scope.policyScope();
        std::string const mode = policy.showErrors.empty() ? _doc->showErrors : policy.showErrors;
        bool const submittedNow = policy.submitted != nullptr && policy.submitted->get();
        if (mode == "always" || (mode == "submit" && submittedNow) ||
            (mode != "submit" && (submittedNow || element.touched.get()))) {
            return errors();
        }
        return Value{ValueList{}};
    }
    if (kind == "dialog" || kind == "drawer") {
        if (name == "open") {
            return node.prop("open") ? flagOf("open", false) : Value{element.open.get()};
        }
        if (name == "ready") {
            RtScope const* const inner = element.inner.get();
            return inner != nullptr && inner->ready();
        }
        if (name == "submitted") {
            return element.submitted.get();
        }
        return Value{};
    }
    if (kind == "table") {
        if (name == "selection") {
            return tableSelection(element);
        }
        if (name == "activeKey") {
            return element.activeKey.get();
        }
        if (name == "selectedCount") {
            return count(tableSelection(element));
        }
        if (name == "sort") {
            return tableSort(element);
        }
        if (name == "viewCount" || name == "page" || name == "pageCount" || name == "pageFirst" ||
            name == "pageLast") {
            return tablePageValue(element, name);
        }
        if (name == "sourceCount") {
            return element.rows == nullptr ? Value{std::int64_t{0}} : count(element.rows->get());
        }
        ExprPtr const rows = node.prop("rows");
        if ((name == "pending" || name == "error") && rows && rows->kind == ExprKind::Query) {
            ScopeEnv const env{scope, nullptr};
            return env.control(name == "pending" ? ExprKind::Pending : ExprKind::Error, rows->name);
        }
        return name == "pending" ? Value{false} : Value{};
    }
    if (kind == "tabs") {
        // The shown tab's id: the bound one, else the user's own pick, else the first tab's.
        ExprPtr const selected = node.prop("selected");
        if (name != "selected") {
            return Value{};
        }
        if (selected && (selected->kind != ExprKind::Literal || !selected->literal.isNull())) {
            return evalIn(scope, nullptr, selected);
        }
        if (Value picked = element.draft.get(); !picked.isNull()) {
            return picked;
        }
        auto const tabs = node.partList("tabs");
        return tabs.empty() || tabs.front()->id.empty() ? Value{} : Value{tabs.front()->id};
    }
    if (kind == "form") {
        return formValue(element, name);
    }
    if (name == "draft") {
        return element.draft.get();
    }
    if (name == "dirty") {
        return element.edited.get();
    }
    if (name == "focused") {
        return element.focused.get();
    }
    if (name == "touched") {
        return element.touched.get();
    }
    if (name == "checked" || name == "selected") {
        return element.bound != nullptr ? element.bound->get() : element.draft.get();
    }
    if (name == "selectedOption") {
        Value const selected = elementValue(element, "selected");
        ValueList const* const options = element.rows == nullptr ? nullptr : element.rows->get().asList();
        if (options != nullptr && !selected.isNull()) {
            for (Value const& option : *options) {
                if (optionKey(option) == selected) {
                    return option;
                }
            }
        }
        return Value{};
    }
    if (name == "value") {
        if (element.bound != nullptr) {
            return element.bound->get();
        }
        return element.committed.get();
    }
    return Value{};
}

// --- Commands --------------------------------------------------------------------------------------------------

// NOLINTBEGIN(misc-no-recursion): command lists nest
inline void ScreenCore::runList(Commands const& commands, CommandCtx const& ctx, Done done) {
    if (!done) {
        done = [](bool) {};
    }
    if (commands.empty()) {
        done(true);
        return;
    }
    struct Join {
        std::size_t left = 0;
        bool ok = true;
        Done done;
    };
    if (recording) {
        logList(commands);
    }
    auto join = std::make_shared<Join>(Join{.left = commands.size() + 1, .ok = true, .done = std::move(done)});
    auto finish = [join](bool succeeded) {
        join->ok = join->ok && succeeded;
        if (--join->left == 0) {
            join->done(join->ok);
        }
    };
    for (Command const& command : commands) {
        runOne(command, ctx, finish);
    }
    finish(true);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per command of ui/1.
inline void ScreenCore::runOne(Command const& command, CommandCtx const& ctx, Done done) {
    if (!done) {
        done = [](bool) {};
    }
    RtScope const& scope = *ctx.scope;
    auto eval = [&](ExprPtr const& expr) { return evalIn(scope, ctx.bindings.get(), expr); };
    auto finish = [&done](bool succeeded) { done(succeeded); };
    switch (command.kind) {
        case CommandKind::Set:
            runSet(command, ctx, done);
            return;
        case CommandKind::Reset:
            runReset(command, ctx, done);
            return;
        case CommandKind::Run:
            runMutation(command, ctx, std::move(done));
            return;
        case CommandKind::Refetch: {
            // A loaded document's `refetch` names a query in scope (`loadDocument` checks it), as `run` names a
            // mutation and `open` a dialog.
            scope.find(command.target)->query->refetch();
            finish(true);
            return;
        }
        case CommandKind::Open:
        case CommandKind::Close: {
            ElementState* target = nullptr;
            if (command.kind == CommandKind::Close && command.target == "self") {
                // `self` is the innermost dialog or drawer whose content holds the command, found lexically. A
                // component's fallback scope has no parent, so the walk stops at its boundary as its names do.
                RtScope const* walk = &scope;
                while (walk->owner == nullptr && walk->parent) {
                    walk = walk->parent.get();
                }
                target = walk->owner;
                if (target == nullptr && walk != rootScope.lock().get()) {
                    report(command.path, "`close: self` inside a component names no dialog or drawer");
                    finish(false);
                    return;
                }
                if (target == nullptr) {
                    finish((host != nullptr && host->closeSelf(_doc->id, std::nullopt)) ||
                           unsupported(command, "`close: self`"));
                    return;
                }
            } else {
                target = scope.find(command.target)->element;
            }
            ElementState& element = *target;
            if (element.node->prop("open")) {
                report(command.path, "the dialog's `open` is bound to an expression");
                finish(false);
                return;
            }
            bool const opening = command.kind == CommandKind::Open;
            if (opening && !element.open.peek()) {
                element.submitted.set(false);
            }
            session.noteWrite();
            element.open.set(opening);
            finish(true);
            return;
        }
        case CommandKind::Navigate: {
            NavigateRequest const request{.screen = command.target,
                                          .route = eval(command.extra),
                                          .params = eval(command.value),
                                          .as = command.option};
            if (host == nullptr) {
                finish(unsupported(command, "`navigate`"));
                return;
            }
            std::weak_ptr<RtScope> const weak = ctx.scope;
            CommandCtx const kept = detached(ctx);
            auto onResult = [this, weak, kept, list = &command.commands](Value const& result) {
                if (auto const held = weak.lock()) {
                    auto bindings = std::make_shared<Bindings>(*kept.bindings);
                    bindings->push_back(Binding{.name = "event", .read = [result] { return result; }});
                    CommandCtx const next{.scope = held,
                                          .bindings = bindings,
                                          .cancelClose = kept.cancelClose,
                                          .boundary = kept.boundary};
                    runtime().batch([&] { runList(*list, next, {}); });
                }
            };
            auto onCancel = [this, weak, kept, list = &command.alternative] {
                if (auto const held = weak.lock()) {
                    CommandCtx next = kept;
                    next.scope = held;
                    runtime().batch([&] { runList(*list, next, {}); });
                }
            };
            finish(host->navigate(request, std::move(onResult), std::move(onCancel)) ||
                   unsupported(command, "`navigate`"));
            return;
        }
        case CommandKind::Return:
            finish(host != nullptr && host->closeSelf(_doc->id, eval(command.value))
                       ? true
                       : unsupported(command, "`return`"));
            return;
        case CommandKind::Focus:
            if (focusTarget(command, scope, ctx.bindings.get())) {
                finish(true);
                return;
            }
            report(command.path, "`" + command.target + "` is not mounted");
            finish(false);
            return;
        case CommandKind::Host:
            runHostCommand(command, ctx, done);
            return;
        case CommandKind::SwitchBackend: {
            std::weak_ptr<RtScope> const weak = ctx.scope;
            auto committed = [this, weak, target = command.target, path = command.path,
                              done](std::expected<void, std::string> const& outcome) {
                if (!weak.lock()) {
                    return;
                }
                if (!outcome) {
                    report(path, "backendSwitchFailed: " + outcome.error());
                    done(false);
                    return;
                }
                session.backendChanged(target);
                done(true);
            };
            if (host != nullptr && host->switchBackend(command.target, std::move(committed))) {
                return;
            }
            report(command.path, "backend `" + command.target + "` is not listed");
            finish(false);
            return;
        }
        case CommandKind::ResetPrefs: {
            ValueRecord prefsNow = prefs.peek();
            if (command.target == "all") {
                prefsNow = prefDefaults;
            } else if (auto const found = prefDefaults.find(command.target); found != prefDefaults.end()) {
                prefsNow.insert_or_assign(command.target, found->second);
            }
            prefs.set(std::move(prefsNow));
            if (host != nullptr) {
                host->resetPrefs(_doc->id, command.target);
            }
            finish(true);
            return;
        }
        case CommandKind::SetLocale:
            session.setLocale(command.target);
            if (host != nullptr) {
                host->setLocale(command.target);
            }
            finish(true);
            return;
        case CommandKind::Notify: {
            std::string const text = textOf(eval(command.value), session.formatting());
            finish(host != nullptr && host->notify(text, command.option) ? true : unsupported(command, "`notify`"));
            return;
        }
        case CommandKind::Seq: {
            // A step that waits for a reply keeps the rest of the sequence in that reply's handler, which the
            // dispatcher may hold past the screen: the sequence holds its scope weakly and stops once it is gone.
            CommandCtx const inner = detached(CommandCtx{.scope = nullptr,
                                                         .bindings = std::make_shared<Bindings>(*ctx.bindings),
                                                         .cancelClose = ctx.cancelClose,
                                                         .boundary = ctx.boundary});
            std::weak_ptr<RtScope> const weak = ctx.scope;
            auto step = std::make_shared<std::function<void(std::size_t)>>();
            *step = [this, inner, weak, list = &command.commands, done = std::move(done),
                     weakStep = std::weak_ptr<std::function<void(std::size_t)>>{step}](std::size_t index) {
                auto const held = weak.lock();
                if (index == list->size() || !held) {
                    done(held != nullptr);
                    return;
                }
                auto const self = weakStep.lock();
                CommandCtx current = inner;
                current.scope = held;
                runOne(list->at(index), current, [self, index, done](bool succeeded) {
                    if (!succeeded) {
                        done(false);
                        return;
                    }
                    (*self)(index + 1);
                });
            };
            (*step)(0);
            return;
        }
        case CommandKind::All: {
            CommandCtx const inner{.scope = ctx.scope,
                                   .bindings = std::make_shared<Bindings>(*ctx.bindings),
                                   .cancelClose = ctx.cancelClose,
                                   .boundary = ctx.boundary};
            runList(command.commands, inner, std::move(done));
            return;
        }
        case CommandKind::If: {
            Value const condition = eval(command.value);
            bool const* const flag = condition.asBool();
            if (flag == nullptr) {
                finish(true);
                return;
            }
            runList(*flag ? command.commands : command.alternative, ctx, std::move(done));
            return;
        }
        case CommandKind::CancelClose:
            *ctx.cancelClose = true;
            finish(true);
            return;
        case CommandKind::Retry:
            // `retry` is valid only inside a boundary's `error`, which is lowered with its boundary.
            for (QueryRuntime* const query : ctx.boundary->queries) {
                if (!runtime().untracked([&] { return query->error(); }).isNull()) {
                    query->refetch();
                }
            }
            finish(true);
            return;
        case CommandKind::SignIn: {
            NameRef const* const found = scope.find(command.target);
            Value const reply = found != nullptr && found->category == NameCategory::Mutation
                                    ? runtime().untracked([&] { return found->mutation->result(); })
                                    : Value{};
            session.clear();
            finish(host != nullptr && host->signIn(reply) ? true : unsupported(command, "`signIn`"));
            return;
        }
        case CommandKind::SignOut:
            session.clear();
            if (host != nullptr) {
                host->signOut();
            }
            finish(true);
            return;
        case CommandKind::Upload:
        case CommandKind::Download:
        default:
            runTransfer(command, ctx, std::move(done));
            return;
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): a single run, or a run per list item.
inline void ScreenCore::runMutation(Command const& command, CommandCtx const& ctx, Done done) const {
    RtScope const& scope = *ctx.scope;
    MutationRuntime& mutation = *scope.find(command.target)->mutation;
    RtScope const& policy = scope.policyScope();
    if (policy.submitted != nullptr) {
        policy.submitted->set(true);
    }
    if (policy.owner != nullptr && !runtime().untracked([&] { return policy.ready(); })) {
        done(false);
        return;
    }
    RtScope const& declaring = *[&]() -> RtScope const* {
        for (RtScope const* candidate = &scope; candidate != nullptr; candidate = candidate->parent.get()) {
            if (candidate->names.contains(command.target)) {
                return candidate;
            }
        }
        return &scope;
    }();
    auto bodyFor = [&](Bindings const& bindings) {
        if (command.value) {
            return evalIn(scope, &bindings, command.value);
        }
        if (mutation.decl().body) {
            return evalIn(declaring, &bindings, mutation.decl().body);
        }
        return Value{ValueRecord{}};
    };
    if (!command.extra) {
        Value body = bodyFor(*ctx.bindings);
        mutation.run(std::move(body), std::make_shared<Bindings const>(*ctx.bindings),
                     [bindings = ctx.bindings, bindAs = command.binding, done = std::move(done)](bool succeeded,
                                                                                                 Value const& reply) {
                         if (succeeded && !bindAs.empty()) {
                             bindings->push_back(Binding{.name = bindAs, .read = [reply] { return reply; }});
                         }
                         done(succeeded);
                     });
        return;
    }
    Value const each = evalIn(scope, ctx.bindings.get(), command.extra);
    ValueList const items = each.asList() == nullptr ? ValueList{} : *each.asList();
    if (items.empty()) {
        done(true);
        return;
    }
    struct Each {
        std::size_t left = 0;
        bool ok = true;
        Done done;
    };
    auto state = std::make_shared<Each>(Each{.left = items.size(), .ok = true, .done = std::move(done)});
    std::vector<Value> bodies;
    std::vector<std::shared_ptr<Bindings const>> bound;
    for (Value const& item : items) {
        Bindings bindings = *ctx.bindings;
        if (!command.binding.empty()) {
            bindings.push_back(Binding{.name = command.binding, .read = [item] { return item; }});
        }
        bodies.push_back(bodyFor(bindings));
        bound.push_back(std::make_shared<Bindings const>(std::move(bindings)));
    }
    if (mutation.decl().concurrency != Concurrency::Exclusive) {
        for (std::size_t index = 0; index < bodies.size(); ++index) {
            mutation.run(bodies.at(index), bound.at(index), [state](bool succeeded, Value const&) {
                state->ok = state->ok && succeeded;
                if (--state->left == 0) {
                    state->done(state->ok);
                }
            });
        }
        return;
    }
    // An exclusive mutation refuses a run while another is in flight, so its elements go one after another.
    auto step = std::make_shared<std::function<void(std::size_t)>>();
    *step = [&mutation, bodies, bound, state,
             weakStep = std::weak_ptr<std::function<void(std::size_t)>>{step}](std::size_t index) {
        if (index == bodies.size()) {
            state->done(state->ok);
            return;
        }
        auto const self = weakStep.lock();
        mutation.run(bodies.at(index), bound.at(index), [self, state, index](bool succeeded, Value const&) {
            if (!succeeded) {
                state->done(false);
                return;
            }
            (*self)(index + 1);
        });
    };
    (*step)(0);
}

// NOLINTEND(misc-no-recursion)
inline void ScreenCore::writeSlot(StateSlot& slot, std::span<std::string const> within, Value value,
                                  Command const& command, Done const& done) {
    slot.scope->core.initState(*slot.scope, slot);
    Type const& type = slot.decl->type;
    if (within.empty() && !type.isAny() && !type.admits(value)) {
        report(command.path, "`set` writes a value that is not a " + type.name());
        done(false);
        return;
    }
    Value next = withPath(slot.value->peek(), within, std::move(value));
    session.noteWrite();
    slot.value->set(std::move(next));
    done(true);
}

inline void ScreenCore::runSetApp(Command const& command, std::span<std::string const> rest, Value value,
                                  Done const& done) {
    if (RtScope const* const app = session.appScope(); app != nullptr && !rest.empty()) {
        NameRef const* const named = app->find(rest.front());
        if (named != nullptr && named->category == NameCategory::State) {
            writeSlot(*named->state, rest.subspan(1), std::move(value), command, done);
            return;
        }
        report(command.path, "`app." + rest.front() + "` is not state of the app document");
        done(false);
        return;
    }
    std::string name;
    for (std::string const& segment : rest) {
        name += name.empty() ? segment : "." + segment;
    }
    done(host != nullptr && host->setAppValue(name, value) ? true : unsupported(command, "`set` of `app`"));
}

inline void ScreenCore::runSet(Command const& command, CommandCtx const& ctx, Done const& done) {
    RtScope const& scope = *ctx.scope;
    auto path = splitPath(command.target);
    if (!path || path->empty()) {
        done(false);
        return;
    }
    std::string const head = path->front();
    std::span<std::string const> const rest = std::span<std::string const>{*path}.subspan(1);
    Value value = evalIn(scope, ctx.bindings.get(), command.value);
    if (head == "app") {
        runSetApp(command, rest, std::move(value), done);
        return;
    }
    NameRef const* const found = scope.find(head);
    if (found != nullptr && found->category == NameCategory::State) {
        writeSlot(*found->state, rest, std::move(value), command, done);
        return;
    }
    if (found != nullptr && found->category == NameCategory::Element) {
        ElementState& element = *found->element;
        session.noteWrite();
        element.draft.set(value);
        if (element.bound == nullptr) {
            element.committed.set(std::move(value));
        }
        done(true);
        return;
    }
    report(command.path, "`set` names nothing writable here");
    done(false);
}

inline void ScreenCore::runReset(Command const& command, CommandCtx const& ctx, Done const& done) {
    RtScope const& scope = *ctx.scope;
    NameRef const* const found = scope.find(command.target);
    auto resetStates = [this](RtScope const& owner) {
        for (auto const& slot : owner.states) {
            if (slot->value) {
                Value init = runtime().untracked([&] { return evalIn(owner, nullptr, slot->decl->init); });
                slot->value->set(std::move(init));
            }
        }
        for (auto const& element : owner.elements) {
            element->draft.set(element->bound != nullptr ? runtime().untracked([&] { return element->bound->get(); })
                                                         : Value{});
            element->committed.set(Value{});
            element->edited.set(false);
            element->touched.set(false);
        }
    };
    bool known = true;
    session.noteWrite();
    if (found != nullptr && found->category == NameCategory::State) {
        StateSlot& slot = *found->state;
        initState(*slot.scope, slot);
        slot.value->set(runtime().untracked([&] { return evalIn(*slot.scope, nullptr, slot.decl->init); }));
    } else if (found != nullptr && found->category == NameCategory::Element) {
        ElementState& element = *found->element;
        element.submitted.set(false);
        if (RtScope const* const inner = element.inner.peek()) {
            resetStates(*inner);
        }
    } else {
        report(command.path, "`reset` names no state or dialog here");
        known = false;
    }
    done(known);
}

inline void ScreenCore::runHostCommand(Command const& command, CommandCtx const& ctx, Done const& done) {
    Value const args = evalIn(*ctx.scope, ctx.bindings.get(), command.value);
    if (auto const& run = session.hostCommandRunner()) {
        HostCommandResult const result = run(command.target, args);
        if (result.kind == HostCommandResult::Kind::Missing) {
            report(command.path, "host command `" + command.target + "` is missing");
        } else if (result.kind == HostCommandResult::Kind::Refused) {
            report(command.path, result.reason);
        }
        done(result.kind == HostCommandResult::Kind::Ran);
        return;
    }
    bool const known = host != nullptr && host->hostCommand(command.target, args);
    if (!known) {
        report(command.path, "host command `" + command.target + "` is missing");
    }
    done(known);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): a download and an upload, each settled.
/// @brief A transfer's share of its screen's pending count.
///
/// A transfer is pending from the host accepting it to its settle: its completion carries on the command list that
/// started it, which the close protocol and save-and-close wait for. The count is taken once the host accepts, so a
/// refused transfer never counts, and a host that settles before it returns leaves nothing taken. It is given back
/// once, also when the transfer settles after its scope is gone, but not after the screen itself is gone, since the
/// count went with it.
class TransferHold {
public:
    /// @brief A hold that counts nothing yet.
    /// @param core  The screen whose count it moves.
    /// @param alive Expires with the screen.
    TransferHold(ScreenCore& core, std::weak_ptr<bool> alive) : _core{&core}, _alive{std::move(alive)} {}

    /// @brief Counts the transfer, unless it has already settled.
    void take() {
        if (_state == State::NotTaken) {
            _state = State::Taken;
            _core->addPending(1);
        }
    }

    /// @brief Gives the count back, if one was taken and the screen is still there; the transfer counts no more.
    void release() {
        if (_state == State::Taken && !_alive.expired()) {
            _core->addPending(-1);
        }
        _state = State::Released;
    }

private:
    enum class State : std::uint8_t { NotTaken, Taken, Released };

    ScreenCore* _core;
    std::weak_ptr<bool> _alive;
    State _state = State::NotTaken;
};

/// @brief Bytes as base64 (RFC 4648, with padding): how a document carries an inline file.
/// @param bytes The bytes.
/// @return The text.
[[nodiscard]] inline std::string base64Encode(std::string_view bytes) {
    static constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t index = 0; index < bytes.size(); index += 3) {
        std::size_t const left = std::min<std::size_t>(3, bytes.size() - index);
        std::uint32_t group = 0;
        for (std::size_t offset = 0; offset < 3; ++offset) {
            group <<= 8U;
            if (offset < left) {
                group |= static_cast<unsigned char>(bytes.at(index + offset));
            }
        }
        for (std::size_t digit = 0; digit < 4; ++digit) {
            out.push_back(digit <= left ? kAlphabet.at((group >> (18U - (6U * digit))) & 0x3FU) : '=');
        }
    }
    return out;
}

/// @brief Base64 text as bytes (RFC 4648, with or without padding).
/// @param text The text.
/// @return The bytes, or `std::nullopt` for text that is not base64.
[[nodiscard]] inline std::optional<std::string> base64Decode(std::string_view text) {
    while (!text.empty() && text.back() == '=') {
        text.remove_suffix(1);
    }
    if (text.size() % 4 == 1) {
        return std::nullopt;
    }
    auto digitOf = [](char chr) -> int {
        if (chr >= 'A' && chr <= 'Z') {
            return chr - 'A';
        }
        if (chr >= 'a' && chr <= 'z') {
            return chr - 'a' + 26;
        }
        if (chr >= '0' && chr <= '9') {
            return chr - '0' + 52;
        }
        if (chr == '+') {
            return 62;
        }
        return chr == '/' ? 63 : -1;
    };
    std::string out;
    out.reserve((text.size() * 3) / 4);
    std::uint32_t group = 0;
    int bits = 0;
    for (char const chr : text) {
        int const digit = digitOf(chr);
        if (digit < 0) {
            return std::nullopt;
        }
        group = (group << 6U) | static_cast<std::uint32_t>(digit);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((group >> static_cast<std::uint32_t>(bits)) & 0xFFU));
        }
    }
    return out;
}

/// @brief How many bytes base64 text decodes to, read from its length alone.
/// @param text The text.
/// @return The byte count.
[[nodiscard]] inline std::size_t base64Size(std::string_view text) {
    while (!text.empty() && text.back() == '=') {
        text.remove_suffix(1);
    }
    return (text.size() * 3) / 4;
}

/// @brief A byte count as a person reads it: `512 bytes`, `2.4 KiB`, `1 MiB`.
/// @param bytes The count.
/// @return The text.
[[nodiscard]] inline std::string sizeText(std::size_t bytes) {
    constexpr std::size_t kKiB = 1024;
    if (bytes < kKiB) {
        return std::to_string(bytes) + " bytes";
    }
    bool const mebi = bytes >= kKiB * kKiB;
    std::size_t const unit = mebi ? kKiB * kKiB : kKiB;
    // One decimal place, rounded half up, and none when it is zero.
    std::size_t const tenths = ((bytes * 10) + (unit / 2)) / unit;
    std::string text = std::to_string(tenths / 10);
    if (tenths % 10 != 0) {
        text += "." + std::to_string(tenths % 10);
    }
    return text + (mebi ? " MiB" : " KiB");
}

/// @brief A document's inline file content as bytes: base64 text, or a list of integers 0 to 255.
/// @param value The value.
/// @return The bytes, or why the value is neither.
[[nodiscard]] inline std::expected<std::string, std::string> inlineBytes(Value const& value) {
    if (std::string const* const text = value.asString()) {
        if (std::optional<std::string> bytes = base64Decode(*text)) {
            return *std::move(bytes);
        }
        return std::unexpected(std::string{"the text is not base64"});
    }
    if (ValueList const* const list = value.asList()) {
        std::string bytes;
        bytes.reserve(list->size());
        for (Value const& item : *list) {
            std::int64_t const* const byte = item.asInt();
            if (byte == nullptr || *byte < 0 || *byte > 255) {
                return std::unexpected(std::string{"a byte list holds integers 0 to 255"});
            }
            bytes.push_back(static_cast<char>(static_cast<unsigned char>(*byte)));
        }
        return bytes;
    }
    return std::unexpected(std::string{"inline bytes are base64 text or a list of bytes"});
}

inline void ScreenCore::runTransfer(Command const& command, CommandCtx const& ctx, Done done) {
    Value const file = evalIn(*ctx.scope, ctx.bindings.get(), command.value);
    if (command.kind == CommandKind::Download) {
        runDownload(command, ctx, file, std::move(done));
    } else {
        runUpload(command, ctx, file, std::move(done));
    }
}

inline void ScreenCore::runDownload(Command const& command, CommandCtx const& ctx, Value const& file, Done done) {
    Value const name = evalIn(*ctx.scope, ctx.bindings.get(), command.extra);
    auto const hold = std::make_shared<TransferHold>(*this, _alive);
    auto const settled = [this, weak = std::weak_ptr<RtScope>{ctx.scope}, target = command.target,
                          preview = command.option == "preview", done,
                          hold](std::expected<Value, std::string> const& reply) {
        auto const held = weak.lock();
        if (!held) {
            hold->release();
            return;
        }
        runtime().batch([&] {
            bool const succeeded = reply.has_value();
            NameRef const* const found = succeeded && preview ? held->find(target) : nullptr;
            if (found != nullptr && found->category == NameCategory::State) {
                initState(*found->state->scope, *found->state);
                session.noteWrite();
                found->state->value->set(*reply);
            }
            done(succeeded);
            hold->release();
        });
    };
    if (command.bytes) {
        std::optional<InlineFile> const inlined = inlineDownload(command, ctx, file, name);
        if (!inlined) {
            done(false);
            return;
        }
        if (host == nullptr || !host->saveFile(*inlined, command.option, settled)) {
            unsupported(command, "`download`");
            done(false);
            return;
        }
        hold->take();
        return;
    }
    if (host == nullptr || !host->download(file, command.option, name, settled)) {
        unsupported(command, "`download`");
        done(false);
        return;
    }
    hold->take();
}

// Inline bytes are checked before the host is asked: content over the cap, or that does not decode, never reaches it.
inline std::optional<InlineFile> ScreenCore::inlineDownload(
    Command const& command, CommandCtx const& ctx,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the command's `file` and `name`, in its order.
    Value const& file, Value const& name) {
    std::size_t const cap = session.options().maxInlineFileBytes;
    std::size_t size = 0;
    if (std::string const* const text = file.asString()) {
        size = base64Size(*text);
    } else if (ValueList const* const list = file.asList()) {
        size = list->size();
    }
    std::expected<std::string, std::string> bytes = size > cap ? std::unexpected(std::string{}) : inlineBytes(file);
    if (size > cap || (bytes && bytes->size() > cap)) {
        report(command.path, "download: " + sizeText(bytes ? bytes->size() : size) + " is over the " + sizeText(cap) +
                                 " inline cap; serve it through the file store");
        return std::nullopt;
    }
    if (!bytes) {
        report(command.path, "download: " + bytes.error());
        return std::nullopt;
    }
    return InlineFile{
        .name = textOf(name, session.formatting()),
        .mediaType = command.mediaType
                         ? textOf(evalIn(*ctx.scope, ctx.bindings.get(), command.mediaType), session.formatting())
                         : std::string{},
        .bytes = *std::move(bytes)};
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one branch per outcome an upload can have.
inline void ScreenCore::runUpload(Command const& command, CommandCtx const& ctx, Value const& file, Done done) {
    RtScope const& scope = *ctx.scope;
    std::weak_ptr<RtScope> const weak = ctx.scope;
    auto const hold = std::make_shared<TransferHold>(*this, _alive);
    NameRef const* const transferName = command.transfer.empty() ? nullptr : scope.find(command.transfer);
    TransferState* const transfer =
        transferName != nullptr && transferName->category == NameCategory::Transfer ? transferName->transfer : nullptr;
    auto const progress = [weak, transfer](Value fraction) {
        if (weak.lock() && transfer != nullptr) {
            transfer->progress.set(std::move(fraction));
        }
    };
    // Settles with the members the mutation's body gets beside its own (the reference or the inline file), or the
    // failure.
    auto const settled = [this, weak, kept = detached(ctx), transfer, command = &command, done,
                          hold](std::expected<ValueRecord, std::string> reply) {
        auto const held = weak.lock();
        if (!held) {
            hold->release();
            return;
        }
        runtime().batch([&] {
            if (transfer != nullptr) {
                transfer->pending.set(false);
                transfer->error.set(reply ? Value{} : Value{reply.error()});
            }
            if (!reply) {
                done(false);
                hold->release();
                return;
            }
            NameRef const* const found = held->find(command->target);
            // The mutation's body cannot name the reference (a body is checked where the mutation is declared),
            // so the reference is added to it under `as`, `file` by default.
            MutationRuntime& mutation = *found->mutation;
            Value const base = evalIn(*held, kept.bindings.get(), mutation.decl().body);
            ValueRecord fields = base.asRecord() == nullptr ? ValueRecord{} : *base.asRecord();
            for (auto& [member, value] : *reply) {
                fields.insert_or_assign(member, std::move(value));
            }
            // The run takes its own count before this one is given back, so pending never reads zero in between.
            mutation.run(Value{std::move(fields)}, std::make_shared<Bindings const>(*kept.bindings),
                         [done](bool succeeded, Value const&) { done(succeeded); });
            hold->release();
        });
    };
    if (host == nullptr) {
        unsupported(command, "`upload`");
        done(false);
        return;
    }
    if (transfer != nullptr) {
        transfer->pending.set(true);
        transfer->progress.set(Value{std::int64_t{0}});
        transfer->error.set(Value{});
    }
    NameRef const* const target = scope.find(command.target);
    UploadRequest request{.file = file, .modelType = {}, .action = {}};
    if (target != nullptr && target->category == NameCategory::Mutation && target->mutation != nullptr) {
        request.modelType = target->mutation->typeId();
        request.action = target->mutation->decl().action;
    }
    std::string const member = command.binding.empty() ? std::string{"file"} : command.binding;
    bool started = false;
    if (command.bytes) {
        std::size_t const cap = session.options().maxInlineFileBytes;
        started = host->readFile(
            file, cap,
            [this, settled, progress, cap, member, command = &command](std::expected<InlineFile, std::string> read) {
                if (!read) {
                    settled(std::unexpected(read.error()));
                    return;
                }
                if (read->bytes.size() > cap) {
                    // The host should have refused it; the cap holds whether or not it did.
                    std::string why = "upload: " + sizeText(read->bytes.size()) + " is over the " + sizeText(cap) +
                                      " inline cap; send it through the file store";
                    report(command->path, why);
                    settled(std::unexpected(std::move(why)));
                    return;
                }
                progress(Value{std::int64_t{1}});
                ValueRecord added;
                added.insert_or_assign(member, Value{base64Encode(read->bytes)});
                if (!command->nameAs.empty()) {
                    added.insert_or_assign(command->nameAs, Value{read->name});
                }
                if (!command->typeAs.empty()) {
                    added.insert_or_assign(command->typeAs, Value{read->mediaType});
                }
                settled(std::move(added));
            });
    } else {
        started = host->uploadFor(request, progress, [settled, member](std::expected<Value, std::string> reference) {
            if (!reference) {
                settled(std::unexpected(reference.error()));
                return;
            }
            ValueRecord added;
            added.insert_or_assign(member, *std::move(reference));
            settled(std::move(added));
        });
    }
    if (!started) {
        if (transfer != nullptr) {
            transfer->pending.set(false);
            transfer->progress.set(Value{});
        }
        unsupported(command, "`upload`");
        done(false);
        return;
    }
    hold->take();
}

}  // namespace detail

namespace detail {

// --- Lowering --------------------------------------------------------------------------------------------------

/// @brief A value as a `bool` property: `true` only for `true`.
/// @param value  The value.
/// @param format Unused.
/// @return It.
[[nodiscard]] inline bool boolOf(Value const& value, FormatContext const& format) {
    static_cast<void>(format);
    bool const* const flag = value.asBool();
    return flag != nullptr && *flag;
}

/// @brief A value as a text property.
/// @param value  The value.
/// @param format The formatting facts.
/// @return Its text.
[[nodiscard]] inline std::string stringOf(Value const& value, FormatContext const& format) {
    return textOf(value, format);
}

/// @brief A value as a custom component's text prop (spec 5 §10): an `int` or `exact` as plain decimal text, so no
///        id or amount loses precision; `json` as its JSON text; `string` as the formatted text.
/// @param value  The value.
/// @param type   The prop type the component declares.
/// @param format The formatting facts, for `string`.
/// @return The text.
[[nodiscard]] inline std::string componentText(Value const& value, std::string_view type,
                                               FormatContext const& format) {
    if (type == "json") {
        return canonicalJson(value);
    }
    if (type == "int" || type == "exact") {
        if (std::int64_t const* const whole = value.asInt()) {
            return std::to_string(*whole);
        }
        // A value that cannot be rounded at its own places is written as its exact JSON, never clamped.
        if (Decimal const* const number = value.asDecimal()) {
            return detail::decimalText(number->value, static_cast<std::uint32_t>(number->value.decimalPlaces.value))
                .value_or(canonicalJson(value));
        }
        if (Quantity const* const quantity = value.asQuantity()) {
            auto const text =
                detail::decimalText(quantity->value, static_cast<std::uint32_t>(quantity->value.decimalPlaces.value));
            return text ? *text + " " + quantity->unit : canonicalJson(value);
        }
    }
    return textOf(value, format);
}

/// @brief An exact rational as the nearest double, unrounded.
///
/// `Rational::toDouble` rounds to the value's own places, which is a display reading: `29 / 100` keeps its operands'
/// 0 places and would read as 0. The view tree wants the magnitude, so the lowering reads it here.
/// @param value The rational.
/// @return Its numerator divided by its denominator.
[[nodiscard]] inline double realOf(math::Rational const& value) noexcept {
    return static_cast<double>(value.numerator) / static_cast<double>(value.denominator);
}

/// @brief A value as a custom component's `number` prop: the one kind that crosses as a double.
/// @param value The value.
/// @return It; zero for anything that is not a number.
[[nodiscard]] inline double componentNumber(Value const& value) {
    if (std::int64_t const* const whole = value.asInt()) {
        return static_cast<double>(*whole);
    }
    if (Decimal const* const number = value.asDecimal()) {
        return realOf(number->value);
    }
    if (Quantity const* const quantity = value.asQuantity()) {
        return realOf(quantity->value);
    }
    return 0.0;
}

/// @brief A custom component's event payload as a value: `i:<int64>` an `int`, `s:<text>` a `string`, anything else
///        JSON text read as the event's declared type, and text that is none of those a `string`.
/// @param payload The payload, as the backend reports it.
/// @param type    The payload type the component declares; null for an undeclared event.
/// @return The value.
[[nodiscard]] inline Value componentPayload(std::string const& payload, Type const* type) {
    std::string_view const text{payload};
    if (text.starts_with("i:")) {
        std::int64_t whole = 0;
        std::string_view const digits = text.substr(2);
        char const* const last = std::to_address(digits.end());
        auto const [end, error] = std::from_chars(std::to_address(digits.begin()), last, whole);
        if (error == std::errc{} && end == last) {
            return Value{whole};
        }
    }
    if (text.starts_with("s:")) {
        return Value{std::string{text.substr(2)}};
    }
    if (auto json = parseJson(text); json && type != nullptr) {
        if (auto value = valueFromJson(*json, *type)) {
            return std::move(*value);
        }
    }
    return Value{payload};
}

/// @brief A value as an integer: an `int`, or a decimal truncated toward zero.
/// @param value    The value.
/// @param fallback What anything else gives.
/// @return It.
[[nodiscard]] inline std::int64_t intOf(Value const& value, std::int64_t fallback) {
    if (std::int64_t const* const number = value.asInt()) {
        return *number;
    }
    if (Decimal const* const number = value.asDecimal()) {
        // Integer division truncates toward zero, and reads the exact value rather than one rounded to its places.
        return number->value.numerator / number->value.denominator;
    }
    return fallback;
}

/// @brief A text role by name.
/// @param name `normal`, `muted`, `heading` (or `title`), `section`, `error`, `success`, `mono`.
/// @return The role; `nullopt` for any other name.
[[nodiscard]] inline std::optional<TextRole> roleNamed(std::string_view name) {
    if (name == "normal") {
        return TextRole::Normal;
    }
    if (name == "muted") {
        return TextRole::Muted;
    }
    if (name == "heading" || name == "title") {
        return TextRole::Heading;
    }
    if (name == "section") {
        return TextRole::Section;
    }
    if (name == "error") {
        return TextRole::Error;
    }
    if (name == "success") {
        return TextRole::Success;
    }
    if (name == "mono") {
        return TextRole::Mono;
    }
    return std::nullopt;
}

/// @brief What a role that names none is told: the names `roleNamed` reads.
inline constexpr std::string_view kRoleNames = "a role is normal, muted, heading, section, error, success or mono";

/// @brief A text role by name, as `roleNamed` reads it.
/// @param value  The name.
/// @param format Unused.
/// @return The role; `Normal` for any other name.
[[nodiscard]] inline TextRole roleOf(Value const& value, FormatContext const& format) {
    return roleNamed(textOf(value, format)).value_or(TextRole::Normal);
}

/// @brief A tone by name.
/// @param value  `neutral`, `info`, `ok` (or `success`), `warn` (or `warning`), `err` (or `error`).
/// @param format Unused.
/// @return The tone; `Neutral` for anything else.
[[nodiscard]] inline Tone toneOf(Value const& value, FormatContext const& format) {
    std::string const text = textOf(value, format);
    if (text == "info") {
        return Tone::Info;
    }
    if (text == "ok" || text == "success") {
        return Tone::Ok;
    }
    if (text == "warn" || text == "warning") {
        return Tone::Warn;
    }
    if (text == "err" || text == "error") {
        return Tone::Err;
    }
    return Tone::Neutral;
}

/// @brief A splitter's `sizes`: a list of numbers, each read as `intOf` reads it.
/// @param value The value.
/// @return The sizes; `nullopt` for anything but a list whose every item is an `int` or a decimal.
[[nodiscard]] inline std::optional<std::vector<int>> splitterSizesOf(Value const& value) {
    ValueList const* const items = value.asList();
    if (items == nullptr) {
        return std::nullopt;
    }
    std::vector<int> sizes;
    sizes.reserve(items->size());
    for (Value const& item : *items) {
        if (item.asInt() == nullptr && item.asDecimal() == nullptr) {
            return std::nullopt;
        }
        sizes.push_back(static_cast<int>(intOf(item, 0)));
    }
    return sizes;
}

/// @brief A sizing by its document form: `"content"`, `{"fixed": n}` or `{"stretch": w}`.
/// @param value The value.
/// @return The sizing.
[[nodiscard]] inline Sizing sizingOf(Value const& value) {
    if (Value const* const fixed = value.field("fixed")) {
        return Sizing::fixed(static_cast<int>(intOf(*fixed, 0)));
    }
    if (Value const* const stretch = value.field("stretch")) {
        return Sizing::stretch(static_cast<int>(intOf(*stretch, 1)));
    }
    return Sizing::content();
}

/// @brief Whether a value is a sizing of one dimension: `"content"`, `{"fixed": n}` or `{"stretch": w}`.
/// @param value The value.
/// @return Whether `sizingOf` reads it as written rather than as content by default.
[[nodiscard]] inline bool isSizing(Value const& value) {
    if (std::string const* const text = value.asString()) {
        return *text == "content";
    }
    ValueRecord const* const record = value.asRecord();
    return record != nullptr && record->size() == 1 && (record->contains("fixed") || record->contains("stretch")) &&
           record->begin()->second.asInt() != nullptr;
}

/// @brief An alignment by name, as the loader checked it: `start`, `center`, `end`, or stretch for none.
/// @param name The name; empty for none.
/// @return The alignment.
[[nodiscard]] inline Align alignOf(std::string_view name) {
    if (name == "start") {
        return Align::Start;
    }
    if (name == "center") {
        return Align::Center;
    }
    if (name == "end") {
        return Align::End;
    }
    return Align::Stretch;
}

/// @brief A table cell's place by name: across its column (`align`) or down its row (`valign`).
/// @param name `start`, `center` or `end`.
/// @return The place; `nullopt` for any other name, `stretch` included.
[[nodiscard]] inline std::optional<Align> cellPlaceNamed(std::string_view name) {
    if (name == "start") {
        return Align::Start;
    }
    if (name == "center") {
        return Align::Center;
    }
    if (name == "end") {
        return Align::End;
    }
    return std::nullopt;
}

/// @brief A justification by name, as the loader checked it: `center`, `end`, `between`, or start for none.
/// @param name The name; empty for none.
/// @return The justification.
[[nodiscard]] inline Justify justifyOf(std::string_view name) {
    if (name == "center") {
        return Justify::Center;
    }
    if (name == "end") {
        return Justify::End;
    }
    if (name == "between") {
        return Justify::Between;
    }
    return Justify::Start;
}

/// @brief An axis by name: `horizontal`, or vertical for anything else.
/// @param value The value.
/// @return The axis.
[[nodiscard]] inline Axis axisOf(Value const& value) {
    std::string const* const text = value.asString();
    return text != nullptr && *text == "horizontal" ? Axis::Horizontal : Axis::Vertical;
}

/// @brief A step's state by name: `active`, `done`, `failed`, or pending for anything else.
/// @param name The name.
/// @return The state.
[[nodiscard]] inline StepState stepStateOf(std::string_view name) {
    if (name == "active") {
        return StepState::Active;
    }
    if (name == "done") {
        return StepState::Done;
    }
    return name == "failed" ? StepState::Failed : StepState::Pending;
}

/// @brief The options of a select, from a list of scalars or of records with a key and a `label`.
/// @param options The list.
/// @param format  The formatting facts.
/// @return The options.
[[nodiscard]] inline std::vector<SelectOption> selectOptions(Value const& options, FormatContext const& format) {
    std::vector<SelectOption> out;
    if (ValueList const* const items = options.asList()) {
        for (Value const& option : *items) {
            Value const key = optionKey(option);
            Value const* const label = option.field("label");
            out.push_back(SelectOption{.key = keyOf(key), .label = textOf(label != nullptr ? *label : key, format)});
        }
    }
    return out;
}

/// @brief Content whose scope is made when it mounts and released when it unmounts: a one-row `forEach` whose
///        row view builds it.
/// @param build Builds the content, its scope included.
/// @return The node.
[[nodiscard]] inline Node lazy(std::function<Node()> build) {
    return ui::forEach<int>([] { return std::vector<int>{0}; }, [](int const&) { return Key{std::int64_t{0}}; },
                            [build = std::move(build)](reactive::Signal<int> const&) { return build(); });
}

/// @brief A column of content that holds its scope for as long as it is mounted: its `visible` is a binding
///        that keeps the scope alive and reads nothing.
/// @param children The content.
/// @param scope    The scope.
/// @param keys     The chords of the content.
/// @return The node.
[[nodiscard]] inline Node holding(std::vector<Node> children, std::shared_ptr<RtScope> scope,
                                  std::vector<KeyBinding> keys = {}) {
    Column column{.children = std::move(children)};
    column.common.visible = Prop<bool>{[held = std::move(scope)] { return held != nullptr; }};
    column.common.keys = std::move(keys);
    return ui::column(std::move(column));
}

// NOLINTBEGIN(misc-no-recursion): a menu nests, and lowering descends the document.
inline Node ScreenCore::lower(DocNode const& node, LowerCtx const& ctx) {
    if (recording) {
        if (loweredNodes.size() >= kPruneLoweredAt && loweredNodes.size() % kPruneLoweredAt == 0) {
            std::erase_if(loweredNodes, [](LoweredNode const& entry) { return entry.scope.expired(); });
        }
        loweredNodes.push_back(LoweredNode{.node = &node, .scope = ctx.scope, .bindings = ctx.bindings});
    }
    return lowerKind(node, ctx, elementFor(*ctx.scope, node));
}

inline Prop<TextRole> ScreenCore::roleProp(LowerCtx const& ctx, ExprPtr const& expr) {
    if (!expr || expr->kind == ExprKind::Literal) {
        return bindProp<TextRole>(ctx, expr, TextRole::Normal, roleOf);
    }
    return slotProp<TextRole>([this, scope = ctx.scope, bindings = ctx.bindings, expr] {
        Value const value = evalIn(*scope, bindings.get(), expr);
        std::optional<TextRole> const role = roleNamed(textOf(value, session.formatting()));
        if (!role) {
            report(expr->path, "role is " + canonicalJson(value) + "; " + std::string{kRoleNames});
        }
        return role.value_or(TextRole::Normal);
    });
}

inline Prop<std::string> ScreenCore::surfaceOf(DocNode const& node, LowerCtx const& ctx) {
    ExprPtr const expr = node.prop("surface");
    auto nameOf = [this, path = pointerAppend(node.path, "surface")](Value const& value) -> std::string {
        if (std::string const* const name = value.asString()) {
            return *name;
        }
        if (!value.isNull()) {
            report(path, "a surface is the name of one; " + canonicalJson(value) + " is not a string");
        }
        return {};
    };
    if (!expr || expr->kind == ExprKind::Literal) {
        return expr ? Prop<std::string>{nameOf(expr->literal)} : Prop<std::string>{std::string{}};
    }
    return slotProp<std::string>([scope = ctx.scope, bindings = ctx.bindings, expr, nameOf = std::move(nameOf)] {
        return nameOf(evalIn(*scope, bindings.get(), expr));
    });
}

// The size request: `sizing` (the width, or both dimensions), `alignSelf`, and the bounds.
inline void ScreenCore::layoutOf(DocNode const& node, LowerCtx const& ctx, LayoutHints& layout) {
    if (ExprPtr const sizing = node.prop("sizing")) {
        // The width, or both dimensions from a `{width, height}` record, one left out being content. The loader
        // refuses a literal of another form; a computed one that is none is reported and lowers to content.
        Value const value = once(ctx, sizing);
        Value const* const width = value.field("width");
        Value const* const height = value.field("height");
        bool const record = width != nullptr || height != nullptr;
        bool const valid = record ? (width == nullptr || isSizing(*width)) && (height == nullptr || isSizing(*height))
                                  : isSizing(value);
        if (!valid) {
            report(sizing->path, "sizing is " + canonicalJson(value) +
                                     "; a sizing is \"content\", {\"fixed\": n}, {\"stretch\": w}, or a record of "
                                     "width and height of them");
        } else if (record) {
            layout.width = width != nullptr ? sizingOf(*width) : Sizing::content();
            layout.height = height != nullptr ? sizingOf(*height) : Sizing::content();
        } else {
            layout.width = sizingOf(value);
        }
    }
    if (std::string_view const placed = node.name("alignSelf"); !placed.empty()) {
        layout.alignSelf = alignOf(placed);
    }
    // Literal integers, checked at load.
    for (auto const& [key, bound] :
         {std::pair{"minWidth", &LayoutHints::minWidth}, std::pair{"maxWidth", &LayoutHints::maxWidth},
          std::pair{"minHeight", &LayoutHints::minHeight}, std::pair{"maxHeight", &LayoutHints::maxHeight}}) {
        if (ExprPtr const expr = node.prop(key); expr != nullptr && expr->literal.asInt() != nullptr) {
            layout.*bound = static_cast<int>(*expr->literal.asInt());
        }
    }
}

/// @brief Where a declaration is lowered, as a focus key reads it.
struct FocusPlace {
    /// @brief The focus key of the component use it is lowered for, and `>`; empty outside a component.
    std::string prefix;
    /// @brief The keys of the rows that enclose it, outermost first.
    std::vector<std::string> rows;
};

/// @brief Where @p scope is: the row keys of its chain and the component use it is lowered for.
/// @param scope The scope.
/// @return It.
[[nodiscard]] inline FocusPlace focusPlace(RtScope const& scope) {
    FocusPlace place;
    for (RtScope const* current = &scope; current != nullptr; current = current->parent.get()) {
        if (current->row != nullptr) {
            place.rows.push_back(current->rowKey);
        }
        if (current->parent == nullptr) {
            place.prefix = current->usePrefix;
        }
    }
    std::ranges::reverse(place.rows);
    return place;
}

/// @brief A focus key: a declaration's pointer under its component use's prefix, with one key per enclosing row.
/// @param place Where the declaration is lowered.
/// @param path  The declaring node's JSON Pointer.
/// @return The key, unique per declaration, row and component use.
[[nodiscard]] inline std::string focusKey(FocusPlace const& place, std::string_view path) {
    std::string key = place.prefix;
    key += path;
    for (std::string const& row : place.rows) {
        key += '@';
        key += row;
    }
    return key;
}

/// @brief The focus key of the element @p path declares, lowered in @p scope.
/// @param path  The declaring node's JSON Pointer.
/// @param scope The scope it is lowered in.
/// @return The key.
[[nodiscard]] inline std::string focusKeyOf(std::string_view path, RtScope const& scope) {
    return focusKey(focusPlace(scope), path);
}

inline void ScreenCore::commonOf(DocNode const& node, LowerCtx const& ctx, ElementState* element, Common& common) {
    static_cast<void>(element);
    common.visible = bindProp<bool>(ctx, node.prop("visible"), true, boolOf);
    common.enabled = bindProp<bool>(ctx, node.prop("enabled"), true, boolOf);
    common.tooltip = bindProp<std::string>(ctx, node.prop("tooltip"), std::string{}, stringOf);
    layoutOf(node, ctx, common.layout);
    common.testId = std::string{node.name("testId")};
    if (!node.id.empty()) {
        common.focusId = focusKeyOf(node.path, *ctx.scope);
    }
    common.surface = surfaceOf(node, ctx);
    if (ExprPtr const autofocus = node.prop("autofocus")) {
        common.autofocus = boolOf(once(ctx, autofocus), session.formatting());
    }
    for (DocNodePtr const& a11y : node.partList("a11y")) {
        common.a11y.name = bindProp<std::string>(ctx, a11y->prop("name"), std::string{}, stringOf);
        common.a11y.role = std::string{a11y->name("role")};
    }
    for (DocNodePtr const& drag : node.partList("drag")) {
        ExprPtr const payload = drag->prop("payload");
        common.dragKey = slotProp<std::optional<Key>>(
            [scope = ctx.scope, bindings = ctx.bindings, payload]() -> std::optional<Key> {
                Value const value = evalIn(*scope, bindings.get(), payload);
                return value.isNull() ? std::nullopt : std::optional<Key>{keyOf(value)};
            });
    }
    for (DocNodePtr const& drop : node.partList("drop")) {
        ExprPtr const accepts = drop->prop("accepts");
        common.accepts = [this, scope = ctx.scope, bindings = ctx.bindings, accepts](Key const& key) {
            if (!accepts) {
                return true;
            }
            Value const accepted = runtime().untracked([&] { return evalIn(*scope, bindings.get(), accepts); });
            if (ValueList const* const items = accepted.asList()) {
                return std::ranges::any_of(*items, [&](Value const& item) { return keyOf(item) == key; });
            }
            return accepted.isNull() || keyOf(accepted) == key;
        };
        common.onDrop = [this, ctx, commands = drop->event("onDrop")](Key const& key) {
            runEvent(commands, ctx, valueOfKey(key));
        };
    }
    for (auto const& [chord, commands] : node.keys) {
        common.keys.push_back(
            KeyBinding{.chord = chord, .onPress = [this, ctx, list = &commands] { runEvent(list, ctx, Value{}); }});
    }
}

inline void ScreenCore::fieldOf(DocNode const& node, LowerCtx const& ctx, ElementState* element, FieldState& field) {
    field.readonly = bindProp<bool>(ctx, node.prop("readonly"), false, boolOf);
    field.required = bindProp<bool>(ctx, node.prop("required"), false, boolOf);
    field.stale = bindProp<bool>(ctx, node.prop("stale"), false, boolOf);
    if (element != nullptr) {
        field.errors = slotProp<std::vector<std::string>>(
            [this, scope = ctx.scope, element] { return stringsOf(elementValue(*element, "shownErrors")); });
    } else if (ExprPtr const errors = node.prop("errors")) {
        field.errors = slotProp<std::vector<std::string>>([scope = ctx.scope, bindings = ctx.bindings, errors] {
            return stringsOf(evalIn(*scope, bindings.get(), errors));
        });
    }
}

inline std::vector<MenuItem> ScreenCore::menuItems(std::span<DocNodePtr const> parts, LowerCtx const& ctx) {
    std::vector<MenuItem> items;
    for (DocNodePtr const& part : parts) {
        MenuItem item;
        item.label = bindProp<std::string>(ctx, part->prop("label"), std::string{}, stringOf);
        if (Commands const* const commands = part->event("onClick")) {
            item.onSelect = [this, ctx, commands] { runEvent(commands, ctx, Value{}); };
        }
        if (ExprPtr const icon = part->prop("icon")) {
            item.icon = textOf(once(ctx, icon), session.formatting());
        }
        item.keys = std::string{part->name("keys")};
        if (ExprPtr const checked = part->prop("checked")) {
            item.checked = slotProp<std::optional<bool>>(
                [scope = ctx.scope, bindings = ctx.bindings, checked]() -> std::optional<bool> {
                    Value const value = evalIn(*scope, bindings.get(), checked);
                    bool const* const flag = value.asBool();
                    return flag == nullptr ? std::nullopt : std::optional<bool>{*flag};
                });
        }
        item.enabled = bindProp<bool>(ctx, part->prop("enabled"), true, boolOf);
        item.items = menuItems(part->partList("items"), ctx);
        items.push_back(std::move(item));
    }
    return items;
}

inline ActionModels ScreenCore::lowerActionParts(std::span<DocNodePtr const> parts, LowerCtx const& ctx) {
    ActionModels models;
    models.reserve(parts.size());
    for (DocNodePtr const& part : parts) {
        ActionModel model{.id = part->id,
                          .path = part->path,
                          .label = bindProp<std::string>(ctx, part->prop("label"), std::string{}, stringOf),
                          .icon = bindProp<std::string>(ctx, part->prop("icon"), std::string{}, stringOf),
                          .emphasis = ButtonEmphasis::Normal,
                          .enabled = bindProp<bool>(ctx, part->prop("enabled"), true, boolOf),
                          .visible = bindProp<bool>(ctx, part->prop("visible"), true, boolOf),
                          .run = {}};
        if (part->name("emphasis") == "primary") {
            model.emphasis = ButtonEmphasis::Primary;
        } else if (part->name("emphasis") == "danger") {
            model.emphasis = ButtonEmphasis::Danger;
        } else if (part->name("emphasis") == "ghost") {
            model.emphasis = ButtonEmphasis::Ghost;
        }
        if (Commands const* const commands = part->event("onClick")) {
            model.run = [this, ctx, commands] { runEvent(commands, ctx, Value{}); };
        }
        models.push_back(std::move(model));
    }
    return models;
}

inline Node ScreenCore::actionBarOf(ActionModels actions) {
    ActionBar bar;
    for (ActionModel& action : actions) {
        Button button{.label = std::move(action.label),
                      .emphasis = action.emphasis,
                      .onClick = std::move(action.run),
                      .common = {}};
        button.common.visible = std::move(action.visible);
        button.common.enabled = std::move(action.enabled);
        button.common.testId = "action_" + action.id;
        bar.children.push_back(ui::button(std::move(button)));
    }
    return ui::actionBar(std::move(bar));
}

inline std::vector<BadgeModel> ScreenCore::lowerBadgeParts(std::span<DocNodePtr const> parts, LowerCtx const& ctx) {
    std::vector<BadgeModel> badges;
    badges.reserve(parts.size());
    for (DocNodePtr const& part : parts) {
        badges.push_back(BadgeModel{.text = bindProp<std::string>(ctx, part->prop("text"), std::string{}, stringOf),
                                    .tone = bindProp<Tone>(ctx, part->prop("tone"), Tone::Neutral, toneOf),
                                    .icon = bindProp<std::string>(ctx, part->prop("icon"), std::string{}, stringOf),
                                    .visible = bindProp<bool>(ctx, part->prop("visible"), true, boolOf)});
    }
    return badges;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per kind of ui/1.
inline Node ScreenCore::lowerKind(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    std::string_view const kind = node.kind;
    FormatContext const& format = session.formatting();
    auto children = [&](std::string_view key) {
        std::vector<Node> out;
        for (DocNodePtr const& child : node.nodes(key)) {
            out.push_back(lower(*child, ctx));
        }
        return out;
    };
    auto child = [&](std::string_view key) -> Node {
        auto const nodes = node.nodes(key);
        return nodes.empty() ? Node{} : lowerAll(nodes, ctx);
    };
    auto onceInt = [&](std::string_view key, std::int64_t fallback) {
        ExprPtr const expr = node.prop(key);
        return expr ? intOf(once(ctx, expr), fallback) : fallback;
    };
    auto onceBool = [&](std::string_view key, bool fallback) {
        ExprPtr const expr = node.prop(key);
        return expr ? boolOf(once(ctx, expr), format) : fallback;
    };
    auto onceText = [&](std::string_view key) {
        ExprPtr const expr = node.prop(key);
        return expr ? textOf(once(ctx, expr), format) : std::string{};
    };
    auto textProp = [&](std::string_view key) {
        return bindProp<std::string>(ctx, node.prop(key), std::string{}, stringOf);
    };
    // A field shows no caption of its own, so its label is what names it to assistive technology. An `a11y` part
    // that gives a name was written for that purpose and wins; one that gives only a role leaves the label.
    auto nameByLabel = [&](Common& common) {
        bool const named = std::ranges::any_of(node.partList("a11y"),
                                               [](DocNodePtr const& a11y) { return a11y->prop("name") != nullptr; });
        if (node.prop("label") && !named) {
            common.a11y.name = textProp("label");
        }
    };
    // A standalone input draws its label as a caption only when the document asks for it (`showLabel`).
    auto captionOf = [&](auto& spec) {
        if (node.prop("label")) {
            spec.label = textProp("label");
        }
        spec.showLabel = node.flag("showLabel");
    };
    auto action = [&](std::string_view key) -> Action {
        Commands const* const commands = node.event(key);
        if (commands == nullptr) {
            return {};
        }
        return [this, ctx, commands] { runEvent(commands, ctx, Value{}); };
    };
    auto actionPart = [&]() {
        ActionButton button;
        for (DocNodePtr const& part : node.partList("action")) {
            button.label = bindProp<std::string>(ctx, part->prop("label"), std::string{}, stringOf);
            if (Commands const* const commands = part->event("onClick")) {
                button.onClick = [this, ctx, commands] { runEvent(commands, ctx, Value{}); };
            }
        }
        return button;
    };
    auto read = [element, this](std::string_view name) {
        return [element, this, name = std::string{name}] { return elementValue(*element, name); };
    };

    if (kind == "text") {
        Text spec{.text = textProp("text"), .role = roleProp(ctx, node.prop("role"))};
        fieldOf(node, ctx, element, spec.field);
        commonOf(node, ctx, element, spec.common);
        return ui::text(std::move(spec));
    }
    if (kind == "button") {
        Button spec{.label = textProp("label")};
        spec.icon = onceText("icon");
        if (node.name("emphasis") == "primary") {
            spec.emphasis = ButtonEmphasis::Primary;
        } else if (node.name("emphasis") == "danger") {
            spec.emphasis = ButtonEmphasis::Danger;
        } else if (node.name("emphasis") == "ghost") {
            spec.emphasis = ButtonEmphasis::Ghost;
        }
        if (node.name("style") == "item") {
            spec.style = ButtonStyle::Item;
        }
        if (node.prop("selected")) {
            spec.selected = bindProp<bool>(ctx, node.prop("selected"), false, boolOf);
        }
        Commands const* const onClick = node.event("onClick");
        Commands const* const onActivate = node.event("onActivate");
        if (onClick != nullptr || onActivate != nullptr) {
            spec.onClick = [this, ctx, onClick, onActivate] {
                runEvent(onClick, ctx, Value{});
                runEvent(onActivate, ctx, Value{});
            };
        }
        commonOf(node, ctx, element, spec.common);
        return ui::button(std::move(spec));
    }
    if (kind == "textInput") {
        std::string const commitOn = node.name("commitOn").empty() ? "change" : std::string{node.name("commitOn")};
        auto commit = [this, ctx, element, commitOn, onChange = node.event("onChange")](std::string const& when,
                                                                                        std::string const& text) {
            if (commitOn != when) {
                return;
            }
            if (element->bound == nullptr) {
                session.noteWrite();
                element->committed.set(Value{text});
            }
            runEvent(onChange, ctx, Value{text});
        };
        TextInput spec;
        spec.value =
            slotProp<std::string>([this, element] { return textOf(element->draft.get(), session.formatting()); });
        spec.onChange = [this, element, commit](std::string text) {
            session.asUser([&] {
                session.noteWrite();
                element->draft.set(Value{text});
                element->edited.set(true);
            });
            commit("change", text);
        };
        spec.onCommit = [this, ctx, element, commit, onCommit = node.event("onCommit")](std::string const& text) {
            element->touched.set(true);
            commit("blur", text);
            runEvent(onCommit, ctx, Value{text});
        };
        // Only a field that does something on Enter submits: one without `onSubmit` that does not commit on Enter
        // lets Enter go on to its ancestors' chords, a dialog's `default` among them.
        if (Commands const* const onSubmit = node.event("onSubmit"); onSubmit != nullptr || commitOn == "enter") {
            spec.onSubmit = [this, ctx, commit, onSubmit](std::string const& text) {
                commit("enter", text);
                runEvent(onSubmit, ctx, Value{text});
            };
        }
        spec.placeholder = textProp("placeholder");
        std::string const mode = onceText("mode");
        spec.mode = TextInputMode::SingleLine;
        if (mode == "multiline") {
            spec.mode = TextInputMode::Multiline;
        } else if (mode == "password") {
            spec.mode = TextInputMode::Password;
        }
        spec.icon = onceText("icon");
        fieldOf(node, ctx, element, spec.field);
        commonOf(node, ctx, element, spec.common);
        nameByLabel(spec.common);
        captionOf(spec);
        return ui::textInput(std::move(spec));
    }
    if (kind == "checkbox") {
        Checkbox spec{.label = textProp("label"), .checked = slotProp<bool>([read = read("checked")] {
                                                      Value const value = read();
                                                      bool const* const flag = value.asBool();
                                                      return flag != nullptr && *flag;
                                                  })};
        spec.onToggle = [this, ctx, element, onToggle = node.event("onToggle"),
                         onChange = node.event("onChange")](bool checked) {
            if (element->bound == nullptr) {
                session.asUser([&] {
                    session.noteWrite();
                    element->draft.set(checked);
                });
            }
            runEvent(onToggle, ctx, Value{checked});
            runEvent(onChange, ctx, Value{checked});
        };
        fieldOf(node, ctx, element, spec.field);
        commonOf(node, ctx, element, spec.common);
        return ui::checkbox(std::move(spec));
    }
    if (kind == "select") {
        Select spec;
        spec.options = slotProp<std::vector<SelectOption>>(
            [this, element] { return selectOptions(element->rows->get(), session.formatting()); });
        spec.selected = slotProp<std::optional<Key>>([read = read("selected")]() -> std::optional<Key> {
            Value const selected = read();
            return selected.isNull() ? std::nullopt : std::optional<Key>{keyOf(selected)};
        });
        spec.onSelect = [this, ctx, element, onChoose = node.event("onChoose"),
                         onChange = node.event("onChange")](Key const& key) {
            Value const chosen = valueOfKey(key);
            if (element->bound == nullptr) {
                session.asUser([&] {
                    session.noteWrite();
                    element->draft.set(chosen);
                });
            }
            runEvent(onChoose, ctx, chosen);
            runEvent(onChange, ctx, chosen);
        };
        spec.style = onceText("style") == "radio" ? SelectStyle::Radio : SelectStyle::Dropdown;
        fieldOf(node, ctx, element, spec.field);
        commonOf(node, ctx, element, spec.common);
        nameByLabel(spec.common);
        captionOf(spec);
        return ui::select(std::move(spec));
    }
    if (kind == "slider") {
        Slider spec;
        spec.value = slotProp<std::int64_t>([read = read("value"), element] {
            return intOf(element->bound != nullptr ? read() : element->draft.get(), 0);
        });
        spec.minimum = onceInt("min", 0);
        spec.maximum = onceInt("max", 100);
        spec.step = onceInt("step", 1);
        spec.onChange = [this, ctx, element, onChange = node.event("onChange")](std::int64_t value) {
            session.asUser([&] {
                session.noteWrite();
                element->draft.set(value);
                element->edited.set(true);
                if (element->bound == nullptr) {
                    element->committed.set(value);
                }
            });
            runEvent(onChange, ctx, Value{value});
        };
        spec.onCommit = [this, ctx, element, onCommit = node.event("onCommit")](std::int64_t value) {
            element->touched.set(true);
            runEvent(onCommit, ctx, Value{value});
        };
        fieldOf(node, ctx, element, spec.field);
        commonOf(node, ctx, element, spec.common);
        nameByLabel(spec.common);
        return ui::slider(std::move(spec));
    }
    if (kind == "dateTimeInput") {
        DateTimeInput spec;
        DateMode const mode = onceText("mode") == "date" ? DateMode::Date : DateMode::DateTime;
        std::optional<TypeKind> heldAtLoad;
        if (auto const found = _doc->dateValueKinds.find(&node); found != _doc->dateValueKinds.end()) {
            heldAtLoad = found->second;
        }
        std::string const valuePath = pointerAppend(node.path, "value");
        std::int32_t const offsetMinutes = format.utcOffsetMinutes;
        spec.value = slotProp<std::optional<time::Timestamp>>([this, element, offsetMinutes,
                                                               valuePath]() -> std::optional<time::Timestamp> {
            DateReading const reading = dateInstantOf(element->draft.get(), offsetMinutes);
            if (!reading.readable) {
                report(valuePath, "`dateTimeInput` shows a timestamp or an ISO-8601 date or date-time string");
            }
            return reading.instant ? std::optional<time::Timestamp>{time::Timestamp{*reading.instant}} : std::nullopt;
        });
        spec.onChange = [this, ctx, element, heldAtLoad, mode, offsetMinutes, onChange = node.event("onChange"),
                         onCommit = node.event("onCommit")](std::optional<time::Timestamp> value) {
            bool asString = element->bound != nullptr && element->bound->get().asString() != nullptr;
            if (heldAtLoad) {
                asString = *heldAtLoad == TypeKind::String;
            }
            std::optional<time::DateTime> const instant = value ? value->value : std::nullopt;
            Value entered;
            if (!asString) {
                if (instant) {
                    entered = Value{*instant};
                }
            } else if (!instant) {
                entered = Value{std::string{}};
            } else if (mode == DateMode::Date) {
                entered = Value{dateTextOf(*instant, offsetMinutes)};
            } else {
                entered = Value{instant->toIso8601()};
            }
            element->touched.set(true);
            session.asUser([&] {
                session.noteWrite();
                element->draft.set(entered);
                element->edited.set(true);
                if (element->bound == nullptr) {
                    element->committed.set(entered);
                }
            });
            runEvent(onChange, ctx, entered);
            runEvent(onCommit, ctx, entered);
        };
        spec.mode = mode;
        spec.offsetMinutes = offsetMinutes;
        fieldOf(node, ctx, element, spec.field);
        commonOf(node, ctx, element, spec.common);
        nameByLabel(spec.common);
        captionOf(spec);
        return ui::dateTimeInput(std::move(spec));
    }
    if (kind == "filePicker") {
        FilePicker spec;
        spec.path =
            slotProp<std::string>([this, element] { return textOf(element->draft.get(), session.formatting()); });
        spec.onPicked = [this, ctx, element, onPick = node.event("onPick")](std::string path) {
            session.asUser([&] {
                session.noteWrite();
                element->draft.set(Value{path});
                element->committed.set(Value{path});
            });
            runEvent(onPick, ctx, Value{path});
        };
        spec.mode = onceText("mode") == "save" ? FilePickerMode::Save : FilePickerMode::Open;
        fieldOf(node, ctx, element, spec.field);
        commonOf(node, ctx, element, spec.common);
        nameByLabel(spec.common);
        return ui::filePicker(std::move(spec));
    }
    if (kind == "menu") {
        Menu spec{.items = menuItems(node.partList("items"), ctx)};
        commonOf(node, ctx, element, spec.common);
        return ui::menu(std::move(spec));
    }
    if (kind == "column" || kind == "row") {
        int const gap = static_cast<int>(onceInt("gap", 0));
        if (std::string const arrangement = arrangedBy(node.id); !arrangement.empty()) {
            return lowerArranged(node, ctx, element, arrangement, kind == "column" ? Axis::Vertical : Axis::Horizontal,
                                 gap);
        }
        if (kind == "column") {
            Column spec{.children = children("children"),
                        .gap = gap,
                        .align = alignOf(node.name("align")),
                        .justify = justifyOf(node.name("justify"))};
            commonOf(node, ctx, element, spec.common);
            return ui::column(std::move(spec));
        }
        Row spec{.children = children("children"),
                 .gap = gap,
                 .align = alignOf(node.name("align")),
                 .justify = justifyOf(node.name("justify"))};
        commonOf(node, ctx, element, spec.common);
        return ui::row(std::move(spec));
    }
    if (kind == "grid") {
        Grid spec{.columns = static_cast<int>(onceInt("columns", 1)),
                  .gap = static_cast<int>(onceInt("gap", 0)),
                  .align = alignOf(node.name("align"))};
        for (DocNodePtr const& cell : node.nodes("children")) {
            ExprPtr const span = cell->prop("span");
            spec.cells.push_back(
                GridCell{.node = lower(*cell, ctx), .span = static_cast<int>(span ? intOf(once(ctx, span), 1) : 1)});
        }
        commonOf(node, ctx, element, spec.common);
        return ui::grid(std::move(spec));
    }
    if (kind == "spacer") {
        Spacer spec;
        commonOf(node, ctx, element, spec.common);
        return ui::spacer(std::move(spec));
    }
    if (kind == "panel") {
        Panel spec{.title = textProp("title"), .padding = static_cast<int>(onceInt("padding", 0))};
        spec.header = child("header");
        spec.child = child("children");
        spec.collapsible = onceBool("collapsible", false);
        spec.collapsed = bindProp<bool>(ctx, node.prop("collapsed"), false, boolOf);
        if (Commands const* const commands = node.event("onToggle")) {
            spec.onToggle = [this, ctx, commands](bool collapsed) { runEvent(commands, ctx, Value{collapsed}); };
        }
        commonOf(node, ctx, element, spec.common);
        return ui::panel(std::move(spec));
    }
    if (kind == "scroll") {
        ExprPtr const axis = node.prop("axis");
        Scroll spec{.child = child("children"), .axis = axis ? axisOf(once(ctx, axis)) : Axis::Vertical};
        commonOf(node, ctx, element, spec.common);
        return ui::scroll(std::move(spec));
    }
    if (kind == "switch") {
        return lowerSwitch(node, ctx);
    }
    if (kind == "tabs") {
        return lowerTabs(node, ctx, element);
    }
    if (kind == "dialog" || kind == "drawer") {
        return lowerDialog(node, ctx, element);
    }
    if (kind == "busy") {
        Busy spec{.active = bindProp<bool>(ctx, node.prop("active"), false, boolOf), .label = textProp("label")};
        commonOf(node, ctx, element, spec.common);
        return ui::busy(std::move(spec));
    }
    if (kind == "forEach" || kind == "table") {
        return lowerRows(node, ctx, element);
    }
    if (kind == "banner") {
        Banner spec{.tone = bindProp<Tone>(ctx, node.prop("tone"), Tone::Info, toneOf), .text = textProp("text")};
        spec.action = actionPart();
        spec.dismissible = onceBool("dismissible", false);
        spec.onDismiss = action("onDismiss");
        commonOf(node, ctx, element, spec.common);
        return ui::banner(std::move(spec));
    }
    if (kind == "badge") {
        Badge spec{.tone = bindProp<Tone>(ctx, node.prop("tone"), Tone::Neutral, toneOf), .text = textProp("text")};
        spec.icon = onceText("icon");
        commonOf(node, ctx, element, spec.common);
        return ui::badge(std::move(spec));
    }
    if (kind == "progress") {
        Progress spec;
        spec.value = slotProp<std::optional<double>>(
            [scope = ctx.scope, bindings = ctx.bindings, expr = node.prop("value")]() -> std::optional<double> {
                Value const value = evalIn(*scope, bindings.get(), expr);
                if (std::int64_t const* const number = value.asInt()) {
                    return static_cast<double>(*number);
                }
                if (Decimal const* const number = value.asDecimal()) {
                    return realOf(number->value);
                }
                return std::nullopt;
            });
        spec.label = textProp("label");
        commonOf(node, ctx, element, spec.common);
        return ui::progress(std::move(spec));
    }
    if (kind == "steps") {
        Steps spec;
        spec.items =
            slotProp<std::vector<Step>>([this, scope = ctx.scope, bindings = ctx.bindings, expr = node.prop("items")] {
                std::vector<Step> steps;
                Value const items = evalIn(*scope, bindings.get(), expr);
                if (ValueList const* const list = items.asList()) {
                    for (Value const& item : *list) {
                        Value const* const label = item.field("label");
                        Value const* const state = item.field("state");
                        std::string const name = state == nullptr ? std::string{} : textOf(*state, {});
                        steps.push_back(
                            Step{.label = label == nullptr ? std::string{} : textOf(*label, session.formatting()),
                                 .state = stepStateOf(name)});
                    }
                }
                return steps;
            });
        spec.current = slotProp<std::optional<std::size_t>>(
            [scope = ctx.scope, bindings = ctx.bindings, expr = node.prop("current")]() -> std::optional<std::size_t> {
                std::int64_t const index = intOf(evalIn(*scope, bindings.get(), expr), -1);
                return index < 0 ? std::nullopt : std::optional<std::size_t>{static_cast<std::size_t>(index)};
            });
        commonOf(node, ctx, element, spec.common);
        return ui::steps(std::move(spec));
    }
    if (kind == "keyValue") {
        KeyValue spec;
        spec.items = slotProp<std::vector<KeyValueItem>>(
            [this, scope = ctx.scope, bindings = ctx.bindings, expr = node.prop("items")] {
                std::vector<KeyValueItem> out;
                Value const items = evalIn(*scope, bindings.get(), expr);
                if (ValueList const* const list = items.asList()) {
                    for (Value const& item : *list) {
                        Value const* const label = item.field("label");
                        Value const* const value = item.field("value");
                        out.push_back(KeyValueItem{
                            .label = label == nullptr ? std::string{} : textOf(*label, session.formatting()),
                            .value = value == nullptr ? std::string{} : textOf(*value, session.formatting())});
                    }
                }
                return out;
            });
        commonOf(node, ctx, element, spec.common);
        return ui::keyValue(std::move(spec));
    }
    if (kind == "emptyState") {
        EmptyState spec{.title = textProp("title"), .text = textProp("text")};
        spec.icon = onceText("icon");
        spec.action = actionPart();
        commonOf(node, ctx, element, spec.common);
        return ui::emptyState(std::move(spec));
    }
    if (kind == "splitter") {
        ExprPtr const orientation = node.prop("orientation");
        Splitter spec{.children = children("children"),
                      .orientation = orientation && axisOf(once(ctx, orientation)) == Axis::Vertical
                                         ? Axis::Vertical
                                         : Axis::Horizontal};
        // No `onResize`: a user's drag stays the splitter's own, and the document's sizes are where it starts.
        if (ExprPtr const sizes = node.prop("sizes")) {
            spec.sizes = bindProp<std::vector<int>>(
                ctx, sizes, {}, [this, path = sizes->path](Value const& value, FormatContext const& /*format*/) {
                    std::optional<std::vector<int>> read = splitterSizesOf(value);
                    if (!read) {
                        report(path,
                               "sizes is " + canonicalJson(value) + "; sizes is a list of numbers, one per pane");
                    }
                    return std::move(read).value_or(std::vector<int>{});
                });
        }
        commonOf(node, ctx, element, spec.common);
        return ui::splitter(std::move(spec));
    }
    if (kind == "collapsible") {
        Collapsible spec{.title = textProp("title"), .open = bindProp<bool>(ctx, node.prop("open"), true, boolOf)};
        if (Commands const* const commands = node.event("onToggle")) {
            spec.onToggle = [this, ctx, commands](bool open) { runEvent(commands, ctx, Value{open}); };
        }
        spec.header = child("header");
        spec.child = child("children");
        commonOf(node, ctx, element, spec.common);
        return ui::collapsible(std::move(spec));
    }
    if (kind == "dropZone") {
        DropZone spec;
        spec.accept = stringsOf(node.prop("accept") ? once(ctx, node.prop("accept")) : Value{});
        spec.multiple = onceBool("multiple", false);
        if (Commands const* const commands = node.event("onDrop")) {
            spec.onDrop = [this, ctx, commands](std::vector<std::string> const& paths) {
                runEvent(commands, ctx, listOfStrings(paths));
            };
        }
        spec.child = child("children");
        commonOf(node, ctx, element, spec.common);
        return ui::dropZone(std::move(spec));
    }
    if (kind == "boundary") {
        return lowerBoundary(node, ctx);
    }
    if (kind == "custom") {
        return lowerCustom(node, ctx);
    }
    if (kind == "customize") {
        return lowerCustomize(node, ctx);
    }
    if (kind == "form") {
        return lowerForm(node, ctx, element);
    }
    if (kind == "collection" || kind == "wizard") {
        return lowerComposite(node, ctx, element);
    }
    report(node.path, "`" + node.kind + "` is a node kind this interpreter does not lower");
    return ui::text(Text{.text = std::string{node.kind}, .role = TextRole::Muted});
}

/// @brief A document field overlay's expressions, evaluated once, as the engine's overlay.
/// @param scope The scope the form is declared in.
/// @param part  The `fields` entry.
/// @param format How values read as text and numbers.
/// @return The overlay.
[[nodiscard]] inline forms::FieldOverlay overlayOf(RtScope const& scope, DocNode const& part,
                                                   FormatContext const& format) {
    auto const read = [&](std::string_view key) -> std::optional<Value> {
        ExprPtr const expr = part.prop(key);
        if (!expr) {
            return std::nullopt;
        }
        return scope.core.runtime().untracked([&] { return evalIn(scope, nullptr, expr); });
    };
    auto const text = [&](std::string_view key) -> std::optional<std::string> {
        auto const value = read(key);
        return value.has_value() && !value->isNull() ? std::optional<std::string>{textOf(*value, format)}
                                                     : std::nullopt;
    };
    auto const flag = [&](std::string_view key) {
        auto const value = read(key);
        bool const* const set = value.has_value() ? value->asBool() : nullptr;
        return set != nullptr && *set;
    };
    forms::FieldOverlay overlay{.omit = flag("omit"), .hidden = flag("hidden")};
    overlay.label = text("label");
    overlay.unit = text("unit");
    overlay.widget = text("widget");
    if (auto const decimals = read("decimals");
        decimals.has_value() && decimals->asInt() != nullptr && *decimals->asInt() >= 0) {
        overlay.decimals = static_cast<std::uint32_t>(std::min<std::int64_t>(*decimals->asInt(), 18));
    }
    if (auto const blankAs = text("blankAs")) {
        overlay.blankAs = *blankAs == "empty" ? forms::BlankAs::Empty : forms::BlankAs::Omit;
    }
    return overlay;
}

/// @brief A lowering context whose scope is held weakly: what an Effect the scope itself owns may keep, so the scope
///        does not keep itself alive.
struct DetachedLower {
    /// @brief The scope, weakly.
    std::weak_ptr<RtScope> scope;
    /// @brief The names bound there.
    std::shared_ptr<Bindings const> bindings;
    /// @brief The innermost boundary.
    std::shared_ptr<BoundaryRuntime> boundary;
};

/// @brief Detaches a lowering context's scope.
/// @param ctx The context.
/// @return The context, its scope held weakly.
[[nodiscard]] inline DetachedLower detachedLower(LowerCtx const& ctx) {
    return DetachedLower{.scope = ctx.scope, .bindings = ctx.bindings, .boundary = ctx.boundary};
}

/// @brief Puts a detached context's scope back.
/// @param held The detached context.
/// @return The context; its scope is null once the scope is gone.
[[nodiscard]] inline LowerCtx attached(DetachedLower const& held) {
    return LowerCtx{.scope = held.scope.lock(), .bindings = held.bindings, .boundary = held.boundary};
}

// Builds the form's session when its element is declared, so its values can be read before the form is drawn.
inline void ScreenCore::wireForm(ElementState& element) {
    DocNode const& node = *element.node;
    RtScope& scope = *element.scope;
    std::string const action{node.name("action")};
    Catalog const& catalog = session.options().catalog;
    std::optional<std::string> const schema = catalog.schema ? catalog.schema(action) : std::nullopt;
    if (!schema.has_value()) {
        report(node.path, "no schema for `" + action + "`: the catalog has none for a `form` to build from");
        return;
    }
    NameRef const* const alias = scope.find(node.name("model"));
    if (alias == nullptr || alias->model == nullptr) {
        report(node.path, "`" + std::string{node.name("model")} + "` is not a model alias");
        return;
    }
    forms::FieldOverlays overlays;
    // `fields` is one record part holding a `field` part per overlaid member.
    for (DocNodePtr const& record : node.partList("fields")) {
        for (DocNodePtr const& part : record->partList("fields")) {
            overlays.insert_or_assign(part->id, overlayOf(scope, *part, session.formatting()));
        }
    }
    auto model = forms::FormModel::fromSchema(action, *schema, overlays);
    if (!model) {
        report(node.path,
               "the form of `" + action + "` does not build: " + model.error().path + ": " + model.error().message);
        return;
    }
    if (node.name("submit") == "explicit") {
        model = model->withSubmitMode(forms::SubmitMode::Explicit);
    } else if (node.name("submit") == "auto") {
        model = model->withSubmitMode(forms::SubmitMode::Automatic);
    }
    element.form = std::make_unique<forms::FormSession>(runtime(), *std::move(model), aliasSubmits(alias->model),
                                                        aliasCalls(alias->model), engineOptions());
    if (ExprPtr const prefill = node.prop("prefill")) {
        // A prefill follows its expression. A value of the record being edited (the first value, or one under the
        // same `key`) keeps the fields the user changed; another record, a new `key`, or any later value of a form
        // without one, loads every field.
        forms::FormSession* const form = element.form.get();
        struct Followed {
            bool loaded = false;
            Value key;
        };
        auto const followed = std::make_shared<Followed>();
        scope.nodes.effect([this, &scope, prefill, key = node.prop("key"), form, followed] {
            Value const value = evalIn(scope, nullptr, prefill);
            if (value.isNull()) {
                return;
            }
            runtime().untracked([&] {
                Value const current = key ? evalIn(scope, nullptr, key) : Value{};
                bool const same = !followed->loaded || (key && current == followed->key);
                followed->loaded = true;
                followed->key = current;
                std::string const json = glz::write_json(valueToJson(value)).value_or(std::string{});
                if (same) {
                    form->refill(json);
                } else {
                    form->prefill(json);
                }
            });
        });
    }
}

inline Node ScreenCore::lowerForm(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    forms::FormSession* const form = element == nullptr ? nullptr : element->form.get();
    if (form == nullptr) {
        return ui::text(Text{.text = std::string{node.name("action")}, .role = TextRole::Muted});
    }
    // The form's events, each run once per change of the count it watches.
    auto const watch = [this, &ctx](Commands const* commands, auto count, auto payload) {
        if (commands == nullptr || commands->empty()) {
            return;
        }
        auto const seen = std::make_shared<std::uint64_t>(runtime().untracked(count));
        ctx.scope->nodes.effect([this, held = detachedLower(ctx), commands, count, payload, seen] {
            std::uint64_t const now = count();
            if (now == *seen) {
                return;
            }
            *seen = now;
            runtime().untracked([&] { runEvent(commands, attached(held), payload()); });
        });
    };
    watch(
        node.event("onSubmit"), [form] { return form->submitAttempts(); },
        [form] {
            std::optional<std::string> const body = form->body();
            return body.has_value() ? jsonTextValue(*body) : Value{};
        });
    watch(
        node.event("onSuccess"), [form] { return form->successCount(); },
        [form] {
            std::optional<std::string> const& reply = form->lastReply();
            return reply.has_value() ? jsonTextValue(*reply) : Value{};
        });
    if (Commands const* const onError = node.event("onError"); onError != nullptr && !onError->empty()) {
        // Each failure is a new exception: run once per failure, with its text.
        auto const seen =
            std::make_shared<std::exception_ptr>(runtime().untracked([form] { return form->lastError(); }));
        ctx.scope->nodes.effect([this, held = detachedLower(ctx), onError, form, seen] {
            std::exception_ptr const error = form->lastError();
            if (error == nullptr || error == *seen) {
                return;
            }
            *seen = error;
            runtime().untracked([&] { runEvent(onError, attached(held), Value{failureText(error)}); });
        });
    }
    forms::FormViewOptions options{.showTitle = false};
    presentForm(node, ctx, options);
    options.title = bindProp<std::string>(ctx, node.prop("title"), std::string{}, stringOf);
    // A field whose overlay names a component is drawn by it; only top-level fields take one, as every overlay.
    for (DocNodePtr const& record : node.partList("fields")) {
        for (DocNodePtr const& overlay : record->partList("fields")) {
            if (!overlay->name("component").empty()) {
                options.overrides.byField(overlay->id,
                                          [this, ctx, field = overlay.get()](forms::FieldView const& view) {
                                              return fieldComponent(view, *field, ctx);
                                          });
            }
        }
    }
    Column column{.children = {forms::formView(*form, std::move(options))}};
    commonOf(node, ctx, element, column.common);
    return ui::column(std::move(column));
}

/// @brief A component's `change` payload as the wire value of the field it draws, read as the `value` prop's
///        declared type.
/// @param payload The payload, as the backend reports it.
/// @param type    The `value` prop's type.
/// @return The wire value, or why the payload is not one.
[[nodiscard]] inline std::expected<forms::detail::engine::Json, std::string> changeWire(std::string const& payload,
                                                                                        std::string_view type) {
    std::string_view text{payload};
    if (type == "string") {
        if (text.starts_with("s:")) {
            text.remove_prefix(2);
        }
        // Parentheses: braces would pick the initializer-list constructor and make a one-element array.
        return forms::detail::engine::Json(std::string{text});
    }
    if (text.starts_with("i:")) {
        text.remove_prefix(2);
    }
    std::optional<forms::detail::engine::Json> json = forms::detail::engine::parseJson(text);
    if (!json) {
        return std::unexpected(std::string{"it is not JSON"});
    }
    bool const fits = type == "json" || json->is_null() || (type == "bool" && json->is_boolean()) ||
                      ((type == "int" || type == "number") && json->is_number()) ||
                      (type == "exact" && (json->is_number() || json->is_string()));
    if (!fits) {
        bool const vowel = type == "int" || type == "exact";
        return std::unexpected("it is not " + std::string{vowel ? "an " : "a "} + std::string{type});
    }
    return *std::move(json);
}

/// @brief What a form fills of a field component's props: the field's value, errors, read-only and required state,
///        and label, each a tracked read of the field.
/// @param prop  The prop's name.
/// @param state The field.
/// @return The read, or empty for a prop the form does not fill.
[[nodiscard]] inline std::function<Value()> formFilled(std::string const& prop, forms::FieldState* state) {
    if (prop == "value") {
        return [state] {
            forms::Encoding const& encoding = state->encoded().get();
            if (!encoding.has_value() || !encoding->has_value()) {
                return Value{};
            }
            return (*encoding)->value.isNull() ? jsonTextValue((*encoding)->json) : (*encoding)->value;
        };
    }
    if (prop == "errors") {
        return [state] {
            std::optional<forms::FieldError> const& error = state->error().get();
            return error.has_value() ? Value{ValueList{Value{error->message}}} : Value{ValueList{}};
        };
    }
    if (prop == "readonly") {
        return [state] { return Value{state->readonly().get()}; };
    }
    if (prop == "required") {
        return [state] { return Value{state->required().get()}; };
    }
    if (prop == "label") {
        return [state] { return Value{state->label()}; };
    }
    return {};
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a prop's name, then its declared type.
inline CustomProp ScreenCore::componentProp(std::string const& prop, std::string const& type,
                                            std::function<Value()> read) {
    if (type == "bool") {
        return CustomProp{.name = prop, .value = slotProp<bool>([this, read = std::move(read)] {
                                            return boolOf(read(), session.formatting());
                                        })};
    }
    if (type == "number") {
        return CustomProp{.name = prop,
                          .value = slotProp<double>([read = std::move(read)] { return componentNumber(read()); })};
    }
    return CustomProp{.name = prop, .value = slotProp<std::string>([this, read = std::move(read), type] {
                                        return componentText(read(), type, session.formatting());
                                    })};
}

inline void ScreenCore::changeField(forms::FieldState& state, forms::FieldSpec const& spec, std::string const& path,
                                    std::string_view valueType, std::string const& payload) {
    auto wire = changeWire(payload, valueType);
    // A base64 member holds a file's bytes, which a document moves inline only under the session's cap.
    if (std::size_t const cap = session.options().maxInlineFileBytes;
        wire && spec.bytes && wire->is_string() && base64Size(wire->get_string()) > cap) {
        report(path, "field " + spec.name + ": " + std::to_string(base64Size(wire->get_string())) +
                         " bytes is over the " + std::to_string(cap) + " inline cap");
        return;
    }
    if (wire) {
        if (auto edited = state.edit(*wire); !edited) {
            wire = std::unexpected(edited.error());
        }
    }
    if (!wire) {
        report(path, "field " + spec.name + ": the component's change does not decode: " + wire.error());
    }
}

inline Node ScreenCore::fieldComponent(forms::FieldView const& view, DocNode const& overlay, LowerCtx const& ctx) {
    std::string const name{overlay.name("component")};
    // A loaded document's field component is declared, with a `value` prop and a `change` event.
    Component const& component = _doc->components.at(name);
    forms::FieldState* const state = &view.state();
    Custom spec{.name = name,
                .fallback = view.defaultControl(),
                .props = {},
                .minVersion =
                    static_cast<int>(std::clamp<std::int64_t>(component.version, 0, std::numeric_limits<int>::max())),
                .onEvent = {}};
    for (auto const& [prop, type] : component.props) {
        if (prop == "value" && type == "json") {
            // The wire JSON itself, so an object's or a collection's every digit reaches the component.
            spec.props.push_back(CustomProp{.name = prop, .value = slotProp<std::string>([state] {
                                                              forms::Encoding const& encoding = state->encoded().get();
                                                              return encoding.has_value() && encoding->has_value()
                                                                         ? (*encoding)->json
                                                                         : std::string{"null"};
                                                          })});
            continue;
        }
        std::function<Value()> read = formFilled(prop, state);
        if (!read) {
            ExprPtr given;
            for (DocNodePtr const& record : overlay.partList("props")) {
                if (ExprPtr const expr = record->prop(prop)) {
                    given = expr;
                }
            }
            read = [scope = ctx.scope, bindings = ctx.bindings, given] {
                return given ? evalIn(*scope, bindings.get(), given) : Value{};
            };
        }
        spec.props.push_back(componentProp(prop, type, std::move(read)));
    }
    spec.onEvent = [this, state, path = overlay.path, fieldSpec = &view.spec(),
                    valueType = component.props.at("value")](std::string const& event, std::string const& payload) {
        if (event == "change") {
            changeField(*state, *fieldSpec, path, valueType, payload);
        }
    };
    return ui::custom(std::move(spec));
}

// Every call goes through the alias's channel, bound for its current instance when the call is made.
inline forms::Submitter ScreenCore::aliasCalls(ModelAlias* alias) {
    return [this, alias](std::string_view actionId, std::string const& body) -> async::Completion<std::string> {
        std::shared_ptr<ModelChannel> const channel = channelOf(*alias, *this);
        if (channel == nullptr) {
            auto [failed, promise] = async::Completion<std::string>::makeSettleable(&runtime().owner());
            promise.reject(std::make_exception_ptr(std::runtime_error{"the node's model alias is not bound"}));
            return std::move(failed);
        }
        return channel->execute(std::string{actionId}, body, CallOptions{});
    };
}

// The calls of a form, collection or wizard: a success other than of @p read, the collection's own query, tells the
// session, which applies the action's effects, as a mutation's success does.
inline forms::Submitter ScreenCore::aliasSubmits(ModelAlias* alias, std::string read) {
    return [this, alias, call = aliasCalls(alias), read = std::move(read)](
               std::string_view actionId, std::string body) -> async::Completion<std::string> {
        async::Completion<std::string> completion = call(actionId, std::move(body));
        if (actionId != read) {
            completion.then([this, alive = std::weak_ptr<bool>{_alive}, alias, typeId = alias->decl->type,
                             action = std::string{actionId}](std::string const& reply) {
                if (alive.expired()) {
                    return;
                }
                auto json = parseJson(reply);
                auto value = json ? valueFromJson(*json) : std::unexpected(json.error());
                std::string const token = runtime().untracked([&] { return alias->token->get(); });
                session.actionSucceeded(typeId, action, value ? *value : Value{}, ReplyUse::Applied, token);
            });
        }
        return completion;
    };
}

// The session's locale, translations and display zone, as every form the engine builds here reads them.
inline forms::FormSessionOptions ScreenCore::engineOptions() const {
    FormatContext const& format = session.formatting();
    forms::FormSessionOptions options{};
    options.locale = format.numeric;
    options.bcp47 = format.locale;
    options.translations = format.translations;
    options.displayOffsetMinutes = format.utcOffsetMinutes;
    return options;
}

// Builds a collection's or wizard's session when its element is declared. The node's `action` is the collection's
// query or the wizard's first step; the catalog answers the document for it, and every call (the query, the actions,
// every step's submission) goes through the node's model alias.
inline void ScreenCore::wireComposite(ElementState& element) {
    DocNode const& node = *element.node;
    RtScope const& scope = *element.scope;
    bool const collection = node.kind == "collection";
    std::string const action{node.name("action")};
    Catalog const& catalog = session.options().catalog;
    auto const& lookup = collection ? catalog.view : catalog.wizard;
    std::optional<std::string> const document = lookup ? lookup(action) : std::nullopt;
    if (!document.has_value()) {
        report(node.path, std::string{"no "} + (collection ? "view" : "wizard") + " document for `" + action +
                              "`: the catalog has none for a `" + node.kind + "` to build from");
        return;
    }
    NameRef const* const alias = scope.find(node.name("model"));
    if (alias == nullptr || alias->model == nullptr) {
        report(node.path, "`" + std::string{node.name("model")} + "` is not a model alias");
        return;
    }
    forms::SchemaLookup const schemas = catalog.schema ? forms::SchemaLookup{catalog.schema} : forms::SchemaLookup{};
    forms::Submitter const call = aliasCalls(alias->model);
    if (collection) {
        auto model = forms::CollectionModel::fromSchema(*document);
        if (!model) {
            report(node.path, "the collection of `" + action + "` does not build: " + model.error().path + ": " +
                                  model.error().message);
            return;
        }
        if (model->query() != action) {
            report(node.path, "the view document of `" + action + "` lists `" + std::string{model->query()} + "`");
            return;
        }
        std::string query{model->query()};
        element.collection = std::make_unique<forms::CollectionSession>(runtime(), *std::move(model), schemas,
                                                                        aliasSubmits(alias->model, std::move(query)),
                                                                        call, engineOptions());
        return;
    }
    // The wizard's id, the stem of its translation keys, is the action it starts with.
    auto model = forms::WizardModel::fromSchema(action, *document);
    if (!model) {
        report(node.path,
               "the wizard of `" + action + "` does not build: " + model.error().path + ": " + model.error().message);
        return;
    }
    if (model->steps().front().action != action) {
        report(node.path,
               "the wizard document of `" + action + "` starts with `" + model->steps().front().action + "`");
        return;
    }
    try {
        element.wizard = std::make_unique<forms::WizardSession>(runtime(), *std::move(model), schemas,
                                                                aliasSubmits(alias->model), call, engineOptions());
    } catch (std::invalid_argument const& error) {
        report(node.path, error.what());
    }
}

// The presentation keys a form, a collection and a wizard share, but the title: the label of the explicit-mode button
// and the status line naming it, bound in the node's scope; whether the status line shows, the grid's columns and the
// field layout, as read at load.
inline void ScreenCore::presentForm(DocNode const& node, LowerCtx const& ctx, forms::FormViewOptions& options) {
    if (ExprPtr const label = node.prop("submitLabel")) {
        options.submitLabel = bindProp<std::string>(ctx, label, std::string{}, stringOf);
    }
    if (auto const shown = node.flags.find("showStatus"); shown != node.flags.end()) {
        options.showStatus = shown->second;
    }
    if (ExprPtr const columns = node.prop("columns"); columns != nullptr && columns->literal.asInt() != nullptr) {
        options.gridColumns = static_cast<int>(*columns->literal.asInt());
        options.flatGridColumns = options.gridColumns;
    }
    if (node.name("fieldLayout") == "inline") {
        options.fieldLayout = forms::FieldLayout::Inline;
    }
}

inline Node ScreenCore::lowerComposite(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    // The editor's and the steps' titles are the dialog's and the header's; the forms do not repeat them. A `title`
    // replaces the view's own heading.
    forms::FormViewOptions form{};
    form.showTitle = false;
    presentForm(node, ctx, form);
    Prop<std::string> title = bindProp<std::string>(ctx, node.prop("title"), std::string{}, stringOf);
    Node view;
    if (element != nullptr && element->collection != nullptr) {
        forms::CollectionViewOptions options{};
        options.form = std::move(form);
        options.title = std::move(title);
        view = forms::collectionView(*element->collection, std::move(options));
    } else if (element != nullptr && element->wizard != nullptr) {
        forms::WizardViewOptions options{};
        options.form = std::move(form);
        options.title = std::move(title);
        view = forms::wizardView(*element->wizard, options);
    } else {
        return ui::text(Text{.text = std::string{node.name("action")}, .role = TextRole::Muted});
    }
    Column column{.children = {std::move(view)}};
    commonOf(node, ctx, element, column.common);
    return ui::column(std::move(column));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): an overlay's scope, content, actions and keys are one lowering.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): an overlay's scope, content, actions and keys are one lowering.
inline Node ScreenCore::lowerDialog(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    Prop<bool> open = node.prop("open") ? bindProp<bool>(ctx, node.prop("open"), false, boolOf)
                                        : slotProp<bool>([element] { return element->open.get(); });
    Node body = lazy([this, ctx, &node, element]() -> Node {
        auto inner = std::make_shared<RtScope>(*this, ctx.scope, nullptr, ctx.scope->nodes.depth() + 2);
        inner->owner = element;
        addPending(1);
        inner->submitted = &element->submitted;
        inner->showErrors = std::string{node.name("showErrors")};
        std::vector<DocNode const*> content;
        for (DocNodePtr const& child : node.nodes("children")) {
            content.push_back(child.get());
        }
        std::vector<Commands const*> lists;
        for (std::string_view const key : {"default", "cancel"}) {
            if (Commands const* const commands = node.event(key)) {
                lists.push_back(commands);
            }
        }
        for (DocNodePtr const& action : node.partList("actions")) {
            if (Commands const* const commands = action->event("onClick")) {
                lists.push_back(commands);
            }
        }
        declare(*inner, node.scope ? &*node.scope : nullptr, content, lists);
        element->inner.set(inner.get());
        LowerCtx const innerCtx{.scope = inner, .bindings = ctx.bindings, .boundary = ctx.boundary};
        std::vector<Node> children;
        children.reserve(content.size());
        for (DocNode const* const child : content) {
            children.push_back(lower(*child, innerCtx));
        }
        // The overlay's actions end its scoped content, so they bind in its scope; the mount puts them in its footer.
        if (ActionModels actions = lowerActionParts(node.partList("actions"), innerCtx); !actions.empty()) {
            children.push_back(actionBarOf(std::move(actions)));
        }
        std::vector<KeyBinding> keys;
        if (Commands const* const commands = node.event("default")) {
            keys.push_back(KeyBinding{
                .chord = "Enter", .onPress = [this, innerCtx, commands] { runEvent(commands, innerCtx, Value{}); }});
        }
        if (Commands const* const commands = node.event("cancel")) {
            keys.push_back(KeyBinding{
                .chord = "Esc", .onPress = [this, innerCtx, commands] { runEvent(commands, innerCtx, Value{}); }});
        }
        return holding(std::move(children), inner, std::move(keys));
    });
    Action onDismiss = [this, ctx, element, commands = node.event("onDismiss")] {
        session.asUser([&] {
            session.noteWrite();
            element->open.set(false);
        });
        runEvent(commands, ctx, Value{});
    };
    if (node.kind == "drawer") {
        ExprPtr const side = node.prop("side");
        Drawer spec{
            .open = std::move(open),
            .title = bindProp<std::string>(ctx, node.prop("title"), std::string{}, stringOf),
            .child = std::move(body),
            .onDismiss = std::move(onDismiss),
            .side = side && textOf(once(ctx, side), session.formatting()) == "start" ? Side::Start : Side::End};
        commonOf(node, ctx, element, spec.common);
        return ui::drawer(std::move(spec));
    }
    Dialog spec{.open = std::move(open),
                .title = bindProp<std::string>(ctx, node.prop("title"), std::string{}, stringOf),
                .child = std::move(body),
                .onDismiss = std::move(onDismiss)};
    commonOf(node, ctx, element, spec.common);
    return ui::dialog(std::move(spec));
}

inline Node ScreenCore::lowerSwitch(DocNode const& node, LowerCtx const& ctx) {
    std::vector<ExprPtr> whens;
    Switch spec;
    std::int64_t index = 0;
    for (DocNodePtr const& part : node.partList("cases")) {
        whens.push_back(part->prop("when"));
        DocNode const* const casePart = part.get();
        spec.cases.push_back(SwitchCase{
            .key = Key{index++},
            .node = lazy([this, ctx, casePart]() -> Node {
                auto inner = std::make_shared<RtScope>(*this, ctx.scope, nullptr, ctx.scope->nodes.depth() + 2);
                std::vector<DocNode const*> content;
                for (DocNodePtr const& child : casePart->nodes("children")) {
                    content.push_back(child.get());
                }
                declare(*inner, casePart->scope ? &*casePart->scope : nullptr, content, {});
                LowerCtx const innerCtx{.scope = inner, .bindings = ctx.bindings, .boundary = ctx.boundary};
                std::vector<Node> children;
                children.reserve(content.size());
                for (DocNode const* const child : content) {
                    children.push_back(lower(*child, innerCtx));
                }
                return holding(std::move(children), inner);
            })});
    }
    spec.selector = slotProp<Key>(
        [scope = ctx.scope, bindings = ctx.bindings, selected = node.prop("on"), whens = std::move(whens)] {
            Value const value = evalIn(*scope, bindings.get(), selected);
            for (std::size_t at = 0; at < whens.size(); ++at) {
                if (evalIn(*scope, bindings.get(), whens.at(at)) == value) {
                    return Key{static_cast<std::int64_t>(at)};
                }
            }
            return Key{std::int64_t{-1}};
        });
    auto const fallback = node.nodes("default");
    spec.fallback = fallback.empty() ? Node{} : lowerAll(fallback, ctx);
    commonOf(node, ctx, nullptr, spec.common);
    return ui::switchOf(std::move(spec));
}

// A document names tabs by id; the view tree by index, so the ids map to indices once. A bound `selected` is the
// controlled index: a pick runs `onChoose` and the page follows `selected` alone. Without it the node keeps the
// user's pick itself, as a bound index too, whose page follows the pick: a constant would move only the highlight.
inline Node ScreenCore::lowerTabs(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    Tabs spec;
    auto ids = std::make_shared<std::vector<std::string>>();
    for (DocNodePtr const& tab : node.partList("tabs")) {
        auto const nodes = tab->nodes("children");
        ids->push_back(tab->id);
        spec.tabs.push_back(Tab{.label = bindProp<std::string>(ctx, tab->prop("label"), std::string{}, stringOf),
                                .node = nodes.empty() ? Node{} : lowerAll(nodes, ctx)});
    }
    ExprPtr const selected = node.prop("selected");
    Commands const* const onChoose = node.event("onChoose");
    bool const controlled = selected && (selected->kind != ExprKind::Literal || !selected->literal.isNull());
    auto const picked = std::make_shared<reactive::Signal<std::size_t>>(runtime(), std::size_t{0});
    if (controlled) {
        // A value that names no tab shows the first, and is unknownReported once.
        auto const unknownReported = std::make_shared<bool>(false);
        spec.selected = [this, scope = ctx.scope, bindings = ctx.bindings, selected, ids, unknownReported,
                         path = pointerAppend(node.path, "selected")]() -> std::size_t {
            Value const value = evalIn(*scope, bindings.get(), selected);
            std::string const* const named = value.asString();
            auto const found = named == nullptr ? ids->end() : std::ranges::find(*ids, *named);
            if (found != ids->end()) {
                return static_cast<std::size_t>(std::distance(ids->begin(), found));
            }
            if (!*unknownReported) {
                *unknownReported = true;
                report(path, canonicalJson(value) + " names no tab; the first is shown");
            }
            return 0;
        };
    } else {
        spec.selected = [picked] { return picked->get(); };
    }
    spec.onSelect = [this, ctx, onChoose, ids, picked, controlled, element](std::size_t index) {
        if (!controlled) {
            picked->set(index);
            if (element != nullptr && index < ids->size()) {
                element->draft.set(Value{ids->at(index)});
            }
        }
        if (onChoose != nullptr && index < ids->size()) {
            runEvent(onChoose, ctx, Value{ids->at(index)});
        }
    };
    commonOf(node, ctx, element, spec.common);
    return ui::tabs(std::move(spec));
}

// Focuses the declaration the command's target resolved to: of several candidates (cases of one switch), the one
// that is mounted. Each candidate is keyed by the command's own enclosing row keys, as many as enclose the
// candidate; with `row`, the innermost of them is the row's key instead.
inline bool ScreenCore::focusTarget(Command const& command, RtScope const& scope, Bindings const* bindings) {
    if (!focusElement) {
        return false;
    }
    auto const found = _doc->focusTargets.find(command.path);
    if (found == _doc->focusTargets.end()) {
        return focusElement(command.target);
    }
    FocusPlace const place = focusPlace(scope);
    std::optional<std::string> row;
    if (found->second.row) {
        Value const value = evalIn(scope, bindings, command.extra);
        if (value.isNull()) {
            return false;
        }
        row = canonicalJson(valueOfKey(keyOf(value)));
    }
    for (FocusCandidate const& candidate : found->second.candidates) {
        std::size_t const kept = row ? candidate.rows - 1 : candidate.rows;
        if (candidate.rows == 0 && row) {
            continue;
        }
        if (kept > place.rows.size()) {
            continue;
        }
        FocusPlace candidatePlace{
            .prefix = place.prefix,
            .rows = {place.rows.begin(), std::next(place.rows.begin(), static_cast<std::ptrdiff_t>(kept))}};
        if (row) {
            candidatePlace.rows.push_back(*row);
        }
        if (focusElement(focusKey(candidatePlace, candidate.path))) {
            return true;
        }
    }
    return false;
}

inline std::shared_ptr<RtScope> ScreenCore::rowScope(DocNode const& node, LowerCtx const& ctx,
                                                     reactive::Signal<Value> const& row,
                                                     std::vector<DocNode const*> const& content, std::string key) {
    auto scope = std::make_shared<RtScope>(*this, ctx.scope, &row, ctx.scope->nodes.depth() + 2);
    scope->rowKey = std::move(key);
    declare(*scope, node.scope ? &*node.scope : nullptr, content, {});
    return scope;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): a list of rows and a table.
inline Node ScreenCore::lowerRows(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    ExprPtr const rowsExpr = node.prop("rows");
    if (!rowsExpr) {
        report(node.path, "a model-backed `table` needs the table engine, which this interpreter does not lower yet");
        return ui::text(Text{.text = std::string{"table"}, .role = TextRole::Muted});
    }
    std::function<std::vector<Value>()> rows = [scope = ctx.scope, bindings = ctx.bindings, rowsExpr] {
        Value const value = evalIn(*scope, bindings.get(), rowsExpr);
        ValueList const* const items = value.asList();
        return items == nullptr ? std::vector<Value>{} : std::vector<Value>{items->begin(), items->end()};
    };
    auto keyFn = [scope = ctx.scope, bindings = ctx.bindings, keyExpr = node.prop("key")](Value const& row) -> Key {
        if (Value const* const heading = row.field(kGroupHeadingField); heading != nullptr && heading->asString()) {
            return Key{*heading->asString()};
        }
        if (!keyExpr) {
            Value const rowKey = rowId(row);
            return keyOf(rowKey.isNull() ? row : rowKey);
        }
        ScopeEnv const env{*scope, bindings.get(), &row};
        return keyOf(evaluate(*keyExpr, env));
    };
    if (node.kind == "forEach") {
        std::vector<DocNode const*> content;
        for (DocNodePtr const& child : node.nodes("children")) {
            content.push_back(child.get());
        }
        auto rowView = [this, ctx, &node, content, keyFn](reactive::Signal<Value> const& row) -> Node {
            std::string key = runtime().untracked([&] { return canonicalJson(valueOfKey(keyFn(row.peek()))); });
            auto scope = rowScope(node, ctx, row, content, std::move(key));
            LowerCtx const rowCtx{.scope = scope, .bindings = ctx.bindings, .boundary = ctx.boundary};
            std::vector<Node> children;
            children.reserve(content.size());
            for (DocNode const* const child : content) {
                children.push_back(lower(*child, rowCtx));
            }
            return holding(std::move(children), scope);
        };
        ExprPtr const axis = node.prop("axis");
        ExprPtr const gap = node.prop("gap");
        Common common;
        commonOf(node, ctx, element, common);
        return ui::forEach<Value>(std::move(rows), std::move(keyFn), std::move(rowView),
                                  axis ? axisOf(once(ctx, axis)) : Axis::Vertical,
                                  gap ? static_cast<int>(intOf(once(ctx, gap), 0)) : 0, common);
    }
    // The table's rows, sorted: the element's own view, which its element values read too, unless names bound
    // where the table is lowered (a component's `props`) reach its rows, which the element cannot see.
    reactive::Computed<TableView> const* view = element == nullptr ? nullptr : element->view;
    if (view != nullptr && ctx.bindings != nullptr && !ctx.bindings->empty()) {
        view = &ctx.scope->nodes.computed(
            [this, element, bindings = ctx.bindings] { return tableView(*element, bindings.get()); });
    }
    if (view != nullptr) {
        // The page's rows, each group's heading entry before the first of its rows the page holds, so a group split
        // across pages gets its heading on both.
        rows = [view] {
            TableView const& shown = view->get();
            std::vector<Value> entries;
            entries.reserve(shown.last - shown.first);
            for (std::size_t index = shown.first; index < shown.last; ++index) {
                if (!shown.runOf.empty() &&
                    (index == shown.first || shown.runOf.at(index) != shown.runOf.at(index - 1))) {
                    entries.push_back(shown.runs.at(shown.runOf.at(index)));
                }
                entries.push_back(shown.rows.at(index));
            }
            return entries;
        };
    }
    std::vector<TableColumn> columns;
    std::vector<DocNode const*> cells;
    std::vector<std::string> fields;
    // One entry per column, in the same order as `cells`/`fields`. Captured by `cellsOf` below (not `columns`
    // itself, which is moved into `ui::table<Value>(...)` further down and so cannot be read from inside a lambda
    // that outlives this function) so the default-cell closure can apply each column's role to its generated cell.
    std::vector<TextRole> roles;
    FormatContext const& format = session.formatting();
    // The table's `valign` is the default of every column that gives none; the loader checked both names.
    Align const tableValign = cellPlaceNamed(node.name("valign")).value_or(Align::Center);
    if (ExprPtr const derived = node.prop("columns")) {
        Value const list = once(ctx, derived);
        // A computed place that names none is reported once for the table, whichever column and key it was.
        bool placeReported = false;
        auto const placeOf = [&](Value const& item, std::string_view key, Align fallback) {
            Value const* const given = item.field(key);
            if (given == nullptr || given->isNull()) {
                return fallback;
            }
            std::string const* const name = given->asString();
            std::optional<Align> const placed = name == nullptr ? std::nullopt : cellPlaceNamed(*name);
            if (!placed && !placeReported) {
                placeReported = true;
                report(derived->path, "a column's `" + std::string{key} + "` is " + canonicalJson(*given) +
                                          "; a cell's place is start, center or end");
            }
            return placed.value_or(fallback);
        };
        if (ValueList const* const items = list.asList()) {
            for (Value const& item : *items) {
                Value const* const field = item.field("id");
                Value const* const label = item.field("label");
                fields.push_back(field == nullptr ? std::string{} : textOf(*field, format));
                columns.push_back(
                    TableColumn{.label = textOf(label != nullptr ? *label : Value{fields.back()}, format),
                                .align = placeOf(item, "align", Align::Start),
                                .valign = placeOf(item, "valign", tableValign)});
                cells.push_back(nullptr);
                roles.push_back(TextRole::Normal);
            }
        }
    }
    // A part's column index, which a header click reports, counts the derived columns before it.
    std::size_t const derivedCount = columns.size();
    for (DocNodePtr const& part : node.partList("columns")) {
        ExprPtr const label = part->prop("label");
        ExprPtr const width = part->prop("width");
        ExprPtr const role = part->prop("role");
        TextRole columnRole = TextRole::Normal;
        if (role) {
            // Read once, with the rest of the column; a value that names no role is reported, as a text's is.
            Value const value = once(ctx, role);
            std::optional<TextRole> const named = roleNamed(textOf(value, format));
            if (!named) {
                report(role->path, "role is " + canonicalJson(value) + "; " + std::string{kRoleNames});
            }
            columnRole = named.value_or(TextRole::Normal);
        }
        columns.push_back(TableColumn{.label = label ? textOf(once(ctx, label), format) : std::string{},
                                      .width = width ? sizingOf(once(ctx, width)) : Sizing::content(),
                                      .role = columnRole,
                                      .align = cellPlaceNamed(part->name("align")).value_or(Align::Start),
                                      .valign = cellPlaceNamed(part->name("valign")).value_or(tableValign),
                                      .sortable = part->flag("sortable")});
        cells.push_back(part.get());
        fields.emplace_back(part->id);
        roles.push_back(columnRole);
    }
    auto registry = std::make_shared<std::map<std::string, std::weak_ptr<RtScope>, std::less<>>>();
    // A value that names no tone is reported once for the table, whichever row and value it was.
    auto toneReported = std::make_shared<bool>(false);
    auto cellsOf = [this, ctx, &node, cells, fields, roles, registry, keyFn,
                    toneReported](reactive::Signal<Value> const& row) {
        if (row.peek().field(kGroupHeadingField) != nullptr) {
            return headingRow(node, ctx, row,
                              runtime().untracked([&] { return canonicalJson(valueOfKey(keyFn(row.peek()))); }));
        }
        std::vector<DocNode const*> content;
        for (DocNode const* const column : cells) {
            if (column != nullptr) {
                for (DocNodePtr const& cell : column->nodes("cell")) {
                    content.push_back(cell.get());
                }
            }
        }
        std::string key = runtime().untracked([&] { return canonicalJson(valueOfKey(keyFn(row.peek()))); });
        auto scope = rowScope(node, ctx, row, content, key);
        (*registry)[std::move(key)] = scope;
        LowerCtx const rowCtx{.scope = scope, .bindings = ctx.bindings, .boundary = ctx.boundary};
        std::vector<Node> out;
        for (std::size_t index = 0; index < cells.size(); ++index) {
            DocNode const* const column = cells.at(index);
            auto const nodes = column == nullptr ? std::span<DocNodePtr const>{} : column->nodes("cell");
            if (!nodes.empty()) {
                out.push_back(lowerAll(nodes, rowCtx));
                continue;
            }
            out.push_back(ui::text(Text{.text = slotProp<std::string>([this, &row, field = fields.at(index)] {
                                            Value const* const value = row.get().field(field);
                                            return value == nullptr ? std::string{}
                                                                    : textOf(*value, session.formatting());
                                        }),
                                        .role = roles.at(index)}));
        }
        if (!out.empty()) {
            // The row's first cell owns the row's scope, which lives as long as the row's node: a cell is placed by
            // its column, so the scope is not held by a wrapping container, which the column would place instead.
            auto owner = std::make_shared<std::pair<Node, std::shared_ptr<RtScope>> const>(out.front(), scope);
            out.front() = Node{owner, owner->first.get()};
        }
        TableRowView view{.cells = std::move(out), .tone = std::nullopt};
        // The row's name, in its own scope: what its select box and assistive technology read.
        if (ExprPtr const labelExpr = node.prop("rowLabel")) {
            view.label = bindProp<std::string>(rowCtx, labelExpr, std::string{}, stringOf);
        }
        if (ExprPtr const toneExpr = node.prop("rowTone")) {
            view.tone = slotProp<std::optional<Tone>>(
                [this, scope, bindings = ctx.bindings, toneExpr, toneReported]() -> std::optional<Tone> {
                    return rowToneOf(evalIn(*scope, bindings.get(), toneExpr), toneExpr->path, *toneReported);
                });
        }
        return view;
    };
    TableOptions options;
    options.showHeader = node.flag("showHeader", true);
    options.selectColumn = node.flag("selectColumn");
    if (ExprPtr const selectAll = node.prop("selectAllLabel")) {
        options.selectAllLabel = bindProp<std::string>(ctx, selectAll, std::string{}, stringOf);
    }
    if (ExprPtr const threshold = node.prop("virtualizeThreshold")) {
        options.virtualize.threshold =
            static_cast<std::size_t>(std::max<std::int64_t>(0, intOf(once(ctx, threshold), 2000)));
    }
    if (ExprPtr const overscan = node.prop("virtualizeOverscan")) {
        options.virtualize.overscan =
            static_cast<std::size_t>(std::max<std::int64_t>(0, intOf(once(ctx, overscan), 10)));
    }
    std::string const mode =
        node.prop("selectionMode") ? textOf(once(ctx, node.prop("selectionMode")), format) : std::string{};
    options.selectionMode = SelectionMode::None;
    if (mode == "single") {
        options.selectionMode = SelectionMode::Single;
    } else if (mode == "multiple") {
        options.selectionMode = SelectionMode::Multiple;
    }
    // The selection the table shows: the bound `selection`, or the table's own. A slot even for a literal list, so
    // a bound selection is fixed until the document writes it, and a user's change the document leaves unwritten
    // snaps back. A bound value that is neither a list nor `null` is reported once.
    auto selectionReported = std::make_shared<bool>(false);
    options.selection = slotProp<std::vector<Key>>([this, element, selectionReported, path = node.path] {
        std::vector<Key> keys;
        if (element->bound != nullptr) {
            Value const bound = element->bound->get();
            if (!bound.isNull() && bound.asList() == nullptr && !*selectionReported) {
                *selectionReported = true;
                report(pointerAppend(path, "selection"),
                       "selection is " + canonicalJson(bound) + "; a table's selection is a list of keys");
            }
        }
        if (ValueList const* const items = tableSelection(*element).asList()) {
            for (Value const& item : *items) {
                keys.push_back(keyOf(item));
            }
        }
        return keys;
    });
    // A user's change: the table's own selection takes it when no key binds one, then `onSelectionChange` runs with
    // `event` the new selection, in the table's scope, in the same batch. The backend holds one page: a `Replace`
    // is the whole new selection, and an `Extend` keeps the selected keys of the rows on other pages, before the
    // reported ones. A key whose row `rows` no longer has drops on any user change.
    options.onSelectionChange = [this, ctx, element, view, keyFn, commands = node.event("onSelectionChange")](
                                    std::vector<Key> const& keys, SelectionChange change) {
        ValueList items;
        if (change == SelectionChange::Extend && view != nullptr) {
            items = runtime().untracked([&] { return selectedOffPage(view->get(), tableSelection(*element), keyFn); });
        }
        // A heading is never selected. A virtualized backend knows the keys of rows it has not realized but not
        // which of them are headings, so its Ctrl+A or range can name one.
        for (Key const& key : keys) {
            if (std::string const* const text = std::get_if<std::string>(&key);
                text != nullptr && text->starts_with("\x1Fgroup:")) {
                continue;
            }
            items.push_back(valueOfKey(key));
        }
        Value const selected{std::move(items)};
        runtime().batch([&] {
            if (element->bound == nullptr) {
                session.asUser([&] {
                    session.noteWrite();
                    element->selection.set(selected);
                });
            }
            runEvent(commands, ctx, selected);
        });
    };
    // The header marks the sort's first key; a sort that names no sortable column marks none.
    auto const columnIndex = [&node, derivedCount](std::string_view columnId) -> std::optional<std::size_t> {
        auto const parts = node.partList("columns");
        auto const found =
            std::ranges::find_if(parts, [columnId](DocNodePtr const& part) { return part->id == columnId; });
        if (found == parts.end()) {
            return std::nullopt;
        }
        return derivedCount + static_cast<std::size_t>(std::distance(parts.begin(), found));
    };
    options.sort = slotProp<std::optional<TableSort>>([element, &node, columnIndex]() -> std::optional<TableSort> {
        std::optional<std::vector<SortKey>> const keys = sortKeysOf(node, tableSort(*element));
        if (!keys || keys->empty()) {
            return std::nullopt;
        }
        std::optional<std::size_t> const index = columnIndex(keys->front().column->id);
        if (!index) {
            return std::nullopt;
        }
        return TableSort{.column = *index,
                         .direction = keys->front().descending ? SortDirection::Descending : SortDirection::Ascending};
    });
    // A header click makes its column the single key: descending when it already is the first key ascending,
    // ascending otherwise. The table's own sort takes it when no key binds one, then `onSortChange` runs with
    // `event` the new sort.
    options.onSort = [this, ctx, element, &node, derivedCount, commands = node.event("onSortChange")](
                         std::size_t index) {
        auto const parts = node.partList("columns");
        if (index < derivedCount || index - derivedCount >= parts.size() ||
            !parts.subspan(index - derivedCount).front()->flag("sortable")) {
            return;
        }
        std::string const& columnId = parts.subspan(index - derivedCount).front()->id;
        Value const current = runtime().untracked([&] { return tableSort(*element); });
        std::optional<std::vector<SortKey>> const keys = sortKeysOf(node, current);
        bool const descending =
            keys && !keys->empty() && keys->front().column->id == columnId && !keys->front().descending;
        ValueRecord key;
        key.insert_or_assign("column", Value{columnId});
        key.insert_or_assign("direction", Value{std::string{descending ? "desc" : "asc"}});
        Value const next{ValueList{Value{std::move(key)}}};
        runtime().batch([&] {
            if (element->sortBound == nullptr) {
                session.asUser([&] {
                    session.noteWrite();
                    element->sort.set(next);
                });
            }
            runEvent(commands, ctx, next);
        });
    };
    options.onActivate = [this, ctx, element, registry, commands = node.event("onActivate")](Key const& key) {
        Value const activated = valueOfKey(key);
        session.asUser([&] {
            session.noteWrite();
            element->activeKey.set(activated);
        });
        auto const found = registry->find(canonicalJson(activated));
        std::shared_ptr<RtScope> const scope = found == registry->end() ? nullptr : found->second.lock();
        LowerCtx const rowCtx{.scope = scope ? scope : ctx.scope, .bindings = ctx.bindings, .boundary = ctx.boundary};
        runEvent(commands, rowCtx, activated);
    };
    commonOf(node, ctx, element, options.common);
    std::span<DocNodePtr const> const empty = node.nodes("empty");
    if (empty.empty()) {
        return ui::table<Value>(std::move(columns), std::move(rows), std::move(keyFn), std::move(cellsOf),
                                std::move(options));
    }
    // With `empty`, a column holds the table and the content shown in its place while `rows` is an empty list; a
    // `null` (a query not answered yet) shows the table. The column is placed as the table would be: it takes the
    // node's visibility and layout, and every other common property stays on the table, so its test id and the
    // element id `focus` finds name the table.
    auto const listed = [scope = ctx.scope, bindings = ctx.bindings, rowsExpr] {
        ValueList const* const items = evalIn(*scope, bindings.get(), rowsExpr).asList();
        return items == nullptr || !items->empty();
    };
    Column wrapper;
    wrapper.common.visible = std::move(options.common.visible);
    wrapper.common.layout = options.common.layout;
    options.common.visible = slotProp<bool>(listed);
    Column content{.children = {lowerAll(empty, ctx)}};
    content.common.visible = slotProp<bool>([listed] { return !listed(); });
    wrapper.children.push_back(ui::table<Value>(std::move(columns), std::move(rows), std::move(keyFn),
                                                std::move(cellsOf), std::move(options)));
    wrapper.children.push_back(ui::column(std::move(content)));
    return ui::column(std::move(wrapper));
}

inline TableRowView ScreenCore::headingRow(DocNode const& node, LowerCtx const& ctx,
                                            reactive::Signal<Value> const& row, std::string key) {
    auto const heading = node.partList("group").front()->nodes("heading");
    std::vector<DocNode const*> content;
    content.reserve(heading.size());
    for (DocNodePtr const& child : heading) {
        content.push_back(child.get());
    }
    auto scope = rowScope(node, ctx, row, content, std::move(key));
    LowerCtx const rowCtx{.scope = scope, .bindings = ctx.bindings, .boundary = ctx.boundary};
    // The cell owns the heading's scope, for as long as the row's node lives, as a data row's first cell does.
    auto owner = std::make_shared<std::pair<Node, std::shared_ptr<RtScope>> const>(lowerAll(heading, rowCtx), scope);
    return TableRowView{.cells = {Node{owner, owner->first.get()}}, .tone = std::nullopt, .heading = true};
}

inline std::optional<Tone> ScreenCore::rowToneOf(Value const& value, std::string const& path, bool& noted) {
    if (value.isNull()) {
        return std::nullopt;
    }
    static constexpr std::array<std::pair<std::string_view, Tone>, 5> kTones = {{
        {"neutral", Tone::Neutral},
        {"info", Tone::Info},
        {"ok", Tone::Ok},
        {"warn", Tone::Warn},
        {"err", Tone::Err},
    }};
    if (std::string const* const text = value.asString()) {
        // A loop, not std::ranges::find: an array's iterator is a pointer on some standard libraries and not on
        // others, so `auto` there reads differently to each.
        for (auto const& [name, tone] : kTones) {
            if (name == *text) {
                return tone;
            }
        }
    }
    if (!noted) {
        noted = true;
        report(path, "rowTone is " + canonicalJson(value) + "; a tone is neutral, info, ok, warn or err");
    }
    return std::nullopt;
}

inline void ScreenCore::ownQueriesOf(ExprPtr const& expr, RtScope const& scope, BoundaryRuntime& boundary,
                                     std::set<std::string, std::less<>>& seen) {
    if (!expr) {
        return;
    }
    bool const control = expr->kind == ExprKind::Query || expr->kind == ExprKind::Pending ||
                         expr->kind == ExprKind::Error || expr->kind == ExprKind::ErrorKind;
    if (control || expr->kind == ExprKind::Ref) {
        NameRef const* const found = scope.find(expr->name);
        if (found != nullptr && found->category == NameCategory::Query && seen.insert(expr->name).second) {
            boundary.queries.push_back(found->query);
        }
        if (expr->kind == ExprKind::Ref && found != nullptr && found->category == NameCategory::Let &&
            seen.insert("let:" + expr->name).second) {
            ownQueriesOf(found->letExpr, scope, boundary, seen);
        }
    }
    for (ExprPtr const& arg : expr->args) {
        ownQueriesOf(arg, scope, boundary, seen);
    }
    for (auto const& [name, field] : expr->fields) {
        static_cast<void>(name);
        ownQueriesOf(field, scope, boundary, seen);
    }
}

inline void ScreenCore::ownQueries(DocNode const& node, RtScope const& scope, BoundaryRuntime& boundary,
                                   std::set<std::string, std::less<>>& seen) {
    if (node.kind == "boundary") {
        return;
    }
    for (auto const& [key, expr] : node.props) {
        static_cast<void>(key);
        ownQueriesOf(expr, scope, boundary, seen);
    }
    for (auto const& [key, nodes] : node.children) {
        static_cast<void>(key);
        for (DocNodePtr const& child : nodes) {
            ownQueries(*child, scope, boundary, seen);
        }
    }
    for (auto const& [key, parts] : node.parts) {
        static_cast<void>(key);
        for (DocNodePtr const& part : parts) {
            ownQueries(*part, scope, boundary, seen);
        }
    }
}

inline Node ScreenCore::lowerBoundary(DocNode const& node, LowerCtx const& ctx) {
    auto boundary = std::make_shared<BoundaryRuntime>();
    std::set<std::string, std::less<>> seen;
    for (DocNodePtr const& child : node.nodes("children")) {
        ownQueries(*child, *ctx.scope, *boundary, seen);
    }
    auto bindings = std::make_shared<Bindings>(*ctx.bindings);
    bindings->push_back(Binding{.name = "boundary", .read = [boundary] {
                                    for (QueryRuntime const* const query : boundary->queries) {
                                        Value const error = query->error();
                                        if (!error.isNull() && query->value().isNull()) {
                                            return Value{ValueRecord{{"error", error}}};
                                        }
                                    }
                                    return Value{ValueRecord{{"error", Value{}}}};
                                }});
    LowerCtx const errorCtx{.scope = ctx.scope, .bindings = bindings, .boundary = boundary};
    LowerCtx const childCtx{.scope = ctx.scope, .bindings = ctx.bindings, .boundary = boundary};
    auto group = [this](std::span<DocNodePtr const> nodes, LowerCtx const& where) -> Node {
        return nodes.empty() ? ui::spacer() : lowerAll(nodes, where);
    };
    Switch spec;
    spec.cases.push_back(SwitchCase{.key = Key{std::int64_t{0}}, .node = group(node.nodes("loading"), childCtx)});
    spec.cases.push_back(SwitchCase{.key = Key{std::int64_t{1}}, .node = group(node.nodes("error"), errorCtx)});
    spec.cases.push_back(SwitchCase{.key = Key{std::int64_t{2}}, .node = group(node.nodes("children"), childCtx)});
    spec.selector = slotProp<Key>([boundary] {
        bool loading = false;
        for (QueryRuntime const* const query : boundary->queries) {
            if (!query->settled()) {
                loading = true;
            } else if (query->value().isNull() && !query->error().isNull()) {
                return Key{std::int64_t{1}};
            }
        }
        return Key{std::int64_t{loading ? 0 : 2}};
    });
    commonOf(node, ctx, nullptr, spec.common);
    return ui::switchOf(std::move(spec));
}

inline Node ScreenCore::lowerCustom(DocNode const& node, LowerCtx const& ctx) {
    std::string const name{node.name("name")};
    // A loaded document's custom node names a declared component, and every component has a fallback.
    Component const& component = _doc->components.at(name);
    std::vector<std::pair<std::string, ExprPtr>> props;
    for (DocNodePtr const& record : node.partList("props")) {
        for (auto const& [key, expr] : record->props) {
            props.emplace_back(key, expr);
        }
    }
    auto bindings = std::make_shared<Bindings>();
    bindings->push_back(
        Binding{.name = "props", .read = [scope = ctx.scope, outer = ctx.bindings, props = std::move(props)] {
                    ValueRecord record;
                    for (auto const& [key, expr] : props) {
                        record.insert_or_assign(key, evalIn(*scope, outer.get(), expr));
                    }
                    return Value{std::move(record)};
                }});
    auto scope = std::make_shared<RtScope>(*this, nullptr, nullptr, ctx.scope->nodes.depth() + 1);
    scope->usePrefix = focusKeyOf(node.path, *ctx.scope) + ">";
    DocNode const& fallback = *component.fallback;
    declare(*scope, nullptr, {&fallback}, {});
    Node lowered = lower(fallback, LowerCtx{.scope = scope, .bindings = bindings, .boundary = nullptr});
    Custom spec{.name = name,
                .fallback = std::move(lowered),
                .props = {},
                .minVersion =
                    static_cast<int>(std::clamp<std::int64_t>(component.version, 0, std::numeric_limits<int>::max())),
                .onEvent = {}};
    // Every declared prop is given, a missing one as its type's empty value: a component lacking one is refused.
    for (auto const& [prop, type] : component.props) {
        ExprPtr expr;
        for (DocNodePtr const& record : node.partList("props")) {
            if (ExprPtr const given = record->prop(prop)) {
                expr = given;
            }
        }
        if (type == "bool") {
            spec.props.push_back(CustomProp{.name = prop, .value = bindProp<bool>(ctx, expr, false, boolOf)});
        } else if (type == "number") {
            spec.props.push_back(CustomProp{
                .name = prop, .value = bindProp<double>(ctx, expr, 0.0, [](Value const& value, FormatContext const&) {
                                  return componentNumber(value);
                              })});
        } else {
            spec.props.push_back(
                CustomProp{.name = prop,
                           .value = bindProp<std::string>(ctx, expr, std::string{},
                                                          [type](Value const& value, FormatContext const& format) {
                                                              return componentText(value, type, format);
                                                          })});
        }
    }
    spec.onEvent = [this, ctx, site = &node, declared = &component](std::string const& event,
                                                                    std::string const& payload) {
        Commands const* const commands = site->event("on." + event);
        if (commands == nullptr) {
            return;
        }
        auto const type = declared->events.find(event);
        runEvent(commands, ctx, componentPayload(payload, type == declared->events.end() ? nullptr : &type->second));
    };
    return ui::custom(std::move(spec));
}

/// @brief One child of an arranged container, as the user's choice places it.
struct ArrangedChild {
    /// @brief The child's id.
    std::string id;
    /// @brief Whether the choice hides it.
    bool hidden = false;
};

/// @brief A container's children in the order a stored `arrange` choice gives, each marked shown or hidden.
///
/// A child the choice does not name keeps its declared place after the named ones, shown, and an entry that names no
/// child, names one twice or is not a `{"id", "hidden"}` record is skipped, so a new document version survives an old
/// choice (spec 5 §12b). A choice that is not a list leaves the declared order. Only `"hidden": true` hides.
/// @param stored   The point's current choice.
/// @param declared The container's children, in document order.
/// @return Every child, in order.
[[nodiscard]] inline std::vector<ArrangedChild> arrangedChildren(Value const& stored,
                                                                 std::span<DocNode const* const> declared) {
    auto const isDeclared = [declared](std::string const& childId) {
        return std::ranges::find(declared, childId, &DocNode::id) != declared.end();
    };
    std::vector<ArrangedChild> out;
    std::set<std::string, std::less<>> placed;
    if (ValueList const* const entries = stored.asList()) {
        for (Value const& entry : *entries) {
            Value const* const named = entry.field("id");
            std::string const* const name = named == nullptr ? nullptr : named->asString();
            if (name == nullptr || !isDeclared(*name) || !placed.insert(*name).second) {
                continue;
            }
            Value const* const hidden = entry.field("hidden");
            bool const* const flag = hidden == nullptr ? nullptr : hidden->asBool();
            out.push_back(ArrangedChild{.id = *name, .hidden = flag != nullptr && *flag});
        }
    }
    for (DocNode const* const child : declared) {
        if (!placed.contains(child->id)) {
            out.push_back(ArrangedChild{.id = child->id});
        }
    }
    return out;
}

/// @brief The ids of the children an `arrange` choice shows, in its order (`arrangedChildren`, without the hidden).
/// @param stored   The point's current choice.
/// @param declared The container's children, in document order.
/// @return The ids to show, in order.
[[nodiscard]] inline std::vector<Value> arrangedOrder(Value const& stored, std::span<DocNode const* const> declared) {
    std::vector<Value> order;
    for (ArrangedChild const& child : arrangedChildren(stored, declared)) {
        if (!child.hidden) {
            order.emplace_back(child.id);
        }
    }
    return order;
}

/// @brief An arrangement as the choice an `arrange` point stores.
/// @param children Every child, in order.
/// @return `[{"id", "hidden"}, …]`.
[[nodiscard]] inline Value arrangementValue(std::vector<ArrangedChild> const& children) {
    ValueList entries;
    entries.reserve(children.size());
    for (ArrangedChild const& child : children) {
        entries.emplace_back(ValueRecord{{"id", Value{child.id}}, {"hidden", Value{child.hidden}}});
    }
    return Value{std::move(entries)};
}

/// @brief The element of a document tree with @p elementId.
/// @param node The subtree.
/// @param elementId The element's id.
/// @return The element, or `nullptr` when the subtree has none with that id.
// NOLINTNEXTLINE(misc-no-recursion): one level per level of the document tree.
[[nodiscard]] inline DocNode const* elementNode(DocNode const& node, std::string_view elementId) {
    if (node.id == elementId) {
        return &node;
    }
    for (auto const* const groups : {&node.children, &node.parts}) {
        for (auto const& [slot, nodes] : *groups) {
            static_cast<void>(slot);
            for (DocNodePtr const& child : nodes) {
                if (DocNode const* const found = elementNode(*child, elementId)) {
                    return found;
                }
            }
        }
    }
    return nullptr;
}

inline std::string ScreenCore::arrangedBy(std::string const& elementId) const {
    if (elementId.empty()) {
        return {};
    }
    for (auto const& [name, point] : _doc->customize) {
        if (point.kind == "arrange" && point.of == elementId) {
            return name;
        }
    }
    return {};
}

// A keyed list over the children's ids rather than a stack, so a new choice moves the shown widgets, keeping their
// state and focus, and mounts or unmounts only what it shows or hides. A child is lowered in the container's own
// scope: arranging changes where it is, not what it reads.
inline Node ScreenCore::lowerArranged(DocNode const& node, LowerCtx const& ctx, ElementState* element,
                                      std::string const& point, Axis axis, int gap) {
    std::vector<DocNode const*> declared;
    for (DocNodePtr const& child : node.nodes("children")) {
        declared.push_back(child.get());
    }
    auto order = [this, point, declared] { return arrangedOrder(prefs.get().at(point), declared); };
    auto key = [](Value const& childId) { return keyOf(childId); };
    auto view = [this, ctx, declared](reactive::Signal<Value> const& row) -> Node {
        // The order lists only declared ids, so the lookup always finds the child.
        auto const found = std::ranges::find(declared, textOf(row.peek(), session.formatting()), &DocNode::id);
        return lower(**found, ctx);
    };
    ForEach spec{
        .model = std::make_shared<TypedForEach<Value> const>(std::move(order), std::move(key), std::move(view)),
        .axis = axis,
        .gap = gap};
    commonOf(node, ctx, element, spec.common);
    return makeNode(std::move(spec));
}

// One row per child of the arranged container, hidden ones included: a checkbox shows or hides it, and two buttons
// move it a place. Each edit stores the whole arrangement, so the stored list always names every child.
inline Node ScreenCore::lowerArrangeEditor(DocNode const& node, LowerCtx const& ctx, std::string const& point,
                                           CustomizePoint const& spec, std::function<void(Value)> const& choose) {
    // The loader checked that `of` names a column or row of this screen.
    DocNode const* const container = elementNode(*_doc->view, spec.of);
    std::vector<DocNode const*> declared;
    for (DocNodePtr const& child : container->nodes("children")) {
        declared.push_back(child.get());
    }
    auto const current = [this, point, declared] { return arrangedChildren(prefs.get().at(point), declared); };
    auto const place = [current](std::string const& childId) {
        auto const children = current();
        return static_cast<std::size_t>(std::ranges::find(children, childId, &ArrangedChild::id) - children.begin());
    };
    // Applies an edit to the arrangement as it stands, read untracked: the edit is a transition, not a binding.
    auto const edit = [this, current, choose](std::string const& childId, auto const& change) {
        auto children = runtime().untracked(current);
        auto const found = std::ranges::find(children, childId, &ArrangedChild::id);
        change(children, static_cast<std::size_t>(found - children.begin()));
        choose(arrangementValue(children));
    };
    auto rows = [current] {
        std::vector<Value> ids;
        for (ArrangedChild const& child : current()) {
            ids.emplace_back(child.id);
        }
        return ids;
    };
    auto key = [](Value const& childId) { return keyOf(childId); };
    auto view = [this, current, place, edit](reactive::Signal<Value> const& row) -> Node {
        std::string const childId = textOf(row.peek(), session.formatting());
        Checkbox shown{.label = childId};
        shown.checked = slotProp<bool>([current, childId] {
            auto const children = current();
            return !std::ranges::find(children, childId, &ArrangedChild::id)->hidden;
        });
        shown.onToggle = [edit, childId](bool checked) {
            edit(childId, [checked](std::vector<ArrangedChild>& children, std::size_t index) {
                children.at(index).hidden = !checked;
            });
        };
        Button moveUp{.label = std::string{"\xE2\x86\x91"}};
        moveUp.onClick = [edit, childId] {
            // Clamped rather than refused: at the edge the child swaps with itself, which stores the same order.
            edit(childId, [](std::vector<ArrangedChild>& children, std::size_t index) {
                std::swap(children.at(index), children.at(std::max(index, std::size_t{1}) - 1));
            });
        };
        moveUp.common.enabled = slotProp<bool>([place, childId] { return place(childId) > 0; });
        moveUp.common.a11y.name = "Move " + childId + " up";
        moveUp.common.testId = "arrange-up-" + childId;
        Button moveDown{.label = std::string{"\xE2\x86\x93"}};
        moveDown.onClick = [edit, childId] {
            edit(childId, [](std::vector<ArrangedChild>& children, std::size_t index) {
                std::swap(children.at(index), children.at(std::min(index + 1, children.size() - 1)));
            });
        };
        moveDown.common.enabled =
            slotProp<bool>([current, place, childId] { return place(childId) + 1 < current().size(); });
        moveDown.common.a11y.name = "Move " + childId + " down";
        moveDown.common.testId = "arrange-down-" + childId;
        shown.common.testId = "arrange-shown-" + childId;
        return ui::row(Row{.children = {ui::checkbox(std::move(shown)), ui::button(std::move(moveUp)),
                                        ui::button(std::move(moveDown))},
                           .gap = 1});
    };
    ForEach editor{.model =
                       std::make_shared<TypedForEach<Value> const>(std::move(rows), std::move(key), std::move(view))};
    commonOf(node, ctx, nullptr, editor.common);
    return makeNode(std::move(editor));
}

inline Node ScreenCore::lowerCustomize(DocNode const& node, LowerCtx const& ctx) {
    std::string const point{node.name("point")};
    // A loaded document's customize node names a declared point, and every point has a choice in `prefs`.
    CustomizePoint const& spec = _doc->customize.at(point);
    auto choose = [this, point](Value value) {
        session.asUser([&] {
            ValueRecord next = prefs.peek();
            next.insert_or_assign(point, value);
            prefs.set(std::move(next));
        });
        if (host != nullptr) {
            host->preferenceChanged(_doc->id, point, value);
        }
    };
    if (spec.kind == "arrange") {
        return lowerArrangeEditor(node, ctx, point, spec, choose);
    }
    if (spec.kind == "choice") {
        Select select;
        std::vector<SelectOption> options;
        options.reserve(spec.options.size());
        for (std::string const& option : spec.options) {
            options.push_back(SelectOption{.key = Key{option}, .label = option});
        }
        select.options = std::move(options);
        select.selected = slotProp<std::optional<Key>>([this, point]() -> std::optional<Key> {
            Value const& chosen = prefs.get().at(point);
            return chosen.isNull() ? std::nullopt : std::optional<Key>{keyOf(chosen)};
        });
        select.onSelect = [choose](Key const& key) { choose(valueOfKey(key)); };
        commonOf(node, ctx, nullptr, select.common);
        return ui::select(std::move(select));
    }
    if (spec.kind == "value" && (spec.type.kind == TypeKind::String || spec.type.isAny())) {
        TextInput input;
        input.value =
            slotProp<std::string>([this, point] { return textOf(prefs.get().at(point), session.formatting()); });
        input.onChange = [choose](std::string text) { choose(Value{std::move(text)}); };
        commonOf(node, ctx, nullptr, input.common);
        return ui::textInput(std::move(input));
    }
    Text placeholder{.text = point, .role = TextRole::Muted};
    commonOf(node, ctx, nullptr, placeholder.common);
    return ui::text(std::move(placeholder));
}
// NOLINTEND(misc-no-recursion)

}  // namespace detail

inline BorrowedModel Screen::lend(std::string_view alias) const {
    detail::NameRef const* const found = _scope->find(alias);
    if (found == nullptr || found->category != detail::NameCategory::Model) {
        return {};
    }
    detail::ModelAlias& model = *found->model;
    return BorrowedModel{.channel = detail::channelOf(model, *_core),
                         .token = _core->runtime().untracked([&] { return model.token->get(); })};
}

inline std::expected<std::unique_ptr<Screen>, Refusal> Interpreter::mount(std::shared_ptr<Document const> doc,
                                                                          ValueRecord const& params,
                                                                          IViewBackend& backend,
                                                                          MountOptions const& options) {
    auto refuse = [&](std::string path, std::string reason) {
        return std::unexpected(Refusal{.document = doc->id, .path = std::move(path), .reason = std::move(reason)});
    };
    if (!doc->view) {
        return refuse("", "the document has no view");
    }
    ValueRecord given;
    for (auto const& [name, type] : doc->params) {
        auto const found = params.find(name);
        Value value = found == params.end() ? Value{} : found->second;
        if (!type.admits(value)) {
            return refuse(pointerAppend("/params", name), value.isNull()
                                                              ? "param `" + name + "` is missing"
                                                              : "param `" + name + "` is not a " + type.name());
        }
        given.insert_or_assign(name, std::move(value));
    }
    for (ModelDecl const& model : doc->scope.models) {
        if (model.instance == InstanceMode::Borrowed && !options.borrowed.contains(model.name)) {
            return refuse(model.path, "alias `" + model.name + "` borrows a model its opener did not lend");
        }
    }
    reactive::Runtime& runtime = _session->runtime();
    std::unique_ptr<Screen> screen{new Screen{}};
    screen->_core = std::make_unique<detail::ScreenCore>(doc, *_session, *_dispatcher, _options, options);
    detail::ScreenCore& core = *screen->_core;
    if (options.keepStateOf != nullptr) {
        core.keptStates = options.keepStateOf->ownState();
    }
    screen->_scope = std::make_shared<detail::RtScope>(core, nullptr, nullptr, options.depth);
    core.rootScope = screen->_scope;
    detail::RtScope& scope = *screen->_scope;
    scope.submitted = &core.submitted;
    scope.showErrors = doc->showErrors;
    for (auto const& [name, value] : given) {
        detail::NameRef ref;
        ref.category = detail::NameCategory::Param;
        ref.param = value;
        scope.names.insert_or_assign(name, std::move(ref));
    }
    std::vector<Commands const*> lists;
    for (auto const& [key, commands] : doc->lifecycle) {
        static_cast<void>(key);
        lists.push_back(&commands);
    }
    for (auto const& [chord, commands] : doc->keys) {
        static_cast<void>(chord);
        lists.push_back(&commands);
    }
    for (DocNodePtr const& action : doc->actions) {
        if (Commands const* const commands = action->event("onClick")) {
            lists.push_back(commands);
        }
    }
    Node root = runtime.untracked([&] {
        core.declare(scope, &doc->scope, {doc->view.get()}, lists);
        detail::LowerCtx const ctx{
            .scope = screen->_scope, .bindings = std::make_shared<detail::Bindings const>(), .boundary = nullptr};
        Node lowered = core.lower(*doc->view, ctx);
        screen->_headStatus = core.lowerBadgeParts(doc->status, ctx);
        screen->_headActions = core.lowerActionParts(doc->actions, ctx);
        if (!doc->keys.empty()) {
            std::vector<KeyBinding> keys;
            for (auto const& [chord, commands] : doc->keys) {
                keys.push_back(KeyBinding{
                    .chord = chord, .onPress = [&core, ctx, list = &commands] { core.runEvent(list, ctx, Value{}); }});
            }
            Column column{.children = {std::move(lowered)}};
            column.common.keys = std::move(keys);
            lowered = ui::column(std::move(column));
        }
        return lowered;
    });
    screen->_mounted.emplace(runtime, backend, std::move(root), options.parent, options.depth + 1);
    screen->_core->focusElement = [mounted = &*screen->_mounted](std::string_view element) {
        return mounted->focus(element);
    };
    screen->runLifecycle("onMount");
    return screen;
}

inline std::expected<std::unique_ptr<AppRoot>, Refusal> Interpreter::mountApp(std::shared_ptr<Document const> doc) {
    if (doc->id != "app") {
        return std::unexpected(
            Refusal{.document = doc->id, .path = "/id", .reason = "the app document's id is `app`"});
    }
    if (_session->appScope() != nullptr) {
        return std::unexpected(
            Refusal{.document = doc->id, .path = {}, .reason = "an app document is already mounted on the session"});
    }
    std::unique_ptr<AppRoot> root{new AppRoot{}};
    root->_core = std::make_unique<detail::ScreenCore>(doc, *_session, *_dispatcher, _options, MountOptions{});
    detail::ScreenCore& core = *root->_core;
    root->_scope = std::make_shared<detail::RtScope>(core, nullptr, nullptr, 0);
    core.rootScope = root->_scope;
    std::vector<Commands const*> lists;
    for (auto const& [key, commands] : doc->lifecycle) {
        static_cast<void>(key);
        lists.push_back(&commands);
    }
    _session->runtime().untracked([&] { core.declare(*root->_scope, &doc->scope, {}, lists); });
    _session->setAppScope(root->_scope.get());
    if (auto const found = doc->lifecycle.find("onMount"); found != doc->lifecycle.end()) {
        root->runCommands(found->second);
    }
    return root;
}

}  // namespace morph::ui

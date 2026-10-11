// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
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
#include "../reactive/runtime.hpp"
#include "../reactive/scheduler.hpp"
#include "../reactive/scope.hpp"
#include "../reactive/signal.hpp"
#include "backend.hpp"
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

/// @brief A `Dispatcher` over the bridge's model-free seams: `bindByType`, `executeRaw` and the string-keyed
///        `attach`, through `RawHandler`.
///
/// It sends no `evaluate` request (that envelope belongs to the UI service) and has no offline queue: an evaluation
/// is refused with a `std::runtime_error`, and `queueOffline` is ignored. A shared binding without a key is bound
/// unattached, which the bridge refuses at the first call; a document reaches it only for a stateful model whose
/// alias says `"shared"`.
class BridgeDispatcher final : public Dispatcher {
public:
    /// @brief Dispatches through @p bridge.
    /// @param bridge The bridge. Borrowed: it must outlive every channel.
    /// @param cbExec The executor replies are delivered on: the runtime's owner. Borrowed.
    BridgeDispatcher(bridge::Bridge& bridge MORPH_LIFETIMEBOUND, exec::IExecutor* cbExec MORPH_LIFETIMEBOUND)
        : _bridge{&bridge}, _cbExec{cbExec} {}

    /// @brief Binds through a `RawHandler`.
    /// @param typeId      The model's registered type id.
    /// @param sharing     Private, or shared.
    /// @param instanceKey For a shared binding, the key to attach to; empty to stay unattached.
    /// @return The channel; destroying it deregisters the binding.
    [[nodiscard]] std::shared_ptr<ModelChannel> bind(std::string const& typeId, bridge::BindSharing sharing,
                                                     std::string const& instanceKey) override {
        return std::make_shared<Channel>(*_bridge, _cbExec, typeId, sharing, instanceKey);
    }

private:
    class Channel final : public ModelChannel {
    public:
        Channel(bridge::Bridge& bridge, exec::IExecutor* cbExec, std::string const& typeId,
                bridge::BindSharing sharing, std::string const& instanceKey)
            : _cbExec{cbExec}, _handler{bridge, cbExec, typeId, sharing, instanceKey} {}

        [[nodiscard]] async::Completion<std::string> execute(std::string const& actionId, std::string const& bodyJson,
                                                             CallOptions options) override {
            if (options.evaluate) {
                auto [refused, promise] = async::Completion<std::string>::makeSettleable(_cbExec);
                promise.reject(std::make_exception_ptr(std::runtime_error{"evaluate needs the UI service"}));
                return std::move(refused);
            }
            return _handler.execute(actionId, bodyJson);
        }

    private:
        exec::IExecutor* _cbExec;
        bridge::RawHandler _handler;
    };

    bridge::Bridge* _bridge;
    exec::IExecutor* _cbExec;
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

    /// @brief Moves keyboard focus to an element.
    /// @param screen  The screen's id.
    /// @param element The element id.
    /// @return Whether focus moved.
    virtual bool focus(std::string const& screen, std::string const& element) {
        static_cast<void>(screen);
        static_cast<void>(element);
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

    /// @brief Switches to a backend the application's source lists.
    /// @param name The backend.
    /// @return Whether it is listed.
    virtual bool switchBackend(std::string const& name) {
        static_cast<void>(name);
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
};

// ---------------------------------------------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------------------------------------------

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

/// @brief Replaces, in a list of rows, the row whose `id` equals @p key.
/// @param rows The list.
/// @param key  The row's id.
/// @param with The new content, merged over the row.
/// @return The new list, or `std::nullopt` when no row matched or @p rows is not a list.
[[nodiscard]] inline std::optional<Value> patchRows(Value const& rows, Value const& key, Value const& with) {
    ValueList const* const items = rows.asList();
    if (items == nullptr || key.isNull()) {
        return std::nullopt;
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
        return std::nullopt;
    }
    return Value{std::move(out)};
}

/// @brief Removes the named fields from a record, and from each record of a list.
/// @param value  The value.
/// @param fields The field names.
/// @return The value without them.
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

/// @brief One optimistic write still waiting for its reply.
struct OptimisticLayer {
    /// @brief The run that made it.
    std::uint64_t run = 0;
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
    /// @brief Marked stale by an invalidation, `refreshOn` or a backend switch.
    bool stale = false;
    /// @brief The mounted queries using it.
    int users = 0;
    /// @brief The eviction timer, while no query uses it.
    reactive::TimerHandle evict;
    /// @brief The fields never cached.
    std::vector<std::string> secretFields;
};

class QueryRuntime;

}  // namespace detail

/// @brief What screens of one client session share (spec 5 §5, §12b): the query cache, `refreshOn` bookkeeping, the
///        principal's permitted actions, the formatting facts and the client's `env`, `session` and `host` values.
///
/// It belongs to the runtime's owner, outlives every screen mounted on it, and is cleared by `signOut` and on a
/// principal change.
class Session {
public:
    /// @brief An empty session.
    /// @param runtime The runtime. Borrowed.
    /// @param options Timers, clock and catalog.
    explicit Session(reactive::Runtime& runtime MORPH_LIFETIMEBOUND, SessionOptions options = {})
        : _runtime{&runtime},
          _options{std::move(options)},
          _permitted{runtime},
          _format{runtime, std::make_shared<FormatContext const>()},
          _env{runtime},
          _sessionValues{runtime},
          _hostValues{runtime} {}

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

    /// @brief Sets the action ids the principal may run, and the `host:<command>` names the client has, as the
    ///        catalog reply lists them; `{"can": ...}` follows them.
    /// @param permitted The ids.
    void setPermitted(std::set<std::string, std::less<>> permitted) { _permitted.set(std::move(permitted)); }

    /// @brief Whether the principal may run an action, or the client has a host command. A tracked read.
    /// @param id The action id, or `host:<command>`.
    /// @return Whether it is permitted.
    [[nodiscard]] bool can(std::string_view id) const { return _permitted.get().contains(id); }

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

    /// @brief Sets the `env` values (`widthClass`, `locale`, `platform`, `online`, `backend`).
    /// @param values The values.
    void setEnvironment(ValueRecord values) { _env.set(std::move(values)); }

    /// @brief Sets the `session` values (`principal`, `userName`, …).
    /// @param values The values.
    void setSessionValues(ValueRecord values) { _sessionValues.set(std::move(values)); }

    /// @brief Sets the `host` values the application registered.
    /// @param values The values.
    void setHostValues(ValueRecord values) { _hostValues.set(std::move(values)); }

    /// @brief Reads `env`, `session` or `host`. A tracked read.
    /// @param form `Env`, `Session` or `Host`.
    /// @param name The name.
    /// @return The value, or `null`.
    [[nodiscard]] Value client(ExprKind form, std::string_view name) const {
        ValueRecord const& values = form == ExprKind::Env       ? _env.get()
                                    : form == ExprKind::Session ? _sessionValues.get()
                                                                : _hostValues.get();
        auto const found = values.find(name);
        return found == values.end() ? Value{} : found->second;
    }

    /// @brief Drops every cache entry: `signOut` and a principal change (spec 5 §5, "Isolation").
    void clear() {
        _runtime->batch([this] {
            for (auto const& [key, entry] : _entries) {
                static_cast<void>(key);
                ++entry->generation;
                entry->evict.cancel();
            }
            _entries.clear();
            for (detail::QueryRuntime* const query : _mounted) {
                detachQuery(*query);
            }
        });
    }

    /// @brief Marks every entry stale and refetches those a mounted query uses: a backend switch (spec 6 §9).
    void backendChanged();

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

    /// @cond INTERNAL

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
            if (_options.catalog.action) {
                if (auto const info = _options.catalog.action(action)) {
                    entry->secretFields = info->secretFields;
                }
            }
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
                _entries.erase(held->key);
            }
        });
    }

    /// @brief Sends an entry's request under a new generation; a reply of an older generation is dropped.
    /// @param entry   The entry.
    /// @param channel The channel to send it through; `nullptr` fails the request at once.
    void fetch(std::shared_ptr<detail::CacheEntry> const& entry, std::shared_ptr<ModelChannel> const& channel) {
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
    void confirm(detail::CacheEntry& entry, Value confirmed) {
        entry.confirmed = detail::withoutFields(confirmed, entry.secretFields);
        showLayers(entry);
    }

    /// @brief Recomputes what an entry shows from its confirmed value and its layers.
    /// @param entry The entry.
    void showLayers(detail::CacheEntry& entry) {
        Value shown = entry.confirmed;
        for (detail::OptimisticLayer const& layer : entry.layers) {
            if (auto patched = detail::patchRows(shown, layer.key, layer.with)) {
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

    /// @brief Registers a mounted query, for `refreshOn` and invalidation.
    /// @param query The query.
    void attachQuery(detail::QueryRuntime& query) { _mounted.push_back(&query); }

    /// @brief Unregisters a mounted query.
    /// @param query The query.
    void forgetQuery(detail::QueryRuntime& query) { std::erase(_mounted, &query); }

    /// @brief A mutation of @p typeId running @p actionId succeeded: refetch every mounted query of that model type
    ///        whose `refreshOn` names it (spec 5 §5).
    /// @param typeId   The model type.
    /// @param actionId The action.
    void actionSucceeded(std::string const& typeId, std::string const& actionId);

    /// @endcond

private:
    void settleReply(detail::CacheEntry& entry, std::string const& reply) {
        auto json = parseJson(reply);
        auto value = json ? valueFromJson(*json) : std::unexpected(json.error());
        if (!value) {
            settleFailure(entry, "the reply is not a document value: " + value.error(), "error");
            return;
        }
        _runtime->batch([&] {
            confirm(entry, std::move(*value));
            entry.hasValue.set(true);
            entry.error.set({});
            entry.errorKind.set({});
            entry.pending.set(false);
            entry.fetchedAt = now();
            entry.stale = false;
        });
    }

    void settleFailure(detail::CacheEntry& entry, std::string text, std::string kind) {
        _runtime->batch([&] {
            noteWrite();
            entry.error.set(std::move(text));
            entry.errorKind.set(std::move(kind));
            entry.pending.set(false);
        });
    }

    void detachQuery(detail::QueryRuntime& query);

    reactive::Runtime* _runtime;
    SessionOptions _options;
    reactive::Signal<std::set<std::string, std::less<>>> _permitted;
    reactive::Signal<std::shared_ptr<FormatContext const>> _format;
    reactive::Signal<ValueRecord> _env;
    reactive::Signal<ValueRecord> _sessionValues;
    reactive::Signal<ValueRecord> _hostValues;
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

/// @brief What `Interpreter::mount` is given beside the document.
struct MountOptions {
    /// @brief The container the screen's root widget is appended to; `nullptr` makes a backend root.
    ContainerWidget* parent = nullptr;
    /// @brief The models the opener lends, by the alias of this screen that borrows them.
    std::map<std::string, BorrowedModel, std::less<>> borrowed;
    /// @brief The reactive scope depth of the screen's own scope.
    std::size_t depth = 0;
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
    /// @brief A table's last activated key.
    reactive::Signal<Value> activeKey;
    /// @brief Bumped when a check targeting it comes or goes.
    reactive::Signal<int> checksVersion;
    /// @brief A dialog's or drawer's scope while its content is mounted.
    reactive::Signal<RtScope*> inner;
    /// @brief The checks targeting it.
    std::vector<CheckRuntime*> checks;
    /// @brief The value an expression binds (`value`, `checked`, `selected`); `nullptr` when the element is
    ///        uncontrolled.
    reactive::Computed<Value>* bound = nullptr;
    /// @brief What is wrong now, as a list of strings: `required` unmet, the node's `errors`, the reply's field
    ///        errors and the failing checks' messages.
    reactive::Computed<Value>* errors = nullptr;
    /// @brief A table's rows or a select's options, for its counts and `selectedOption`.
    reactive::Computed<Value>* rows = nullptr;
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
    /// @param core   The screen.
    /// @param parent The enclosing scope; `nullptr` for the screen and a component fallback.
    /// @param row    The row of a row scope; `nullptr` otherwise.
    /// @param depth  The reactive scope depth of what it owns.
    RtScope(ScreenCore& core, std::shared_ptr<RtScope> parent, reactive::Signal<Value> const* row, std::size_t depth);

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
    if (Quantity const* const quantity = value.asQuantity()) {
        return formatNumber(value, format.numeric).value_or(std::string{}) + " " +
               unitDisplay(quantity->unit, format.units);
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
    reactive::TimerHandle _debounce;
    reactive::TimerHandle _refresh;
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
};

/// @brief One run of a mutation, as a command sees it: whether it succeeded, and the reply.
using RunDone = std::function<void(bool succeeded, Value const& reply)>;

/// @brief A mutation at run time (spec 5 §5): concurrency, patch, optimistic writes, invalidation.
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
    ~MutationRuntime() { *_alive = false; }

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

/// @brief Whether a node kind keeps per-element state even without an id: the inputs, dialogs and tables.
/// @param kind The kind.
/// @return It.
[[nodiscard]] inline bool statefulKind(std::string_view kind) {
    static constexpr std::array<std::string_view, 9> kStateful = {
        "textInput", "checkbox", "select", "dateTimeInput", "slider", "filePicker", "dialog", "drawer", "table"};
    return listed(kStateful, kind);
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
          _doc{std::move(doc)} {
        ValueRecord defaults;
        for (auto const& [name, point] : _doc->customize) {
            defaults.insert_or_assign(name, point.defaultValue);
        }
        prefDefaults = defaults;
        prefs.set(std::move(defaults));
    }

    ScreenCore(ScreenCore const&) = delete;
    ScreenCore& operator=(ScreenCore const&) = delete;
    ScreenCore(ScreenCore&&) = delete;
    ScreenCore& operator=(ScreenCore&&) = delete;
    ~ScreenCore() = default;

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
        std::string key = std::string{path} + '\x1f' + std::string{reason};
        if (!reported.insert(std::move(key)).second) {
            return;
        }
        Refusal refusal{.document = _doc->id, .path = std::string{path}, .reason = std::string{reason}};
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
    void initState(RtScope const& scope, StateSlot& slot) {
        if (slot.value || slot.initializing) {
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
        auto bindings = std::make_shared<Bindings>(ctx.bindings ? *ctx.bindings : Bindings{});
        bindings->push_back(Binding{.name = "event", .read = [event = std::move(event)] { return event; }});
        CommandCtx const command{.scope = ctx.scope,
                                 .bindings = std::move(bindings),
                                 .cancelClose = std::make_shared<bool>(false),
                                 .boundary = ctx.boundary};
        session.asUser([&] { runtime().batch([&] { runList(*commands, command, {}); }); });
    }

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

    /// @brief The session.
    Session& session;
    /// @brief The dispatcher.
    Dispatcher& dispatcher;
    /// @brief The host; may be `nullptr`.
    InterpreterHost* host;
    /// @brief The diagnostic callback.
    std::function<void(Refusal const&)> onDiagnostic;
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

private:
    void wireElement(ElementState& element);
    void collect(DocNode const& node, RtScope& scope, std::vector<Commands const*>& commands);
    Node lowerKind(DocNode const& node, LowerCtx const& ctx, ElementState* element);
    Node lowerDialog(DocNode const& node, LowerCtx const& ctx, ElementState* element);
    Node lowerSwitch(DocNode const& node, LowerCtx const& ctx);
    Node lowerRows(DocNode const& node, LowerCtx const& ctx, ElementState* element);
    Node lowerBoundary(DocNode const& node, LowerCtx const& ctx);
    Node lowerCustom(DocNode const& node, LowerCtx const& ctx);
    Node lowerCustomize(DocNode const& node, LowerCtx const& ctx);
    std::shared_ptr<RtScope> rowScope(DocNode const& node, LowerCtx const& ctx, reactive::Signal<Value> const& row,
                                      std::vector<DocNode const*> const& content);
    void commonOf(DocNode const& node, LowerCtx const& ctx, ElementState* element, Common& common);
    void fieldOf(DocNode const& node, LowerCtx const& ctx, ElementState* element, FieldState& field);
    std::vector<MenuItem> menuItems(std::span<DocNodePtr const> parts, LowerCtx const& ctx);
    void ownQueries(DocNode const& node, RtScope const& scope, BoundaryRuntime& boundary,
                    std::set<std::string, std::less<>>& seen);
    void ownQueriesOf(ExprPtr const& expr, RtScope const& scope, BoundaryRuntime& boundary,
                      std::set<std::string, std::less<>>& seen);
    void runMutation(Command const& command, CommandCtx const& ctx, Done done);
    void runSet(Command const& command, CommandCtx const& ctx, Done const& done);
    void runReset(Command const& command, CommandCtx const& ctx, Done const& done);
    void runHostCommand(Command const& command, CommandCtx const& ctx, Done done);
    void runTransfer(Command const& command, CommandCtx const& ctx, Done done);
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

    /// @brief A property read through @p read, as a numbered slot.
    template <typename T, typename Read>
    Prop<T> slotProp(Read read) {
        return Prop<T>{ui::slot(nextSlot(), std::move(read))};
    }

    /// @brief Evaluates an expression once, untracked: a property the view tree sets once.
    Value once(LowerCtx const& ctx, ExprPtr const& expr) {
        return runtime().untracked([&] { return evalIn(*ctx.scope, ctx.bindings.get(), expr); });
    }

    std::shared_ptr<Document const> _doc;
    SlotId _slots = 1;
};

}  // namespace detail

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
        if (Commands const* const commands = lifecycleList("onUnmount")) {
            runCommands(*commands);
        }
        _mounted.reset();
        _scope.reset();
    }

    /// @brief The root widget.
    /// @return It, valid as long as the screen.
    [[nodiscard]] Widget& root() const { return _mounted->root(); }

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

    /// @brief Evaluates the document's `dirty`.
    /// @return Whether it is `true`.
    [[nodiscard]] bool dirty() const {
        Value const value = detail::evalIn(*_scope, nullptr, _core->doc().dirty);
        bool const* const flag = value.asBool();
        return flag != nullptr && *flag;
    }

    /// @brief Runs `onClose`, as the host does before it closes the screen.
    /// @return False when `onClose` ran `cancelClose`: the screen stays open.
    bool requestClose() {
        Commands const* const commands = lifecycleList("onClose");
        if (commands == nullptr) {
            return true;
        }
        return !runCommands(*commands);
    }

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

    /// @brief What an alias of this screen lends to a screen it opens that borrows it.
    /// @param alias The alias.
    /// @return Its channel and token; an empty channel when it has none.
    [[nodiscard]] BorrowedModel lend(std::string_view alias) const;

private:
    friend class Interpreter;

    Screen() = default;

    [[nodiscard]] Commands const* lifecycleList(std::string_view key) const {
        auto const found = _core->doc().lifecycle.find(key);
        return found == _core->doc().lifecycle.end() ? nullptr : &found->second;
    }

    /// Runs a command list as the screen's own (not a user's); returns whether it ran `cancelClose`.
    bool runCommands(Commands const& commands) {
        detail::CommandCtx const ctx{.scope = _scope,
                                     .bindings = std::make_shared<detail::Bindings>(),
                                     .cancelClose = std::make_shared<bool>(false),
                                     .boundary = nullptr};
        _core->runtime().batch([&] { _core->runList(commands, ctx, {}); });
        return *ctx.cancelClose;
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
                                                                        MountOptions options = {});

private:
    Session* _session;
    Dispatcher* _dispatcher;
    InterpreterOptions _options;
};

// ---------------------------------------------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------------------------------------------

inline void Session::backendChanged() {
    _runtime->batch([this] {
        for (auto const& [key, entry] : _entries) {
            static_cast<void>(key);
            entry->stale = true;
        }
        std::vector<detail::QueryRuntime*> const mounted = _mounted;
        for (detail::QueryRuntime* const query : mounted) {
            query->invalidate();
        }
    });
}

inline void Session::actionSucceeded(std::string const& typeId, std::string const& actionId) {
    std::vector<detail::QueryRuntime*> const mounted = _mounted;
    for (detail::QueryRuntime* const query : mounted) {
        auto const& names = query->decl().refreshOn;
        if (query->typeId() == typeId && std::ranges::find(names, actionId) != names.end()) {
            query->invalidate();
        }
    }
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
            alias.channel = core.dispatcher.bind(type, bridge::BindSharing::Shared, {});
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
    : core{screen}, parent{std::move(enclosing)}, row{current}, nodes{screen.runtime(), depth} {}

inline RtScope::~RtScope() {
    if (owner != nullptr && owner->inner.peek() == this) {
        owner->inner.set(nullptr);
    }
}

inline bool RtScope::ready() const {
    for (auto const& element : elements) {
        if (element->errors == nullptr) {
            continue;
        }
        bool const* const visible = core.elementValue(*element, "visible").asBool();
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

inline Value ScopeEnv::ref(std::string_view head, std::span<std::string const> path) const {
    if (_bindings != nullptr) {
        for (auto bound = _bindings->rbegin(); bound != _bindings->rend(); ++bound) {
            if (bound->name == head) {
                return walk(bound->read(), path);
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
            return name->state->value ? walk(name->state->value->get(), path) : Value{};
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
            Value const value = path.front() == "progress"  ? transfer.progress.get()
                                : path.front() == "pending" ? Value{transfer.pending.get()}
                                : path.front() == "error"   ? transfer.error.get()
                                                            : Value{};
            return walk(value, path.subspan(1));
        }
        case NameCategory::Model:
        case NameCategory::Mutation:
        default:
            return Value{};
    }
}

inline Value ScopeEnv::control(ExprKind form, std::string_view name) const {
    Session& session = _scope->core.session;
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
    return form == ExprKind::Pending ? Value{false} : Value{};
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
      _typeId{alias == nullptr ? std::string{} : alias->decl->type},
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

inline std::shared_ptr<ModelChannel> QueryRuntime::channel() const {
    return _alias == nullptr ? nullptr : channelOf(*_alias, _scope->core);
}

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
        if (_alias == nullptr) {
            return target;
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
      _typeId{alias == nullptr ? std::string{} : alias->decl->type},
      _inFlight{scope.core.runtime(), 0},
      _result{scope.core.runtime()},
      _error{scope.core.runtime()},
      _errorKind{scope.core.runtime()} {}

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
    if (_decl->concurrency == Concurrency::Serial) {
        Value const key = evalIn(*_scope, run->bindings.get(), _decl->serialKey);
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
    std::shared_ptr<ModelChannel> const channel = _alias == nullptr ? nullptr : channelOf(*_alias, _scope->core);
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
    Bindings bindings = run.bindings ? *run.bindings : Bindings{};
    bindings.push_back(Binding{.name = "body", .read = [body = run.body] { return body; }});
    Value key = _scope->core.runtime().untracked([&] { return evalIn(*_scope, &bindings, _decl->optimistic->key); });
    Value with = _scope->core.runtime().untracked([&] { return evalIn(*_scope, &bindings, _decl->optimistic->with); });
    entry->layers.push_back(OptimisticLayer{.run = run.id, .key = std::move(key), .with = std::move(with)});
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
    core.runtime().batch([&] {
        _inFlight.set(_inFlight.peek() - 1);
        if (reply) {
            succeeded = true;
            // The run's optimistic writes are confirmed by the reply: they become part of the confirmed rows.
            for (auto const& entry : run->layered) {
                Value confirmed = entry->confirmed;
                for (OptimisticLayer const& layer : entry->layers) {
                    if (layer.run == run->id) {
                        if (auto patched = patchRows(confirmed, layer.key, layer.with)) {
                            confirmed = std::move(*patched);
                        }
                    }
                }
                std::erase_if(entry->layers, [&](OptimisticLayer const& layer) { return layer.run == run->id; });
                session.confirm(*entry, std::move(confirmed));
            }
            run->layered.clear();
            std::vector<std::string> secret;
            if (session.options().catalog.action) {
                if (auto const info = session.options().catalog.action(_decl->action)) {
                    secret = info->secretFields;
                }
            }
            delivered = withoutFields(*reply, secret);
            if (!applied) {
                return;
            }
            _result.set(*reply);
            _error.set(Value{});
            _errorKind.set(Value{});
            if (_decl->patch) {
                Bindings bindings = run->bindings ? *run->bindings : Bindings{};
                bindings.push_back(Binding{.name = "result", .read = [value = *reply] { return value; }});
                NameRef const* const target = _scope->find(_decl->patch->query);
                if (target != nullptr && target->category == NameCategory::Query) {
                    auto entry = core.runtime().untracked([&] { return target->query->entry(); });
                    Value const key =
                        core.runtime().untracked([&] { return evalIn(*_scope, &bindings, _decl->patch->key); });
                    Value const with =
                        core.runtime().untracked([&] { return evalIn(*_scope, &bindings, _decl->patch->with); });
                    if (entry) {
                        if (auto patched = patchRows(entry->confirmed, key, with)) {
                            session.confirm(*entry, std::move(*patched));
                        }
                    }
                }
            }
            for (std::string const& name : _decl->invalidates) {
                NameRef const* const target = _scope->find(name);
                if (target != nullptr && target->category == NameCategory::Query) {
                    target->query->invalidate();
                }
            }
            session.actionSucceeded(_typeId, _decl->action);
            core.runList(_decl->onSuccess, commandCtx(run->bindings ? *run->bindings : Bindings{}), {});
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
            core.runList(_decl->onError, commandCtx(run->bindings ? *run->bindings : Bindings{}), {});
        }
        if (kind == "denied" && core.host != nullptr) {
            core.host->permissionsStale();
        }
    });
    if (_decl->concurrency == Concurrency::Serial) {
        next(run->serialKey);
    }
    if (run->done) {
        run->done(succeeded, delivered);
    }
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
        scope.elements.push_back(std::move(element));
    }
    bool const createsScope = spec != nullptr && (spec->flags & kScope) != 0;
    for (auto const& [key, list] : node.events) {
        KeySpec const* const keyed = spec == nullptr ? nullptr : keySpec(*spec, key);
        if (!(createsScope && keyed != nullptr && (keyed->flags & kInner) != 0)) {
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
    for (std::string const& id : transferIds) {
        if (scope.names.contains(id)) {
            continue;
        }
        scope.transfers.push_back(std::make_unique<TransferState>(runtime()));
        NameRef name;
        name.category = NameCategory::Transfer;
        name.transfer = scope.transfers.back().get();
        scope.names.insert_or_assign(id, name);
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
                    raw->token = &scope.nodes.computed([token] { return token; });
                    break;
                }
                case InstanceMode::Private:
                case InstanceMode::Default:
                default: {
                    std::string token = session.privateToken();
                    raw->token = &scope.nodes.computed([token] { return token; });
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
            bool const* const flag = evalIn(scope, nullptr, expr).asBool();
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
        auto& on = scope.nodes.computed([&scope, expr = watch.on] { return evalIn(scope, nullptr, expr); });
        scope.nodes.effect([this, &scope, &on, watched = &watch, first = true]() mutable {
            static_cast<void>(on.get());
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

/// @brief Whether two values show the same in an input: by text for a text input, by value otherwise.
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

inline void ScreenCore::wireElement(ElementState& element) {
    DocNode const& node = *element.node;
    RtScope& scope = *element.scope;
    std::string_view const kind = node.kind;
    bool const input = kind == "textInput" || kind == "checkbox" || kind == "select" || kind == "dateTimeInput" ||
                       kind == "slider" || kind == "filePicker";
    if (input) {
        std::string_view const key = kind == "checkbox" ? "checked" : kind == "select" ? "selected" : "value";
        ExprPtr const expr = kind == "filePicker" ? nullptr : node.prop(key);
        bool const controlled = expr && !(expr->kind == ExprKind::Literal && expr->literal.isNull());
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
    }
    element.errors = &scope.nodes.computed([this, &element]() -> Value {
        static_cast<void>(element.checksVersion.get());
        DocNode const& field = *element.node;
        RtScope const& owner = *element.scope;
        std::vector<std::string> errors;
        if (ExprPtr const required = field.prop("required")) {
            bool const* const flag = evalIn(owner, nullptr, required).asBool();
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

inline Value ScreenCore::elementValue(ElementState const& element, std::string_view name) const {
    DocNode const& node = *element.node;
    RtScope const& scope = *element.scope;
    std::string_view const kind = node.kind;
    auto flagOf = [&](std::string_view key, bool fallback) -> Value {
        ExprPtr const expr = node.prop(key);
        if (!expr) {
            return fallback;
        }
        bool const* const flag = evalIn(scope, nullptr, expr).asBool();
        return flag != nullptr && *flag;
    };
    auto errors = [&]() -> Value { return element.errors == nullptr ? Value{ValueList{}} : element.errors->get(); };
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
        ValueList const* const items = errors().asList();
        return items == nullptr || items->empty();
    }
    if (name == "shownErrors") {
        RtScope const& policy = scope.policyScope();
        std::string mode = policy.showErrors.empty() ? _doc->showErrors : policy.showErrors;
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
            return element.selection.get();
        }
        if (name == "activeKey") {
            return element.activeKey.get();
        }
        if (name == "selectedCount") {
            return count(element.selection.get());
        }
        if (name == "viewCount" || name == "sourceCount") {
            return element.rows == nullptr ? Value{std::int64_t{0}} : count(element.rows->get());
        }
        ExprPtr const rows = node.prop("rows");
        if ((name == "pending" || name == "error") && rows && rows->kind == ExprKind::Query) {
            ScopeEnv const env{scope, nullptr};
            return env.control(name == "pending" ? ExprKind::Pending : ExprKind::Error, rows->name);
        }
        return name == "pending" ? Value{false} : Value{};
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

inline void ScreenCore::runList(Commands const& commands, CommandCtx const& ctx, Done done) {
    if (commands.empty()) {
        if (done) {
            done(true);
        }
        return;
    }
    struct Join {
        std::size_t left = 0;
        bool ok = true;
        Done done;
    };
    auto join = std::make_shared<Join>(Join{.left = commands.size() + 1, .ok = true, .done = std::move(done)});
    auto finish = [join](bool succeeded) {
        join->ok = join->ok && succeeded;
        if (--join->left == 0 && join->done) {
            join->done(join->ok);
        }
    };
    for (Command const& command : commands) {
        runOne(command, ctx, finish);
    }
    finish(true);
}

// NOLINTBEGIN(misc-no-recursion): command lists nest
inline void ScreenCore::runOne(Command const& command, CommandCtx const& ctx, Done done) {
    RtScope const& scope = *ctx.scope;
    auto eval = [&](ExprPtr const& expr) { return evalIn(scope, ctx.bindings.get(), expr); };
    auto finish = [&done](bool succeeded) {
        if (done) {
            done(succeeded);
        }
    };
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
            NameRef const* const found = scope.find(command.target);
            if (found == nullptr || found->category != NameCategory::Query) {
                report(command.path, "`refetch` names no query here");
                finish(false);
                return;
            }
            found->query->refetch();
            finish(true);
            return;
        }
        case CommandKind::Open:
        case CommandKind::Close: {
            if (command.kind == CommandKind::Close && command.target == "self") {
                finish(host != nullptr
                           ? host->closeSelf(_doc->id, std::nullopt) || unsupported(command, "`close: self`")
                           : unsupported(command, "`close: self`"));
                return;
            }
            NameRef const* const found = scope.find(command.target);
            if (found == nullptr || found->category != NameCategory::Element) {
                report(command.path, "`open` and `close` name no dialog here");
                finish(false);
                return;
            }
            ElementState& element = *found->element;
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
            NavigateRequest request{.screen = command.target,
                                    .route = eval(command.extra),
                                    .params = eval(command.value),
                                    .as = command.option};
            if (host == nullptr) {
                finish(unsupported(command, "`navigate`"));
                return;
            }
            std::weak_ptr<RtScope> const weak = ctx.scope;
            auto onResult = [this, weak, ctx, list = &command.commands](Value result) {
                if (auto const held = weak.lock()) {
                    auto bindings = std::make_shared<Bindings>(*ctx.bindings);
                    bindings->push_back(Binding{.name = "event", .read = [result] { return result; }});
                    CommandCtx const next{
                        .scope = held, .bindings = bindings, .cancelClose = ctx.cancelClose, .boundary = ctx.boundary};
                    runtime().batch([&] { runList(*list, next, {}); });
                }
            };
            auto onCancel = [this, weak, ctx, list = &command.alternative] {
                if (auto const held = weak.lock()) {
                    runtime().batch([&] { runList(*list, ctx, {}); });
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
            finish(host != nullptr && host->focus(_doc->id, command.target) ? true : unsupported(command, "`focus`"));
            return;
        case CommandKind::Host:
            runHostCommand(command, ctx, std::move(done));
            return;
        case CommandKind::SwitchBackend:
            if (host != nullptr && host->switchBackend(command.target)) {
                session.backendChanged();
                finish(true);
                return;
            }
            report(command.path, "backend `" + command.target + "` is not listed");
            finish(false);
            return;
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
            auto bindings = std::make_shared<Bindings>(*ctx.bindings);
            CommandCtx const inner{
                .scope = ctx.scope, .bindings = bindings, .cancelClose = ctx.cancelClose, .boundary = ctx.boundary};
            auto step = std::make_shared<std::function<void(std::size_t)>>();
            *step = [this, inner, list = &command.commands, done = std::move(done),
                     weakStep = std::weak_ptr<std::function<void(std::size_t)>>{step}](std::size_t index) {
                if (index == list->size()) {
                    if (done) {
                        done(true);
                    }
                    return;
                }
                auto const self = weakStep.lock();
                runOne(list->at(index), inner, [self, index, done](bool succeeded) {
                    if (!succeeded) {
                        if (done) {
                            done(false);
                        }
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
            if (ctx.cancelClose) {
                *ctx.cancelClose = true;
            }
            finish(true);
            return;
        case CommandKind::Retry:
            if (ctx.boundary) {
                for (QueryRuntime* const query : ctx.boundary->queries) {
                    if (!runtime().untracked([&] { return query->error(); }).isNull()) {
                        query->refetch();
                    }
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
// NOLINTEND(misc-no-recursion)

inline void ScreenCore::runMutation(Command const& command, CommandCtx const& ctx, Done done) {
    RtScope const& scope = *ctx.scope;
    NameRef const* const found = scope.find(command.target);
    if (found == nullptr || found->category != NameCategory::Mutation) {
        report(command.path, "`run` names no mutation here");
        if (done) {
            done(false);
        }
        return;
    }
    MutationRuntime& mutation = *found->mutation;
    RtScope const& policy = scope.policyScope();
    if (policy.submitted != nullptr) {
        policy.submitted->set(true);
    }
    if (policy.owner != nullptr && !runtime().untracked([&] { return policy.ready(); })) {
        if (done) {
            done(false);
        }
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
                     [bindings = ctx.bindings, as = command.binding, done = std::move(done)](bool succeeded,
                                                                                             Value const& reply) {
                         if (succeeded && !as.empty()) {
                             bindings->push_back(Binding{.name = as, .read = [reply] { return reply; }});
                         }
                         if (done) {
                             done(succeeded);
                         }
                     });
        return;
    }
    Value const each = evalIn(scope, ctx.bindings.get(), command.extra);
    ValueList const items = each.asList() == nullptr ? ValueList{} : *each.asList();
    if (items.empty()) {
        if (done) {
            done(true);
        }
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
                if (--state->left == 0 && state->done) {
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
            if (state->done) {
                state->done(state->ok);
            }
            return;
        }
        auto const self = weakStep.lock();
        mutation.run(bodies.at(index), bound.at(index), [self, state, index](bool succeeded, Value const&) {
            if (!succeeded) {
                if (state->done) {
                    state->done(false);
                }
                return;
            }
            (*self)(index + 1);
        });
    };
    (*step)(0);
}

inline void ScreenCore::runSet(Command const& command, CommandCtx const& ctx, Done const& done) {
    RtScope const& scope = *ctx.scope;
    auto finish = [&done](bool succeeded) {
        if (done) {
            done(succeeded);
        }
    };
    auto path = splitPath(command.target);
    if (!path || path->empty()) {
        finish(false);
        return;
    }
    std::string const head = path->front();
    std::span<std::string const> const rest = std::span<std::string const>{*path}.subspan(1);
    Value value = evalIn(scope, ctx.bindings.get(), command.value);
    if (head == "app") {
        std::string name;
        for (std::string const& segment : rest) {
            name += name.empty() ? segment : "." + segment;
        }
        finish(host != nullptr && host->setAppValue(name, value) ? true : unsupported(command, "`set` of `app`"));
        return;
    }
    NameRef const* const found = scope.find(head);
    if (found != nullptr && found->category == NameCategory::State) {
        StateSlot& slot = *found->state;
        initState(*slot.scope, slot);
        Type const& type = slot.decl->type;
        if (rest.empty() && !type.isAny() && !type.admits(value)) {
            report(command.path, "`set` writes a value that is not a " + type.name());
            finish(false);
            return;
        }
        Value next = withPath(slot.value->peek(), rest, std::move(value));
        session.noteWrite();
        slot.value->set(std::move(next));
        finish(true);
        return;
    }
    if (found != nullptr && found->category == NameCategory::Element) {
        ElementState& element = *found->element;
        session.noteWrite();
        element.draft.set(value);
        if (element.bound == nullptr) {
            element.committed.set(std::move(value));
        }
        finish(true);
        return;
    }
    report(command.path, "`set` names nothing writable here");
    finish(false);
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
    bool ok = true;
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
        ok = false;
    }
    if (done) {
        done(ok);
    }
}

inline void ScreenCore::runHostCommand(Command const& command, CommandCtx const& ctx, Done done) {
    Value const args = evalIn(*ctx.scope, ctx.bindings.get(), command.value);
    bool const known = host != nullptr && host->hostCommand(command.target, args);
    if (!known) {
        report(command.path, "host command `" + command.target + "` is missing");
    }
    if (done) {
        done(known);
    }
}

inline void ScreenCore::runTransfer(Command const& command, CommandCtx const& ctx, Done done) {
    RtScope const& scope = *ctx.scope;
    Value const file = evalIn(scope, ctx.bindings.get(), command.value);
    std::weak_ptr<RtScope> const weak = ctx.scope;
    if (command.kind == CommandKind::Download) {
        Value const name = evalIn(scope, ctx.bindings.get(), command.extra);
        auto settled = [this, weak, ctx, target = command.target, preview = command.option == "preview",
                        done](std::expected<Value, std::string> const& reply) {
            auto const held = weak.lock();
            if (!held) {
                return;
            }
            runtime().batch([&] {
                bool ok = reply.has_value();
                if (ok && preview) {
                    NameRef const* const found = held->find(target);
                    if (found != nullptr && found->category == NameCategory::State) {
                        initState(*found->state->scope, *found->state);
                        session.noteWrite();
                        found->state->value->set(*reply);
                    }
                }
                if (done) {
                    done(ok);
                }
            });
        };
        if (host == nullptr || !host->download(file, command.option, name, settled)) {
            unsupported(command, "`download`");
            if (done) {
                done(false);
            }
        }
        return;
    }
    NameRef const* const transferName = command.transfer.empty() ? nullptr : scope.find(command.transfer);
    TransferState* const transfer =
        transferName != nullptr && transferName->category == NameCategory::Transfer ? transferName->transfer : nullptr;
    auto progress = [weak, transfer](Value fraction) {
        if (weak.lock() && transfer != nullptr) {
            transfer->progress.set(std::move(fraction));
        }
    };
    auto settled = [this, weak, ctx, transfer, command = &command, done](std::expected<Value, std::string> reply) {
        auto const held = weak.lock();
        if (!held) {
            return;
        }
        runtime().batch([&] {
            if (transfer != nullptr) {
                transfer->pending.set(false);
                transfer->error.set(reply ? Value{} : Value{reply.error()});
            }
            if (!reply) {
                if (done) {
                    done(false);
                }
                return;
            }
            auto bindings = std::make_shared<Bindings>(*ctx.bindings);
            if (!command->binding.empty()) {
                bindings->push_back(Binding{.name = command->binding, .read = [ref = *reply] { return ref; }});
            }
            Command run;
            run.kind = CommandKind::Run;
            run.path = command->path;
            run.target = command->target;
            CommandCtx const next{
                .scope = held, .bindings = bindings, .cancelClose = ctx.cancelClose, .boundary = ctx.boundary};
            runMutation(run, next, done);
        });
    };
    if (host == nullptr) {
        unsupported(command, "`upload`");
        if (done) {
            done(false);
        }
        return;
    }
    if (transfer != nullptr) {
        transfer->pending.set(true);
        transfer->progress.set(Value{std::int64_t{0}});
        transfer->error.set(Value{});
    }
    if (!host->upload(file, progress, settled)) {
        if (transfer != nullptr) {
            transfer->pending.set(false);
        }
        unsupported(command, "`upload`");
        if (done) {
            done(false);
        }
    }
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

/// @brief A value as an integer: an `int`, or a decimal truncated toward zero.
/// @param value    The value.
/// @param fallback What anything else gives.
/// @return It.
[[nodiscard]] inline std::int64_t intOf(Value const& value, std::int64_t fallback) {
    if (std::int64_t const* const number = value.asInt()) {
        return *number;
    }
    if (Decimal const* const number = value.asDecimal()) {
        return static_cast<std::int64_t>(number->value.toDouble());
    }
    return fallback;
}

/// @brief A text role by name.
/// @param value  `normal`, `muted`, `heading` (or `title`), `error`, `success`.
/// @param format Unused.
/// @return The role; `Normal` for anything else.
[[nodiscard]] inline TextRole roleOf(Value const& value, FormatContext const& format) {
    std::string const text = textOf(value, format);
    if (text == "muted") {
        return TextRole::Muted;
    }
    if (text == "heading" || text == "title") {
        return TextRole::Heading;
    }
    if (text == "error") {
        return TextRole::Error;
    }
    if (text == "success") {
        return TextRole::Success;
    }
    return TextRole::Normal;
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

/// @brief An axis by name: `horizontal`, or vertical for anything else.
/// @param value The value.
/// @return The axis.
[[nodiscard]] inline Axis axisOf(Value const& value) {
    std::string const* const text = value.asString();
    return text != nullptr && *text == "horizontal" ? Axis::Horizontal : Axis::Vertical;
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

inline Node ScreenCore::lower(DocNode const& node, LowerCtx const& ctx) {
    return lowerKind(node, ctx, elementFor(*ctx.scope, node));
}

inline void ScreenCore::commonOf(DocNode const& node, LowerCtx const& ctx, ElementState* element, Common& common) {
    static_cast<void>(element);
    common.visible = bindProp<bool>(ctx, node.prop("visible"), true, boolOf);
    common.enabled = bindProp<bool>(ctx, node.prop("enabled"), true, boolOf);
    common.tooltip = bindProp<std::string>(ctx, node.prop("tooltip"), std::string{}, stringOf);
    if (ExprPtr const sizing = node.prop("sizing")) {
        common.layout.width = sizingOf(once(ctx, sizing));
    }
    common.testId = std::string{node.name("testId")};
    common.surface = std::string{node.name("surface")};
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
        common.onDrop = [this, ctx, commands = drop->event("onDrop")](Key key) {
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
    if (element != nullptr && element->errors != nullptr) {
        field.errors = slotProp<std::vector<std::string>>(
            [this, scope = ctx.scope, element] { return stringsOf(elementValue(*element, "shownErrors")); });
    } else if (ExprPtr const errors = node.prop("errors")) {
        field.errors = slotProp<std::vector<std::string>>([scope = ctx.scope, bindings = ctx.bindings, errors] {
            return stringsOf(evalIn(*scope, bindings.get(), errors));
        });
    }
}

// NOLINTBEGIN(misc-no-recursion): a menu nests, and lowering descends the document.
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
                    bool const* const flag = evalIn(*scope, bindings.get(), checked).asBool();
                    return flag == nullptr ? std::nullopt : std::optional<bool>{*flag};
                });
        }
        item.enabled = bindProp<bool>(ctx, part->prop("enabled"), true, boolOf);
        item.items = menuItems(part->partList("items"), ctx);
        items.push_back(std::move(item));
    }
    return items;
}

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
    auto text = [&](std::string_view key) {
        return bindProp<std::string>(ctx, node.prop(key), std::string{}, stringOf);
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
        Text spec{.text = text("text"), .role = bindProp<TextRole>(ctx, node.prop("role"), TextRole::Normal, roleOf)};
        fieldOf(node, ctx, element, spec.field);
        commonOf(node, ctx, element, spec.common);
        return ui::text(std::move(spec));
    }
    if (kind == "button") {
        Button spec{.label = text("label")};
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
        spec.onCommit = [this, ctx, element, commit, onCommit = node.event("onCommit")](std::string text) {
            element->touched.set(true);
            commit("blur", text);
            runEvent(onCommit, ctx, Value{text});
        };
        spec.onSubmit = [this, ctx, commit, onSubmit = node.event("onSubmit")](std::string text) {
            commit("enter", text);
            runEvent(onSubmit, ctx, Value{text});
        };
        spec.placeholder = text("placeholder");
        std::string const mode = onceText("mode");
        spec.mode = mode == "multiline"  ? TextInputMode::Multiline
                    : mode == "password" ? TextInputMode::Password
                                         : TextInputMode::SingleLine;
        fieldOf(node, ctx, element, spec.field);
        commonOf(node, ctx, element, spec.common);
        return ui::textInput(std::move(spec));
    }
    if (kind == "checkbox") {
        Checkbox spec{.label = text("label"), .checked = slotProp<bool>([read = read("checked")] {
                                                  bool const* const flag = read().asBool();
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
                         onChange = node.event("onChange")](Key key) {
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
        fieldOf(node, ctx, element, spec.field);
        commonOf(node, ctx, element, spec.common);
        return ui::slider(std::move(spec));
    }
    if (kind == "dateTimeInput") {
        DateTimeInput spec;
        spec.value = slotProp<std::optional<time::Timestamp>>([element]() -> std::optional<time::Timestamp> {
            time::DateTime const* const instant = element->draft.get().asTimestamp();
            return instant == nullptr ? std::nullopt : std::optional<time::Timestamp>{time::Timestamp{*instant}};
        });
        spec.onChange = [this, ctx, element, onChange = node.event("onChange")](std::optional<time::Timestamp> value) {
            Value const entered = value && value->hasValue() ? Value{*value->value} : Value{};
            session.asUser([&] {
                session.noteWrite();
                element->draft.set(entered);
                element->edited.set(true);
                if (element->bound == nullptr) {
                    element->committed.set(entered);
                }
            });
            runEvent(onChange, ctx, entered);
        };
        spec.mode = onceText("mode") == "date" ? DateMode::Date : DateMode::DateTime;
        spec.offsetMinutes = format.utcOffsetMinutes;
        fieldOf(node, ctx, element, spec.field);
        commonOf(node, ctx, element, spec.common);
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
        return ui::filePicker(std::move(spec));
    }
    if (kind == "menu") {
        Menu spec{.items = menuItems(node.partList("items"), ctx)};
        commonOf(node, ctx, element, spec.common);
        return ui::menu(std::move(spec));
    }
    if (kind == "column" || kind == "row") {
        int const gap = static_cast<int>(onceInt("gap", 0));
        if (kind == "column") {
            Column spec{.children = children("children"), .gap = gap};
            commonOf(node, ctx, element, spec.common);
            return ui::column(std::move(spec));
        }
        Row spec{.children = children("children"), .gap = gap};
        commonOf(node, ctx, element, spec.common);
        return ui::row(std::move(spec));
    }
    if (kind == "grid") {
        Grid spec{.columns = static_cast<int>(onceInt("columns", 1)), .gap = static_cast<int>(onceInt("gap", 0))};
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
        Panel spec{.title = text("title"), .padding = static_cast<int>(onceInt("padding", 0))};
        spec.child = child("children");
        spec.collapsible = onceBool("collapsible", false);
        spec.collapsed = bindProp<bool>(ctx, node.prop("collapsed"), false, boolOf);
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
        Tabs spec;
        for (DocNodePtr const& tab : node.partList("tabs")) {
            ExprPtr const label = tab->prop("label");
            auto const nodes = tab->nodes("children");
            spec.tabs.push_back(Tab{.label = label ? textOf(once(ctx, label), format) : std::string{},
                                    .node = nodes.empty() ? Node{} : lowerAll(nodes, ctx)});
        }
        spec.selected = std::size_t{0};
        commonOf(node, ctx, element, spec.common);
        return ui::tabs(std::move(spec));
    }
    if (kind == "dialog" || kind == "drawer") {
        return lowerDialog(node, ctx, element);
    }
    if (kind == "busy") {
        Busy spec{.active = bindProp<bool>(ctx, node.prop("active"), false, boolOf), .label = text("label")};
        commonOf(node, ctx, element, spec.common);
        return ui::busy(std::move(spec));
    }
    if (kind == "forEach" || kind == "table") {
        return lowerRows(node, ctx, element);
    }
    if (kind == "banner") {
        Banner spec{.tone = bindProp<Tone>(ctx, node.prop("tone"), Tone::Info, toneOf), .text = text("text")};
        spec.action = actionPart();
        spec.dismissible = onceBool("dismissible", false);
        spec.onDismiss = action("onDismiss");
        commonOf(node, ctx, element, spec.common);
        return ui::banner(std::move(spec));
    }
    if (kind == "badge") {
        Badge spec{.tone = bindProp<Tone>(ctx, node.prop("tone"), Tone::Neutral, toneOf), .text = text("text")};
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
                    return number->value.toDouble();
                }
                return std::nullopt;
            });
        spec.label = text("label");
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
                                 .state = name == "active"   ? StepState::Active
                                          : name == "done"   ? StepState::Done
                                          : name == "failed" ? StepState::Failed
                                                             : StepState::Pending});
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
        EmptyState spec{.title = text("title"), .text = text("text")};
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
        commonOf(node, ctx, element, spec.common);
        return ui::splitter(std::move(spec));
    }
    if (kind == "collapsible") {
        Collapsible spec{.title = text("title"), .open = bindProp<bool>(ctx, node.prop("open"), true, boolOf)};
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
            spec.onDrop = [this, ctx, commands](std::vector<std::string> paths) {
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
    report(node.path, "`" + node.kind + "` needs the forms engine, which this interpreter does not lower yet");
    return ui::text(Text{.text = std::string{node.kind}, .role = TextRole::Muted});
}

inline Node ScreenCore::lowerDialog(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    Prop<bool> open = node.prop("open") ? bindProp<bool>(ctx, node.prop("open"), false, boolOf)
                                        : slotProp<bool>([element] { return element->open.get(); });
    Node content = lazy([this, ctx, &node, element]() -> Node {
        auto inner = std::make_shared<RtScope>(*this, ctx.scope, nullptr, ctx.scope->nodes.depth() + 2);
        inner->owner = element;
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
        declare(*inner, node.scope ? &*node.scope : nullptr, content, lists);
        element->inner.set(inner.get());
        LowerCtx const innerCtx{.scope = inner, .bindings = ctx.bindings, .boundary = ctx.boundary};
        std::vector<Node> children;
        for (DocNode const* const child : content) {
            children.push_back(lower(*child, innerCtx));
        }
        std::vector<KeyBinding> keys;
        if (Commands const* const commands = node.event("default")) {
            keys.push_back(KeyBinding{
                .chord = "Enter", .onPress = [this, innerCtx, commands] { runEvent(commands, innerCtx, Value{}); }});
        }
        if (Commands const* const commands = node.event("cancel")) {
            keys.push_back(KeyBinding{
                .chord = "Escape", .onPress = [this, innerCtx, commands] { runEvent(commands, innerCtx, Value{}); }});
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
            .child = std::move(content),
            .onDismiss = std::move(onDismiss),
            .side = side && textOf(once(ctx, side), session.formatting()) == "start" ? Side::Start : Side::End};
        commonOf(node, ctx, element, spec.common);
        return ui::drawer(std::move(spec));
    }
    Dialog spec{.open = std::move(open),
                .title = bindProp<std::string>(ctx, node.prop("title"), std::string{}, stringOf),
                .child = std::move(content),
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
                for (DocNode const* const child : content) {
                    children.push_back(lower(*child, innerCtx));
                }
                return holding(std::move(children), inner);
            })});
    }
    spec.selector =
        slotProp<Key>([scope = ctx.scope, bindings = ctx.bindings, on = node.prop("on"), whens = std::move(whens)] {
            Value const value = evalIn(*scope, bindings.get(), on);
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

inline std::shared_ptr<RtScope> ScreenCore::rowScope(DocNode const& node, LowerCtx const& ctx,
                                                     reactive::Signal<Value> const& row,
                                                     std::vector<DocNode const*> const& content) {
    auto scope = std::make_shared<RtScope>(*this, ctx.scope, &row, ctx.scope->nodes.depth() + 2);
    declare(*scope, node.scope ? &*node.scope : nullptr, content, {});
    return scope;
}

inline Node ScreenCore::lowerRows(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    ExprPtr const rowsExpr = node.prop("rows");
    if (!rowsExpr) {
        report(node.path, "a model-backed `table` needs the table engine, which this interpreter does not lower yet");
        return ui::text(Text{.text = std::string{"table"}, .role = TextRole::Muted});
    }
    auto rows = [scope = ctx.scope, bindings = ctx.bindings, rowsExpr] {
        Value const value = evalIn(*scope, bindings.get(), rowsExpr);
        ValueList const* const items = value.asList();
        return items == nullptr ? std::vector<Value>{} : std::vector<Value>{items->begin(), items->end()};
    };
    auto keyFn = [scope = ctx.scope, bindings = ctx.bindings, keyExpr = node.prop("key")](Value const& row) -> Key {
        if (!keyExpr) {
            Value const id = rowId(row);
            return keyOf(id.isNull() ? row : id);
        }
        ScopeEnv const env{*scope, bindings.get(), &row};
        return keyOf(evaluate(*keyExpr, env));
    };
    if (node.kind == "forEach") {
        std::vector<DocNode const*> content;
        for (DocNodePtr const& child : node.nodes("children")) {
            content.push_back(child.get());
        }
        auto rowView = [this, ctx, &node, content](reactive::Signal<Value> const& row) -> Node {
            auto scope = rowScope(node, ctx, row, content);
            LowerCtx const rowCtx{.scope = scope, .bindings = ctx.bindings, .boundary = ctx.boundary};
            std::vector<Node> children;
            for (DocNode const* const child : content) {
                children.push_back(lower(*child, rowCtx));
            }
            return holding(std::move(children), scope);
        };
        ExprPtr const axis = node.prop("axis");
        ExprPtr const gap = node.prop("gap");
        return ui::forEach<Value>(std::move(rows), std::move(keyFn), std::move(rowView),
                                  axis ? axisOf(once(ctx, axis)) : Axis::Vertical,
                                  gap ? static_cast<int>(intOf(once(ctx, gap), 0)) : 0);
    }
    std::vector<TableColumn> columns;
    std::vector<DocNode const*> cells;
    std::vector<std::string> fields;
    FormatContext const& format = session.formatting();
    if (ExprPtr const derived = node.prop("columns")) {
        Value const list = once(ctx, derived);
        if (ValueList const* const items = list.asList()) {
            for (Value const& item : *items) {
                Value const* const id = item.field("id");
                Value const* const label = item.field("label");
                fields.push_back(id == nullptr ? std::string{} : textOf(*id, format));
                columns.push_back(
                    TableColumn{.label = textOf(label != nullptr ? *label : Value{fields.back()}, format)});
                cells.push_back(nullptr);
            }
        }
    }
    for (DocNodePtr const& part : node.partList("columns")) {
        ExprPtr const label = part->prop("label");
        ExprPtr const width = part->prop("width");
        columns.push_back(TableColumn{.label = label ? textOf(once(ctx, label), format) : std::string{},
                                      .width = width ? sizingOf(once(ctx, width)) : Sizing::content()});
        cells.push_back(part.get());
        fields.push_back(std::string{part->id});
    }
    auto registry = std::make_shared<std::map<std::string, std::weak_ptr<RtScope>, std::less<>>>();
    auto cellsOf = [this, ctx, &node, cells, fields, registry, keyFn](reactive::Signal<Value> const& row) {
        std::vector<DocNode const*> content;
        for (DocNode const* const column : cells) {
            if (column != nullptr) {
                for (DocNodePtr const& cell : column->nodes("cell")) {
                    content.push_back(cell.get());
                }
            }
        }
        auto scope = rowScope(node, ctx, row, content);
        (*registry)[canonicalJson(valueOfKey(keyFn(row.peek())))] = scope;
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
                                        })}));
        }
        if (!out.empty()) {
            out.front() = holding({out.front()}, scope);
        }
        return out;
    };
    TableOptions options;
    std::string const mode =
        node.prop("selectionMode") ? textOf(once(ctx, node.prop("selectionMode")), format) : std::string{};
    options.selectionMode = mode == "single"     ? SelectionMode::Single
                            : mode == "multiple" ? SelectionMode::Multiple
                                                 : SelectionMode::None;
    options.selection = slotProp<std::vector<Key>>([element] {
        std::vector<Key> keys;
        if (ValueList const* const items = element->selection.get().asList()) {
            for (Value const& item : *items) {
                keys.push_back(keyOf(item));
            }
        }
        return keys;
    });
    options.onSelectionChange = [this, element](std::vector<Key> keys) {
        ValueList items;
        for (Key const& key : keys) {
            items.push_back(valueOfKey(key));
        }
        session.asUser([&] {
            session.noteWrite();
            element->selection.set(Value{std::move(items)});
        });
    };
    options.onActivate = [this, ctx, element, registry, commands = node.event("onActivate")](Key key) {
        Value const activated = valueOfKey(key);
        session.asUser([&] {
            session.noteWrite();
            element->activeKey.set(activated);
        });
        auto const found = registry->find(canonicalJson(activated));
        std::shared_ptr<RtScope> scope = found == registry->end() ? nullptr : found->second.lock();
        LowerCtx const rowCtx{.scope = scope ? scope : ctx.scope, .bindings = ctx.bindings, .boundary = ctx.boundary};
        runEvent(commands, rowCtx, activated);
    };
    commonOf(node, ctx, element, options.common);
    return ui::table<Value>(std::move(columns), std::move(rows), std::move(keyFn), std::move(cellsOf),
                            std::move(options));
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
    auto bindings = std::make_shared<Bindings>(ctx.bindings ? *ctx.bindings : Bindings{});
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
    auto const found = _doc->components.find(name);
    if (found == _doc->components.end() || !found->second.fallback) {
        report(node.path, "component `" + name + "` is not declared");
        return ui::text(Text{.text = name});
    }
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
    DocNode const& fallback = *found->second.fallback;
    declare(*scope, nullptr, {&fallback}, {});
    Node lowered = lower(fallback, LowerCtx{.scope = scope, .bindings = bindings, .boundary = nullptr});
    return ui::custom(Custom{.name = name, .fallback = std::move(lowered)});
}

inline Node ScreenCore::lowerCustomize(DocNode const& node, LowerCtx const& ctx) {
    std::string const point{node.name("point")};
    auto const found = _doc->customize.find(point);
    if (found == _doc->customize.end()) {
        report(node.path, "customization point `" + point + "` is not declared");
        return ui::text(Text{.text = point});
    }
    CustomizePoint const& spec = found->second;
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
    if (spec.kind == "choice") {
        Select select;
        std::vector<SelectOption> options;
        for (std::string const& option : spec.options) {
            options.push_back(SelectOption{.key = Key{option}, .label = option});
        }
        select.options = std::move(options);
        select.selected = slotProp<std::optional<Key>>([this, point]() -> std::optional<Key> {
            auto const chosen = prefs.get().find(point);
            if (chosen == prefs.get().end() || chosen->second.isNull()) {
                return std::nullopt;
            }
            return keyOf(chosen->second);
        });
        select.onSelect = [choose](Key key) { choose(valueOfKey(key)); };
        commonOf(node, ctx, nullptr, select.common);
        return ui::select(std::move(select));
    }
    if (spec.kind == "value" && (spec.type.kind == TypeKind::String || spec.type.isAny())) {
        TextInput input;
        input.value = slotProp<std::string>([this, point] {
            auto const chosen = prefs.get().find(point);
            return chosen == prefs.get().end() ? std::string{} : textOf(chosen->second, session.formatting());
        });
        input.onChange = [choose](std::string text) { choose(Value{std::move(text)}); };
        commonOf(node, ctx, nullptr, input.common);
        return ui::textInput(std::move(input));
    }
    return ui::text(Text{.text = point, .role = TextRole::Muted});
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
                                                                          MountOptions options) {
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
    screen->_scope = std::make_shared<detail::RtScope>(core, nullptr, nullptr, options.depth);
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
    Node root = runtime.untracked([&] {
        core.declare(scope, &doc->scope, {doc->view.get()}, lists);
        detail::LowerCtx const ctx{
            .scope = screen->_scope, .bindings = std::make_shared<detail::Bindings const>(), .boundary = nullptr};
        Node lowered = core.lower(*doc->view, ctx);
        if (doc->keys.empty()) {
            return lowered;
        }
        std::vector<KeyBinding> keys;
        for (auto const& [chord, commands] : doc->keys) {
            keys.push_back(KeyBinding{
                .chord = chord, .onPress = [&core, ctx, list = &commands] { core.runEvent(list, ctx, Value{}); }});
        }
        Column column{.children = {std::move(lowered)}};
        column.common.keys = std::move(keys);
        return ui::column(std::move(column));
    });
    screen->_mounted.emplace(runtime, backend, std::move(root), options.parent, options.depth + 1);
    screen->runLifecycle("onMount");
    return screen;
}

}  // namespace morph::ui

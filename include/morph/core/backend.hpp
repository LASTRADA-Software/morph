// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../session/session.hpp"
#include "completion.hpp"
#include "model.hpp"
#include "observability.hpp"
#include "registry.hpp"
#include "strand.hpp"

namespace morph::backend {

namespace detail {

/// @brief All the information needed to dispatch one action call through a backend.
///
/// Backends that execute locally use `localOp` directly. Remote backends
/// serialize via `serializeAction` and deserialize replies via `deserializeResult`.
struct ActionCall {
    /// @brief String id of the target model type (from `ModelTraits`).
    std::string modelTypeId;

    /// @brief String id of the action type (from `ActionTraits`).
    std::string actionTypeId;

    /// @brief Serialises the action to JSON. Called only on the remote path.
    std::function<std::string()> serializeAction;

    /// @brief Deserialises a JSON reply into the opaque result `shared_ptr<void>`.
    std::function<std::shared_ptr<void>(std::string_view)> deserializeResult;

    /// @brief Executes the action directly against a model holder. Used on the local path.
    std::function<std::shared_ptr<void>(::morph::model::detail::IModelHolder&)> localOp;

    /// @brief Session context attached to this call.
    ///
    /// Local backends thread it through a thread-local before invoking `localOp`;
    /// remote backends serialise it into the wire envelope.
    ::morph::session::Context session;
};

/// @brief The two identities a model instance can carry, passed together.
///
/// Bundled into one struct rather than passed as two adjacent `string_view`
/// parameters because they are trivially swappable at a call site and mean
/// entirely different things: transposing them would silently file journal
/// entries under the directory key and share instances under the log's entity
/// key. Keeping them named at every call site makes that mistake unwritable.
struct InstanceIdentity {
    /// @brief Entity key for the action log; empty if none. See `journal::LogEntry::entityKey`.
    std::string_view contextKey;

    /// @brief Canonical string encoding of the primary key; empty if the
    ///        instance is anonymous and therefore unshareable.
    std::string_view primary;
};

/// @brief Abstract interface for execution backends (local, remote, …).
///
/// A backend owns model instances and dispatches actions against them.
/// `Bridge` holds one active backend at a time and can swap it atomically
/// via `Bridge::switchBackend()`.
// NOLINTBEGIN(cppcoreguidelines-special-member-functions)
struct IBackend {
    virtual ~IBackend() = default;

    /// @brief Registers a new model instance and returns its opaque id.
    virtual ::morph::exec::detail::ModelId registerModel(
        const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory) = 0;

    /// @brief Registers a new model instance, additionally passing @p contextKey —
    ///        the instance's stable identity (e.g. an account id) — through to
    ///        backends that can make use of it.
    ///
    /// Default implementation forwards to `registerModel()` and drops @p contextKey,
    /// which is exactly correct for `LocalBackend`: the caller's own @p factory
    /// closure already captures whatever identity it needs directly (see
    /// `IModelHolder::attachActionLog`), so there is nothing for the backend to
    /// forward. Backends whose model instances live behind a wire protocol
    /// (`SimulatedRemoteBackend`) override this to carry @p contextKey across —
    /// see `wire::Envelope::contextKey` and `RemoteServer::setLogProvider`.
    /// @param typeId     String type-id of the model to instantiate.
    /// @param factory    Callable that constructs the `IModelHolder` (local path only).
    /// @param contextKey Stable identity of the new instance; empty if none.
    /// @return Newly assigned `ModelId`.
    virtual ::morph::exec::detail::ModelId registerModelWithContext(
        const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
        std::string_view contextKey) {
        (void)contextKey;
        return registerModel(typeId, std::move(factory));
    }

    /// @brief Optional non-blocking counterpart to `registerModelWithContext`.
    ///
    /// `registerModelWithContext`/`registerModel` are synchronous: a backend
    /// whose registration requires a round-trip (a socket backend) can only
    /// implement that by blocking the calling thread until the reply arrives —
    /// `QtWebSocketBackend` does this via a nested `QEventLoop`. On a WASM main
    /// thread, Qt refuses to spin a nested loop at all
    /// (`WaitForMoreEvents is not supported on the main thread without asyncify`),
    /// so that blocking call aborts the page — the very first `registerModel`
    /// a WASM client makes.
    ///
    /// Overriding this lets such a backend register without blocking: send the
    /// request and return `true` immediately, then invoke exactly one of
    /// @p onRegistered / @p onError once the reply arrives, on the backend's own
    /// thread (unless the backend is destroyed first, in which case neither
    /// fires). `Bridge::registerHandler()` prefers this path when it is
    /// available and falls back to the synchronous `registerModelWithContext`
    /// otherwise, so every backend that has not opted in (every backend as of
    /// this writing, other than `QtWebSocketBackend`) is unaffected.
    ///
    /// The default implementation offers no async path and returns `false`
    /// without calling either callback — the caller (`Bridge::registerHandler`)
    /// falls back to `registerModelWithContext` in that case, matching every
    /// caller's behavior before this method existed.
    ///
    /// @note Scope: only `Bridge::registerHandler()`'s plain (non-shared)
    ///       registration path — a `BridgeHandler`'s initial construction —
    ///       uses this. Shared/keyed registration (`registerModelShared`,
    ///       `attachModel`) and the re-registration `switchBackend()`/the
    ///       reconnect handler perform after a backend swap remain
    ///       synchronous; see docs/spec/core/backend.md.
    /// @param typeId       String type-id of the model to instantiate.
    /// @param factory      Callable that constructs the `IModelHolder` (local path only).
    /// @param contextKey   Stable identity of the new instance; empty if none.
    /// @param onRegistered Invoked with the assigned `ModelId` on success.
    /// @param onError      Invoked with a diagnostic message on failure.
    /// @return `true` if this backend accepted the request and will invoke
    ///         exactly one callback later; `false` if it has no async path
    ///         (neither callback is invoked in that case).
    virtual bool registerModelAsync(
        const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
        std::string_view contextKey, std::function<void(::morph::exec::detail::ModelId)> onRegistered,
        std::function<void(const std::string&)> onError) {
        (void)typeId;
        (void)factory;
        (void)contextKey;
        (void)onRegistered;
        (void)onError;
        return false;
    }

    /// @brief Registers or attaches to the shared instance holding @p primary.
    ///
    /// A *register-or-attach*: if an instance for `(typeId, primary)` is already
    /// live in the backend's shared directory, its id is returned and its attach
    /// count incremented — no new instance is created and @p factory is not
    /// called. Otherwise a new instance is created, entered in the directory,
    /// and returned with an attach count of one.
    ///
    /// An empty @p primary means "no identity": the call degrades to
    /// `registerModelWithContext`, producing a private instance that never
    /// enters the directory and can never be shared.
    ///
    /// The default implementation ignores @p primary and forwards to
    /// `registerModelWithContext`, so a backend that has not implemented sharing
    /// keeps its existing one-instance-per-caller behaviour rather than silently
    /// handing two callers the same instance.
    ///
    /// @param typeId     String type-id of the model.
    /// @param factory    Callable that constructs the `IModelHolder` (local path only).
    /// @param identity   Entity key for the action log plus the directory primary key.
    /// @return Id of the shared (or newly created) instance.
    virtual ::morph::exec::detail::ModelId registerModelShared(
        const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
        InstanceIdentity identity) {
        return registerModelWithContext(typeId, std::move(factory), identity.contextKey);
    }

    /// @brief Re-points from @p current to the shared instance holding @p primary.
    ///
    /// The default implementation acquires the replacement via
    /// `registerModelShared` first and only then releases @p current (when
    /// non-zero), so a same-key re-attach never destroys and recreates the
    /// instance it already holds, and a throwing acquire never strands the
    /// caller with neither instance. Backends behind a wire protocol override
    /// this with the single `attach` request so a re-pointing client cannot
    /// lose its slot to `LimitPolicy::maxLiveModels` between the release and
    /// the acquire.
    ///
    /// @param typeId     String type-id of the model.
    /// @param factory    Callable that constructs the `IModelHolder` (local path only).
    /// @param identity   Entity key for the action log plus the directory primary key.
    /// @param current    Instance currently held, or `ModelId{0}` if none.
    /// @return Id of the instance now attached to.
    virtual ::morph::exec::detail::ModelId attachModel(
        const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
        InstanceIdentity identity, ::morph::exec::detail::ModelId current) {
        // Acquire the replacement before releasing `current`, not after: a
        // same-key re-attach (registerModelShared finds `current` already
        // live in the directory and takes a second reference to it) then
        // hands back the identical id instead of destroying and recreating
        // it, and a throwing acquire never touches `current` at all, so the
        // caller's existing instance is never stranded by a failed attach.
        // Either way, exactly one reference on `current` needs releasing
        // afterward: the genuinely old instance's, if this re-pointed to a
        // different key; or the redundant one registerModelShared just took,
        // if it did not.
        auto next = registerModelShared(typeId, std::move(factory), identity);
        if (current.v != 0U) {
            deregisterModel(current);
        }
        return next;
    }

    /// @brief Enters an already-live instance into the directory under @p primary.
    ///
    /// The *promotion* half of keyed instances, and what makes a result-sourced
    /// key work without losing state: an action that creates its own entity runs
    /// on an instance that does not yet have a key, and the key only exists once
    /// the result comes back. Re-pointing to a freshly created instance would
    /// strand everything the create just did, so instead the instance the action
    /// ran on is given the generated key in place.
    ///
    /// A no-op when @p primary is empty, when @p mid is not live, when another
    /// instance already holds that key, or when @p mid itself already holds a
    /// *different* real key. The existing holder of a key always wins (a
    /// promotion can never silently displace a directory entry other handlers
    /// are attached to), and an already-keyed instance never changes key (a
    /// promotion can never silently move one out from under handlers already
    /// attached to it) — only a still-anonymous instance can ever be promoted.
    ///
    /// @param mid     Live instance to promote.
    /// @param typeId  Model type id — the directory's first key component.
    /// @param primary Canonical string encoding of the key to file it under.
    virtual void assignPrimary(::morph::exec::detail::ModelId mid, const std::string& typeId,
                               std::string_view primary) {
        (void)mid;
        (void)typeId;
        (void)primary;
    }

    /// @brief Lists the primary keys of live shared instances of @p typeId.
    ///
    /// Only instances created through `registerModelShared`/`attachModel` with a
    /// non-empty primary appear; a private instance is invisible to the
    /// directory by construction. The result is a snapshot and is stale the
    /// moment it is returned.
    ///
    /// Synchronous, matching `registerModel`, which already blocks on remote
    /// backends. The asynchronous surface users see is
    /// `BridgeHandler::instances()`, which wraps this in a `Completion` so the
    /// call site reads identically local and remote.
    ///
    /// @param typeId String type-id to enumerate.
    /// @return Canonical key strings of the live shared instances; empty by default.
    virtual std::vector<std::string> listInstances(const std::string& typeId) {
        (void)typeId;
        return {};
    }

    /// @brief Removes the model identified by @p mid from the backend.
    ///
    /// For a shared instance this *decrements* its attach count and destroys the
    /// instance only when the count reaches zero, so one caller releasing an
    /// instance never tears it out from under another that is still attached.
    virtual void deregisterModel(::morph::exec::detail::ModelId mid) = 0;

    /// @brief Dispatches @p call against the model identified by @p mid.
    virtual ::morph::async::Completion<std::shared_ptr<void>> execute(::morph::exec::detail::ModelId mid,
                                                                      ActionCall call,
                                                                      ::morph::exec::IExecutor* cbExec) = 0;

    /// @brief Called by `Bridge::switchBackend()` after all handlers are re-registered.
    virtual void notifyBackendChanged() = 0;

    /// @brief Resolves every still-pending completion this backend produced with @p exc.
    ///
    /// Called by `Bridge::switchBackend()` on the outgoing backend after the swap,
    /// and by `Bridge`'s destructor. After this call, any later `setValue` /
    /// `setException` on those states is a no-op (the state is already ready), so
    /// in-flight server replies cannot resurrect a cancelled completion.
    virtual void cancelPending(const std::exception_ptr& exc) = 0;

    /// @brief Installs a callback invoked when the backend reconnects to its peer.
    ///
    /// Used by backends that may lose and re-establish their transport (e.g.
    /// `QtWebSocketBackend`). `Bridge` installs a handler that re-registers every
    /// live `HandlerBinding` so model ids stay valid after the reconnect.
    ///
    /// Deliberately fires only on the *second and later* connects, never the
    /// first — re-registering handlers only makes sense after a drop; on the
    /// first connect there is nothing yet to re-register. See
    /// `setConnectHandler` for a hook that also covers the first connect.
    ///
    /// Default implementation: store-and-ignore. Backends with no transport (e.g.
    /// `LocalBackend`) never invoke it.
    /// @param handler Callable invoked on the backend's transport thread after a
    ///                successful reconnect. Pass `nullptr` to clear.
    virtual void setReconnectHandler(const std::function<void()>& handler) { (void)handler; }

    /// @brief Installs a callback invoked on every successful connect, including the first.
    ///
    /// `setReconnectHandler` deliberately skips the first connect (there is
    /// nothing to re-register yet); this is the complementary hook for UI that
    /// needs to know the transport is up at all — a "connecting… / connected /
    /// offline" status indicator, for instance. `waitForConnected()` (where a
    /// concrete backend offers one, e.g. `QtWebSocketBackend`) answers the same
    /// question but blocks the calling thread, which is unusable on a
    /// browser/WASM main thread and undesirable even on desktop if it means
    /// blocking startup on a network round-trip; this hook is fired
    /// asynchronously instead.
    ///
    /// Default implementation: store-and-ignore. Backends with no transport (e.g.
    /// `LocalBackend`) never invoke it.
    /// @param handler Callable invoked on the backend's transport thread after
    ///                every successful connect (first and subsequent). Pass
    ///                `nullptr` to clear.
    virtual void setConnectHandler(const std::function<void()>& handler) { (void)handler; }

    /// @brief Installs a callback invoked whenever the transport drops.
    ///
    /// Fires before any reconnect is scheduled, so an observer sees the
    /// disconnected state even when a retry follows immediately — a status
    /// indicator that skipped straight from "connected" to a fresh "connected"
    /// (after an instant reconnect) would misreport an outage that did happen.
    /// Without this hook a client learns the socket dropped only indirectly,
    /// when a later action fails.
    ///
    /// Default implementation: store-and-ignore. Backends with no transport (e.g.
    /// `LocalBackend`) never invoke it.
    /// @param handler Callable invoked on the backend's transport thread whenever
    ///                the connection drops. Pass `nullptr` to clear.
    virtual void setDisconnectHandler(const std::function<void()>& handler) { (void)handler; }
};
// NOLINTEND(cppcoreguidelines-special-member-functions)

}  // namespace detail

/// @brief Thrown to in-flight `Completion`s when `Bridge::switchBackend()` runs.
///
/// Surfaces in the `.onError(...)` callback so the GUI can retry on the new backend
/// or surface a "backend changed" message — there is no public cancel API on
/// `Completion` itself.
struct BackendChangedError : std::runtime_error {
    /// @brief Constructs the error with a canned diagnostic message.
    BackendChangedError() : std::runtime_error{"backend changed before completion resolved"} {}
};

/// @brief Thrown to in-flight `Completion`s when `Bridge` is destroyed.
struct BridgeDestroyedError : std::runtime_error {
    /// @brief Constructs the error with a canned diagnostic message.
    BridgeDestroyedError() : std::runtime_error{"bridge destroyed before completion resolved"} {}
};

/// @brief Thrown to in-flight `Completion`s when a transport drops mid-call (e.g. a
///        Qt WebSocket disconnect). The framework retries the call on reconnect if
///        the backend supports it; otherwise the GUI's `.onError(...)` runs.
struct DisconnectedError : std::runtime_error {
    /// @brief Constructs the error with a canned diagnostic message.
    DisconnectedError() : std::runtime_error{"transport disconnected before completion resolved"} {}
};

/// @brief Thrown to a pending `Completion` when the server-side
///        `morph::backend::LimitPolicy::executeTimeout` elapses before the
///        model's action replies.
///
/// The action keeps running to completion on its strand — morph never
/// interrupts an in-flight `Model::execute` — but the caller's wait is bounded.
/// Distinguishes a timeout from any other `err` reply (a generic
/// `std::runtime_error` on `QtWebSocketBackend` / `SimulatedRemoteBackend`), so
/// callers can retry or surface a specific "request timed out" message. See
/// `docs/spec/core/backend.md` (`LimitPolicy`).
struct TimeoutError : std::runtime_error {
    /// @brief Constructs the error with a canned diagnostic message.
    TimeoutError() : std::runtime_error{"execute timed out on the server"} {}
};

/// @brief In-process backend that executes model actions on a thread pool strand.
///
/// Each model instance gets its own strand so actions are serialised per-model
/// without a global lock on the pool.
class LocalBackend : public detail::IBackend {
public:
    /// @brief Constructs the backend using @p workerPool to run model actions.
    /// @param workerPool Executor (typically a `ThreadPoolExecutor`) for model work.
    explicit LocalBackend(::morph::exec::IExecutor& workerPool) : _strand{workerPool} {}

    /// @brief Creates a model instance via @p factory and registers it.
    ///
    /// @p typeId is accepted for interface compatibility but not used — the
    /// concrete type is captured by the factory closure. If the new holder's
    /// `isBackendChangeAware()` returns `true`, @p mid is also recorded in
    /// `_changeAware` so `notifyBackendChanged()` finds it without a
    /// `dynamic_cast` sweep.
    /// @param factory  Callable that constructs the `IModelHolder`.
    /// @return Newly assigned `ModelId`.
    ::morph::exec::detail::ModelId registerModel(
        const std::string& /*typeId*/,
        std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory) override {
        ::morph::observe::detail::emitMetric(::morph::observe::Metric::registerCount, 1.0);
        ::morph::exec::detail::ModelId mid{_nextId.fetch_add(1) + 1};
        std::scoped_lock const lock{_regMtx};
        auto holder = factory();
        if (holder->isBackendChangeAware()) {
            _changeAware.insert(mid);
        }
        _models[mid] = std::move(holder);
        return mid;
    }

    /// @brief Registers or attaches to the shared instance holding @p primary.
    ///
    /// An empty @p primary bypasses the directory entirely and produces a
    /// private instance, exactly as `registerModel` does.
    /// @param typeId     String type-id of the model — the directory's first key component.
    /// @param factory    Callable that constructs the `IModelHolder`; not called on an attach.
    /// @param identity   Entity key for the action log plus the directory primary key.
    /// @return Id of the shared (or newly created) instance.
    ::morph::exec::detail::ModelId registerModelShared(
        const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
        detail::InstanceIdentity identity) override {
        if (identity.primary.empty()) {
            return registerModelWithContext(typeId, std::move(factory), identity.contextKey);
        }
        ::morph::observe::detail::emitMetric(::morph::observe::Metric::registerCount, 1.0);
        DirectoryKey dirKey{typeId, std::string{identity.primary}};
        std::scoped_lock const lock{_regMtx};
        if (auto found = _directory.find(dirKey); found != _directory.end()) {
            auto const foundMid = found->second;
            auto flagsIter = _hydrationFlags.find(foundMid);
            if (flagsIter != _hydrationFlags.end() && flagsIter->second->poisoned.load()) {
                // Its first action already failed; it must not be handed to a
                // new attacher. Evict it and fall through to the fresh-
                // instance path below, exactly as if this had been a
                // directory miss. Its own attachCount reference is untouched,
                // so whoever created it still tears it down normally when
                // they release it.
                _directory.erase(found);
                _sharedKeyOf.erase(foundMid);
            } else {
                _attachCount[foundMid] += 1;
                return foundMid;
            }
        }
        ::morph::exec::detail::ModelId const mid{_nextId.fetch_add(1) + 1};
        auto holder = factory();
        if (holder->isBackendChangeAware()) {
            _changeAware.insert(mid);
        }
        _models[mid] = std::move(holder);
        _directory.emplace(dirKey, mid);
        _sharedKeyOf.emplace(mid, std::move(dirKey));
        _attachCount[mid] = 1;
        _hydrationFlags[mid] = std::make_shared<HydrationFlags>();
        return mid;
    }

    /// @brief Enters an already-live, still-anonymous instance into the
    ///        directory under @p primary. Thread-safe.
    /// @param mid     Live instance to promote.
    /// @param typeId  Model type id — the directory's first key component.
    /// @param primary Canonical string encoding of the key to file it under.
    void assignPrimary(::morph::exec::detail::ModelId mid, const std::string& typeId,
                       std::string_view primary) override {
        if (primary.empty()) {
            return;
        }
        std::scoped_lock const lock{_regMtx};
        if (!_models.contains(mid)) {
            return;
        }
        DirectoryKey dirKey{typeId, std::string{primary}};
        if (_directory.contains(dirKey)) {
            return;
        }
        // Only a truly anonymous instance (no existing directory entry) can
        // be promoted. An instance already filed under a *different* real key
        // must not be silently re-filed onto this one -- instances never
        // change key; re-pointing the handler is the supported way to move to
        // a different entity, and it leaves the old key, and every other
        // client still attached under it, untouched.
        if (_sharedKeyOf.contains(mid)) {
            return;
        }
        _directory.emplace(dirKey, mid);
        _sharedKeyOf.emplace(mid, std::move(dirKey));
        _attachCount.try_emplace(mid, 1);
    }

    /// @brief Lists the primary keys of live shared instances of @p typeId. Thread-safe.
    /// @param typeId String type-id to enumerate.
    /// @return Canonical key strings of the live shared instances, in unspecified order.
    std::vector<std::string> listInstances(const std::string& typeId) override {
        std::vector<std::string> keys;
        std::scoped_lock const lock{_regMtx};
        for (const auto& [dirKey, mid] : _directory) {
            if (dirKey.first == typeId) {
                keys.push_back(dirKey.second);
            }
        }
        return keys;
    }

    /// @brief Removes the model with @p mid, or releases one attachment to it. Thread-safe.
    ///
    /// A private instance is erased outright. A shared instance has its attach
    /// count decremented and is erased — and removed from the directory — only
    /// when that count reaches zero, so releasing one handler never destroys an
    /// instance another handler still holds.
    /// @param mid Id returned by a prior `registerModel()`/`registerModelShared()` call.
    void deregisterModel(::morph::exec::detail::ModelId mid) override {
        ::morph::observe::detail::emitMetric(::morph::observe::Metric::deregisterCount, 1.0);
        std::scoped_lock const lock{_regMtx};
        if (auto refIter = _attachCount.find(mid); refIter != _attachCount.end()) {
            refIter->second -= 1;
            if (refIter->second > 0) {
                return;
            }
            _attachCount.erase(refIter);
            if (auto keyIter = _sharedKeyOf.find(mid); keyIter != _sharedKeyOf.end()) {
                _directory.erase(keyIter->second);
                _sharedKeyOf.erase(keyIter);
            }
        }
        _models.erase(mid);
        _changeAware.erase(mid);
        _hydrationFlags.erase(mid);
    }

    /// @brief Schedules `onBackendChanged()` on each change-aware model's strand. Thread-safe.
    ///
    /// Only models recorded in `_changeAware` — maintained by `registerModel`/
    /// `deregisterModel` from `IModelHolder::isBackendChangeAware()`, a
    /// compile-time answer per model type — are visited; there is no
    /// `dynamic_cast` and no scan of models that never opted in. Each such
    /// model's `onBackendChanged()` (the `IModelHolder` base virtual) is
    /// **posted onto that model's own strand** (the same per-`ModelId` serial
    /// queue `execute` uses), rather than invoked inline on the caller's thread.
    /// Two properties follow, and they are the whole point:
    ///
    /// - **Strand-serialised, lock-free model code.** `onBackendChanged()` runs
    ///   on the pool inside the model's strand, so it never overlaps an
    ///   `execute()` on the same model. A model reacting to a backend switch
    ///   (e.g. draining an offline queue and mutating counters) needs no locking
    ///   of its own state — exactly what `offline.md` promises.
    /// - **Not under `Bridge::_mtx`.** `Bridge::switchBackend` calls this while
    ///   holding `_mtx`, but only the cheap `post()` runs there; the model body
    ///   runs later on a pool thread. A model that re-enters the bridge from
    ///   `onBackendChanged()` (`switchBackend`/`registerHandler`/`deregisterHandler`)
    ///   therefore acquires `_mtx` freshly on the strand thread instead of
    ///   deadlocking on a lock the switch caller still holds.
    ///
    /// A `shared_ptr` copy of each holder is captured into the posted task so a
    /// concurrent `deregisterModel` cannot free the model out from under its
    /// pending notification (mirrors `execute`'s holder capture).
    void notifyBackendChanged() override {
        std::vector<std::pair<::morph::exec::detail::ModelId, std::shared_ptr<::morph::model::detail::IModelHolder>>>
            aware;
        {
            std::scoped_lock const lock{_regMtx};
            aware.reserve(_changeAware.size());
            for (auto modelId : _changeAware) {
                auto iter = _models.find(modelId);
                if (iter != _models.end()) {
                    aware.emplace_back(modelId, iter->second);
                }
            }
        }
        for (auto& [modelId, holder] : aware) {
            _strand.post(modelId, [h = std::move(holder)]() mutable { h->onBackendChanged(); });
        }
    }

    /// @brief Schedules `call.localOp` on the model's strand and returns a `Completion`.
    ///
    /// The completion resolves with the opaque result on the strand thread and
    /// the callbacks are delivered via @p cbExec.
    ///
    /// @param mid    Target model id.
    /// @param call   Bundled action; `localOp` is the only field used here.
    /// @param cbExec Executor for delivering callbacks.
    /// @return Completion that will carry the result or an exception.
    ::morph::async::Completion<std::shared_ptr<void>> execute(::morph::exec::detail::ModelId mid,
                                                              detail::ActionCall call,
                                                              ::morph::exec::IExecutor* cbExec) override {
        auto compState = std::make_shared<::morph::async::detail::CompletionState<std::shared_ptr<void>>>();
        ::morph::async::Completion<std::shared_ptr<void>> comp{compState, cbExec};

        std::shared_ptr<::morph::model::detail::IModelHolder> holder;
        std::shared_ptr<HydrationFlags> hydration;
        {
            std::scoped_lock const lock{_regMtx};
            auto iter = _models.find(mid);
            if (iter != _models.end()) {
                holder = iter->second;
            }
            if (auto flagsIter = _hydrationFlags.find(mid); flagsIter != _hydrationFlags.end()) {
                hydration = flagsIter->second;
            }
        }
        if (!holder) {
            compState->setException(
                std::make_exception_ptr(std::runtime_error("model not found: id=" + std::to_string(mid.v))));
            return comp;
        }
        trackPending(compState);
        auto localOp = std::move(call.localOp);
        auto session = std::move(call.session);
        auto modelTypeId = call.modelTypeId;
        auto actionTypeId = call.actionTypeId;
        // Captured by shared_ptr, never by raw `this`: see the Global
        // Constraints note on `~StrandExecutor`'s member-destruction-order
        // subtlety. A shared_ptr copy has its own lifetime, independent of
        // LocalBackend's, so it stays valid even if the backend is torn down
        // while this task is still queued or running. `hydration` follows the
        // same rule and may be null (a private instance has no entry).
        auto inFlightCounter = _inFlight;
        auto const inFlightAfterInc = inFlightCounter->fetch_add(1, std::memory_order_relaxed) + 1;
        ::morph::observe::detail::emitMetric(::morph::observe::Metric::executeInFlight,
                                             static_cast<double>(inFlightAfterInc));
        _strand.post(mid, [localOp = std::move(localOp), holder = std::move(holder), compState,
                           session = std::move(session), modelTypeId = std::move(modelTypeId),
                           actionTypeId = std::move(actionTypeId), inFlightCounter, hydration]() mutable {
            auto const start = std::chrono::steady_clock::now();
            auto const spanId = ::morph::observe::detail::beginSpan(session.requestId, modelTypeId, actionTypeId);
            bool ok = false;
            // Resolve `compState` only after every metric and `endSpan` below are
            // recorded — nothing synchronizes a `.then()`/`.onError()` callback
            // (delivered via `cbExec`, which may run inline/synchronously) with
            // anything after `setValue`/`setException` returns, so resolving first
            // would let the caller observe completion before these metrics are
            // emitted. This is a real race, not just a theoretical one.
            std::shared_ptr<void> value;
            std::exception_ptr error;
            try {
                ::morph::session::detail::ScopedContext const scoped{session};
                value = localOp(*holder);
                ok = true;
            } catch (...) {
                error = std::current_exception();
            }
            ::morph::observe::detail::endSpan(spanId, ok);
            auto const elapsedMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            std::array<std::pair<std::string_view, std::string_view>, 2> const tags{
                {{"modelType", modelTypeId}, {"actionType", actionTypeId}}};
            ::morph::observe::detail::emitMetric(::morph::observe::Metric::executeLatencyMs, elapsedMs, tags);
            if (!ok) {
                ::morph::observe::detail::emitMetric(::morph::observe::Metric::executeErrors, 1.0, tags);
            }
            auto const inFlightAfterDec = inFlightCounter->fetch_sub(1, std::memory_order_relaxed) - 1;
            ::morph::observe::detail::emitMetric(::morph::observe::Metric::executeInFlight,
                                                 static_cast<double>(inFlightAfterDec));
            // Only a freshly created shared instance carries a HydrationFlags
            // token (a private instance, or one that was already live at
            // attach time, has none). Its very first action's outcome decides
            // whether it gets poisoned; a later action's failure is an
            // ordinary error, not a hydration failure.
            if (hydration && hydration->firstActionPending.exchange(false) && !ok) {
                hydration->poisoned.store(true);
            }
            // Resolve last: the Completion is still settled exactly once, only
            // its position relative to the now-recorded instrumentation moved.
            if (ok) {
                compState->setValue(std::move(value));
            } else {
                compState->setException(error);
            }
        });
        return comp;
    }

    /// @brief Resolves every still-pending completion this backend produced with @p exc.
    /// @param exc Exception delivered to every pending completion's error sink.
    void cancelPending(const std::exception_ptr& exc) override {
        std::vector<std::weak_ptr<::morph::async::detail::CompletionState<std::shared_ptr<void>>>> snapshot;
        {
            std::scoped_lock const lock{_pendingMtx};
            snapshot.swap(_pending);
        }
        for (auto& weak : snapshot) {
            if (auto state = weak.lock()) {
                state->setException(exc);
            }
        }
    }

private:
    void trackPending(const std::shared_ptr<::morph::async::detail::CompletionState<std::shared_ptr<void>>>& state) {
        std::scoped_lock const lock{_pendingMtx};
        std::erase_if(_pending, [](const auto& weak) { return weak.expired(); });
        _pending.emplace_back(state);
    }

    ::morph::exec::detail::StrandExecutor _strand;
    std::mutex _regMtx;
    std::unordered_map<::morph::exec::detail::ModelId, std::shared_ptr<::morph::model::detail::IModelHolder>,
                       ::morph::exec::detail::ModelIdHash>
        _models;
    // Ids of models whose holder answered `isBackendChangeAware() == true` at
    // registration time. Maintained in lockstep with `_models` (inserted in
    // `registerModel`, erased in `deregisterModel`, both under `_regMtx`) so
    // `notifyBackendChanged()` never needs to inspect a model it doesn't have
    // to. Always a subset of `_models`' keys.
    std::unordered_set<::morph::exec::detail::ModelId, ::morph::exec::detail::ModelIdHash> _changeAware;
    // Shared-instance directory: (typeId, primary) -> ModelId, plus the reverse
    // lookup and per-instance attach count. All three are maintained under
    // _regMtx alongside _models, so directory membership can never desync from
    // instance existence — the same invariant RemoteServer's connection scopes
    // maintain. Only instances registered with a non-empty primary appear here;
    // a private instance has no entry in any of the three, which is exactly what
    // makes deregisterModel's decrement path a no-op for it.
    // Tracks, per freshly created shared instance, whether its very first
    // action has settled yet and — if it has — whether that first action
    // failed. Consulted lazily by registerModelShared's directory-hit branch,
    // which evicts a "poisoned" instance (its first action failed, so per
    // docs/spec/core/shared_instances.md's Failure modes it must not be left
    // half-hydrated in the directory) and falls through to creating a fresh
    // one, instead of handing the broken instance to a new attacher. Owned
    // via shared_ptr and captured that way — never via raw `this` — into
    // execute()'s strand task, which may still be running after LocalBackend
    // itself is destroyed (see execute()'s existing capture-by-shared_ptr
    // rationale).
    struct HydrationFlags {
        std::atomic<bool> firstActionPending{true};
        std::atomic<bool> poisoned{false};
    };

    using DirectoryKey = std::pair<std::string, std::string>;
    std::unordered_map<DirectoryKey, ::morph::exec::detail::ModelId, ::morph::model::detail::PairKeyHash> _directory;
    std::unordered_map<::morph::exec::detail::ModelId, DirectoryKey, ::morph::exec::detail::ModelIdHash> _sharedKeyOf;
    std::unordered_map<::morph::exec::detail::ModelId, std::size_t, ::morph::exec::detail::ModelIdHash> _attachCount;
    std::unordered_map<::morph::exec::detail::ModelId, std::shared_ptr<HydrationFlags>, ::morph::exec::detail::ModelIdHash>
        _hydrationFlags;
    std::atomic<uint64_t> _nextId{0};
    std::mutex _pendingMtx;
    std::vector<std::weak_ptr<::morph::async::detail::CompletionState<std::shared_ptr<void>>>> _pending;
    // Concurrent in-flight executes, for the executeInFlight metric. A
    // shared_ptr (not a plain atomic member) so strand tasks hold their own
    // reference instead of capturing `this` — see execute()'s comment and the
    // Global Constraints note on ~StrandExecutor's destruction order.
    std::shared_ptr<std::atomic<std::size_t>> _inFlight = std::make_shared<std::atomic<std::size_t>>(0);
};

}  // namespace morph::backend

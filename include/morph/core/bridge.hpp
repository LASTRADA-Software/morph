// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <any>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../forms/forms.hpp"
#include "../session/session.hpp"
#include "backend.hpp"
#include "completion.hpp"
#include "registry.hpp"

namespace morph::bridge {

/// @brief Type-erased, JSON-in/JSON-out execute path for actions whose
///        concrete C++ type is only known by its registered string id at
///        the call site (e.g. a schema-driven GUI that reads action names
///        out of a JSON Schema at runtime).
///
/// Populated automatically by `BRIDGE_REGISTER_ACTION` — no action-specific
/// code is required at any call site. Every entry calls through the real
/// `BridgeHandler<Model>::execute<Action>()` (so sessions, backend
/// switches, and completions all behave exactly as they do for hand-written
/// call sites), unlike `morph::model::detail::ActionDispatcher`, which
/// calls `Model::execute` directly against an already-owned model holder
/// and is only ever used server-side.
///
/// HARD REQUIREMENT (other direction of the same constraint documented on
/// `morph::model::detail::registerActionExecutorOnce` in `registry.hpp`): registration into
/// this registry happens via `registerActionExecutorOnce<Model, Action>`, which
/// `BRIDGE_REGISTER_ACTION` calls unconditionally but which is only defined here, in
/// `bridge.hpp`. Every translation unit that calls `BRIDGE_REGISTER_ACTION` must therefore
/// include this header (directly or transitively), or that translation unit's static
/// initializer will fail to link.
class ActionExecuteRegistry {
public:
    /// @brief Deserialises `bodyJson`, dispatches through the handler's
    ///        `Bridge`, and resolves with the JSON-encoded result.
    using Executor = std::function<::morph::async::Completion<std::string>(void*, std::string_view)>;

    /// @brief Registers the executor for `(Model, Action)` under the given string ids.
    /// Defined out-of-line after BridgeHandler to avoid forward reference issues.
    /// @tparam Model  Model type whose handler will execute the action.
    /// @tparam Action Action type to register.
    /// @param modelId  String id the model is registered under.
    /// @param actionId String id the action is registered under.
    template <typename Model, typename Action>
    void registerAction(std::string_view modelId, std::string_view actionId);

    /// @brief Looks up and invokes the executor for `(modelId, actionId)`.
    /// @param modelId  String id of the target model.
    /// @param actionId String id of the action to execute.
    /// @param handler  Type-erased `BridgeHandler<Model>*` matching `modelId`.
    /// @param bodyJson JSON-encoded action payload.
    /// @return Completion that resolves with the JSON-encoded action result.
    /// @throws std::runtime_error if no executor was registered for that pair.
    [[nodiscard]] ::morph::async::Completion<std::string> execute(std::string_view modelId, std::string_view actionId,
                                                                  void* handler, std::string_view bodyJson) const {
        auto iter = _executors.find(Key{std::string{modelId}, std::string{actionId}});
        if (iter == _executors.end()) {
            throw std::runtime_error("unknown action for executeJson: " + std::string{modelId} + "/" +
                                     std::string{actionId});
        }
        return iter->second(handler, bodyJson);
    }

    /// @brief Returns the process-level singleton registry.
    /// @return Reference to the singleton `ActionExecuteRegistry`.
    static ActionExecuteRegistry& instance();

private:
    using Key = std::pair<std::string, std::string>;
    std::unordered_map<Key, Executor, ::morph::model::detail::PairKeyHash> _executors;
};

inline ActionExecuteRegistry& ActionExecuteRegistry::instance() {
    static ActionExecuteRegistry inst;
    return inst;
}

}  // namespace morph::bridge

namespace morph::model::detail {

template <typename Model, typename Action>
inline bool registerActionExecutorOnce(std::string_view modelId, std::string_view actionId) noexcept {
    ::morph::bridge::ActionExecuteRegistry::instance().registerAction<Model, Action>(modelId, actionId);
    return true;
}

}  // namespace morph::model::detail

namespace morph::bridge {

namespace detail {

/// @brief Compile-time decomposition of a pointer-to-data-member type.
///
/// Used by `BridgeHandler::set<auto FieldPtr>` to recover both the action
/// type and the field type from a single non-type template parameter, so
/// callers write `handler.set<&MyAction::c>(7.0)` with no redundant type
/// arguments.
///
/// @tparam T The pointer-to-member type (e.g. `double MyAction::*`).
template <typename T>
struct MemberPointerTraits;

template <typename V, typename A>
struct MemberPointerTraits<V A::*> {
    /// @brief The class the member belongs to.
    using ClassType = A;
    /// @brief The type of the member itself.
    using ValueType = V;
};

/// @brief Internal linkage record between a model type and its active backend id.
///
/// Shared between `Bridge` and `BridgeHandler`. `Bridge::switchBackend()` updates
/// `currentId` atomically under its mutex so that concurrent `executeVia()` calls
/// always see a consistent value.
struct HandlerBinding {
    /// @brief String type-id of the model.
    std::string typeId;

    /// @brief Factory used to re-register the model on a backend switch.
    std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> modelFactory;

    /// @brief Stable identity of this model instance (e.g. an account id).
    ///
    /// Empty by default — the common local-mode case needs nothing here, since
    /// `modelFactory` can already call `IModelHolder::attachActionLog` directly
    /// with whatever identity it captures. Set this when the active backend may
    /// be remote (`SimulatedRemoteBackend`, `QtWebSocketBackend`): it travels in
    /// the `register` wire envelope so a server-side `RemoteServer::LogProvider`
    /// can attach a log to the instance it creates. See `IBackend::registerModelWithContext`.
    std::string contextKey;

    /// @brief Current `ModelId` value in the active backend (0 = unbound).
    std::atomic<uint64_t> currentId{0};
};

}  // namespace detail

/// @brief Central dispatcher that routes typed actions to an `IBackend`.
///
/// `Bridge` owns exactly one active backend at a time. It tracks all registered
/// `HandlerBinding` instances and re-registers them automatically when
/// `switchBackend()` is called, enabling seamless local ↔ remote transitions.
///
/// @par Thread safety
/// All public methods are thread-safe. `executeVia()` uses a lock-free snapshot
/// of the backend pointer so it does not block `switchBackend()`.
class Bridge {
public:
    /// @brief Constructs a bridge that dispatches through @p backend.
    ///
    /// Installs a reconnect handler so backends with a recoverable transport
    /// (e.g. `QtWebSocketBackend`) can ask the bridge to re-register every live
    /// handler against the freshly reconnected peer.
    ///
    /// @param backend Initial backend. Ownership is transferred.
    explicit Bridge(std::unique_ptr<::morph::backend::detail::IBackend> backend)
        : _backend{std::shared_ptr<::morph::backend::detail::IBackend>(std::move(backend))} {
        installReconnectHandler(_backend);
    }

    /// @brief Cancels every still-pending completion on the active backend.
    ///
    /// Each `.onError(...)` callback receives a `BridgeDestroyedError`. In-flight
    /// server replies that arrive after destruction are no-ops because each
    /// `CompletionState::setValue`/`setException` is idempotent.
    ///
    /// Before cancelling, the active backend's reconnect handler is cleared: the
    /// installed handler captures `this`, and a co-owned backend that outlives the
    /// `Bridge` could otherwise fire it after destruction and dereference freed
    /// memory. Clearing it here (outside `_mtx` — `setReconnectHandler` only stores
    /// a callback and never re-enters the bridge) closes the common case; the
    /// handler additionally guards on `_liveness` so a reconnect already in flight
    /// on the transport thread becomes a no-op too. See docs/spec/concurrency_and_lifetimes.md.
    ~Bridge() {
        if (auto active = loadBackend()) {
            active->setReconnectHandler(nullptr);
            active->cancelPending(std::make_exception_ptr(::morph::backend::BridgeDestroyedError{}));
        }
    }

    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;
    Bridge(Bridge&&) = delete;
    Bridge& operator=(Bridge&&) = delete;

    /// @brief Creates and registers a new `HandlerBinding` for `Model`.
    ///
    /// Uses the default `ModelFactory::create<Model>()` factory.
    /// @tparam Model Concrete model type. Must have a registered `ModelTraits` specialisation.
    /// @return Shared pointer to the new binding.
    template <typename Model>
    std::shared_ptr<detail::HandlerBinding> registerHandler() {
        auto binding = std::make_shared<detail::HandlerBinding>();
        binding->typeId = std::string{::morph::model::ModelTraits<Model>::typeId()};
        binding->modelFactory = [] { return ::morph::model::detail::ModelFactory::create<Model>(); };
        std::scoped_lock const lock{_mtx};
        binding->currentId.store(
            loadBackend()->registerModelWithContext(binding->typeId, binding->modelFactory, binding->contextKey).v);
        _handlers.push_back(binding);
        return binding;
    }

    /// @brief Registers a pre-built binding with a custom factory.
    ///
    /// Use this overload when the model factory needs to capture external
    /// dependencies that the type-erasing default factory cannot carry, or when
    /// `binding->contextKey` needs to be set before registration (see `HandlerBinding::contextKey`).
    /// @param binding Pre-constructed binding. Its `typeId` and `modelFactory` must be set.
    void registerHandler(const std::shared_ptr<detail::HandlerBinding>& binding) {
        std::scoped_lock const lock{_mtx};
        binding->currentId.store(
            loadBackend()->registerModelWithContext(binding->typeId, binding->modelFactory, binding->contextKey).v);
        _handlers.push_back(binding);
    }

    /// @brief Installs a default session context that `executeVia` stamps onto the
    ///        `ActionCall` of every subsequent call.
    ///
    /// There is no per-call session override — this default is applied to all calls
    /// until replaced or cleared.
    ///
    /// Typical pattern: call this once after login to bind the user's principal
    /// and locale, then every subsequent `handler.execute(action)` carries the
    /// session automatically. Thread-safe.
    ///
    /// @param session The new default. Pass `{}` to clear.
    void setDefaultSession(::morph::session::Context session) {
        std::scoped_lock const lock{_sessionMtx};
        _defaultSession = std::move(session);
    }

    /// @brief Returns a copy of the currently installed default session. Thread-safe.
    /// @return Snapshot of the default `Context`.
    [[nodiscard]] ::morph::session::Context defaultSession() const {
        std::scoped_lock const lock{_sessionMtx};
        return _defaultSession;
    }

    /// @brief Atomically replaces the active backend with @p newBackend.
    ///
    /// All live bindings are re-registered on the new backend and their
    /// `currentId` values are updated. The old backend is released after the
    /// swap. Any in-flight `Completion` objects targeting the old backend will
    /// fail naturally.
    ///
    /// @note Lock ordering: `Bridge::_mtx` is acquired before
    ///       `LocalBackend::_regMtx`. `notifyBackendChanged()` runs under `_mtx`
    ///       but only **posts** each model's `onBackendChanged()` onto that
    ///       model's strand (see `LocalBackend::notifyBackendChanged`); the model
    ///       body runs later on a pool thread, off `_mtx`. So an
    ///       `onBackendChanged()` implementation **may** re-enter the bridge
    ///       (`switchBackend`/`registerHandler`/`deregisterHandler`) — it acquires
    ///       `_mtx` freshly on the strand thread rather than deadlocking on the
    ///       switch caller's still-held lock. It is also strand-serialised against
    ///       `execute` on the same model, so it needs no locking of model state.
    ///
    /// @param newBackend Replacement backend. Ownership is transferred.
    void switchBackend(std::unique_ptr<::morph::backend::detail::IBackend> newBackend) {
        auto newShared = std::shared_ptr<::morph::backend::detail::IBackend>(std::move(newBackend));
        std::shared_ptr<::morph::backend::detail::IBackend> previous;
        {
            std::scoped_lock const lock{_mtx};

            // Phase 1 — register every live binding on the new backend WITHOUT
            // mutating any `currentId` yet, staging (binding, newId) pairs. If a
            // registration throws partway (a plausible remote/transport failure),
            // roll back the ones already registered and rethrow, leaving the old
            // backend and every `currentId` untouched — so the switch is atomic:
            // it either fully succeeds or is a no-op.
            std::vector<std::weak_ptr<detail::HandlerBinding>> live;
            std::vector<std::pair<std::shared_ptr<detail::HandlerBinding>, uint64_t>> staged;
            try {
                for (auto& weak : _handlers) {
                    auto binding = weak.lock();
                    if (!binding) {
                        continue;
                    }
                    auto newId = newShared->registerModelWithContext(binding->typeId, binding->modelFactory,
                                                                     binding->contextKey);
                    staged.emplace_back(binding, newId.v);
                    live.push_back(weak);
                }
            } catch (...) {
                for (const auto& [binding, newId] : staged) {
                    try {
                        newShared->deregisterModel(::morph::exec::detail::ModelId{newId});
                    } catch (const std::exception& exc) {
                        ::morph::log::logError(std::string{"[switchBackend] rollback deregister failed: "} +
                                               exc.what());
                    }
                }
                throw;
            }

            // Phase 2 — commit. Every registration succeeded, so it is now safe to
            // publish the new ids and swap the backend in.
            for (const auto& [binding, newId] : staged) {
                binding->currentId.store(newId);
            }
            _handlers = std::move(live);

            previous = exchangeBackend(newShared);
            newShared->notifyBackendChanged();
        }
        installReconnectHandler(newShared);
        if (previous && previous != newShared) {
            previous->setReconnectHandler(nullptr);
        }
        // Drain the outgoing backend outside the bridge mutex: cancelPending
        // delivers callbacks through the caller's gui executor, and we never want
        // to hold _mtx while user code runs.
        if (previous && previous != newShared) {
            previous->cancelPending(std::make_exception_ptr(::morph::backend::BackendChangedError{}));
        }
    }

    /// @brief Deregisters @p binding from the active backend and removes it from tracking.
    ///
    /// After this call, any future `executeVia()` using this binding will
    /// complete with an error.
    /// @param binding Binding to remove. Must have been returned by `registerHandler()`.
    void deregisterHandler(const std::shared_ptr<detail::HandlerBinding>& binding) {
        std::scoped_lock const lock{_mtx};
        uint64_t const raw = binding->currentId.load();
        if (raw != 0U) {
            loadBackend()->deregisterModel(::morph::exec::detail::ModelId{raw});
        }
        // Reset to the "0 = unbound" sentinel so a late/concurrent executeVia on
        // this binding fails fast on the documented guard rather than sending a
        // now-destroyed ModelId to the backend.
        binding->currentId.store(0);
        auto iter = std::ranges::find_if(_handlers, [&](auto& weak) {
            auto sptr = weak.lock();
            return sptr && sptr.get() == binding.get();
        });
        if (iter != _handlers.end()) {
            _handlers.erase(iter);
        }
    }

    /// @brief Dispatches @p action against the model identified by @p binding.
    ///
    /// Uses a lock-free snapshot of the backend and model id so the call does
    /// not block `switchBackend()`. If a backend switch happens concurrently,
    /// the old backend still exists (its `shared_ptr` refcount is > 0) and the
    /// call either succeeds or fails with "model not found" — both are safe.
    ///
    /// On `LocalBackend`, the `localOp` this method builds enforces
    /// `morph::model::ActionValidator<Action>::ready(action)` before calling
    /// `Model::execute`, mirroring `ActionDispatcher::registerAction`'s runner
    /// (`registry.hpp`) for the in-process path. A `false` result resolves the
    /// returned `Completion` through `onError` with a `morph::model::ValidationError`
    /// instead of executing the action — see docs/spec/core/registry.md.
    ///
    /// @tparam Model  Model type that owns the handler.
    /// @tparam Action Action type to dispatch.
    /// @param binding Binding returned by `registerHandler<Model>()`.
    /// @param action  Action to execute (moved in).
    /// @param cbExec  Executor on which the `Completion` callbacks are posted.
    /// @return Completion that resolves with the typed result or an exception
    ///         (including `ValidationError` on `LocalBackend` when the action
    ///         fails its validator).
    template <typename Model, typename Action>
    ::morph::async::Completion<typename ::morph::model::ActionTraits<Action>::Result> executeVia(
        const std::shared_ptr<detail::HandlerBinding>& binding, Action action, ::morph::exec::IExecutor* cbExec) {
        using R = ::morph::model::ActionTraits<Action>::Result;

        auto backend = loadBackend();
        uint64_t const raw = binding->currentId.load();

        auto typedState = std::make_shared<::morph::async::detail::CompletionState<R>>();
        ::morph::async::Completion<R> typed{typedState, cbExec};
        if (raw == 0U) {
            typedState->setException(std::make_exception_ptr(std::runtime_error("handler not bound")));
            return typed;
        }
        ::morph::backend::detail::ActionCall call;
        call.modelTypeId = std::string{::morph::model::ModelTraits<Model>::typeId()};
        call.actionTypeId = std::string{::morph::model::ActionTraits<Action>::typeId()};
        auto sharedAction = std::make_shared<Action>(std::move(action));
        call.serializeAction = [sharedAction] { return ::morph::model::ActionTraits<Action>::toJson(*sharedAction); };
        call.deserializeResult = [](std::string_view jsonStr) -> std::shared_ptr<void> {
            return std::make_shared<R>(::morph::model::ActionTraits<Action>::resultFromJson(jsonStr));
        };
        call.localOp = [sharedAction](::morph::model::detail::IModelHolder& holder) -> std::shared_ptr<void> {
            // Enforce the action's validator on the local execution path too, so
            // a caller that constructs an Action by hand and calls
            // BridgeHandler<Model>::execute<Action>() directly — bypassing the
            // reactive set<>/tryFireImpl gate that already checks ready() — is
            // rejected the same way a hand-built wire envelope is rejected by
            // ActionDispatcher::registerAction's runner (registry.hpp). No JSON is
            // involved on this path, so there is no declared-precision
            // reconciliation step here (that only applies to decoded wire
            // payloads); the Quantity fields carry whatever precision the caller
            // constructed them with. ActionValidator<Action>::ready defaults to
            // `true` for actions with no validator, so this is a no-op for
            // unvalidated actions (zero behavior change). The thrown exception is
            // caught by LocalBackend::execute's strand task (backend.hpp) and
            // resolves this Completion through onError.
            if (!::morph::model::ActionValidator<Action>::ready(*sharedAction)) {
                throw ::morph::model::ValidationError{::morph::model::ModelTraits<Model>::typeId(),
                                                      ::morph::model::ActionTraits<Action>::typeId()};
            }
            auto& model = holder.template into<Model>();
            auto result = std::make_shared<R>(model.execute(*sharedAction));
            // Local mode has no client/server split, so this is the same execution
            // site `ActionDispatcher::registerAction`'s runner is for remote modes
            // (registry.hpp) — see that overload's doc comment for the full story.
            if constexpr (::morph::model::detail::actionLoggable<Action>() == ::morph::model::Loggable::Yes) {
                if (holder.hasActionLog()) {
                    // entityKey/principal/timestampMs are filled in by recordIfAttached.
                    holder.recordIfAttached(::morph::journal::LogEntry{
                        .seq = 0,
                        .modelType = std::string{::morph::model::ModelTraits<Model>::typeId()},
                        .entityKey = {},
                        .actionType = std::string{::morph::model::ActionTraits<Action>::typeId()},
                        .payload = ::morph::model::ActionTraits<Action>::toJson(*sharedAction),
                        .result = ::morph::model::ActionTraits<Action>::resultToJson(*result),
                        .principal = {},
                        .timestampMs = 0,
                    });
                }
            }
            return result;
        };
        {
            std::scoped_lock const lock{_sessionMtx};
            call.session = _defaultSession;
        }
        auto anyCompletion = backend->execute(::morph::exec::detail::ModelId{raw}, std::move(call), cbExec);
        anyCompletion
            .then([typedState](const std::shared_ptr<void>& vAny) {
                // Guard the value-forwarding: if R's move/copy throws (or the cast
                // is somehow wrong), route the exception to the typed completion's
                // error sink instead of letting it escape the callback executor —
                // where ThreadPoolExecutor swallows it (the outer completion would
                // then hang forever) and QtExecutor lets it reach the event loop
                // and std::terminate. Mirrors the forwarding guard in remote.hpp's
                // SimulatedRemoteBackend::execute. See docs/spec/bridge.md.
                try {
                    typedState->setValue(std::move(*static_cast<R*>(vAny.get())));
                } catch (...) {
                    typedState->setException(std::current_exception());
                }
            })
            .onError([typedState](const std::exception_ptr& err) { typedState->setException(err); });
        return typed;
    }

private:
    template <typename>
    friend class BridgeHandler;

    /// @brief Weak observer of this bridge's lifetime, handed to each handler.
    ///
    /// A `BridgeHandler` checks this in its destructor: if the token has expired
    /// the `Bridge` is already gone, so it skips deregistration instead of
    /// dereferencing a dangling `Bridge&`. The bridge must still outlive its
    /// handlers for normal `execute`/`set` calls; this only makes the *teardown*
    /// order-independent so a mis-ordered destruction is defined behaviour.
    [[nodiscard]] std::weak_ptr<const void> liveness() const { return _liveness; }

    std::shared_ptr<::morph::backend::detail::IBackend> loadBackend() const {
        std::scoped_lock const lock{_backendMtx};
        return _backend;
    }

    std::shared_ptr<::morph::backend::detail::IBackend> exchangeBackend(
        std::shared_ptr<::morph::backend::detail::IBackend> next) {
        std::scoped_lock const lock{_backendMtx};
        auto previous = std::move(_backend);
        _backend = std::move(next);
        return previous;
    }

    void installReconnectHandler(const std::shared_ptr<::morph::backend::detail::IBackend>& backend) {
        if (!backend) {
            return;
        }
        // The handler is invoked on the backend's transport thread. We keep a
        // weak_ptr to the same shared backend so a stale callback fired after a
        // switchBackend is a no-op, and a weak_ptr to the bridge's liveness token
        // so a callback fired after the Bridge is destroyed (a co-owned backend
        // outliving its bridge) is also a no-op instead of a use-after-free on
        // `this`. `~Bridge` additionally clears the handler; this guard covers a
        // reconnect that is already in flight when the Bridge is torn down.
        std::weak_ptr<::morph::backend::detail::IBackend> const weakBackend{backend};
        std::weak_ptr<const void> const weakLiveness{_liveness};
        backend->setReconnectHandler([this, weakBackend, weakLiveness] {
            auto aliveToken = weakLiveness.lock();
            if (!aliveToken) {
                return;  // The Bridge is gone; do not touch `this`.
            }
            auto pinned = weakBackend.lock();
            std::scoped_lock const lock{_mtx};
            if (!pinned || pinned != loadBackend()) {
                return;  // We've moved on to a different backend; ignore.
            }
            for (auto& weak : _handlers) {
                auto binding = weak.lock();
                if (!binding) {
                    continue;
                }
                auto newId =
                    pinned->registerModelWithContext(binding->typeId, binding->modelFactory, binding->contextKey);
                binding->currentId.store(newId.v);
            }
        });
    }

    mutable std::mutex _backendMtx;
    std::shared_ptr<::morph::backend::detail::IBackend> _backend;
    std::mutex _mtx;
    std::vector<std::weak_ptr<detail::HandlerBinding>> _handlers;
    mutable std::mutex _sessionMtx;
    ::morph::session::Context _defaultSession;
    // Destroyed with the Bridge; handlers hold weak_ptrs to it (see liveness()).
    std::shared_ptr<const void> _liveness{std::make_shared<char>()};
};

/// @brief RAII wrapper that binds a single model type to a `Bridge`.
///
/// On construction, registers a `HandlerBinding` on the bridge. On destruction,
/// deregisters it automatically. The handler is non-copyable.
///
/// @par Fielded actions and subscriptions
/// Beyond the one-shot `execute(action) -> Completion<R>` API, the handler
/// offers a streaming surface for actions whose values arrive field-by-field
/// from a GUI (typically one widget per field):
///
/// - `subscribe<A>(cb)` stashes a result callback for action type `A`.
/// - `set<&A::field>(value)` updates one field of the in-progress draft of `A`.
/// - `unsubscribe<A>()` drops the callback.
/// - `reset<A>()` discards the in-progress draft of `A`.
///
/// @tparam Model Concrete model type.
template <typename Model>
// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions)
class BridgeHandler {
public:
    /// @brief Constructs and registers the handler using the default model factory.
    ///
    /// @param bridge   The bridge to register on.
    /// @param guiExec  Executor used to deliver `Completion` callbacks (e.g. the GUI thread).
    BridgeHandler(Bridge& bridge, ::morph::exec::IExecutor* guiExec)
        : _bridge{bridge},
          _bridgeAlive{bridge.liveness()},
          _guiExec{guiExec},
          _binding{bridge.template registerHandler<Model>()},
          _subs{std::make_shared<SubscriberState>()} {
        _subs->bridge = &_bridge;
        _subs->binding = _binding;
        _subs->guiExec = _guiExec;
    }

    /// @brief Constructs the handler with a pre-built binding (for dependency injection).
    ///
    /// @param bridge   The bridge to register on.
    /// @param guiExec  Executor for callback delivery.
    /// @param binding  Pre-built binding whose factory captures injected dependencies.
    BridgeHandler(Bridge& bridge, ::morph::exec::IExecutor* guiExec, std::shared_ptr<detail::HandlerBinding> binding)
        : _bridge{bridge},
          _bridgeAlive{bridge.liveness()},
          _guiExec{guiExec},
          _binding{std::move(binding)},
          _subs{std::make_shared<SubscriberState>()} {
        _bridge.registerHandler(_binding);
        _subs->bridge = &_bridge;
        _subs->binding = _binding;
        _subs->guiExec = _guiExec;
    }

    /// @brief Deregisters the binding from the bridge.
    ///
    /// If the `Bridge` has already been destroyed (liveness token expired), this
    /// is a no-op: there is nothing to deregister from and dereferencing the
    /// dangling `Bridge&` would be undefined behaviour. Destroying the bridge
    /// before its handlers is still discouraged, but is now safe rather than a
    /// use-after-free.
    ~BridgeHandler() {
        if (auto alive = _bridgeAlive.lock()) {
            _bridge.deregisterHandler(_binding);
        }
    }

    BridgeHandler(const BridgeHandler&) = delete;
    BridgeHandler& operator=(const BridgeHandler&) = delete;

    /// @brief Dispatches @p action via the underlying `Bridge` and returns a `Completion`.
    ///
    /// The bridge's currently-installed default session (set once at startup or
    /// after login via `Bridge::setDefaultSession`) is attached automatically —
    /// callers never thread the session through individual call sites.
    ///
    /// @tparam Action Concrete action type registered with `BRIDGE_REGISTER_ACTION`.
    /// @param action Action to execute (moved into the dispatch).
    /// @return Completion that resolves on the GUI executor.
    template <typename Action>
    ::morph::async::Completion<typename ::morph::model::ActionTraits<Action>::Result> execute(Action action) {
        return _bridge.template executeVia<Model, Action>(_binding, std::move(action), _guiExec);
    }

    /// @brief Type-erased execute: looks up the action by its registered
    ///        string id and dispatches it through `ActionExecuteRegistry`.
    ///
    /// Use this only when the concrete `Action` type is not known at the
    /// call site (e.g. a schema-driven UI reading action names out of a
    /// JSON Schema at runtime). Prefer the templated `execute<Action>()`
    /// whenever the type is known at compile time.
    ///
    /// @param actionType Registered action type-id (the `NAME` passed to `BRIDGE_REGISTER_ACTION`).
    /// @param bodyJson   JSON-encoded action body.
    /// @return Completion resolving with the JSON-encoded result.
    /// @throws std::runtime_error if `actionType` was never registered for `Model`.
    [[nodiscard]] ::morph::async::Completion<std::string> executeJson(std::string_view actionType,
                                                                      std::string_view bodyJson) {
        return ActionExecuteRegistry::instance().execute(std::string{::morph::model::ModelTraits<Model>::typeId()},
                                                         actionType, this, bodyJson);
    }

    /// @brief The executor used to deliver this handler's `Completion` callbacks.
    /// @return The GUI/callback executor passed at construction.
    [[nodiscard]] ::morph::exec::IExecutor* guiExecutor() const noexcept { return _guiExec; }

    /// @brief Subscribes to results of action type @p Action.
    ///
    /// @tparam Action Concrete action type registered with `BRIDGE_REGISTER_ACTION`.
    /// @param cb Callable receiving the action's `Result` by value on the GUI executor.
    template <typename Action>
    void subscribe(std::function<void(typename ::morph::model::ActionTraits<Action>::Result)> cb) {
        using R = ::morph::model::ActionTraits<Action>::Result;
        auto wrapper = [cb = std::move(cb)](const std::any& boxed) { cb(std::any_cast<const R&>(boxed)); };
        std::scoped_lock lock{_subs->mtx};
        _subs->entries[::morph::model::ActionTraits<Action>::typeId()].sink = std::move(wrapper);
    }

    /// @brief Subscribes to both results and errors of action type @p Action.
    ///
    /// @tparam Action Concrete action type registered with `BRIDGE_REGISTER_ACTION`.
    /// @param cb     Result callback invoked on success.
    /// @param errCb  Error callback invoked on failure (replaces orphan logging).
    template <typename Action>
    void subscribe(std::function<void(typename ::morph::model::ActionTraits<Action>::Result)> cb,
                   std::function<void(std::exception_ptr)> errCb) {
        subscribe<Action>(std::move(cb));
        std::scoped_lock lock{_subs->mtx};
        _subs->entries[::morph::model::ActionTraits<Action>::typeId()].errSink = std::move(errCb);
    }

    /// @brief Removes the subscriber for action type @p Action.
    template <typename Action>
    void unsubscribe() {
        std::scoped_lock lock{_subs->mtx};
        auto iter = _subs->entries.find(::morph::model::ActionTraits<Action>::typeId());
        if (iter != _subs->entries.end()) {
            iter->second.sink = nullptr;
            iter->second.errSink = nullptr;
        }
    }

    /// @brief Sets one field of the in-progress draft and fires the action if ready.
    ///
    /// @tparam FieldPtr Pointer-to-data-member of the action struct (encodes both action and field type).
    /// @param value New value for the field.
    template <auto FieldPtr>
    void set(detail::MemberPointerTraits<decltype(FieldPtr)>::ValueType value) {
        using A = detail::MemberPointerTraits<decltype(FieldPtr)>::ClassType;
        {
            std::scoped_lock lock{_subs->mtx};
            auto& entry = _subs->entries[::morph::model::ActionTraits<A>::typeId()];
            if (!entry.draft.has_value()) {
                entry.draft = A{};
            }
            std::any_cast<A&>(entry.draft).*FieldPtr = std::move(value);
        }
        tryFireImpl<A>(_subs, ::morph::model::ActionTraits<A>::typeId());
    }

    /// @brief Discards the in-progress draft for action @p Action.
    template <typename Action>
    void reset() {
        std::scoped_lock lock{_subs->mtx};
        auto iter = _subs->entries.find(::morph::model::ActionTraits<Action>::typeId());
        if (iter != _subs->entries.end()) {
            iter->second.draft.reset();
        }
    }

    /// @brief Returns the underlying `HandlerBinding`.
    ///
    /// @return Shared pointer to the binding owned by this handler.
    [[nodiscard]] const std::shared_ptr<detail::HandlerBinding>& binding() const { return _binding; }

private:
    struct SubscriberEntry {
        std::any draft;
        std::function<void(const std::any&)> sink;
        std::function<void(std::exception_ptr)> errSink;
        bool running{false};
        bool pending{false};
    };
    struct SubscriberState {
        std::mutex mtx;
        Bridge* bridge{nullptr};
        std::shared_ptr<detail::HandlerBinding> binding;
        ::morph::exec::IExecutor* guiExec{nullptr};
        /// @note Keys are `std::string_view` pointing to string literals from
        /// `ActionTraits<A>::typeId()` — all call sites pass compile-time strings
        /// with static storage duration, so the map's keys never dangle.
        std::unordered_map<std::string_view, SubscriberEntry> entries;
    };

    struct PostExec {
        std::function<void(const std::any&)> sink;
        std::function<void(std::exception_ptr)> errSink;
        bool refire{false};
    };

    static PostExec consumeFlight(SubscriberState& state, std::string_view typeId) {
        PostExec out;
        std::scoped_lock lock{state.mtx};
        auto& entry = state.entries.find(typeId)->second;
        out.sink = entry.sink;
        out.errSink = entry.errSink;
        entry.running = false;
        if (entry.pending) {
            entry.pending = false;
            out.refire = true;
        }
        return out;
    }

    static void logUnhandledError(std::string_view typeId, const std::exception_ptr& err) {
        try {
            std::rethrow_exception(err);
        } catch (const std::exception& exc) {
            ::morph::log::logError(std::string{"[subscription:"} + std::string{typeId} +
                                   "] unhandled exception: " + exc.what());
        } catch (...) {
            ::morph::log::logError(std::string{"[subscription:"} + std::string{typeId} +
                                   "] unhandled unknown exception");
        }
    }

    template <typename Action>
    static void tryFireImpl(const std::shared_ptr<SubscriberState>& state, std::string_view typeId) {
        // Every caller holds the `SubscriberState` alive for the duration of this
        // call, so no liveness check is needed on entry. The async continuations
        // below may outlive the subscription, so they each re-check through this
        // weak_ptr instead.
        const std::weak_ptr<SubscriberState> weak{state};
        using R = ::morph::model::ActionTraits<Action>::Result;

        Action snapshot;
        {
            std::scoped_lock lock{state->mtx};
            auto iter = state->entries.find(typeId);
            if (iter == state->entries.end() || !iter->second.draft.has_value()) {
                return;
            }
            if (iter->second.running) {
                iter->second.pending = true;
                return;
            }
            snapshot = std::any_cast<const Action&>(iter->second.draft);
        }
        if (!::morph::model::ActionValidator<Action>::ready(snapshot)) {
            return;
        }
        {
            std::scoped_lock lock{state->mtx};
            state->entries[typeId].running = true;
        }

        state->bridge->template executeVia<Model, Action>(state->binding, std::move(snapshot), state->guiExec)
            .then([weak, typeId](R result) {
                auto inner = weak.lock();
                if (!inner) {
                    return;
                }
                auto outcome = consumeFlight(*inner, typeId);
                if (outcome.sink) {
                    std::any boxed{std::move(result)};
                    outcome.sink(boxed);
                }
                if (outcome.refire) {
                    tryFireImpl<Action>(inner, typeId);
                }
            })
            .onError([weak, typeId](const std::exception_ptr& err) {
                auto inner = weak.lock();
                if (!inner) {
                    return;
                }
                auto outcome = consumeFlight(*inner, typeId);
                if (outcome.errSink) {
                    outcome.errSink(err);
                } else {
                    logUnhandledError(typeId, err);
                }
                if (outcome.refire) {
                    tryFireImpl<Action>(inner, typeId);
                }
            });
    }

    Bridge& _bridge;
    std::weak_ptr<const void> _bridgeAlive;  // expires when _bridge is destroyed
    ::morph::exec::IExecutor* _guiExec;
    std::shared_ptr<detail::HandlerBinding> _binding;
    std::shared_ptr<SubscriberState> _subs;
};

/// Out-of-line definition of ActionExecuteRegistry::registerAction.
/// Placed here after BridgeHandler is fully defined so we can safely cast and call its methods.
template <typename Model, typename Action>
inline void ActionExecuteRegistry::registerAction(std::string_view modelId, std::string_view actionId) {
    Key const key{std::string{modelId}, std::string{actionId}};
    _executors[key] = [](void* handlerVoid, std::string_view bodyJson) -> ::morph::async::Completion<std::string> {
        auto* handler = static_cast<BridgeHandler<Model>*>(handlerVoid);
        auto resultState = std::make_shared<::morph::async::detail::CompletionState<std::string>>();
        try {
            Action action = ::morph::model::ActionTraits<Action>::fromJson(bodyJson);
            // Retag any Quantity fields to their declared precision so the stored
            // value matches the schema's advertised `x-decimalPlaces`, rather than
            // silently keeping whatever runtime `dp` the client sent. No-op for
            // actions with no Quantity members. See docs/spec/forms.md.
            ::morph::forms::reconcileDeclaredPrecision(action);
            // Enforce the action's validator on the request/reply dispatch path,
            // just as the reactive `set<>` path does via `tryFireImpl`. Without
            // this, a submitted action that fails its readiness/validity check
            // (empty required Quantity, out-of-range field, …) would reach the
            // handler and either produce a silently wrong result or force every
            // handler to re-check by hand. `ActionValidator<Action>::ready`
            // auto-detects a `bool validate() const` member and defaults to
            // `true` for actions with no validator, so this is a no-op for
            // unvalidated actions and a hard gate for validated ones. An invalid
            // action resolves the completion through `setException` (a proper
            // error reply upstream), never a bad execution.
            if (!::morph::model::ActionValidator<Action>::ready(action)) {
                throw std::invalid_argument{"action failed validation: " +
                                            std::string{::morph::model::ActionTraits<Action>::typeId()}};
            }
            handler
                ->template execute<Action>(std::move(action))
                // NOLINTNEXTLINE(performance-unnecessary-value-param) — lambda captures the result by value
                .then([resultState](auto result) {
                    // Guard the JSON forwarding for the same reason as executeVia:
                    // a throwing resultToJson (or the move it does) must land on
                    // the error sink, not escape the callback executor and either
                    // hang the completion or terminate the Qt loop.
                    try {
                        resultState->setValue(::morph::model::ActionTraits<Action>::resultToJson(result));
                    } catch (...) {
                        resultState->setException(std::current_exception());
                    }
                })
                .onError([resultState](const std::exception_ptr& err) { resultState->setException(err); });
        } catch (...) {
            resultState->setException(std::current_exception());
        }
        return {resultState, handler->guiExecutor()};
    };
}

}  // namespace morph::bridge

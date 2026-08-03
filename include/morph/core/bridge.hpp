// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <any>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../forms/forms.hpp"
#include "../session/session.hpp"
#include "backend.hpp"
#include "completion.hpp"
#include "model_key.hpp"
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
/// Recovers both the class and the member type from a single non-type template
/// parameter, so callers name a field as `&MyAction::c` with no redundant type
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

    /// @brief Canonical string encoding of this instance's primary key.
    ///
    /// Empty until a keyed action or an explicit `attach` supplies one. Only
    /// meaningful when `shared` is set; a private binding never consults it.
    /// Mutated only under `Bridge::_attachMtx` (or, during `switchBackend()`
    /// and the reconnect handler, both `_attachMtx` and `_mtx` together).
    std::string primary;

    /// @brief Whether this binding participates in the shared instance directory.
    ///
    /// Set once at construction from `BridgeHandler`'s `Sharing` template
    /// argument and never changed. A shared binding defers its backend
    /// registration until it has a primary, so `currentId` stays `0` — and
    /// `executeVia` fails fast on "handler not bound" — until then.
    bool shared = false;

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

    /// @brief Creates a shared, initially **unattached** binding for `Model`.
    ///
    /// Unlike `registerHandler<Model>()`, this registers nothing on the backend:
    /// a shared handler has no instance until a keyed action or an explicit
    /// `attach` tells it which one it wants. The binding is tracked from the
    /// start so `switchBackend()` knows about it, but it stays unbound
    /// (`currentId == 0`) until then.
    ///
    /// @tparam Model Concrete model type. Must have a registered `ModelTraits`.
    /// @return Shared pointer to the new, unattached binding.
    template <typename Model>
    std::shared_ptr<detail::HandlerBinding> registerSharedHandler() {
        auto binding = std::make_shared<detail::HandlerBinding>();
        binding->typeId = std::string{::morph::model::ModelTraits<Model>::typeId()};
        binding->modelFactory = [] { return ::morph::model::detail::ModelFactory::create<Model>(); };
        binding->shared = true;
        std::scoped_lock const lock{_mtx};
        _handlers.push_back(binding);
        return binding;
    }

    /// @brief Attaches (or re-points) @p binding to the shared instance for @p primary.
    ///
    /// Idempotent: attaching to the primary a binding already holds is a no-op,
    /// so a keyed action repeated against the same instance costs nothing. A
    /// different primary re-points the binding — the previous instance is
    /// released and survives only if another handler still holds it.
    ///
    /// The binding's `contextKey` is set to @p primary as well, so a keyed
    /// model's journal entries carry the entity key without the caller
    /// arranging it separately.
    ///
    /// @tparam Model Concrete model type.
    /// @param binding Shared binding, as returned by `registerSharedHandler<Model>()`.
    /// @param primary Canonical string encoding of the primary key to attach to.
    template <typename Model>
    void attachHandler(const std::shared_ptr<detail::HandlerBinding>& binding, std::string primary) {
        std::scoped_lock const lock{_attachMtx};
        if (binding->primary == primary && binding->currentId.load() != 0U) {
            return;
        }
        auto const previous = ::morph::exec::detail::ModelId{binding->currentId.load()};
        // IBackend::attachModel's default (local) implementation and
        // RemoteServer's wire-level attach handling both acquire the
        // replacement before releasing `previous`, so a throwing acquire
        // leaves `previous` untouched except in one narrow, harmless case: a
        // remote re-point whose connection scope closes concurrently with
        // the attach, where `previous` may already be released by the time
        // the reply reports failure -- but no further request on this
        // binding will ever run at that point either, so nothing is actually
        // lost from the caller's perspective. Unbind first is therefore
        // unnecessary: publish only on success.
        auto newId = loadBackend()->attachModel(binding->typeId, binding->modelFactory,
                                                {.contextKey = primary, .primary = primary}, previous);
        binding->contextKey = primary;
        binding->primary = std::move(primary);
        binding->currentId.store(newId.v);
    }

    /// @brief Gives @p binding an anonymous instance if it does not have one yet.
    ///
    /// Used before a result-keyed action: such an action generates the key it
    /// will be filed under, so it has to run *somewhere* first. The instance it
    /// gets is private (empty primary, invisible to the directory) until
    /// `assignHandlerPrimary` promotes it in place once the key is known.
    /// @param binding Shared binding to bind.
    void ensureBound(const std::shared_ptr<detail::HandlerBinding>& binding) {
        std::scoped_lock const lock{_attachMtx};
        if (binding->currentId.load() != 0U) {
            return;
        }
        auto newId = loadBackend()->registerModelShared(binding->typeId, binding->modelFactory,
                                                        {.contextKey = binding->contextKey, .primary = {}});
        binding->currentId.store(newId.v);
    }

    /// @brief Files @p binding's current instance under @p primary, in place.
    ///
    /// The instance keeps everything the creating action just did — nothing is
    /// re-created and nothing is stranded. A no-op if @p binding already holds
    /// a different real primary (the locally cached primary must not race
    /// ahead of the backend's own refusal to re-key an already-keyed
    /// instance — see `IBackend::assignPrimary`).
    ///
    /// Known gap: if @p binding was still anonymous but the *target* key is
    /// already held by a different instance, the backend's `assignPrimary`
    /// silently declines to promote (the existing holder always wins), but
    /// this method has no way to learn that and still caches @p primary as
    /// though the promotion succeeded — `binding->primary()` can then report
    /// a key the backend never actually filed this instance under, and a
    /// same-key `attach()` after that becomes a silent no-op (the "already
    /// primary == primary" guard in `attachHandler`), so the binding can
    /// never reach the instance actually holding that key. Closing this
    /// requires `assignPrimary`'s outcome to become observable (e.g. a
    /// `bool` return threaded across every `IBackend` implementation and, for
    /// wire backends, a reply field) — tracked as a follow-up, not fixed
    /// here.
    /// @tparam Model Concrete model type.
    /// @param binding Shared binding whose instance is being promoted.
    /// @param primary Canonical string encoding of the key to file it under.
    template <typename Model>
    void assignHandlerPrimary(const std::shared_ptr<detail::HandlerBinding>& binding, std::string primary) {
        std::scoped_lock const lock{_attachMtx};
        uint64_t const raw = binding->currentId.load();
        if (raw == 0U || primary.empty() || !binding->primary.empty()) {
            return;
        }
        loadBackend()->assignPrimary(::morph::exec::detail::ModelId{raw}, binding->typeId, primary);
        binding->contextKey = primary;
        binding->primary = std::move(primary);
    }

    /// @brief Returns @p binding's current primary key, or empty if unattached.
    /// @param binding Binding to inspect.
    /// @return Canonical key string, or an empty string when unattached.
    [[nodiscard]] std::string bindingPrimary(const std::shared_ptr<detail::HandlerBinding>& binding) {
        std::scoped_lock const lock{_attachMtx};
        return binding->primary;
    }

    /// @brief Lists the live shared primary keys of `Model` on the active backend.
    /// @tparam Model Concrete model type.
    /// @return Canonical key strings, in unspecified order.
    template <typename Model>
    [[nodiscard]] std::vector<std::string> listInstancesOf() {
        return loadBackend()->listInstances(std::string{::morph::model::ModelTraits<Model>::typeId()});
    }

    /// @brief Registers a result-type subscription for @p binding.
    ///
    /// The subscription is stored against the *binding*, not against a fixed
    /// instance id, and is matched at publish time by comparing the binding's
    /// current instance. Re-pointing a handler therefore moves its subscriptions
    /// with it, which is what makes "tell me about the account I am looking at"
    /// keep working when the user switches accounts.
    ///
    /// @param binding Handler binding that owns the subscription.
    /// @param type    Result type being subscribed to.
    /// @param sink    Type-erased delivery callback; receives the boxed result.
    /// @param exec    Executor the callback is delivered on.
    void addSubscription(const std::shared_ptr<detail::HandlerBinding>& binding, std::type_index type,
                         std::function<void(const std::any&)> sink, ::morph::exec::IExecutor* exec) {
        std::scoped_lock const lock{_subMtx};
        for (auto& entry : _subscriptions) {
            auto owner = entry.binding.lock();
            if (owner && owner.get() == binding.get() && entry.type == type) {
                entry.sink = std::move(sink);  // one callback per (handler, result type)
                entry.exec = exec;
                return;
            }
        }
        _subscriptions.push_back({.binding = binding, .type = type, .sink = std::move(sink), .exec = exec});
        _subscriptionCount.store(_subscriptions.size(), std::memory_order_relaxed);
    }

    /// @brief Removes @p binding's subscription for @p type, if any.
    /// @param binding Handler binding that owns the subscription.
    /// @param type    Result type to stop hearing about.
    void removeSubscription(const std::shared_ptr<detail::HandlerBinding>& binding, std::type_index type) {
        std::scoped_lock const lock{_subMtx};
        std::erase_if(_subscriptions, [&](const InstanceSubscription& entry) {
            auto owner = entry.binding.lock();
            return !owner || (owner.get() == binding.get() && entry.type == type);
        });
        _subscriptionCount.store(_subscriptions.size(), std::memory_order_relaxed);
    }

    /// @brief Whether any subscription is currently registered on this bridge.
    ///
    /// A single relaxed atomic load, so the overwhelmingly common case — a
    /// process with no subscribers at all — pays nothing per result. Without
    /// this, every successful action would build a `std::type_index`, copy its
    /// result into a `std::any`, take `_subMtx` and walk the (empty)
    /// subscription list before its `Completion` could resolve: a throughput
    /// regression for every existing caller, on the hot path, to serve a feature
    /// they are not using.
    /// @return `true` if at least one subscription exists.
    [[nodiscard]] bool hasSubscribers() const noexcept {
        return _subscriptionCount.load(std::memory_order_relaxed) != 0U;
    }

    /// @brief Delivers @p value to every subscriber attached to instance @p mid.
    ///
    /// Called for every successful action result. Subscribers are matched on
    /// *the instance the result was produced on*, so a handler hears about work
    /// another handler — or, with a shared instance, another screen entirely —
    /// did on the model it is attached to.
    ///
    /// The producing handler is notified too: suppressing the echo would force
    /// every subscriber to special-case "was this mine", which is exactly the
    /// bookkeeping the feature exists to remove.
    ///
    /// Sinks are snapshotted under the lock and invoked outside it, so a
    /// subscriber that re-enters the bridge cannot deadlock.
    ///
    /// @param mid   Instance the result was produced on.
    /// @param type  Result type produced.
    /// @param value Boxed result.
    void publishResult(::morph::exec::detail::ModelId mid, std::type_index type, const std::any& value) {
        std::vector<std::pair<std::function<void(const std::any&)>, ::morph::exec::IExecutor*>> targets;
        {
            std::scoped_lock const lock{_subMtx};
            // Prune while we are already holding the lock and walking the list:
            // a handler that is destroyed without unsubscribing would otherwise
            // leave its entry behind until some *other* handler happened to call
            // add/removeSubscription, which in a long-lived app with many
            // transient handlers is never.
            std::erase_if(_subscriptions, [](const InstanceSubscription& entry) { return entry.binding.expired(); });
            _subscriptionCount.store(_subscriptions.size(), std::memory_order_relaxed);
            for (const auto& entry : _subscriptions) {
                auto owner = entry.binding.lock();
                if (owner && entry.type == type && owner->currentId.load() == mid.v && entry.sink) {
                    targets.emplace_back(entry.sink, entry.exec);
                }
            }
        }
        for (auto& [sink, exec] : targets) {
            if (exec != nullptr) {
                exec->post([sink, value] { sink(value); });
            } else {
                sink(value);
            }
        }
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
            // Both mutexes: this phase reads/writes every live binding's
            // `primary`/`contextKey` (via `_attachMtx`'s ownership of those
            // fields) as well as `_handlers` itself (via `_mtx`), and must not
            // race a concurrent attachHandler()/ensureBound()/
            // assignHandlerPrimary() call on any one of them.
            std::scoped_lock const lock{_mtx, _attachMtx};

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
                    // A shared binding that never attached has no instance to
                    // re-create: it stays live and unbound, and acquires one on
                    // the new backend the first time it is attached.
                    if (binding->shared && binding->primary.empty()) {
                        live.push_back(weak);
                        continue;
                    }
                    auto newId = binding->shared
                                     ? newShared->registerModelShared(
                                           binding->typeId, binding->modelFactory,
                                           {.contextKey = binding->contextKey, .primary = binding->primary})
                                     : newShared->registerModelWithContext(binding->typeId, binding->modelFactory,
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
            installReconnectHandler(newShared);
        }
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
    /// On `LocalBackend`, the `localOp` this method builds first overwrites any
    /// declared computed fields from their inputs (`morph::forms::recomputeAll`,
    /// a no-op for actions with no `computedFields`), then enforces
    /// `morph::model::ActionValidator<Action>::ready(action)` before calling
    /// `Model::execute`, mirroring `ActionDispatcher::registerAction`'s runner
    /// (`registry.hpp`) for the in-process path. A `false` result resolves the
    /// returned `Completion` through `onError` with a `morph::model::ValidationError`
    /// instead of executing the action — see docs/spec/core/registry.md and
    /// docs/spec/forms/forms.md.
    ///
    /// @tparam Model  Model type that owns the handler.
    /// @tparam Action Action type to dispatch.
    /// @param binding Binding returned by `registerHandler<Model>()`.
    /// @param action  Action to execute (moved in).
    /// @param cbExec  Executor on which the `Completion` callbacks are posted.
    /// @param onResult Optional observer run on the typed result *before* it is
    ///                 moved into the returned `Completion` and before the
    ///                 caller's own `.then`. Used to adopt a result-sourced
    ///                 primary key so the binding is already promoted by the time
    ///                 user code sees the result; empty for every other call.
    /// @return Completion that resolves with the typed result or an exception
    ///         (including `ValidationError` on `LocalBackend` when the action
    ///         fails its validator).
    template <typename Model, typename Action>
    ::morph::async::Completion<typename ::morph::model::ActionTraits<Action>::Result> executeVia(
        const std::shared_ptr<detail::HandlerBinding>& binding, Action action, ::morph::exec::IExecutor* cbExec,
        std::function<void(const typename ::morph::model::ActionTraits<Action>::Result&)> onResult = {}) {
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
            // BridgeHandler<Model>::execute<Action>() directly is rejected the
            // same way a hand-built wire envelope is rejected by
            // ActionDispatcher::registerAction's runner (registry.hpp). No JSON is
            // involved on this path, so there is no declared-precision
            // reconciliation step here (that only applies to decoded wire
            // payloads); the Quantity fields carry whatever precision the caller
            // constructed them with. ActionValidator<Action>::ready defaults to
            // `true` for actions with no validator, so this is a no-op for
            // unvalidated actions (zero behavior change). The thrown exception is
            // caught by LocalBackend::execute's strand task (backend.hpp) and
            // resolves this Completion through onError.
            //
            // Overwrite any computed fields from their declared inputs before the
            // validator runs and the model ever sees the action -- the same
            // authoritative recompute ActionDispatcher::registerAction's runner
            // performs for remote topologies (registry.hpp), applied here for the
            // in-process LocalBackend path (every execute<Action>()/executeJson
            // call). Recompute must run before the validator check so a validator
            // inspecting a computed field sees the authoritative value, not
            // whatever the caller constructed the action with. No-op for actions
            // with no computedFields. See docs/spec/forms/forms.md.
            ::morph::forms::recomputeAll(*sharedAction);
            if (!::morph::model::ActionValidator<Action>::ready(*sharedAction)) {
                throw ::morph::model::ValidationError{::morph::model::ModelTraits<Model>::typeId(),
                                                      ::morph::model::ActionTraits<Action>::typeId()};
            }
            auto& model = holder.template into<Model>();
            // Local mode has no client/server split, so this is the same execution
            // site `ActionDispatcher::registerAction`'s runner is for remote modes
            // (registry.hpp) — see that overload's doc comment for the full story,
            // including why both the success and failure paths below record a
            // journal entry (a rejected/throwing execute must not leave the audit
            // trail silent) and why the exception is rethrown unchanged either way.
            try {
                auto result = std::make_shared<R>(model.execute(*sharedAction));
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
                            .outcome = ::morph::journal::Outcome::Succeeded,
                            .error = {},
                            .principal = {},
                            .timestampMs = 0,
                        });
                    }
                }
                return result;
            } catch (const std::exception& exc [[maybe_unused]]) {
                if constexpr (::morph::model::detail::actionLoggable<Action>() == ::morph::model::Loggable::Yes) {
                    if (holder.hasActionLog()) {
                        holder.recordIfAttached(::morph::journal::LogEntry{
                            .seq = 0,
                            .modelType = std::string{::morph::model::ModelTraits<Model>::typeId()},
                            .entityKey = {},
                            .actionType = std::string{::morph::model::ActionTraits<Action>::typeId()},
                            .payload = ::morph::model::ActionTraits<Action>::toJson(*sharedAction),
                            .result = {},
                            .outcome = ::morph::journal::Outcome::Failed,
                            .error = exc.what(),
                            .principal = {},
                            .timestampMs = 0,
                        });
                    }
                }
                throw;
            }
        };
        {
            std::scoped_lock const lock{_sessionMtx};
            call.session = _defaultSession;
        }
        auto anyCompletion = backend->execute(::morph::exec::detail::ModelId{raw}, std::move(call), cbExec);
        anyCompletion
            .then([typedState, onResult = std::move(onResult), this, raw,
                   alive = liveness()](const std::shared_ptr<void>& vAny) {
                // Guard the value-forwarding: if R's move/copy throws (or the cast
                // is somehow wrong), route the exception to the typed completion's
                // error sink instead of letting it escape the callback executor —
                // where ThreadPoolExecutor swallows it (the outer completion would
                // then hang forever) and QtExecutor lets it reach the event loop
                // and std::terminate. Mirrors the forwarding guard in remote.hpp's
                // SimulatedRemoteBackend::execute. See docs/spec/bridge.md.
                //
                // A backend completion can in principle resolve after the
                // Bridge is gone (see liveness()'s doc comment): the backend
                // may be co-owned and outlive this Bridge, or this callback
                // may already be running when ~Bridge() runs concurrently on
                // another thread. Check liveness FIRST, before touching
                // anything that reaches into the bridge -- both `onResult`
                // (which, for a result-keyed action, calls back into this
                // bridge via a captured raw pointer to assign the binding's
                // primary) and hasSubscribers() (which reads `this`) must
                // never run once the bridge might be gone. The typed result
                // is still delivered to the caller's own Completion either
                // way -- only the two bridge-touching side effects are
                // skipped.
                try {
                    auto* const typedResult = static_cast<R*>(vAny.get());
                    bool const bridgeAlive = !alive.expired();
                    // Runs before the value is moved out and before the caller's
                    // own .then, so a result-sourced primary key is adopted by
                    // the binding before any user code observes the result.
                    if (onResult && bridgeAlive) {
                        onResult(*typedResult);
                    }
                    // Fan the result out to everything attached to this
                    // instance before the value is moved away. Guarded on the
                    // bridge's liveness token: a completion can in principle
                    // resolve after the Bridge is gone.
                    if constexpr (std::is_copy_constructible_v<R>) {
                        if (bridgeAlive && hasSubscribers()) {
                            publishResult(::morph::exec::detail::ModelId{raw}, std::type_index{typeid(R)},
                                          std::any{*typedResult});
                        }
                    }
                    typedState->setValue(std::move(*typedResult));
                } catch (...) {
                    typedState->setException(std::current_exception());
                }
            })
            .onError([typedState](const std::exception_ptr& err) { typedState->setException(err); });
        return typed;
    }

private:
    template <typename, typename>
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
            // Both mutexes: reads `_handlers` (guarded by `_mtx`) and each
            // binding's `contextKey` (guarded by `_attachMtx`), same
            // reasoning as switchBackend() above.
            std::scoped_lock const lock{_mtx, _attachMtx};
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
    // Guards the shared-handler attach/register/assign path (ensureBound,
    // attachHandler, assignHandlerPrimary) separately from `_mtx`, which
    // guards `_handlers` membership and the active-backend swap.
    // attachModel/registerModelShared/assignPrimary can block on a full
    // network round-trip for a remote backend; if that ran under `_mtx`, an
    // unrelated handler's construction or destruction, or a switchBackend()
    // call, on the *same* Bridge would block for the same round-trip, and a
    // reply-delivering thread that itself needed `_mtx` could deadlock
    // against it. `HandlerBinding::primary`/`contextKey` are therefore
    // mutated (and must be read) only under `_attachMtx` — never under `_mtx`
    // alone. `switchBackend()` and the reconnect handler, which also touch
    // them alongside `_handlers`, take both mutexes together via
    // `std::scoped_lock{_mtx, _attachMtx}` (deadlock-safe regardless of
    // acquisition order, by `std::scoped_lock`'s own guarantee).
    std::mutex _attachMtx;
    mutable std::mutex _sessionMtx;
    ::morph::session::Context _defaultSession;
    // Instance subscriptions. Held against the binding rather than a fixed
    // instance id so a re-pointed handler keeps its subscriptions; matched at
    // publish time by comparing the binding's current instance.
    struct InstanceSubscription {
        std::weak_ptr<detail::HandlerBinding> binding;
        std::type_index type;
        std::function<void(const std::any&)> sink;
        ::morph::exec::IExecutor* exec = nullptr;
    };
    std::mutex _subMtx;
    std::vector<InstanceSubscription> _subscriptions;
    // Mirrors _subscriptions.size() for the lock-free hasSubscribers() probe.
    // Maintained under _subMtx; read relaxed off it. A stale-by-one read is
    // harmless: publishResult re-checks under the lock and finds nothing.
    std::atomic<std::size_t> _subscriptionCount{0};
    // Destroyed with the Bridge; handlers hold weak_ptrs to it (see liveness()).
    std::shared_ptr<const void> _liveness{std::make_shared<char>()};
};

/// @brief `BridgeHandler` sharing policy: private, one instance per handler.
///
/// The default, and what every pre-existing call site gets. Such a handler
/// registers its own instance at construction and never enters the shared
/// directory, so two `BridgeHandler<M>` objects are two independent models —
/// byte-for-byte the behaviour morph has always had.
struct NoSharing {};

/// @brief `BridgeHandler` sharing policy: joins the shared instance directory.
///
/// A shared handler registers **nothing** at construction. It acquires an
/// instance the first time a keyed action or an explicit `attach()` names a
/// primary key, and every other `AllowShared` handler naming that same key —
/// in this process or, for a remote backend, in any other client — reaches the
/// same instance. Releasing the last such handler destroys it.
///
/// A shared handler that only ever runs *keyless* actions never attaches, and
/// its `execute` fails fast with "handler not bound": there is no instance to
/// run against and inventing a private one would silently defeat the sharing
/// the caller asked for. Attach first — see docs/planned/shared_model_instances.md.
struct AllowShared {};

/// @brief RAII wrapper that binds a single model type to a `Bridge`.
///
/// On construction, registers a `HandlerBinding` on the bridge. On destruction,
/// deregisters it automatically. The handler is non-copyable.
///
/// @par Instance subscriptions
/// Beyond the one-shot `execute(action) -> Completion<R>` API, a handler can
/// observe *the instance it is attached to*:
///
/// - `subscribe<R>(cb)` fires whenever an `R` is produced on that instance, by
///   any handler attached to it.
/// - `unsubscribe<R>()` drops the callback.
///
/// @tparam Model Concrete model type.
template <typename Model, typename Sharing = NoSharing>
// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions)
class BridgeHandler {
public:
    /// @brief Whether this handler participates in the shared instance directory.
    static constexpr bool kShared = std::is_same_v<Sharing, AllowShared>;

    /// @brief Constructs and registers the handler using the default model factory.
    ///
    /// @param bridge   The bridge to register on.
    /// @param guiExec  Executor used to deliver `Completion` callbacks (e.g. the GUI thread).
    BridgeHandler(Bridge& bridge, ::morph::exec::IExecutor* guiExec)
        : _bridge{bridge},
          _bridgeAlive{bridge.liveness()},
          _guiExec{guiExec},
          _binding{makeBinding(bridge)} {
        static_assert(!kShared || ::morph::model::KeyedModel<Model>,
                      "BridgeHandler<Model, AllowShared> requires Model to declare a PrimaryKey alias");
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
          _binding{std::move(binding)} {
        _bridge.registerHandler(_binding);
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
    /// @return Completion that resolves on the GUI executor. A payload- or
    ///         result-keyed action's attach/promote step never throws out of
    ///         this call, even when the backend refuses it (e.g. a remote
    ///         server at `LimitPolicy::maxLiveModels`, a transport error, or
    ///         an unauthorized attach) — the failure is instead delivered
    ///         through the returned Completion's `.onError(...)`, exactly
    ///         like any other dispatch failure.
    template <typename Action>
    ::morph::async::Completion<typename ::morph::model::ActionTraits<Action>::Result> execute(Action action) {
        using R = ::morph::model::ActionTraits<Action>::Result;
        if constexpr (kShared && ::morph::model::detail::PayloadKeyed<Action>) {
            // The action names its instance: attach (or re-point) before
            // dispatching, so the call lands on the instance it asked for. A
            // remote backend's refusal (LimitPolicy::maxLiveModels, a
            // transport error, unauthorized) must surface through the
            // returned Completion's onError, exactly like every other
            // dispatch failure — not as a synchronous throw out of execute().
            try {
                _bridge.template attachHandler<Model>(_binding, ::morph::model::ActionKeyTraits<Action>::key(action));
            } catch (...) {
                return failedCompletion<R>(std::current_exception());
            }
        }
        if constexpr (kShared && ::morph::model::detail::ResultKeyed<Action>) {
            // The action *creates* the instance and its result carries the
            // generated key, exactly as a database insert returns its primary
            // key. Adopt it before any user callback observes the result, so a
            // .then() can immediately run further actions on the new instance.
            //
            // The action generates its own key, so it must run before the key
            // exists. Give the handler an anonymous instance to run on, then
            // promote *that* instance once the reply names it — re-pointing to a
            // fresh one instead would strand whatever the create just did.
            try {
                _bridge.ensureBound(_binding);
            } catch (...) {
                return failedCompletion<R>(std::current_exception());
            }
            auto* const bridgePtr = &_bridge;
            auto binding = _binding;
            return _bridge.template executeVia<Model, Action>(
                _binding, std::move(action), _guiExec, [bridgePtr, binding](const R& result) {
                    bridgePtr->template assignHandlerPrimary<Model>(
                        binding, ::morph::model::ActionKeyTraits<Action>::template keyOfResult<R>(result));
                });
        } else {
            return _bridge.template executeVia<Model, Action>(_binding, std::move(action), _guiExec);
        }
    }

    /// @brief Attaches (or re-points) this handler to the instance for @p key.
    ///
    /// Creates the instance if no live instance holds @p key, otherwise joins the
    /// existing one. Re-pointing an already-attached handler releases the old
    /// instance, which survives only if another handler still holds it.
    ///
    /// The primary is deliberately **not** write-once: naming a different key is
    /// how a screen switches which entity it is looking at. Instances never
    /// change their own identity — the *handler* moves — so a key always maps to
    /// exactly one instance and no collision case can arise.
    ///
    /// @tparam M Defaulted to `Model`; never named explicitly. Present only so the
    ///         signature is instantiated lazily, since `PrimaryKeyOf` is
    ///         ill-formed for an unkeyed model.
    /// @param key Primary key of the instance to attach to.
    /// @throws std::runtime_error if the backend refuses the attach (e.g. a
    ///         remote server at `LimitPolicy::maxLiveModels`, a transport
    ///         error, or an unauthorized attach). `attach()` is a synchronous
    ///         `void` call with no `Completion` to route a failure through,
    ///         unlike `execute()`; a caller that wants the failure delivered
    ///         asynchronously should attach via a payload-keyed action's
    ///         `execute()` instead.
    template <typename M = Model>
    void attach(const ::morph::model::PrimaryKeyOf<M>& key)
        requires kShared
    {
        _bridge.template attachHandler<Model>(_binding, ::morph::model::keyToString(key));
    }

    /// @brief This handler's current primary key, or `nullopt` if unattached.
    /// @tparam M Defaulted to `Model`; never named explicitly. See `attach`.
    /// @return The attached key, or `nullopt` before the first attach.
    template <typename M = Model>
    [[nodiscard]] std::optional<::morph::model::PrimaryKeyOf<M>> primary()
        requires kShared
    {
        auto raw = _bridge.bindingPrimary(_binding);
        if (raw.empty()) {
            return std::nullopt;
        }
        return ::morph::model::keyFromString<::morph::model::PrimaryKeyOf<M>>(raw);
    }

    /// @brief Snapshot of the live shared instance keys for `Model`.
    ///
    /// Asynchronous even in local mode: the directory is backend state, and in
    /// remote mode answering costs a round trip. Returning a bare `std::vector`
    /// would work in-process and force a different call site everywhere else,
    /// breaking the local/remote symmetry the framework is built on.
    ///
    /// The result is a snapshot, stale the moment it arrives — another client may
    /// attach or release before the callback runs. Treat a returned key as "was
    /// live recently", never as a guarantee that a later `attach` finds the same
    /// instance.
    ///
    /// @tparam M Defaulted to `Model`; never named explicitly. See `attach`.
    /// @return Completion resolving on the GUI executor with the live keys.
    template <typename M = Model>
    [[nodiscard]] ::morph::async::Completion<std::vector<::morph::model::PrimaryKeyOf<M>>> instances()
        requires kShared
    {
        using Key = ::morph::model::PrimaryKeyOf<M>;
        auto state = std::make_shared<::morph::async::detail::CompletionState<std::vector<Key>>>();
        ::morph::async::Completion<std::vector<Key>> comp{state, _guiExec};
        try {
            std::vector<Key> keys;
            for (const auto& raw : _bridge.template listInstancesOf<Model>()) {
                keys.push_back(::morph::model::keyFromString<Key>(raw));
            }
            state->setValue(std::move(keys));
        } catch (...) {
            state->setException(std::current_exception());
        }
        return comp;
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

    /// @brief Creates this handler's binding: deferred when shared, immediate otherwise.
    /// @param bridge Bridge to create the binding on.
    /// @return The new binding.
    static std::shared_ptr<detail::HandlerBinding> makeBinding(Bridge& bridge) {
        if constexpr (kShared) {
            return bridge.template registerSharedHandler<Model>();
        } else {
            return bridge.template registerHandler<Model>();
        }
    }

    /// @brief The executor used to deliver this handler's `Completion` callbacks.
    /// @return The GUI/callback executor passed at construction.
    [[nodiscard]] ::morph::exec::IExecutor* guiExecutor() const noexcept { return _guiExec; }

    /// @brief Subscribes to results of type @p R produced on the attached instance.
    ///
    /// Fires whenever an `R` is produced on the instance this handler is
    /// attached to — by this handler, by another handler sharing the instance,
    /// or by another screen entirely. The subscriber names *what it renders*,
    /// not what somebody else must call to produce it, so adding an action that
    /// also yields an `R` never breaks an existing subscriber.
    ///
    /// One callback per `(handler, R)`: subscribing again replaces the previous
    /// one. Callbacks are delivered on this handler\'s executor. Failed actions
    /// notify nobody; delivery is best-effort and unbuffered, with no replay.
    ///
    /// @tparam R Result/state type to observe.
    /// @param cb Callable receiving the value by value on the GUI executor.
    template <typename R>
    void subscribe(std::function<void(R)> cb) {
        _bridge.addSubscription(
            _binding, std::type_index{typeid(R)},
            [cb = std::move(cb)](const std::any& boxed) { cb(std::any_cast<const R&>(boxed)); }, _guiExec);
    }

    /// @brief Removes this handler\'s subscription for @p R.
    /// @tparam R Result/state type to stop hearing about.
    template <typename R>
    void unsubscribe() {
        _bridge.removeSubscription(_binding, std::type_index{typeid(R)});
    }


    /// @brief Returns the underlying `HandlerBinding`.
    ///
    /// @return Shared pointer to the binding owned by this handler.
    [[nodiscard]] const std::shared_ptr<detail::HandlerBinding>& binding() const { return _binding; }

private:
    /// @brief Builds an already-failed `Completion<R>`, resolved via `.onError(...)`.
    ///
    /// Used to turn a synchronous exception from the attach/promote step of
    /// `execute()` into the same asynchronous failure shape every other
    /// dispatch error takes, instead of letting it escape `execute()` as a
    /// thrown exception.
    /// @tparam R Result type of the action that failed to attach/promote.
    /// @param exc Exception to deliver through `.onError(...)`.
    /// @return A `Completion<R>` already resolved with @p exc.
    template <typename R>
    ::morph::async::Completion<R> failedCompletion(std::exception_ptr exc) {
        auto state = std::make_shared<::morph::async::detail::CompletionState<R>>();
        ::morph::async::Completion<R> comp{state, _guiExec};
        state->setException(std::move(exc));
        return comp;
    }

    Bridge& _bridge;
    std::weak_ptr<const void> _bridgeAlive;  // expires when _bridge is destroyed
    ::morph::exec::IExecutor* _guiExec;
    std::shared_ptr<detail::HandlerBinding> _binding;
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
            // Overwrite any computed fields from their declared inputs -- a
            // computed field is never trusted from the client, on any path.
            // No-op for actions with no computedFields. See docs/spec/forms/forms.md.
            ::morph::forms::recomputeAll(action);
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

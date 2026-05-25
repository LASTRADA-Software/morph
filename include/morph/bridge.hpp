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

#include "backend.hpp"
#include "completion.hpp"
#include "registry.hpp"
#include "session.hpp"

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
        installReconnectHandler(_backend.load());
    }

    /// @brief Cancels every still-pending completion on the active backend.
    ///
    /// Each `.onError(...)` callback receives a `BridgeDestroyedError`. In-flight
    /// server replies that arrive after destruction are no-ops because each
    /// `CompletionState::setValue`/`setException` is idempotent.
    ~Bridge() {
        if (auto active = _backend.load()) {
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
        std::scoped_lock lock{_mtx};
        binding->currentId.store(_backend.load()->registerModel(binding->typeId, binding->modelFactory).v);
        _handlers.push_back(binding);
        return binding;
    }

    /// @brief Registers a pre-built binding with a custom factory.
    ///
    /// Use this overload when the model factory needs to capture external
    /// dependencies that the type-erasing default factory cannot carry.
    /// @param binding Pre-constructed binding. Its `typeId` and `modelFactory` must be set.
    void registerHandler(const std::shared_ptr<detail::HandlerBinding>& binding) {
        std::scoped_lock lock{_mtx};
        binding->currentId.store(_backend.load()->registerModel(binding->typeId, binding->modelFactory).v);
        _handlers.push_back(binding);
    }

    /// @brief Atomically replaces the active backend with @p newBackend.
    ///
    /// All live bindings are re-registered on the new backend and their
    /// `currentId` values are updated. The old backend is released after the
    /// swap. Any in-flight `Completion` objects targeting the old backend will
    /// fail naturally.
    ///
    /// @note Lock ordering: `Bridge::_mtx` is acquired before
    ///       `LocalBackend::_regMtx`. `onBackendChanged()` implementations must
    ///       **not** call `registerHandler()` or `deregisterHandler()` — those
    ///       also acquire `_mtx` and would deadlock.
    ///
    /// @param newBackend Replacement backend. Ownership is transferred.
    /// @brief Installs a default session context applied to every call that does
    ///        not provide one explicitly via `BridgeHandler::executeWith(...)`.
    ///
    /// Typical pattern: call this once after login to bind the user's principal
    /// and locale, then every subsequent `handler.execute(action)` carries the
    /// session automatically. Thread-safe.
    ///
    /// @param session The new default. Pass `{}` to clear.
    void setDefaultSession(::morph::session::Context session) {
        std::scoped_lock lock{_sessionMtx};
        _defaultSession = std::move(session);
    }

    /// @brief Returns a copy of the currently installed default session. Thread-safe.
    [[nodiscard]] ::morph::session::Context defaultSession() const {
        std::scoped_lock lock{_sessionMtx};
        return _defaultSession;
    }

    void switchBackend(std::unique_ptr<::morph::backend::detail::IBackend> newBackend) {
        auto newShared = std::shared_ptr<::morph::backend::detail::IBackend>(std::move(newBackend));
        std::shared_ptr<::morph::backend::detail::IBackend> previous;
        {
            std::scoped_lock lock{_mtx};

            std::vector<std::weak_ptr<detail::HandlerBinding>> live;
            for (auto& weak : _handlers) {
                auto binding = weak.lock();
                if (!binding) {
                    continue;
                }
                auto newId = newShared->registerModel(binding->typeId, binding->modelFactory);
                binding->currentId.store(newId.v);
                live.push_back(weak);
            }
            _handlers = std::move(live);

            previous = _backend.exchange(newShared);
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
        std::scoped_lock lock{_mtx};
        uint64_t raw = binding->currentId.load();
        if (raw != 0U) {
            _backend.load()->deregisterModel(::morph::exec::detail::ModelId{raw});
        }
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
    /// @tparam Model  Model type that owns the handler.
    /// @tparam Action Action type to dispatch.
    /// @param binding Binding returned by `registerHandler<Model>()`.
    /// @param action  Action to execute (moved in).
    /// @param cbExec  Executor on which the `Completion` callbacks are posted.
    /// @return Completion that resolves with the typed result or an exception.
    template <typename Model, typename Action>
    ::morph::async::Completion<typename ::morph::model::ActionTraits<Action>::Result>
    executeVia(const std::shared_ptr<detail::HandlerBinding>& binding, Action action,
               ::morph::exec::IExecutor* cbExec) {
        using R = ::morph::model::ActionTraits<Action>::Result;

        auto backend = _backend.load();
        uint64_t raw = binding->currentId.load();

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
            auto& model = holder.template into<Model>();
            return std::make_shared<R>(model.execute(*sharedAction));
        };
        {
            std::scoped_lock lock{_sessionMtx};
            call.session = _defaultSession;
        }
        auto anyCompletion = backend->execute(::morph::exec::detail::ModelId{raw}, std::move(call), cbExec);
        anyCompletion
            .then([typedState](const std::shared_ptr<void>& vAny) {
                typedState->setValue(std::move(*static_cast<R*>(vAny.get())));
            })
            .onError([typedState](const std::exception_ptr& err) { typedState->setException(err); });
        return typed;
    }

private:
    void installReconnectHandler(const std::shared_ptr<::morph::backend::detail::IBackend>& backend) {
        if (!backend) {
            return;
        }
        // The handler is invoked on the backend's transport thread. We keep a
        // weak_ptr to the same shared backend so a stale callback fired after a
        // switchBackend is a no-op.
        std::weak_ptr<::morph::backend::detail::IBackend> weakBackend{backend};
        backend->setReconnectHandler([this, weakBackend] {
            auto pinned = weakBackend.lock();
            std::scoped_lock lock{_mtx};
            if (!pinned || pinned != _backend.load()) {
                return;  // We've moved on to a different backend; ignore.
            }
            for (auto& weak : _handlers) {
                auto binding = weak.lock();
                if (!binding) {
                    continue;
                }
                auto newId = pinned->registerModel(binding->typeId, binding->modelFactory);
                binding->currentId.store(newId.v);
            }
        });
    }

    std::atomic<std::shared_ptr<::morph::backend::detail::IBackend>> _backend;
    std::mutex _mtx;
    std::vector<std::weak_ptr<detail::HandlerBinding>> _handlers;
    mutable std::mutex _sessionMtx;
    ::morph::session::Context _defaultSession;
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
class BridgeHandler {
public:
    /// @brief Constructs and registers the handler using the default model factory.
    ///
    /// @param bridge   The bridge to register on.
    /// @param guiExec  Executor used to deliver `Completion` callbacks (e.g. the GUI thread).
    BridgeHandler(Bridge& bridge, ::morph::exec::IExecutor* guiExec)
        : _bridge{bridge},
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
    BridgeHandler(Bridge& bridge, ::morph::exec::IExecutor* guiExec,
                  std::shared_ptr<detail::HandlerBinding> binding)
        : _bridge{bridge},
          _guiExec{guiExec},
          _binding{std::move(binding)},
          _subs{std::make_shared<SubscriberState>()} {
        _bridge.registerHandler(_binding);
        _subs->bridge = &_bridge;
        _subs->binding = _binding;
        _subs->guiExec = _guiExec;
    }

    /// @brief Deregisters the binding from the bridge.
    ~BridgeHandler() { _bridge.deregisterHandler(_binding); }

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

    /// @brief Subscribes to results of action type @p Action.
    ///
    /// @tparam Action Concrete action type registered with `BRIDGE_REGISTER_ACTION`.
    /// @param cb Callable receiving the action's `Result` by value on the GUI executor.
    template <typename Action>
    void subscribe(std::function<void(typename ::morph::model::ActionTraits<Action>::Result)> cb) {
        using R = ::morph::model::ActionTraits<Action>::Result;
        auto wrapper = [cb = std::move(cb)](const std::any& boxed) {
            cb(std::any_cast<const R&>(boxed));
        };
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
        tryFireImpl<A>(std::weak_ptr<SubscriberState>(_subs), ::morph::model::ActionTraits<A>::typeId());
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
    static void tryFireImpl(const std::weak_ptr<SubscriberState>& weak, std::string_view typeId) {
        auto state = weak.lock();
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
                    tryFireImpl<Action>(weak, typeId);
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
                    tryFireImpl<Action>(weak, typeId);
                }
            });
    }

    Bridge& _bridge;
    ::morph::exec::IExecutor* _guiExec;
    std::shared_ptr<detail::HandlerBinding> _binding;
    std::shared_ptr<SubscriberState> _subs;
};

}  // namespace morph::bridge

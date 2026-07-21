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

    /// @brief Removes the model identified by @p mid from the backend.
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
    /// Default implementation: store-and-ignore. Backends with no transport (e.g.
    /// `LocalBackend`) never invoke it.
    /// @param handler Callable invoked on the backend's transport thread after a
    ///                successful reconnect. Pass `nullptr` to clear.
    virtual void setReconnectHandler(const std::function<void()>& handler) { (void)handler; }
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
    /// concrete type is captured by the factory closure.
    /// @param factory  Callable that constructs the `IModelHolder`.
    /// @return Newly assigned `ModelId`.
    ::morph::exec::detail::ModelId registerModel(
        const std::string& /*typeId*/,
        std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory) override {
        ::morph::observe::detail::emitMetric(::morph::observe::Metric::registerCount, 1.0);
        ::morph::exec::detail::ModelId mid{_nextId.fetch_add(1) + 1};
        std::scoped_lock const lock{_regMtx};
        _models[mid] = factory();
        return mid;
    }

    /// @brief Removes the model with @p mid. Thread-safe.
    /// @param mid Id returned by a prior `registerModel()` call.
    void deregisterModel(::morph::exec::detail::ModelId mid) override {
        ::morph::observe::detail::emitMetric(::morph::observe::Metric::deregisterCount, 1.0);
        std::scoped_lock const lock{_regMtx};
        _models.erase(mid);
    }

    /// @brief Schedules `onBackendChanged()` on each live model's strand. Thread-safe.
    ///
    /// Every model that implements `IBackendChangedSink` has its
    /// `onBackendChanged()` **posted onto that model's own strand** (the same
    /// per-`ModelId` serial queue `execute` uses), rather than invoked inline on
    /// the caller's thread. Two properties follow, and they are the whole point:
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
            sinks;
        {
            std::scoped_lock const lock{_regMtx};
            for (auto& [modelId, holder] : _models) {
                if (dynamic_cast<::morph::model::detail::IBackendChangedSink*>(holder.get()) != nullptr) {
                    sinks.emplace_back(modelId, holder);
                }
            }
        }
        for (auto& [modelId, holder] : sinks) {
            _strand.post(modelId, [h = std::move(holder)]() mutable {
                auto* sink = dynamic_cast<::morph::model::detail::IBackendChangedSink*>(h.get());
                if (sink != nullptr) {
                    sink->onBackendChanged();
                }
            });
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
        {
            std::scoped_lock const lock{_regMtx};
            auto iter = _models.find(mid);
            if (iter != _models.end()) {
                holder = iter->second;
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
        // while this task is still queued or running.
        auto inFlightCounter = _inFlight;
        auto const inFlightAfterInc = inFlightCounter->fetch_add(1, std::memory_order_relaxed) + 1;
        ::morph::observe::detail::emitMetric(::morph::observe::Metric::executeInFlight,
                                             static_cast<double>(inFlightAfterInc));
        _strand.post(mid, [localOp = std::move(localOp), holder = std::move(holder), compState,
                           session = std::move(session), modelTypeId = std::move(modelTypeId),
                           actionTypeId = std::move(actionTypeId), inFlightCounter]() mutable {
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

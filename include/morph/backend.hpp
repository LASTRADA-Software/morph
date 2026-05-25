// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "completion.hpp"
#include "model.hpp"
#include "registry.hpp"
#include "session.hpp"
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
struct IBackend {
    virtual ~IBackend() = default;

    /// @brief Registers a new model instance and returns its opaque id.
    virtual ::morph::exec::detail::ModelId registerModel(
        const std::string& typeId,
        std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory) = 0;

    /// @brief Removes the model identified by @p mid from the backend.
    virtual void deregisterModel(::morph::exec::detail::ModelId mid) = 0;

    /// @brief Dispatches @p call against the model identified by @p mid.
    virtual ::morph::async::Completion<std::shared_ptr<void>> execute(
        ::morph::exec::detail::ModelId mid, ActionCall call, ::morph::exec::IExecutor* cbExec) = 0;

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
    virtual void setReconnectHandler(std::function<void()> handler) { (void)handler; }
};

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
        ::morph::exec::detail::ModelId mid{_nextId.fetch_add(1) + 1};
        std::scoped_lock lock{_regMtx};
        _models[mid] = factory();
        return mid;
    }

    /// @brief Removes the model with @p mid. Thread-safe.
    /// @param mid Id returned by a prior `registerModel()` call.
    void deregisterModel(::morph::exec::detail::ModelId mid) override {
        std::scoped_lock lock{_regMtx};
        _models.erase(mid);
    }

    /// @brief Notifies every live model that implements `IBackendChangedSink`. Thread-safe.
    void notifyBackendChanged() override {
        std::scoped_lock lock{_regMtx};
        for (auto& [modelId, holder] : _models) {
            if (auto* sink = dynamic_cast<::morph::model::detail::IBackendChangedSink*>(holder.get())) {
                sink->onBackendChanged();
            }
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
    ::morph::async::Completion<std::shared_ptr<void>> execute(
        ::morph::exec::detail::ModelId mid, detail::ActionCall call,
        ::morph::exec::IExecutor* cbExec) override {
        auto compState = std::make_shared<::morph::async::detail::CompletionState<std::shared_ptr<void>>>();
        ::morph::async::Completion<std::shared_ptr<void>> comp{compState, cbExec};

        std::shared_ptr<::morph::model::detail::IModelHolder> holder;
        {
            std::scoped_lock lock{_regMtx};
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
        _strand.post(mid, [localOp = std::move(localOp), holder = std::move(holder), compState,
                           session = std::move(session)]() mutable {
            try {
                ::morph::session::detail::ScopedContext scoped{session};
                compState->setValue(localOp(*holder));
            } catch (...) {
                compState->setException(std::current_exception());
            }
        });
        return comp;
    }

    /// @brief Resolves every still-pending completion this backend produced with @p exc.
    /// @param exc Exception delivered to every pending completion's error sink.
    void cancelPending(const std::exception_ptr& exc) override {
        std::vector<std::weak_ptr<::morph::async::detail::CompletionState<std::shared_ptr<void>>>> snapshot;
        {
            std::scoped_lock lock{_pendingMtx};
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
        std::scoped_lock lock{_pendingMtx};
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
};

}  // namespace morph::backend

// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "backend.hpp"

namespace morph::backend {

/// @brief Server-side message handler that owns model instances and dispatches actions.
///
/// `RemoteServer` receives pipe-delimited text messages (from a WebSocket, a
/// simulated transport, or any other source) and executes the corresponding
/// model operations via an `ActionDispatcher`.
///
/// @par Heap allocation requirement
/// `RemoteServer` **must** be heap-allocated via `std::make_shared`. `handle()`
/// captures `shared_from_this()` to prevent use-after-free when the worker pool
/// outlives the server object.
///
/// @par Message protocol
/// - `register|<typeId>` — creates a model instance; replies `ok|<modelId>`.
/// - `deregister|<modelId>` — destroys the instance; replies `ok`.
/// - `execute|<mid>|<modelTy>|<actionTy>|<body>` — dispatches an action; replies `ok|<result>`.
/// - `execute|<callId>|<mid>|<modelTy>|<actionTy>|<body>` — same with a correlation id.
/// - Any error replies with `err|<message>` or `err|<callId>|<message>`.
class RemoteServer : public std::enable_shared_from_this<RemoteServer> {
public:
    /// @brief Constructs a server backed by @p workerPool.
    ///
    /// @param workerPool Pool used to process messages asynchronously.
    /// @param dispatcher Action dispatcher; defaults to the process-level singleton.
    /// @param registry   Model factory registry; defaults to the process-level singleton.
    explicit RemoteServer(
        ::morph::exec::IExecutor& workerPool,
        ::morph::model::detail::ActionDispatcher& dispatcher = ::morph::model::detail::defaultDispatcher(),
        ::morph::model::detail::ModelRegistryFactory& registry = ::morph::model::detail::defaultRegistry())
        : _pool{workerPool}, _strand{workerPool}, _dispatcher{dispatcher}, _registry{registry} {}

    /// @brief Asynchronously processes @p msg and calls @p reply with the response.
    ///
    /// The message is dispatched to the worker pool. @p reply is called exactly
    /// once from the pool thread when processing completes.
    ///
    /// Thread-safe. Safe to call before the previous call's reply has been delivered.
    ///
    /// @param msg   Pipe-delimited protocol message.
    /// @param reply Callback invoked with the response string.
    void handle(std::string msg, std::function<void(std::string)> reply) {
        auto self = shared_from_this();
        _pool.post(
            [self, msg = std::move(msg), reply = std::move(reply)]() mutable { self->dispatchMessage(msg, reply); });
    }

private:
    void dispatchMessage(const std::string& msg, std::function<void(std::string)>& reply) {
        // Split into at most 6 parts to support both the 5-part SimulatedRemote
        // protocol and the 6-part Qt WebSocket protocol (which carries a callId).
        auto parts = split(msg, '|', 6);
        try {
            if (parts[0] == "register") {
                if (parts.size() < 2) {
                    throw std::runtime_error("register requires a typeId");
                }
                auto holder = _registry.create(parts[1]);
                ::morph::exec::detail::ModelId mid{_nextId.fetch_add(1) + 1};
                {
                    std::scoped_lock lock{_regMtx};
                    _models[mid] = std::move(holder);
                }
                reply("ok|" + std::to_string(mid.v));
            } else if (parts[0] == "deregister") {
                if (parts.size() < 2) {
                    throw std::runtime_error("deregister requires a modelId");
                }
                ::morph::exec::detail::ModelId mid{std::stoull(parts[1])};
                std::scoped_lock lock{_regMtx};
                _models.erase(mid);
                reply("ok");
            } else if (parts[0] == "execute") {
                if (parts.size() == 6) {
                    // Qt WebSocket format: execute|callId|mid|modelTy|actionTy|body
                    std::string callId = parts[1];
                    ::morph::exec::detail::ModelId mid{std::stoull(parts[2])};
                    dispatchExecute(std::move(callId), mid, parts[3], parts[4], parts[5], reply);
                } else if (parts.size() == 5) {
                    // SimulatedRemote format: execute|mid|modelTy|actionTy|body
                    ::morph::exec::detail::ModelId mid{std::stoull(parts[1])};
                    dispatchExecute("", mid, parts[2], parts[3], parts[4], reply);
                } else {
                    throw std::runtime_error("execute requires 5 or 6 parts");
                }
            } else {
                reply("err|bad msg type");
            }
        } catch (const std::exception& exc) {
            reply(std::string{"err|"} + exc.what());
        }
    }

    void dispatchExecute(std::string callId, ::morph::exec::detail::ModelId mid, const std::string& modelTy,
                         const std::string& actionTy, const std::string& body,
                         std::function<void(std::string)> reply) {
        std::shared_ptr<::morph::model::detail::IModelHolder> holder;
        {
            std::scoped_lock lock{_regMtx};
            auto iter = _models.find(mid);
            if (iter != _models.end()) {
                holder = iter->second;
            }
        }
        if (!holder) {
            if (callId.empty()) {
                reply("err|model not found");
            } else {
                reply("err|" + callId + "|model not found");
            }
            return;
        }
        _strand.post(mid, [&disp = _dispatcher, callId = std::move(callId), modelTy, actionTy, body,
                           holder = std::move(holder), reply = std::move(reply)]() mutable {
            try {
                auto result = disp.dispatch(modelTy, actionTy, *holder, body);
                if (callId.empty()) {
                    reply("ok|" + result);
                } else {
                    reply("ok|" + callId + "|" + result);
                }
            } catch (const std::exception& exc) {
                if (callId.empty()) {
                    reply(std::string{"err|"} + exc.what());
                } else {
                    reply("err|" + callId + "|" + exc.what());
                }
            }
        });
    }

    static std::vector<std::string> split(const std::string& src, char sep, int maxParts) {
        std::vector<std::string> out;
        std::size_t pos = 0;
        while (static_cast<int>(out.size()) < maxParts - 1) {
            auto found = src.find(sep, pos);
            if (found == std::string::npos) {
                break;
            }
            out.emplace_back(src.substr(pos, found - pos));
            pos = found + 1;
        }
        out.emplace_back(src.substr(pos));
        return out;
    }

    ::morph::exec::IExecutor& _pool;
    ::morph::exec::detail::StrandExecutor _strand;
    ::morph::model::detail::ActionDispatcher& _dispatcher;
    ::morph::model::detail::ModelRegistryFactory& _registry;
    std::mutex _regMtx;
    std::unordered_map<::morph::exec::detail::ModelId, std::shared_ptr<::morph::model::detail::IModelHolder>,
                       ::morph::exec::detail::ModelIdHash>
        _models;
    std::atomic<uint64_t> _nextId{0};
};

/// @brief `IBackend` adapter that routes all calls through a `RemoteServer` using a
///        synchronous request-reply protocol.
///
/// Intended for testing and in-process simulation of remote execution. All
/// `registerModel()`, `deregisterModel()`, and `execute()` calls are forwarded
/// as protocol messages to the server; the calling thread blocks until the reply
/// arrives via `std::promise`.
class SimulatedRemoteBackend : public detail::IBackend {
public:
    /// @brief Constructs the backend targeting @p server.
    /// @param server The `RemoteServer` instance to forward calls to.
    explicit SimulatedRemoteBackend(RemoteServer& server) : _server{server} {}

    /// @brief Registers the model type on the server and returns its assigned id.
    ///
    /// Blocks until the server replies. The @p factory argument is ignored —
    /// model construction is delegated to the server's `ModelRegistryFactory`.
    ///
    /// @param typeId String type-id sent in the `register` message.
    /// @return `ModelId` assigned by the server.
    /// @throws std::runtime_error if the server replies with an error.
    ::morph::exec::detail::ModelId registerModel(
        const std::string& typeId,
        std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()>) override {
        std::promise<::morph::exec::detail::ModelId> prom;
        auto fut = prom.get_future();
        _server.handle("register|" + typeId, [&prom](const std::string& reply) {
            if (reply.starts_with("ok|")) {
                prom.set_value(::morph::exec::detail::ModelId{std::stoull(reply.substr(3))});
            } else {
                prom.set_exception(std::make_exception_ptr(std::runtime_error("register failed: " + reply)));
            }
        });
        return fut.get();
    }

    /// @brief Deregisters the model on the server. Blocks until the reply arrives.
    /// @param mid Id of the model to deregister.
    void deregisterModel(::morph::exec::detail::ModelId mid) override {
        std::promise<void> prom;
        auto fut = prom.get_future();
        _server.handle("deregister|" + std::to_string(mid.v), [&prom](const std::string&) { prom.set_value(); });
        fut.get();
    }

    /// @brief Serialises the action, sends it to the server, and returns a `Completion`.
    ///
    /// The `Completion` resolves when the server's reply is received and
    /// deserialized. Callbacks are posted via @p cbExec.
    ///
    /// @param mid    Target model id on the server.
    /// @param call   Bundled action; `serializeAction` and `deserializeResult` are used.
    /// @param cbExec Executor for delivering the completion callbacks.
    /// @return Completion that resolves with the deserialized result or an error.
    ::morph::async::Completion<std::shared_ptr<void>> execute(::morph::exec::detail::ModelId mid,
                                                              detail::ActionCall call,
                                                              ::morph::exec::IExecutor* cbExec) override {
        auto state = std::make_shared<::morph::async::detail::CompletionState<std::shared_ptr<void>>>();
        ::morph::async::Completion<std::shared_ptr<void>> comp{state, cbExec};

        std::string body = call.serializeAction();
        std::string msg =
            "execute|" + std::to_string(mid.v) + "|" + call.modelTypeId + "|" + call.actionTypeId + "|" + body;
        auto deser = std::move(call.deserializeResult);

        _server.handle(std::move(msg), [state, deser = std::move(deser)](const std::string& reply) mutable {
            try {
                if (reply.starts_with("ok|")) {
                    state->setValue(deser(reply.substr(3)));
                } else if (reply.starts_with("err|")) {
                    throw std::runtime_error(reply.substr(4));
                } else {
                    throw std::runtime_error("malformed reply: " + reply);
                }
            } catch (...) {
                state->setException(std::current_exception());
            }
        });
        return comp;
    }

    /// @brief No-op — models live in `RemoteServer`, not locally.
    void notifyBackendChanged() override {}

private:
    RemoteServer& _server;
};

}  // namespace morph::backend

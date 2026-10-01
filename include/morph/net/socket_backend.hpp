// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IConnector.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/ThreadedAddressResolver.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/detail/owner_probe.hpp>
#include <morph/core/detail/reply_router.hpp>
#include <morph/core/io_loop.hpp>
#include <morph/core/logger.hpp>
#include <morph/core/wire.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../attributes.hpp"
#include "detail/ws_connection.hpp"
#include "detail/ws_frame.hpp"
#include "detail/ws_handshake.hpp"

namespace morph::net {

/// @brief Reconnect tuning for `SocketBackend`.
struct SocketBackendConfig {
    /// @brief Whether to attempt automatic reconnect after an unsolicited disconnect.
    bool reconnectEnabled = true;
    /// @brief Delay before the first reconnect attempt after a disconnect.
    std::chrono::milliseconds initialReconnectDelay{500};
    /// @brief Upper bound on the exponential backoff between reconnect attempts.
    std::chrono::milliseconds maxReconnectDelay{30000};
    /// @brief Multiplier applied to the delay after each failed attempt.
    double backoffMultiplier = 2.0;
    /// @brief Maximum time for the whole connect: name resolution and every
    ///        resolved address.
    std::chrono::milliseconds connectTimeout{5000};
    /// @brief Bound on writing one chunk (up to 64 KiB) of an outgoing frame.
    ///
    /// A loop timer armed around each chunk's write closes the connection when
    /// it runs out, so a peer that stops reading ends in a disconnect (and the
    /// usual reconnect) rather than in a queue of unsent frames that grows
    /// forever. Generous on purpose: it bounds a write making no progress, not
    /// a slow one. Zero disables it.
    std::chrono::milliseconds sendTimeout{30000};
    /// @brief Bound on the whole handshake response read that follows a
    ///        successful TCP connect, from its first byte to the header's end.
    ///
    /// A peer that accepts the TCP connection and then never completes the
    /// WebSocket upgrade — a TCP load balancer connecting lazily to its
    /// backend would — is dropped after this long, and counts as a failed
    /// connect. Zero disables it.
    std::chrono::milliseconds handshakeTimeout{10000};
};

/// @brief `IBackend` implementation that communicates with a `RemoteServer`
///        over a raw-socket RFC 6455 WebSocket connection — no Qt, no GUI
///        event loop required.
///
/// Mirrors `morph::qt::QtWebSocketBackend`'s observable behavior
/// (`deregisterModel` fire-and-forget, `execute` and `bindModel` asynchronous
/// and callId-multiplexed, `DisconnectedError`/reconnect semantics). Its
/// socket, its pending calls, its session, its reconnect handler and its
/// reconnect state machine live on an `exec::IoLoop`, which it shares with
/// every other socket, timer and probe the application builds on that loop.
///
/// Cross-thread surface: `execute`, `bindModel`, `promoteModel`,
/// `deregisterModel`, `cancelPending`, `setSession` and `setReconnectHandler`
/// post to the loop and return, so calls from one thread take effect in the
/// order they were made; `waitForConnected` posts a waiter and blocks the
/// caller, never the loop; the synchronous control verbs (`registerModel`,
/// `registerModelWithContext`, `assignPrimary`, `listInstances`) issue the same
/// posted request and wait for its reply, never on the loop's own thread.
/// Every field is touched only on the loop, except the `connected` flag.
///
/// @par TLS
/// Not supported. `serverUrl` must be `ws://`; `wss://` throws from the
/// constructor. See `docs/spec/core/backend.md`'s `morph::net` section.
class SocketBackend : public ::morph::backend::detail::IBackend {
public:
    /// @brief Alias for the reconnect configuration struct.
    using Config = SocketBackendConfig;

    /// @brief Parses @p serverUrl and starts connecting on @p loop.
    /// @param loop      The application's I/O loop. Borrowed: it must outlive
    ///                  this backend.
    /// @param serverUrl `ws://host:port[/path]` URL of the remote `RemoteServer`.
    /// @param cfg       Reconnect tuning. Default: enabled, 500ms initial / 30s cap, 2x backoff.
    /// @throws std::runtime_error immediately if @p serverUrl is not a
    ///         well-formed `ws://` URL (see `parseWsUrl`).
    SocketBackend(::morph::exec::IoLoop& loop MORPH_LIFETIMEBOUND, std::string_view serverUrl, Config cfg = {})
        : _url{::morph::net::detail::parseWsUrl(serverUrl)},
          _loop{&loop},
          _core{std::make_shared<Core>(loop, _url, cfg)} {
        start();
    }

    /// @brief Parses @p serverUrl and starts connecting on a loop of its own,
    ///        for a caller with no `IoLoop` to share.
    /// @param serverUrl `ws://host:port[/path]` URL of the remote `RemoteServer`.
    /// @param cfg       Reconnect tuning. Default: enabled, 500ms initial / 30s cap, 2x backoff.
    /// @throws std::runtime_error immediately, before any loop is started, if
    ///         @p serverUrl is not a well-formed `ws://` URL (see `parseWsUrl`).
    explicit SocketBackend(std::string_view serverUrl, Config cfg = {})
        : _url{::morph::net::detail::parseWsUrl(serverUrl)},
          _ownedLoop{std::make_unique<::morph::exec::IoLoop>()},
          _loop{_ownedLoop.get()},
          _core{std::make_shared<Core>(*_ownedLoop, _url, cfg)} {
        start();
    }

    SocketBackend(const SocketBackend&) = delete;
    SocketBackend& operator=(const SocketBackend&) = delete;
    SocketBackend(SocketBackend&&) = delete;
    SocketBackend& operator=(SocketBackend&&) = delete;

    /// @brief Closes the connection and rejects every pending call with
    ///        `DisconnectedError`.
    ///
    /// The close runs on the loop: inline when this runs on the loop's own
    /// thread (from a callback), posted and waited for otherwise. It never
    /// waits for a connect in progress: a dial that completes afterwards finds
    /// the backend closed and drops its socket.
    ~SocketBackend() override {
        _loop->runAndWait([core = _core] { core->close(); });
    }

    /// @brief Blocks the calling thread until connected or @p timeout elapses.
    ///
    /// Posts a waiter to the loop, which releases it when a connection
    /// completes. On the loop's own thread it cannot wait for the loop, so it
    /// answers at once.
    ///
    /// @warning The backend must outlive this call. See
    /// `docs/spec/core/backend.md`'s "Lifetime & ownership" section.
    /// @param timeout Maximum time to wait.
    /// @return `true` if connected before the timeout, `false` otherwise.
    bool waitForConnected(std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) {
        if (_core->connected.load() || _loop->runningHere()) {
            return _core->connected.load();
        }
        auto waiter = std::make_shared<std::promise<void>>();
        std::future<void> const released = waiter->get_future();
        _loop->post([core = _core, waiter] { core->addConnectWaiter(waiter); });
        static_cast<void>(released.wait_for(timeout));
        return _core->connected.load();
    }

    /// @brief Registers a private instance and waits for the reply.
    ///
    /// The same request `bindModel` posts, waited for on the calling thread —
    /// which must not be the I/O loop's, whose reply this would wait on. The
    /// factory argument is ignored — model construction is delegated to the
    /// server.
    /// @param typeId  String type-id of the model to register.
    /// @param factory Ignored — the server constructs via its own registry.
    /// @return `ModelId` assigned by the server.
    /// @throws std::runtime_error if the server replies with an error, the
    ///         socket is not connected, or this is the loop's own thread.
    ::morph::exec::detail::ModelId registerModel(
        const std::string& typeId,
        std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory) override {
        return registerModelWithContext(typeId, std::move(factory), {});
    }

    /// @brief Registers a private instance carrying @p contextKey and waits for the reply.
    ///
    /// The server constructs the holder itself, so `contextKey` is the only
    /// channel by which the instance's identity reaches it:
    /// `RemoteServer::attachLogIfConfigured` consults its `LogProvider` only
    /// for a non-empty one. Same waiting rule as `registerModel`.
    /// @param typeId     String type-id of the model to register.
    /// @param factory    Ignored — the server constructs via its own registry.
    /// @param contextKey Stable identity of the new instance; empty if none.
    /// @return `ModelId` assigned by the server.
    /// @throws std::runtime_error if the server replies with an error, the
    ///         socket is not connected, or this is the loop's own thread.
    ::morph::exec::detail::ModelId registerModelWithContext(
        const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
        std::string_view contextKey) override {
        return awaitReply<::morph::exec::detail::ModelId>("register", [&](::morph::exec::IExecutor& waiter) {
            return bindModel(::morph::backend::detail::BindRequest{.typeId = typeId,
                                                                   .factory = std::move(factory),
                                                                   .contextKey = std::string{contextKey},
                                                                   .primary = {},
                                                                   .current = {}},
                             waiter);
        });
    }

    /// @brief Files a live server-side instance under @p primary and waits for the reply.
    /// @param mid     Live instance to promote.
    /// @param typeId  Model type id.
    /// @param primary Canonical string encoding of the key to file it under.
    void assignPrimary(::morph::exec::detail::ModelId mid, const std::string& typeId,
                       std::string_view primary) override {
        if (primary.empty() || mid.v == 0U) {
            return;
        }
        static_cast<void>(awaitReply<::morph::exec::detail::ModelId>("assign", [&](::morph::exec::IExecutor& waiter) {
            return promoteModel(
                ::morph::backend::detail::PromoteRequest{
                    .mid = mid, .typeId = typeId, .primary = std::string{primary}},
                waiter);
        }));
    }

    // ── The structural registration surface ──────────────────────────────
    //
    // Overridden natively rather than reached through
    // `backend::SynchronousBackendAdapter`: this transport already
    // demultiplexes replies by `callId` on its I/O loop for `execute`, and a
    // control call is the same shape. See docs/spec/core/backend.md,
    // "The structural registration surface, natively".

    /// @brief Acquires a model instance without blocking the calling thread.
    ///
    /// Sends the control envelope @p request's shape names (see
    /// `backend::detail::BindRequest`'s table) carrying a non-zero `callId`
    /// drawn from the same counter `execute` uses, and settles the returned
    /// `Completion` on the I/O loop when the matching reply arrives. No thread
    /// is parked anywhere.
    ///
    /// An empty `primary` with a live `current` gives that instance up first,
    /// and an empty `primary` binds a private instance.
    ///
    /// @param request Owning bind request; moved from.
    /// @param cbExec  Executor the continuation is delivered on. Borrowed: it
    ///                must outlive the returned `Completion`.
    /// @return A `Completion` resolved with the bound `ModelId`; rejected with
    ///         `backend::DisconnectedError` if the socket is down or drops
    ///         before the reply, or with a `std::runtime_error` carrying the
    ///         server's own error message.
    ::morph::async::Completion<::morph::exec::detail::ModelId> bindModel(::morph::backend::detail::BindRequest request,
                                                                         ::morph::exec::IExecutor& cbExec) override {
        if (request.primary.empty() && request.current.v != 0U) {
            // An empty primary with a live instance: the instance being given up
            // is released before the private bind that replaces it.
            deregisterModel(request.current);
        }
        if (request.primary.empty()) {
            return sendControlAsync(::morph::wire::makeRegister(request.typeId, request.contextKey), "register",
                                    std::nullopt, cbExec);
        }
        if (request.current.v != 0U) {
            return sendControlAsync(
                ::morph::wire::makeAttach(request.typeId, request.primary, request.current.v, request.contextKey),
                "attach", std::nullopt, cbExec);
        }
        return sendControlAsync(::morph::wire::makeRegisterShared(request.typeId, request.primary, request.contextKey),
                                "register", std::nullopt, cbExec);
    }

    /// @brief Files an already-live server-side instance under a key, without
    ///        blocking the calling thread.
    ///
    /// The promote counterpart of `bindModel`, on the same callId-multiplexed
    /// path. Resolves with `request.mid` echoed back exactly as
    /// `IBackend::promoteModel` documents — including for the two guards
    /// `assignPrimary` applies locally (empty `primary`, zero `mid`), which
    /// resolve without sending anything.
    ///
    /// @param request Owning promote request; moved from.
    /// @param cbExec  Executor the continuation is delivered on. Borrowed: it
    ///                must outlive the returned `Completion`.
    /// @return A `Completion` resolved with `request.mid`, or rejected as
    ///         `bindModel` documents.
    ::morph::async::Completion<::morph::exec::detail::ModelId> promoteModel(
        ::morph::backend::detail::PromoteRequest request, ::morph::exec::IExecutor& cbExec) override {
        if (request.primary.empty() || request.mid.v == 0U) {
            auto state = std::make_shared<::morph::async::detail::CompletionState<::morph::exec::detail::ModelId>>();
            ::morph::async::Completion<::morph::exec::detail::ModelId> comp{state, &cbExec};
            state->setValue(request.mid);
            return comp;
        }
        return sendControlAsync(::morph::wire::makeAssign(request.typeId, request.primary, request.mid.v), "assign",
                                request.mid, cbExec);
    }

    /// @brief Asks the server for the live shared primary keys of @p typeId
    ///        and waits for the reply, never on the loop's own thread.
    /// @param typeId String type-id to enumerate.
    /// @return Canonical key strings of the live shared instances.
    /// @throws std::runtime_error if the server replies with an error, the
    ///         socket is down, or this is the loop's own thread.
    std::vector<std::string> listInstances(const std::string& typeId) override {
        auto body = awaitReply<std::string>("instances", [&](::morph::exec::IExecutor& waiter) {
            auto state = std::make_shared<::morph::async::detail::CompletionState<std::string>>();
            ::morph::async::Completion<std::string> comp{state, &waiter};
            _loop->post([core = _core, env = ::morph::wire::makeInstances(typeId),
                         pending = PendingControl{
                             .state = nullptr, .what = "instances", .echo = std::nullopt, .body = state}]() mutable {
                core->fileControl(std::move(env), std::move(pending));
            });
            return comp;
        });
        std::vector<std::string> keys;
        if (auto errCode = glz::read_json(keys, body)) {
            throw std::runtime_error("instances decode failed: " + glz::format_error(errCode, body));
        }
        return keys;
    }

    /// @brief Sends a `deregister` message fire-and-forget (does not wait for a reply).
    ///
    /// Posted to the loop, which gives it a real, non-zero `callId` drawn from
    /// the same counter `execute()` uses and stamps the session.
    ///
    /// The reply router drops any `callId` that is not pending, and a
    /// `deregister` files none, so its reply is dropped.
    ///
    /// @param mid Id of the model to remove on the server.
    void deregisterModel(::morph::exec::detail::ModelId mid) override {
        if (!_core->connected.load()) {
            return;
        }
        _loop->post([core = _core, env = ::morph::wire::makeDeregister(mid.v)]() mutable {
            core->sendDeregister(std::move(env));
        });
    }

    /// @brief Sends an `execute` message and returns a `Completion` resolved on reply.
    ///
    /// The action is serialised here, on the calling thread; the loop then
    /// assigns the call id, files the pending record and writes the frame.
    /// @param mid    Target model id on the server.
    /// @param call   Bundled action; `serializeAction` and `deserializeResult` are used.
    /// @param cbExec Executor for delivering the completion callbacks.
    /// @return Completion resolved asynchronously when the server's reply arrives,
    ///         or with `DisconnectedError` if the backend is not connected.
    ::morph::async::Completion<std::shared_ptr<void>> execute(::morph::exec::detail::ModelId mid,
                                                              ::morph::backend::detail::ActionCall call,
                                                              ::morph::exec::IExecutor* cbExec) override {
        auto compState = std::make_shared<::morph::async::detail::CompletionState<std::shared_ptr<void>>>();
        ::morph::async::Completion<std::shared_ptr<void>> comp{compState, cbExec};
        if (!_core->connected.load()) {
            // Answered here rather than on the loop, which would answer the
            // same: the loop re-checks, so a connection lost after this read
            // is caught there.
            compState->setException(std::make_exception_ptr(::morph::backend::DisconnectedError{}));
            return comp;
        }

        ::morph::wire::Envelope env;
        env.kind = "execute";
        env.modelId = mid.v;
        env.modelType = call.modelTypeId;
        env.actionType = call.actionTypeId;
        env.body = call.serializeBody();
        env.session = std::move(call.session);

        _loop->post([core = _core, env,
                     pending = PendingExecute{.state = compState, .deserialize = call.deserializeResult}]() mutable {
            core->fileExecute(std::move(env), std::move(pending));
        });
        return comp;
    }

    /// @brief No-op — this backend holds no local model objects.
    void notifyBackendChanged() override {}

    /// @brief Resolves every pending call's `Completion` with @p exc.
    ///
    /// Posted to the loop, so it covers every call issued before it — the loop
    /// runs posts in order. Covers both in-flight tables: the `execute` calls
    /// and the `bindModel`/`promoteModel` control calls. A bind left out of
    /// this sweep would hang forever on a disconnect, since its reply can now
    /// only arrive on a connection that is gone.
    /// @param exc Exception delivered to every pending completion's error sink.
    void cancelPending(const std::exception_ptr& exc) override {
        _loop->post([core = _core, exc] {
            ::morph::exec::detail::noteOwner("SocketBackend::cancelPending", core->loop.loop(),
                                             core->loop.runningHere());
            core->cancelAll(exc);
        });
    }

    /// @brief Installs the handler posted to @p exec after each *subsequent*
    ///        successful (re)connect.
    ///
    /// Stored on the loop. After a reconnect the loop posts the handler to
    /// @p exec and goes on: whatever the handler does — a `Bridge`
    /// re-registering its bindings through `bindModel` — never runs on the loop
    /// whose replies it needs.
    /// @param handler Callable posted to @p exec. Pass `nullptr` to clear.
    /// @param exec    Executor the handler runs on. Borrowed: it must outlive
    ///                the installation.
    void setReconnectHandler(std::function<void()> handler, ::morph::exec::IExecutor* exec) override {
        _loop->post([core = _core, handler = std::move(handler), exec]() mutable {
            core->note("SocketBackend::setReconnectHandler");
            core->reconnectHandler = std::move(handler);
            core->reconnectExec = exec;
        });
    }

    /// @brief Installs the session stamped onto every control envelope this
    ///        backend subsequently builds (`register`, `registerShared`,
    ///        `attach`, `assign`, `deregister`, `instances`). See
    ///        `IBackend::setSession`.
    ///
    /// Stored on the loop, which stamps every control envelope as it files
    /// it: a verb called after this, from the same thread, carries it.
    /// @param session Session to stamp; typically pushed by `Bridge::setDefaultSession()`.
    void setSession(::morph::session::Context session) override {
        _loop->post([core = _core, session = std::move(session)]() mutable {
            core->note("SocketBackend::setSession");
            core->session = std::move(session);
        });
    }

private:
    /// @brief Waits on the calling thread for the reply to the request @p ask
    ///        sends, settled on the loop.
    ///
    /// The answer's owner is an executor local to this call, which the
    /// calling thread pumps while it waits: the attach and the delivery are
    /// both tasks of it, so the waiting thread is the one place they run.
    /// @tparam T   The answer's type.
    /// @tparam Ask Callable taking the waiter (`morph::exec::IExecutor&`) and
    ///         returning a `Completion<T>` delivered on it.
    /// @param what Verb name for the error message.
    /// @param ask  Sends the request, naming the waiter as the answer's owner.
    /// @return The settled value.
    /// @throws std::runtime_error on the loop's own thread, when the loop has
    ///         stopped, on a disconnect, or with the server's refusal.
    template <typename T, typename Ask>
    T awaitReply(std::string_view what, Ask ask) {
        if (_loop->runningHere()) {
            throw std::runtime_error(std::string{what} +
                                     " failed: a synchronous call cannot wait on the I/O loop's own thread");
        }
        ::morph::exec::MainThreadExecutor waiter;
        auto completion = ask(static_cast<::morph::exec::IExecutor&>(waiter));
        std::optional<T> value;
        std::exception_ptr error;
        bool done = false;
        waiter.post([&completion, &value, &error, &done] {
            completion
                .then([&value, &done](const T& settled) {
                    value.emplace(settled);
                    done = true;
                })
                .onError([&error, &done](const std::exception_ptr& failure) {
                    error = failure;
                    done = true;
                });
        });
        // The request was posted to the loop before this round trip, so a
        // loop that runs it has filed the request too, and will answer it or
        // reject it when it closes. A stopped loop drops both unrun.
        bool alive = false;
        _loop->runAndWait([&alive] { alive = true; });
        if (!alive) {
            throw std::runtime_error(std::string{what} + " failed: the I/O loop has stopped");
        }
        // runFor pumps until its deadline even once `done` is set, so it is
        // only the wait between replies, kept short.
        while (!done) {
            if (!waiter.runOnce()) {
                waiter.runFor(std::chrono::milliseconds{1});
            }
        }
        if (error) {
            try {
                std::rethrow_exception(error);
            } catch (const ::morph::backend::DisconnectedError&) {
                throw std::runtime_error(std::string{what} + " failed: disconnected");
            }
        }
        return std::move(*value);
    }

    struct PendingExecute {
        std::shared_ptr<::morph::async::detail::CompletionState<std::shared_ptr<void>>> state;
        std::shared_ptr<void> (*deserialize)(std::string_view json){nullptr};
    };

    /// @brief One in-flight control call issued through `bindModel`/`promoteModel`.
    ///
    /// Kept in its own table rather than beside the executes: an execute reply
    /// settles a `Completion<shared_ptr<void>>` through a deserializer, a
    /// control reply settles a `Completion<ModelId>` and has none. The two
    /// tables share one `callId` counter (the execute table's), so an id is
    /// never ambiguous between them.
    struct PendingControl {
        /// @brief State of the `Completion<ModelId>` this call settles, or
        ///        null for a call that settles `body` instead.
        std::shared_ptr<::morph::async::detail::CompletionState<::morph::exec::detail::ModelId>> state;
        /// @brief Verb name prefixing the server's error message:
        ///        `"<what> failed: ..."`.
        std::string what;
        /// @brief Id to resolve with, for `promoteModel`, which echoes its
        ///        request's `mid`. `nullopt` means "resolve with the reply's".
        std::optional<::morph::exec::detail::ModelId> echo;
        /// @brief State of a call answered with the reply's body
        ///        (`instances`), or null.
        std::shared_ptr<::morph::async::detail::CompletionState<std::string>> body;

        /// @brief Rejects whichever state this call settles.
        /// @param failure The failure.
        void reject(const std::exception_ptr& failure) const {
            if (state) {
                state->setException(failure);
            }
            if (body) {
                body->setException(failure);
            }
        }
    };

    /// @brief Posts one control envelope to the loop's callId-multiplexed
    ///        path and returns the `Completion` its reply will settle.
    /// @param env    Envelope to send; its `callId` and `session` are filled in
    ///               on the loop.
    /// @param what   Verb name for the error message.
    /// @param echo   Id to resolve with, or `nullopt` to use the reply's `modelId`.
    /// @param cbExec Executor the continuation is delivered on.
    /// @return The `Completion` the reply — or a disconnect — settles.
    ::morph::async::Completion<::morph::exec::detail::ModelId> sendControlAsync(
        ::morph::wire::Envelope env, std::string_view what, std::optional<::morph::exec::detail::ModelId> echo,
        ::morph::exec::IExecutor& cbExec) {
        auto state = std::make_shared<::morph::async::detail::CompletionState<::morph::exec::detail::ModelId>>();
        ::morph::async::Completion<::morph::exec::detail::ModelId> comp{state, &cbExec};
        _loop->post([core = _core, env,
                     pending = PendingControl{
                         .state = state, .what = std::string{what}, .echo = echo, .body = nullptr}]() mutable {
            core->fileControl(std::move(env), std::move(pending));
        });
        return comp;
    }

    /// @brief Settles one control call from its matched reply envelope.
    /// @param pending Entry taken out of the control table.
    /// @param reply   Decoded reply carrying the same `callId`.
    static void settleControl(const PendingControl& pending, const ::morph::wire::Envelope& reply) {
        if (reply.kind != "ok") {
            pending.reject(std::make_exception_ptr(std::runtime_error(pending.what + " failed: " + reply.message)));
            return;
        }
        if (pending.state) {
            pending.state->setValue(pending.echo.value_or(::morph::exec::detail::ModelId{reply.modelId}));
        }
        if (pending.body) {
            pending.body->setValue(reply.body);
        }
    }

    void start() {
        _loop->post([core = _core] { core->startAttempt(); });
    }

    /// @brief Everything the I/O loop owns, touched only in its tasks.
    ///
    /// Held by `shared_ptr` from the backend and from every flow and task on
    /// the loop, so a flow that resumes after the backend is gone finds it
    /// closed and ends. It never reaches the backend object itself.
    struct Core : std::enable_shared_from_this<Core> {
        Core(::morph::exec::IoLoop& ioLoop, ::morph::net::detail::ParsedWsUrl target, Config config)
            : loop{ioLoop}, url{std::move(target)}, cfg{config}, reconnectDelay{config.initialReconnectDelay} {}

        /// Owner check for every loop-side body.
        void note(char const* site) const noexcept {
            ::morph::exec::detail::noteOwner(site, loop.loop(), loop.runningHere());
        }

        void startAttempt() {
            if (!closed) {
                loop.loop().spawn(attemptFlow(shared_from_this()));
            }
        }

        /// Queues @p frame on the live connection.
        /// @return `false` when there is none, or it is closing.
        bool send(std::string frame) {
            note("SocketBackend::send");
            if (!conn) {
                return false;
            }
            return ::morph::net::detail::enqueueFrame(conn, std::move(frame));
        }

        /// Assigns @p env a call id, files @p pending under it and writes it.
        void fileExecute(::morph::wire::Envelope env, PendingExecute pending) {
            note("SocketBackend::execute");
            if (!connected.load() || !conn) {
                pending.state->setException(std::make_exception_ptr(::morph::backend::DisconnectedError{}));
                return;
            }
            std::uint64_t const callId = executes.nextCallId();
            env.callId = callId;
            std::string frame;
            try {
                frame = ::morph::net::detail::encodeWsFrame(::morph::net::detail::WsOpcode::kText,
                                                            ::morph::wire::encode(env), /*mask=*/true);
            } catch (...) {
                pending.state->setException(std::current_exception());
                return;
            }
            executes.insert(callId, std::move(pending));
            // A refused frame is left filed: the connection is closing, and
            // the disconnect that follows sweeps it with `DisconnectedError`.
            static_cast<void>(send(std::move(frame)));
        }

        /// The control-call counterpart of `fileExecute`.
        void fileControl(::morph::wire::Envelope env, PendingControl pending) {
            note("SocketBackend::bindModel");
            if (!connected.load() || !conn) {
                pending.reject(std::make_exception_ptr(::morph::backend::DisconnectedError{}));
                return;
            }
            std::uint64_t const callId = executes.nextCallId();
            env.callId = callId;
            env.session = session;
            std::string frame;
            try {
                frame = ::morph::net::detail::encodeWsFrame(::morph::net::detail::WsOpcode::kText,
                                                            ::morph::wire::encode(env), /*mask=*/true);
            } catch (const std::exception& exc) {
                pending.reject(std::make_exception_ptr(std::runtime_error(pending.what + " failed: " + exc.what())));
                return;
            }
            controls.insert(callId, std::move(pending));
            static_cast<void>(send(std::move(frame)));
        }

        void sendDeregister(::morph::wire::Envelope env) {
            note("SocketBackend::deregisterModel");
            if (!connected.load()) {
                return;
            }
            env.callId = executes.nextCallId();
            env.session = session;
            try {
                static_cast<void>(send(::morph::net::detail::encodeWsFrame(
                    ::morph::net::detail::WsOpcode::kText, ::morph::wire::encode(env), /*mask=*/true)));
            } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
                // Fire-and-forget: same documented trade-off as
                // QtWebSocketBackend — a failed send just leaks the model on
                // the server (see docs/spec/core/backend.md's Limitations).
            }
        }

        void addConnectWaiter(const std::shared_ptr<std::promise<void>>& waiter) {
            note("SocketBackend::waitForConnected");
            if (connected.load()) {
                waiter->set_value();
                return;
            }
            if (!closed) {
                connectWaiters.push_back(waiter);
            }
        }

        /// Settles every pending call, of both kinds, with @p exc. The tables
        /// are emptied first, so a settle that re-enters the backend files
        /// into empty tables.
        void cancelAll(const std::exception_ptr& exc) {
            auto drained = executes.drain();
            auto drainedControl = controls.drain();
            for (auto& entry : drained) {
                if (entry.second.state) {
                    entry.second.state->setException(exc);
                }
            }
            for (auto& entry : drainedControl) {
                entry.second.reject(exc);
            }
        }

        void onConnected() {
            bool const isReconnect = everConnected;
            everConnected = true;
            connected.store(true);
            reconnectDelay = cfg.initialReconnectDelay;
            for (auto const& waiter : std::exchange(connectWaiters, {})) {
                waiter->set_value();
            }
            if (isReconnect && reconnectHandler && reconnectExec != nullptr) {
                // Posted, never run here: the handler re-registers through
                // `bindModel`, whose replies this loop delivers.
                reconnectExec->post(reconnectHandler);
            }
        }

        void onDisconnected() {
            connected.store(false);
            if (conn) {
                ::morph::net::detail::closeAfterFlush(conn);
                conn.reset();
            }
            cancelAll(std::make_exception_ptr(::morph::backend::DisconnectedError{}));
        }

        /// Arms the backoff timer for the next attempt, and grows the delay.
        void scheduleReconnect() {
            note("SocketBackend::reconnect");
            backoffTimer = loop.loop().addTimer(loop.loop().clock().now() + reconnectDelay, &Core::onBackoff, this);
            auto const nextDelayMs = static_cast<std::chrono::milliseconds::rep>(
                static_cast<double>(reconnectDelay.count()) * cfg.backoffMultiplier);
            reconnectDelay = std::min(std::chrono::milliseconds{nextDelayMs}, cfg.maxReconnectDelay);
        }

        /// The backoff timer's callback. `close()` retires the timer, so the
        /// core it points at is alive whenever it runs.
        static void onBackoff(void* corePtr) {
            auto& self = *static_cast<Core*>(corePtr);
            self.backoffTimer = {};
            self.startAttempt();
        }

        /// Ends everything: the connection, the backoff, the waiters, every
        /// pending call. Idempotent. After it, no flow touches the backend.
        void close() {
            note("SocketBackend::close");
            if (closed) {
                return;
            }
            closed = true;
            connected.store(false);
            static_cast<void>(loop.loop().cancelTimer(backoffTimer));
            if (conn) {
                ::morph::net::detail::closeConnection(*conn);
                conn.reset();
            }
            connectWaiters.clear();
            reconnectHandler = nullptr;
            cancelAll(std::make_exception_ptr(::morph::backend::DisconnectedError{}));
        }

        void dispatchIncomingEnvelope(const std::string& payload) {
            ::morph::wire::Envelope env;
            try {
                env = ::morph::wire::decode(payload);
            } catch (const std::exception&) {
                // The callId is unreadable, so this reply cannot be matched to
                // the call it belongs to. Every message
                // here is required to be one envelope, so an undecodable one
                // means the peer's framing is no longer trustworthy: fail the
                // pending calls rather than wait on a stream that may never
                // produce a matching reply. Mirrors
                // QtWebSocketBackend::onTextMessage.
                cancelAll(std::make_exception_ptr(
                    std::runtime_error("protocol error: server sent a message that is not a valid envelope")));
                return;
            }
            note("SocketBackend::reply");
            auto pending = executes.take(env.callId);
            if (!pending) {
                // Not an execute: it may be a control reply for a `bindModel`/
                // `promoteModel` in flight, which shares this id space.
                if (auto control = controls.take(env.callId)) {
                    settleControl(*control, env);
                }
                return;  // otherwise a late/cancelled reply, dropped silently
            }
            // Triage shared with SimulatedRemoteBackend and QtWebSocketBackend
            // (core/detail/reply_router.hpp), so the three cannot drift. The
            // Timeout arm is the server's own `LimitPolicy::executeTimeout`
            // reply, not a generic application error -- surfaced as the same
            // TimeoutError type the other two backends give callers for this
            // case.
            switch (::morph::backend::detail::classifyExecuteReply(env)) {
                case ::morph::backend::detail::ExecuteReplyKind::Value:
                    try {
                        if (pending->deserialize == nullptr) {
                            throw std::runtime_error{"ActionCall::deserializeResult is null"};
                        }
                        pending->state->setValue(pending->deserialize(env.body));
                    } catch (...) {
                        pending->state->setException(std::current_exception());
                    }
                    break;
                case ::morph::backend::detail::ExecuteReplyKind::Timeout:
                    pending->state->setException(std::make_exception_ptr(::morph::backend::TimeoutError{}));
                    break;
                case ::morph::backend::detail::ExecuteReplyKind::Error:
                default:
                    // `default:` only because the project builds with
                    // -Wswitch-default; ExecuteReplyKind is a closed enum and
                    // all three enumerators are handled explicitly.
                    pending->state->setException(std::make_exception_ptr(std::runtime_error(env.message)));
                    break;
            }
        }

        /// Handles every complete frame @p reader holds.
        /// @return `false` when the connection should stop reading (peer sent
        ///         Close, a protocol error, or this backend closed meanwhile).
        bool drainFrames(const std::shared_ptr<::morph::net::detail::LoopConnection>& connection,
                         ::morph::net::detail::WsFrameReader& reader) {
            using ::morph::net::detail::WsOpcode;
            for (;;) {
                std::optional<::morph::net::detail::WsFrame> frame;
                try {
                    frame = reader.tryExtractFrame();
                } catch (const std::exception&) {
                    return false;
                }
                if (!frame) {
                    return true;
                }
                if (frame->opcode == WsOpcode::kClose) {
                    static_cast<void>(::morph::net::detail::enqueueFrame(
                        connection, ::morph::net::detail::encodeWsFrame(WsOpcode::kClose, frame->payload, true)));
                    return false;
                }
                if (frame->opcode == WsOpcode::kPing) {
                    static_cast<void>(::morph::net::detail::enqueueFrame(
                        connection, ::morph::net::detail::encodeWsFrame(WsOpcode::kPong, frame->payload, true)));
                    continue;
                }
                if (frame->opcode == WsOpcode::kText) {
                    dispatchIncomingEnvelope(frame->payload);
                    // A settle may have run user code that destroyed the
                    // backend, closing this core.
                    if (closed) {
                        return false;
                    }
                }
            }
        }

        /// One connection attempt, start to end: dial, handshake, read until
        /// the connection ends, then decide whether to try again.
        static ::core::async::Task<void> attemptFlow(std::shared_ptr<Core> self) {
            bool reachedServer = false;
            {
                ::core::net::DialOptions options;
                options.connectTimeout = self->cfg.connectTimeout;
                auto dialed = co_await ::core::net::connect(&self->loop.loop(), self->url.host, self->url.port,
                                                            &::core::net::defaultAsyncResolver(), options);
                if (self->closed) {
                    co_return;
                }
                if (dialed) {
                    auto connection = std::make_shared<::morph::net::detail::LoopConnection>(
                        self->loop.loop(), std::move(*dialed), self->cfg.sendTimeout);
                    self->conn = connection;
                    std::string const key = ::morph::net::detail::generateClientKey();
                    static_cast<void>(::morph::net::detail::enqueueFrame(
                        connection, ::morph::net::detail::buildClientHandshakeRequest(self->url, key)));
                    auto header =
                        co_await ::morph::net::detail::readHeaderBlockAsync(connection, self->cfg.handshakeTimeout);
                    if (self->closed) {
                        co_return;
                    }
                    bool verified = false;
                    if (header) {
                        try {
                            ::morph::net::detail::verifyServerHandshakeResponse(header->header, key);
                            verified = true;
                        } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
                            // A failed connect, like any other: see below.
                        }
                    }
                    if (verified) {
                        reachedServer = true;
                        self->onConnected();
                        // Client role: RFC 6455 §5.1 forbids a server from
                        // masking the frames it sends, so this reader must
                        // reject a masked one.
                        ::morph::net::detail::WsFrameReader reader{/*expectMasked=*/false};
                        reader.feed(header->leftover);
                        std::array<std::byte, 4096> buf{};
                        while (self->drainFrames(connection, reader) && !connection->closed) {
                            auto const got = co_await connection->socket->read(buf);
                            if (self->closed || !got || *got == 0) {
                                break;
                            }
                            reader.feed(std::string_view{reinterpret_cast<char const*>(buf.data()), *got});
                        }
                    }
                    if (self->closed) {
                        co_return;
                    }
                }
            }
            self->onDisconnected();
            if (!reachedServer && !self->everConnected) {
                // Never reached the server even once — fail fast, no retry
                // (mirrors QtWebSocketBackend's "no reconnect for
                // never-connected sockets").
                co_return;
            }
            if (self->cfg.reconnectEnabled) {
                self->scheduleReconnect();
            }
        }

        ::morph::exec::IoLoop& loop;
        ::morph::net::detail::ParsedWsUrl url;
        Config cfg;
        /// The one field read off the loop: by `execute`'s fast path and
        /// `waitForConnected`.
        std::atomic<bool> connected{false};
        /// Stamped onto every control envelope as it is filed.
        ::morph::session::Context session;
        /// Posted to `reconnectExec` after every reconnect; cleared by `close()`.
        std::function<void()> reconnectHandler;
        ::morph::exec::IExecutor* reconnectExec{nullptr};
        bool everConnected{false};
        bool closed{false};
        std::chrono::milliseconds reconnectDelay;
        ::core::net::TimerId backoffTimer{};
        std::shared_ptr<::morph::net::detail::LoopConnection> conn;
        ::morph::backend::detail::PendingCallTable<PendingExecute> executes;
        // Control calls issued through the structural surface. Draws its ids
        // from `executes`' counter rather than owning a second one, so the two
        // tables can never claim the same id.
        ::morph::backend::detail::PendingCallTable<PendingControl> controls;
        std::vector<std::shared_ptr<std::promise<void>>> connectWaiters;
    };

    /// Parsed before anything else is built, so a bad URL throws before a
    /// loop of the backend's own is started.
    ::morph::net::detail::ParsedWsUrl _url;
    /// Present only for the owning constructor; destroyed after everything
    /// below, once the destructor has closed the core on it.
    std::unique_ptr<::morph::exec::IoLoop> _ownedLoop;
    ::morph::exec::IoLoop* _loop;
    std::shared_ptr<Core> _core;
};

}  // namespace morph::net

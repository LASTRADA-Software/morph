// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
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
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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
/// (`registerModel` synchronous, `deregisterModel` fire-and-forget, `execute`
/// asynchronous and callId-multiplexed, `DisconnectedError`/reconnect
/// semantics). Its socket, its pending calls and its reconnect state machine
/// live on an `exec::IoLoop`, which it shares with every other socket, timer
/// and probe the application builds on that loop.
///
/// Cross-thread surface: `execute`, `bindModel`, `promoteModel`,
/// `deregisterModel` and `cancelPending` post to the loop and return;
/// `waitForConnected` posts a waiter and blocks the caller, never the loop;
/// the synchronous control verbs (`registerModel` and its siblings,
/// `assignPrimary`, `listInstances`) post their request and park the caller
/// until the loop hands the reply over; `setSession` and
/// `setReconnectHandler` store under their own locks. Every other field is
/// touched only on the loop.
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
          _core{std::make_shared<Core>(loop, _url, cfg, *this)} {
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
          _core{std::make_shared<Core>(*_ownedLoop, _url, cfg, *this)} {
        start();
    }

    SocketBackend(const SocketBackend&) = delete;
    SocketBackend& operator=(const SocketBackend&) = delete;
    SocketBackend(SocketBackend&&) = delete;
    SocketBackend& operator=(SocketBackend&&) = delete;

    /// @brief Closes the connection, rejects every pending call with
    ///        `DisconnectedError`, and stops the handler thread.
    ///
    /// The close runs on the loop: inline when this runs on the loop's own
    /// thread (from a callback), posted and waited for otherwise. It never
    /// waits for a connect in progress: a dial that completes afterwards finds
    /// the backend closed and drops its socket.
    ~SocketBackend() override {
        _shuttingDown.store(true);
        _loop->runAndWait([core = _core] { core->close(); });
        // After the close, not before: a reconnect handler parked in sendSync
        // is released by the close's `_syncCv` notify. Waking `_handlerCv`
        // first would not free it -- that wait is on `_syncCv`.
        {
            std::scoped_lock const lock{_handlerMtx};
            _handlerPending = false;
        }
        _handlerCv.notify_all();
        if (_handlerThread.joinable()) {
            _handlerThread.join();
        }
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

    /// @brief Sends a `register` message and blocks until the reply arrives.
    ///
    /// Callable from any thread but the I/O loop's, which would be blocked on a
    /// reply only it can deliver. Only one synchronous control call
    /// (`registerModel`) may be in flight at a time across the whole backend; a
    /// second call while one is outstanding throws immediately rather than
    /// queuing. The factory
    /// argument is ignored — model construction is delegated to the server.
    /// @param typeId  String type-id of the model to register.
    /// @param factory Ignored — the server constructs via its own registry.
    /// @return `ModelId` assigned by the server.
    /// @throws std::runtime_error if the server replies with an error, the
    ///         socket is not connected, or a synchronous call is already in flight.
    ::morph::exec::detail::ModelId registerModel(
        const std::string& typeId,
        std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory) override {
        return registerModelWithContext(typeId, std::move(factory), {});
    }

    /// @brief Sends a `register` message carrying @p contextKey and blocks for the reply.
    ///
    /// `IBackend::registerModelWithContext`'s default drops @p contextKey, which
    /// is right for `LocalBackend` — the caller's own factory closure already
    /// captures the identity — but wrong for a backend whose instances live on
    /// the far side of a wire protocol: the server constructs the holder itself,
    /// so `contextKey` is the *only* channel by which the instance's identity
    /// reaches it. `RemoteServer::attachLogIfConfigured` returns without
    /// consulting its `LogProvider` at all when the envelope's `contextKey` is
    /// empty, so dropping it here does not merely lose an entity key — it leaves
    /// the instance unjournalled. `SimulatedRemoteBackend` overrides
    /// this for the same reason; the two must not disagree.
    ///
    /// Same synchronous-call constraint as `registerModel`. The factory argument
    /// is ignored — model construction is delegated to the server.
    /// @param typeId     String type-id of the model to register.
    /// @param contextKey Stable identity of the new instance; empty if none.
    /// @return `ModelId` assigned by the server.
    /// @throws std::runtime_error if the server replies with an error, the
    ///         socket is not connected, or a synchronous call is already in flight.
    ::morph::exec::detail::ModelId registerModelWithContext(
        const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> /*factory*/,
        std::string_view contextKey) override {
        auto env = ::morph::wire::makeRegister(typeId, std::string{contextKey});
        env.session = currentSession();
        return sendControlForId(env, "register");
    }

    /// @brief Sends a shared (register-or-attach) `register` and blocks for the reply.
    ///
    /// An empty primary degrades to the private path. Same synchronous-call
    /// constraint as `registerModel`.
    /// @param typeId   String type-id of the model.
    /// @param factory  Ignored — the server constructs via its own registry.
    /// @param identity Entity key for the action log plus the directory primary key.
    /// @return `ModelId` of the shared (or newly created) instance.
    /// @throws std::runtime_error if the server replies with an error or the socket is down.
    ::morph::exec::detail::ModelId registerModelShared(
        const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
        ::morph::backend::detail::InstanceIdentity identity) override {
        if (identity.primary.empty()) {
            return registerModelWithContext(typeId, std::move(factory), identity.contextKey);
        }
        auto env =
            ::morph::wire::makeRegisterShared(typeId, std::string{identity.primary}, std::string{identity.contextKey});
        env.session = currentSession();
        return sendControlForId(env, "register");
    }

    /// @brief Sends an `attach` and blocks for the reply, re-pointing from @p current.
    /// @param typeId   String type-id of the model.
    /// @param factory  Ignored — the server constructs via its own registry.
    /// @param identity Entity key for the action log plus the directory primary key.
    /// @param current  Instance currently held, or `ModelId{0}` if none.
    /// @return `ModelId` of the instance now attached to.
    /// @throws std::runtime_error if the server replies with an error or the socket is down.
    ::morph::exec::detail::ModelId attachModel(
        const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
        ::morph::backend::detail::InstanceIdentity identity, ::morph::exec::detail::ModelId current) override {
        if (identity.primary.empty()) {
            if (current.v != 0U) {
                deregisterModel(current);
            }
            return registerModelWithContext(typeId, std::move(factory), identity.contextKey);
        }
        auto env = ::morph::wire::makeAttach(typeId, std::string{identity.primary}, current.v,
                                             std::string{identity.contextKey});
        env.session = currentSession();
        return sendControlForId(env, "attach");
    }

    /// @brief Files a live server-side instance under @p primary.
    /// @param mid     Live instance to promote.
    /// @param typeId  Model type id.
    /// @param primary Canonical string encoding of the key to file it under.
    void assignPrimary(::morph::exec::detail::ModelId mid, const std::string& typeId,
                       std::string_view primary) override {
        if (primary.empty() || mid.v == 0U) {
            return;
        }
        auto env = ::morph::wire::makeAssign(typeId, std::string{primary}, mid.v);
        env.session = currentSession();
        (void)sendControlForId(env, "assign");
    }

    // ── The structural registration surface ──────────────────────────────
    //
    // Overridden natively rather than reached through
    // `backend::SynchronousBackendAdapter`. The reasoning is recorded in
    // docs/spec/core/backend.md (`SocketBackend`'s section, "The structural
    // registration surface, natively"); the short form is that this transport
    // already demultiplexes replies by `callId` on its I/O loop for
    // `execute`, and a control call is the same shape. Running the *blocking*
    // verb on a wrapper's strand would park a thread for a round trip this
    // transport need not park for, and would keep every bind inside
    // `sendSync`'s one-synchronous-call-at-a-time token — which a concurrent
    // `listInstances` or legacy `registerModel` on another thread shares.
    //
    // The synchronous verbs are unaffected: `registerModel`,
    // `registerModelShared`, `attachModel` and `assignPrimary` use `sendSync`
    // and `callId == 0`.

    /// @brief Acquires a model instance without blocking the calling thread.
    ///
    /// Sends the control envelope @p request's shape names (see
    /// `backend::detail::BindRequest`'s table) carrying a non-zero `callId`
    /// drawn from the same counter `execute` uses, and settles the returned
    /// `Completion` on the I/O loop when the matching reply arrives. No thread
    /// is parked anywhere: in particular this never enters `sendSync`, so a
    /// bind neither waits on `_syncCv` nor takes the one-synchronous-call
    /// token, and therefore cannot wait on the loop that would satisfy it.
    ///
    /// The two degradations the legacy verbs perform are preserved exactly: an
    /// empty `primary` with a live `current` gives that instance up first, and
    /// an empty `primary` binds a private instance.
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
            // `attachModel`'s empty-primary branch: the instance being given up
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

    /// @brief Asks the server for the live shared primary keys of @p typeId.
    /// @param typeId String type-id to enumerate.
    /// @return Canonical key strings of the live shared instances.
    /// @throws std::runtime_error if the server replies with an error or the socket is down.
    std::vector<std::string> listInstances(const std::string& typeId) override {
        std::string replyJson;
        try {
            auto env = ::morph::wire::makeInstances(typeId);
            env.session = currentSession();
            replyJson = sendSync(::morph::wire::encode(env));
        } catch (const std::exception& exc) {
            throw std::runtime_error(std::string{"instances failed: "} + exc.what());
        }
        auto reply = ::morph::wire::decode(replyJson);
        if (reply.kind != "ok") {
            throw std::runtime_error("instances failed: " + reply.message);
        }
        std::vector<std::string> keys;
        if (auto errCode = glz::read_json(keys, reply.body)) {
            throw std::runtime_error("instances decode failed: " + glz::format_error(errCode, reply.body));
        }
        return keys;
    }

    /// @brief Sends a `deregister` message fire-and-forget (does not wait for a reply).
    ///
    /// Posted to the loop, which gives it a real, non-zero `callId` drawn from
    /// the same counter `execute()` uses, exactly as `QtWebSocketBackend` does:
    /// `callId == 0` is the discriminator for "hand this payload to whichever
    /// `sendSync()` is parked", so a fire-and-forget `deregister` sharing that
    /// sentinel would have its own stray `ok` reply delivered to an unrelated
    /// `register`/`attach` waiting on `_syncCv` whenever the two landed back to
    /// back on one connection.
    ///
    /// Unlike `QtWebSocketBackend` this needs no `_pendingDeregisters` set to
    /// recognise the reply and drop it: the reply router already drops any
    /// non-zero `callId` that is not pending, and a `deregister` files none.
    ///
    /// @param mid Id of the model to remove on the server.
    void deregisterModel(::morph::exec::detail::ModelId mid) override {
        if (!_core->connected.load()) {
            return;
        }
        auto env = ::morph::wire::makeDeregister(mid.v);
        env.session = currentSession();
        _loop->post([core = _core, env]() mutable { core->sendDeregister(std::move(env)); });
    }

    /// @brief Sends one synchronous control envelope and returns the replied `modelId`.
    /// @param env  Envelope to send.
    /// @param what Verb name used in the error message.
    /// @return `ModelId` carried by the `ok` reply.
    /// @throws std::runtime_error if the server errors or the socket is down.
    ::morph::exec::detail::ModelId sendControlForId(const ::morph::wire::Envelope& env, std::string_view what) {
        std::string replyJson;
        try {
            replyJson = sendSync(::morph::wire::encode(env));
        } catch (const std::exception& exc) {
            throw std::runtime_error(std::string{what} + " failed: " + exc.what());
        }
        auto reply = ::morph::wire::decode(replyJson);
        if (reply.kind == "ok") {
            return ::morph::exec::detail::ModelId{reply.modelId};
        }
        throw std::runtime_error(std::string{what} + " failed: " + reply.message);
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

    /// @brief Whether a caller may wait for a bind to settle.
    ///
    /// The loop settles every bind, so a caller anywhere else may wait for
    /// one. A caller on the loop's own thread — a timer or a monitor callback
    /// sharing it — would be waiting on itself, and is told not to.
    /// @return `kCallerMustNotBlock` on the loop's thread, `kCallerMayBlock`
    ///         elsewhere.
    [[nodiscard]] ::morph::backend::detail::BindWait bindWaitPolicy() const noexcept override {
        return _loop->runningHere() ? ::morph::backend::detail::BindWait::kCallerMustNotBlock
                                    : ::morph::backend::detail::BindWait::kCallerMayBlock;
    }

    /// @brief Resolves every pending call's `Completion` with @p exc.
    ///
    /// Posted to the loop, so it covers every call issued before it — the loop
    /// runs posts in order. Covers both in-flight tables: the `execute` calls
    /// and the `bindModel`/`promoteModel` control calls. A bind left out of
    /// this sweep would hang forever on a disconnect, since its reply can now
    /// only arrive on a connection that is gone — the async counterpart of
    /// `sendSync`'s disconnect wake-up.
    /// @param exc Exception delivered to every pending completion's error sink.
    void cancelPending(const std::exception_ptr& exc) override {
        _loop->post([core = _core, exc] {
            ::morph::exec::detail::noteOwner("SocketBackend::cancelPending", core->loop.loop(),
                                             core->loop.runningHere());
            core->cancelAll(exc);
        });
    }

    /// @brief Installs the handler invoked after each *subsequent* successful (re)connect.
    /// @param handler Callable invoked on this backend's dedicated handler
    ///                thread — deliberately not the I/O loop, see
    ///                `Core::onConnected`. Pass `nullptr` to clear.
    void setReconnectHandler(const std::function<void()>& handler) override {
        std::scoped_lock lock{_reconnectHandlerMtx};
        _reconnectHandler = handler;
    }

    /// @brief Installs the session stamped onto every control envelope this
    ///        backend subsequently builds (`register`, `registerShared`,
    ///        `attach`, `assign`, `deregister`). See `IBackend::setSession`.
    /// @param session Session to stamp; typically pushed by `Bridge::setDefaultSession()`.
    void setSession(::morph::session::Context session) override {
        std::scoped_lock lock{_sessionMtx};
        _session = std::move(session);
    }

private:
    /// @brief Returns a copy of the session last installed via `setSession`.
    [[nodiscard]] ::morph::session::Context currentSession() const {
        std::scoped_lock lock{_sessionMtx};
        return _session;
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
        /// @brief State of the `Completion<ModelId>` this call settles.
        std::shared_ptr<::morph::async::detail::CompletionState<::morph::exec::detail::ModelId>> state;
        /// @brief Verb name prefixing the server's error message, matching the
        ///        legacy verbs' `"<what> failed: ..."` wording.
        std::string what;
        /// @brief Id to resolve with, for `promoteModel`, which echoes its
        ///        request's `mid`. `nullopt` means "resolve with the reply's".
        std::optional<::morph::exec::detail::ModelId> echo;
    };

    /// @brief Posts one control envelope to the loop's callId-multiplexed
    ///        path and returns the `Completion` its reply will settle.
    /// @param env    Envelope to send; its `callId` is assigned on the loop and
    ///               its `session` filled in here.
    /// @param what   Verb name for the error message.
    /// @param echo   Id to resolve with, or `nullopt` to use the reply's `modelId`.
    /// @param cbExec Executor the continuation is delivered on.
    /// @return The `Completion` the reply — or a disconnect — settles.
    ::morph::async::Completion<::morph::exec::detail::ModelId> sendControlAsync(
        ::morph::wire::Envelope env, std::string_view what, std::optional<::morph::exec::detail::ModelId> echo,
        ::morph::exec::IExecutor& cbExec) {
        auto state = std::make_shared<::morph::async::detail::CompletionState<::morph::exec::detail::ModelId>>();
        ::morph::async::Completion<::morph::exec::detail::ModelId> comp{state, &cbExec};
        env.session = currentSession();
        _loop->post([core = _core, env,
                     pending = PendingControl{.state = state, .what = std::string{what}, .echo = echo}]() mutable {
            core->fileControl(std::move(env), std::move(pending));
        });
        return comp;
    }

    /// @brief Settles one control call from its matched reply envelope.
    /// @param pending Entry taken out of the control table.
    /// @param reply   Decoded reply carrying the same `callId`.
    static void settleControl(const PendingControl& pending, const ::morph::wire::Envelope& reply) {
        if (!pending.state) {
            return;
        }
        if (reply.kind == "ok") {
            pending.state->setValue(pending.echo.value_or(::morph::exec::detail::ModelId{reply.modelId}));
            return;
        }
        pending.state->setException(
            std::make_exception_ptr(std::runtime_error(pending.what + " failed: " + reply.message)));
    }

    std::string sendSync(const std::string& payload) {
        if (_loop->runningHere()) {
            // The reply can only be read by the loop this would block.
            throw std::runtime_error("sendSync: a synchronous call cannot wait on the I/O loop's own thread");
        }
        std::unique_lock lock{_syncMtx};
        if (_syncInFlight) {
            throw std::runtime_error("sendSync: a synchronous call is already in flight (reentrant use)");
        }
        if (!_core->connected.load()) {
            throw std::runtime_error("disconnected");
        }
        _syncInFlight = true;
        _syncReply.reset();
        lock.unlock();

        _loop->post([core = _core, frame = ::morph::net::detail::encodeWsFrame(::morph::net::detail::WsOpcode::kText,
                                                                               payload, /*mask=*/true)]() mutable {
            ::morph::exec::detail::noteOwner("SocketBackend::sendSync", core->loop.loop(), core->loop.runningHere());
            // A refused frame needs no answer here: it is refused only because
            // the connection is closing, and the disconnect that follows wakes
            // this call with "disconnected".
            static_cast<void>(core->send(std::move(frame)));
        });

        std::unique_lock waitLock{_syncMtx};
        _syncCv.wait(waitLock, [this] { return _syncReply.has_value() || !_core->connected.load(); });
        bool const gotReply = _syncReply.has_value();
        std::string result = gotReply ? std::move(*_syncReply) : std::string{};
        _syncReply.reset();
        _syncInFlight = false;
        if (!gotReply) {
            throw std::runtime_error("disconnected");
        }
        return result;
    }

    /// Serializes reconnect-handler invocations off the I/O loop. Coalescing
    /// via a flag (rather than queuing every request) is deliberate: if a second
    /// reconnect lands while a handler is still running, re-running it once
    /// afterwards is the correct catch-up, and it bounds concurrent handler runs
    /// to one.
    void handlerThreadMain() {
        for (;;) {
            std::function<void()> handler;
            {
                std::unique_lock lock{_handlerMtx};
                _handlerCv.wait(lock, [this] { return _handlerPending || _shuttingDown.load(); });
                if (_shuttingDown.load()) {
                    return;
                }
                _handlerPending = false;
            }
            {
                std::scoped_lock const lock{_reconnectHandlerMtx};
                handler = _reconnectHandler;
            }
            if (!handler) {
                continue;
            }
            try {
                handler();
            } catch (const std::exception& exc) {
                // A handler that throws (typically because the link dropped
                // again mid-re-registration, surfacing as "disconnected") must
                // not take this thread down: the next reconnect has to find it
                // still waiting.
                ::morph::log::logWarn(std::string{"[net::SocketBackend] reconnect handler threw: "} + exc.what());
            } catch (...) {
                ::morph::log::logWarn("[net::SocketBackend] reconnect handler threw a non-std exception");
            }
        }
    }

    void start() {
        _handlerThread = std::thread{[this] { handlerThreadMain(); }};
        _loop->post([core = _core] { core->startAttempt(); });
    }

    /// @brief Everything the I/O loop owns, touched only in its tasks.
    ///
    /// Held by `shared_ptr` from the backend and from every flow and task on
    /// the loop, so a flow that resumes after the backend is gone finds it
    /// closed and ends. It reaches the backend itself — the synchronous-call and
    /// handler-thread state — only through `owner`, which `close()` clears.
    struct Core : std::enable_shared_from_this<Core> {
        Core(::morph::exec::IoLoop& ioLoop, ::morph::net::detail::ParsedWsUrl target, Config config,
             SocketBackend& backend)
            : loop{ioLoop},
              url{std::move(target)},
              cfg{config},
              owner{&backend},
              reconnectDelay{config.initialReconnectDelay} {}

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
                pending.state->setException(std::make_exception_ptr(::morph::backend::DisconnectedError{}));
                return;
            }
            std::uint64_t const callId = executes.nextCallId();
            env.callId = callId;
            std::string frame;
            try {
                frame = ::morph::net::detail::encodeWsFrame(::morph::net::detail::WsOpcode::kText,
                                                            ::morph::wire::encode(env), /*mask=*/true);
            } catch (const std::exception& exc) {
                pending.state->setException(
                    std::make_exception_ptr(std::runtime_error(pending.what + " failed: " + exc.what())));
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
                if (entry.second.state) {
                    entry.second.state->setException(exc);
                }
            }
        }

        /// Hands @p payload to a parked synchronous call, if there is one.
        /// @return Whether one took it.
        bool handToSyncCall(std::string const& payload) {
            if (owner == nullptr) {
                return false;
            }
            std::scoped_lock const lock{owner->_syncMtx};
            if (!owner->_syncInFlight) {
                return false;
            }
            owner->_syncReply = payload;
            owner->_syncCv.notify_all();
            return true;
        }

        /// Wakes a parked synchronous call to observe the disconnect.
        void wakeSyncCall() {
            if (owner == nullptr) {
                return;
            }
            {
                std::scoped_lock const lock{owner->_syncMtx};
                owner->_syncReply.reset();
            }
            owner->_syncCv.notify_all();
        }

        void onConnected() {
            bool const isReconnect = everConnected;
            everConnected = true;
            connected.store(true);
            reconnectDelay = cfg.initialReconnectDelay;
            for (auto const& waiter : std::exchange(connectWaiters, {})) {
                waiter->set_value();
            }
            if (isReconnect && owner != nullptr) {
                // Handed to the handler thread rather than run here. A
                // reconnect handler is expected to re-register its models
                // (Bridge::installReconnectHandler does exactly that), which
                // goes through sendSync -> wait on _syncCv for a reply that only
                // this loop can deliver. Run on the loop, that wait blocks the
                // one thread responsible for satisfying it.
                std::scoped_lock const lock{owner->_handlerMtx};
                owner->_handlerPending = true;
                owner->_handlerCv.notify_all();
            }
        }

        void onDisconnected() {
            connected.store(false);
            if (conn) {
                ::morph::net::detail::closeAfterFlush(conn);
                conn.reset();
            }
            wakeSyncCall();
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
            wakeSyncCall();
            owner = nullptr;
            cancelAll(std::make_exception_ptr(::morph::backend::DisconnectedError{}));
        }

        void dispatchIncomingEnvelope(const std::string& payload) {
            ::morph::wire::Envelope env;
            try {
                env = ::morph::wire::decode(payload);
            } catch (const std::exception&) {
                // Hand the raw text to a parked synchronous call so it can
                // report something better than "disconnected".
                if (handToSyncCall(payload)) {
                    return;
                }
                // No sync waiter, and the callId is unreadable, so this reply
                // cannot be matched to the execute it belongs to. Every message
                // here is required to be one envelope, so an undecodable one
                // means the peer's framing is no longer trustworthy: fail the
                // pending calls rather than wait on a stream that may never
                // produce a matching reply. Mirrors
                // QtWebSocketBackend::onTextMessage.
                cancelAll(std::make_exception_ptr(
                    std::runtime_error("protocol error: server sent a message that is not a valid envelope")));
                return;
            }
            if (env.callId == 0U) {
                static_cast<void>(handToSyncCall(payload));
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
        /// The backend, until `close()`.
        SocketBackend* owner;
        /// The one field read off the loop: by `execute`'s fast path,
        /// `waitForConnected` and `sendSync`.
        std::atomic<bool> connected{false};
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
    std::atomic<bool> _shuttingDown{false};

    std::mutex _syncMtx;
    std::condition_variable _syncCv;
    bool _syncInFlight{false};
    std::optional<std::string> _syncReply;

    std::mutex _reconnectHandlerMtx;
    std::function<void()> _reconnectHandler;

    mutable std::mutex _sessionMtx;
    ::morph::session::Context _session;

    std::mutex _handlerMtx;
    std::condition_variable _handlerCv;
    bool _handlerPending{false};

    // Declared last: the constructor starts this thread after every other
    // member above is fully constructed, so its body never observes a
    // partially-constructed `this`.
    std::thread _handlerThread;
};

}  // namespace morph::net

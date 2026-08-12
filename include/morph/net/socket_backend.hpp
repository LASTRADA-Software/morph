// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <morph/core/backend.hpp>
#include <morph/core/logger.hpp>
#include <morph/core/wire.hpp>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>

#include "detail/tcp_socket.hpp"
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
    /// @brief Maximum time to wait for the initial TCP connect to complete.
    std::chrono::milliseconds connectTimeout{5000};
};

/// @brief `IBackend` implementation that communicates with a `RemoteServer`
///        over a raw-socket RFC 6455 WebSocket connection — no Qt, no GUI
///        event loop required.
///
/// Mirrors `morph::qt::QtWebSocketBackend`'s observable behavior
/// (`registerModel` synchronous, `deregisterModel` fire-and-forget, `execute`
/// asynchronous and callId-multiplexed, `DisconnectedError`/reconnect
/// semantics) but runs its own dedicated I/O thread and uses
/// `std::condition_variable` instead of a nested Qt event loop. Unlike
/// `QtWebSocketBackend`, `SocketBackend` may safely be driven from multiple
/// threads concurrently (`registerModel`/`execute`/`deregisterModel` are all
/// thread-safe) — there is no single "owning" event-loop thread.
///
/// @par TLS
/// Not supported. `serverUrl` must be `ws://`; `wss://` throws from the
/// constructor. See `docs/spec/core/backend.md`'s `morph::net` section.
class SocketBackend : public ::morph::backend::detail::IBackend {
public:
    /// @brief Alias for the reconnect configuration struct.
    using Config = SocketBackendConfig;

    /// @brief Parses @p serverUrl and starts the background I/O thread, which
    ///        connects asynchronously.
    /// @param serverUrl `ws://host:port[/path]` URL of the remote `RemoteServer`.
    /// @param cfg       Reconnect tuning. Default: enabled, 500ms initial / 30s cap, 2x backoff.
    /// @throws std::runtime_error immediately (before starting the I/O thread)
    ///         if @p serverUrl is not a well-formed `ws://` URL (see `parseWsUrl`).
    // Copy/move are implicitly deleted by the non-copyable/non-movable
    // std::mutex/std::condition_variable/std::thread members below — no
    // explicit `= delete` needed (matches the rest of the codebase's
    // convention, e.g. `LocalBackend`).
    explicit SocketBackend(std::string serverUrl, Config cfg = {})
        : _url{::morph::net::detail::parseWsUrl(serverUrl)},
          _cfg{cfg},
          _currentReconnectDelay{cfg.initialReconnectDelay} {
        _ioThread = std::thread{[this] { ioThreadMain(); }};
        _handlerThread = std::thread{[this] { handlerThreadMain(); }};
    }

    /// @brief Shuts down the I/O thread and resolves any still-pending completions.
    ///
    /// @note May block up to `Config::connectTimeout` if destruction races an
    /// in-flight (re)connect attempt — the TCP connect phase is bounded by
    /// that timeout, but the handshake read that follows a successful TCP
    /// connect has no separate timeout of its own in this reference
    /// implementation. See `docs/spec/core/backend.md`'s `morph::net` section.
    ~SocketBackend() override {
        _shuttingDown.store(true);
        {
            std::scoped_lock lock{_socketMtx};
            if (_socket.valid()) {
                _socket.shutdownBoth();
            }
        }
        _reconnectCv.notify_all();
        if (_ioThread.joinable()) {
            _ioThread.join();
        }
        // Joined after _ioThread, not before: a reconnect handler parked in
        // sendSync is released by onDisconnected()'s _syncCv notify, which only
        // runs as ioThreadMain unwinds. Waking _handlerCv first would not free
        // it -- that wait is on _syncCv.
        {
            std::scoped_lock const lock{_handlerMtx};
            _handlerPending = false;
        }
        _handlerCv.notify_all();
        if (_handlerThread.joinable()) {
            _handlerThread.join();
        }
        cancelPending(std::make_exception_ptr(::morph::backend::DisconnectedError{}));
    }

    /// @brief Blocks the calling thread until connected or @p timeout elapses.
    /// @param timeout Maximum time to wait.
    /// @return `true` if connected before the timeout, `false` otherwise.
    bool waitForConnected(std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) {
        std::unique_lock lock{_connectMtx};
        _connectCv.wait_for(lock, timeout, [this] { return _connected.load() || _shuttingDown.load(); });
        return _connected.load();
    }

    /// @brief Sends a `register` message and blocks until the reply arrives.
    ///
    /// Thread-safe, but only one synchronous control call (`registerModel`) may
    /// be in flight at a time across the whole backend; a second call while one
    /// is outstanding throws immediately rather than queuing. The factory
    /// argument is ignored — model construction is delegated to the server.
    /// @param typeId  String type-id of the model to register.
    /// @return `ModelId` assigned by the server.
    /// @throws std::runtime_error if the server replies with an error, the
    ///         socket is not connected, or a synchronous call is already in flight.
    ::morph::exec::detail::ModelId registerModel(
        const std::string& typeId,
        std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> /*factory*/) override {
        auto env = ::morph::wire::makeRegister(typeId);
        env.session = currentSession();
        std::string replyJson;
        try {
            replyJson = sendSync(::morph::wire::encode(env));
        } catch (const std::exception& exc) {
            throw std::runtime_error(std::string{"register failed: "} + exc.what());
        }
        auto reply = ::morph::wire::decode(replyJson);
        if (reply.kind == "ok") {
            return ::morph::exec::detail::ModelId{reply.modelId};
        }
        throw std::runtime_error("register failed: " + reply.message);
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
        auto env = ::morph::wire::makeRegisterShared(typeId, std::string{identity.primary},
                                                       std::string{identity.contextKey});
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

    /// @brief Asks the server for the live shared primary keys of @p typeId.
    /// @param typeId String type-id to enumerate.
    /// @return Canonical key strings of the live shared instances.
    /// @throws std::runtime_error if the server replies with an error or the socket is down.
    std::vector<std::string> listInstances(const std::string& typeId) override {
        std::string replyJson;
        try {
            replyJson = sendSync(::morph::wire::encode(::morph::wire::makeInstances(typeId)));
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
    /// @param mid Id of the model to remove on the server.
    void deregisterModel(::morph::exec::detail::ModelId mid) override {
        if (_connected.load()) {
            try {
                auto env = ::morph::wire::makeDeregister(mid.v);
                env.session = currentSession();
                sendFrame(::morph::net::detail::WsOpcode::kText, ::morph::wire::encode(env));
            } catch (const std::exception&) {
                // Fire-and-forget: same documented trade-off as
                // QtWebSocketBackend — a failed send just leaks the model on
                // the server (see docs/spec/core/backend.md's Limitations).
            }
        }
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
    /// @param mid    Target model id on the server.
    /// @param call   Bundled action; `serializeAction` and `deserializeResult` are used.
    /// @param cbExec Executor for delivering the completion callbacks.
    /// @return Completion resolved asynchronously when the server's reply arrives,
    ///         or immediately with `DisconnectedError` if not connected.
    ::morph::async::Completion<std::shared_ptr<void>> execute(::morph::exec::detail::ModelId mid,
                                                              ::morph::backend::detail::ActionCall call,
                                                              ::morph::exec::IExecutor* cbExec) override {
        auto compState = std::make_shared<::morph::async::detail::CompletionState<std::shared_ptr<void>>>();
        ::morph::async::Completion<std::shared_ptr<void>> comp{compState, cbExec};

        if (!_connected.load()) {
            compState->setException(std::make_exception_ptr(::morph::backend::DisconnectedError{}));
            return comp;
        }

        std::uint64_t const callId = ++_nextCallId;
        ::morph::wire::Envelope env;
        env.kind = "execute";
        env.callId = callId;
        env.modelId = mid.v;
        env.modelType = call.modelTypeId;
        env.actionType = call.actionTypeId;
        env.body = call.serializeAction();
        env.session = std::move(call.session);

        {
            std::scoped_lock lock{_pendingMtx};
            _pending[callId] = PendingExecute{compState, std::move(call.deserializeResult), cbExec};
        }

        try {
            sendFrame(::morph::net::detail::WsOpcode::kText, ::morph::wire::encode(env));
        } catch (const std::exception&) {
            // The write raced a disconnect; the io thread's disconnect
            // handler drains _pending (including this entry) via cancelPending.
        }
        return comp;
    }

    /// @brief No-op — this backend holds no local model objects.
    void notifyBackendChanged() override {}

    /// @brief Resolves every pending execute call's `Completion` with @p exc.
    /// @param exc Exception delivered to every pending completion's error sink.
    void cancelPending(const std::exception_ptr& exc) override {
        std::unordered_map<std::uint64_t, PendingExecute> drained;
        {
            std::scoped_lock lock{_pendingMtx};
            drained.swap(_pending);
        }
        for (auto& [callId, pending] : drained) {
            (void)callId;
            if (pending.state) {
                pending.state->setException(exc);
            }
        }
    }

    /// @brief Installs the handler invoked after each *subsequent* successful (re)connect.
    /// @param handler Callable invoked on the I/O thread. Pass `nullptr` to clear.
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
        std::function<std::shared_ptr<void>(std::string_view)> deserialize;
        ::morph::exec::IExecutor* cbExec{nullptr};
    };

    void sendFrame(::morph::net::detail::WsOpcode opcode, std::string_view payload) {
        std::scoped_lock lock{_socketMtx};
        if (!_socket.valid()) {
            throw std::runtime_error("SocketBackend::sendFrame: not connected");
        }
        std::string frame = ::morph::net::detail::encodeWsFrame(opcode, payload, /*mask=*/true);
        _socket.sendAll(frame.data(), frame.size());
    }

    std::string sendSync(const std::string& payload) {
        std::unique_lock lock{_syncMtx};
        if (_syncInFlight) {
            throw std::runtime_error("sendSync: a synchronous call is already in flight (reentrant use)");
        }
        if (!_connected.load()) {
            throw std::runtime_error("disconnected");
        }
        _syncInFlight = true;
        _syncReply.reset();
        lock.unlock();

        try {
            sendFrame(::morph::net::detail::WsOpcode::kText, payload);
        } catch (const std::exception&) {
            std::scoped_lock relock{_syncMtx};
            _syncInFlight = false;
            throw std::runtime_error("disconnected");
        }

        std::unique_lock waitLock{_syncMtx};
        _syncCv.wait(waitLock, [this] { return _syncReply.has_value() || !_connected.load(); });
        bool const gotReply = _syncReply.has_value();
        std::string result = gotReply ? std::move(*_syncReply) : std::string{};
        _syncReply.reset();
        _syncInFlight = false;
        if (!gotReply) {
            throw std::runtime_error("disconnected");
        }
        return result;
    }

    void onConnected() {
        bool const isReconnect = _everConnected.exchange(true);
        _connected.store(true);
        _currentReconnectDelay = _cfg.initialReconnectDelay;
        _connectCv.notify_all();
        if (isReconnect) {
            // Hand the callback to _handlerThread rather than running it here.
            // onConnected() is called from ioThreadMain() immediately *before*
            // readLoop() starts, and a reconnect handler is expected to
            // re-register its models (Bridge::installReconnectHandler does
            // exactly that), which goes through sendSync -> wait on _syncCv for
            // a reply that only readLoop can ever deliver. Run inline, that
            // wait blocks the one thread responsible for satisfying it: the
            // transport deadlocks permanently, with no timeout to break it.
            // Off-thread, the handler's sendSync overlaps readLoop as intended
            // -- its request may even reach the socket before readLoop starts,
            // which is harmless, since the reply simply waits in the kernel
            // buffer.
            std::scoped_lock const lock{_handlerMtx};
            _handlerPending = true;
            _handlerCv.notify_all();
        }
    }

    /// Serializes reconnect-handler invocations off the I/O thread. Coalescing
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

    void onDisconnected() {
        _connected.store(false);
        {
            std::scoped_lock lock{_socketMtx};
            _socket = ::morph::net::detail::TcpSocket{};
        }
        {
            std::scoped_lock lock{_syncMtx};
            _syncReply.reset();
        }
        _syncCv.notify_all();
        _connectCv.notify_all();
        cancelPending(std::make_exception_ptr(::morph::backend::DisconnectedError{}));
    }

    void dispatchIncomingEnvelope(const std::string& payload) {
        ::morph::wire::Envelope env;
        try {
            env = ::morph::wire::decode(payload);
        } catch (const std::exception&) {
            {
                std::scoped_lock const lock{_syncMtx};
                if (_syncInFlight) {
                    // Hand the raw text to the parked caller so it can report
                    // something better than "disconnected".
                    _syncReply = payload;
                    _syncCv.notify_all();
                    return;
                }
            }
            // No sync waiter, and the callId is unreadable, so this reply cannot
            // be matched to the execute it belongs to. Dropping it silently left
            // that execute's Completion unsettled forever. Every message here is
            // required to be one envelope, so an undecodable one means the
            // peer's framing is no longer trustworthy: fail the pending calls
            // rather than wait on a stream that may never produce a matching
            // reply. Mirrors QtWebSocketBackend::onTextMessage.
            cancelPending(std::make_exception_ptr(
                std::runtime_error("protocol error: server sent a message that is not a valid envelope")));
            return;
        }
        if (env.callId != 0U) {
            PendingExecute pending;
            {
                std::scoped_lock lock{_pendingMtx};
                auto iter = _pending.find(env.callId);
                if (iter == _pending.end()) {
                    return;  // late/cancelled reply — dropped silently
                }
                pending = std::move(iter->second);
                _pending.erase(iter);
            }
            if (env.kind == "ok") {
                try {
                    pending.state->setValue(pending.deserialize(env.body));
                } catch (...) {
                    pending.state->setException(std::current_exception());
                }
            } else {
                pending.state->setException(std::make_exception_ptr(std::runtime_error(env.message)));
            }
            return;
        }
        std::scoped_lock lock{_syncMtx};
        if (_syncInFlight) {
            _syncReply = payload;
            _syncCv.notify_all();
        }
    }

    // Returns false when the connection should stop reading (peer sent
    // Close, or a protocol error was detected).
    bool drainFrames(::morph::net::detail::WsFrameReader& reader) {
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
                try {
                    sendFrame(WsOpcode::kClose, "");
                } catch (const std::exception&) {
                }
                return false;
            }
            if (frame->opcode == WsOpcode::kPing) {
                try {
                    sendFrame(WsOpcode::kPong, frame->payload);
                } catch (const std::exception&) {
                }
                continue;
            }
            if (frame->opcode == WsOpcode::kPong) {
                continue;
            }
            if (frame->opcode == WsOpcode::kText) {
                dispatchIncomingEnvelope(frame->payload);
            }
        }
    }

    void readLoop(const std::string& leftover) {
        ::morph::net::detail::WsFrameReader reader;
        reader.feed(leftover);
        char buf[4096];
        for (;;) {
            if (!drainFrames(reader)) {
                return;
            }
            std::size_t got = 0;
            try {
                got = _socket.recvSome(buf, sizeof(buf));
            } catch (const std::exception&) {
                return;
            }
            if (got == 0) {
                return;
            }
            reader.feed(std::string_view{buf, got});
        }
    }

    void ioThreadMain() {
        while (!_shuttingDown.load()) {
            bool connectedOk = false;
            try {
                auto socket = ::morph::net::detail::TcpSocket::connect(_url.host, _url.port, _cfg.connectTimeout);
                std::string leftover = ::morph::net::detail::performClientHandshake(socket, _url);
                {
                    std::scoped_lock lock{_socketMtx};
                    _socket = std::move(socket);
                }
                connectedOk = true;
                onConnected();
                readLoop(leftover);
            } catch (const std::exception&) {
                // Falls through to the disconnect/reconnect handling below.
            }
            onDisconnected();
            if (_shuttingDown.load()) {
                return;
            }
            bool const wasEverConnected = _everConnected.load();
            if (!connectedOk && !wasEverConnected) {
                // Never reached the server even once — fail fast, no retry
                // (mirrors QtWebSocketBackend's "no reconnect for
                // never-connected sockets").
                return;
            }
            if (!_cfg.reconnectEnabled) {
                return;
            }
            std::unique_lock lock{_reconnectMtx};
            _reconnectCv.wait_for(lock, _currentReconnectDelay, [this] { return _shuttingDown.load(); });
            auto const nextDelayMs = static_cast<std::chrono::milliseconds::rep>(
                static_cast<double>(_currentReconnectDelay.count()) * _cfg.backoffMultiplier);
            _currentReconnectDelay = std::min(std::chrono::milliseconds{nextDelayMs}, _cfg.maxReconnectDelay);
        }
    }

    ::morph::net::detail::ParsedWsUrl _url;
    Config _cfg;
    std::atomic<bool> _shuttingDown{false};
    std::atomic<bool> _connected{false};
    std::atomic<bool> _everConnected{false};

    std::mutex _socketMtx;
    ::morph::net::detail::TcpSocket _socket;

    std::mutex _connectMtx;
    std::condition_variable _connectCv;

    std::mutex _reconnectMtx;
    std::condition_variable _reconnectCv;
    std::chrono::milliseconds _currentReconnectDelay;

    std::mutex _syncMtx;
    std::condition_variable _syncCv;
    bool _syncInFlight{false};
    std::optional<std::string> _syncReply;

    std::atomic<std::uint64_t> _nextCallId{0};
    std::mutex _pendingMtx;
    std::unordered_map<std::uint64_t, PendingExecute> _pending;

    std::mutex _reconnectHandlerMtx;
    std::function<void()> _reconnectHandler;

    mutable std::mutex _sessionMtx;
    ::morph::session::Context _session;

    std::mutex _handlerMtx;
    std::condition_variable _handlerCv;
    bool _handlerPending{false};

    // Declared last: the constructor starts these threads after every other
    // member above is fully constructed, so the thread bodies never observe a
    // partially-constructed `this`.
    std::thread _ioThread;
    std::thread _handlerThread;
};

}  // namespace morph::net

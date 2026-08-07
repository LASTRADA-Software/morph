// SPDX-License-Identifier: Apache-2.0

#include <QCoreApplication>
#include <QTimer>
#include <algorithm>
#include <cctype>
#include <morph/core/wire.hpp>
#include <morph/qt/qt_websocket_backend.hpp>
#include <stdexcept>
#include <utility>

namespace morph::qt {

QtWebSocketBackend::QtWebSocketBackend(QUrl serverUrl, ::morph::model::detail::ActionDispatcher& /*dispatcher*/,
                                       ::morph::model::detail::ModelRegistryFactory& /*registry*/,
#ifndef QT_NO_SSL
                                       std::optional<QSslConfiguration> tls,
#endif
                                       Config cfg)
    : _serverUrl{std::move(serverUrl)},
#ifndef QT_NO_SSL
      _tls{std::move(tls)},
#endif
      _cfg{cfg},
      _currentReconnectDelay{cfg.initialReconnectDelay} {
#ifndef QT_NO_SSL
    if (_tls.has_value()) {
        _socket.setSslConfiguration(*_tls);
    }
#endif
    _reconnectTimer.setSingleShot(true);
    QObject::connect(&_reconnectTimer, &QTimer::timeout, [this] { attemptReconnect(); });

    QObject::connect(&_socket, &QWebSocket::connected, [this]() {
        const bool isReconnect = _everConnected;
        _connected = true;
        _everConnected = true;
        _currentReconnectDelay = _cfg.initialReconnectDelay;
        if (_syncLoop) {
            _syncLoop->quit();
        }
        // Fires on every successful connect, first included -- the general
        // "transport is up" notification a status indicator wants.
        if (_connectHandler) {
            _connectHandler();
        }
        // Send every registerModelAsync request that arrived before this
        // connect (the first connect included) -- see issue #54. Runs before
        // the reconnect handler below so a caller that gates UI on
        // onRegistered sees it fire promptly on first connect too.
        flushQueuedRegistrations();
        // Fire the reconnect handler only on subsequent connects, never on the
        // first one — initial registration is handled by the BridgeHandler ctors.
        if (isReconnect && _reconnectHandler) {
            _reconnectHandler();
        }
    });
    QObject::connect(&_socket, &QWebSocket::disconnected, [this]() {
        _connected = false;
        // Fires before reconnect scheduling below, so an observer sees the
        // disconnected state even when a retry follows immediately.
        if (_disconnectHandler) {
            _disconnectHandler();
        }
        // Unblock any parked synchronous call (e.g. a register whose reply is
        // outstanding). Without this the nested QEventLoop in sendSync never
        // quits, freezing the Qt thread forever. We clear _pendingReply first so
        // the woken sendSync observes an empty reply and reports a failure rather
        // than acting on a stale one. See docs/spec/backend.md (QtWebSocketBackend).
        if (_syncLoop != nullptr) {
            _pendingReply.clear();
            _syncLoop->quit();
        }
        cancelPending(std::make_exception_ptr(::morph::backend::DisconnectedError{}));
        if (!_shuttingDown && _cfg.reconnectEnabled && _everConnected) {
            scheduleReconnect();
        }
    });
    QObject::connect(&_socket, &QWebSocket::textMessageReceived, [this](const QString& msg) { onTextMessage(msg); });

    _socket.open(_serverUrl);
}

QtWebSocketBackend::~QtWebSocketBackend() {  // NOLINT(modernize-use-equals-default)
    _shuttingDown = true;
    _reconnectTimer.stop();
    // Disconnect all signals first so no slot tries to access our members after they destruct.
    _socket.disconnect();
    // Abort cleanly: sends TCP RST without attempting close handshake.
    _socket.abort();
    // Safety net: if the owner did not run cancelPending() first (e.g. backend used
    // outside a Bridge, or destruction during stack unwinding), drain now.
    cancelPending(std::make_exception_ptr(::morph::backend::DisconnectedError{}));
    // Drain the event queue so Qt's internal WebSocket state machine fully settles
    // before _socket's QObject destructor runs its own cleanup.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents | QEventLoop::ExcludeSocketNotifiers);
}

bool QtWebSocketBackend::waitForConnected(int timeoutMs) {  // NOLINT(readability-make-member-function-const)
    if (_connected) {
        return true;
    }
    QEventLoop loop;
    _syncLoop = &loop;
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    _syncLoop = nullptr;
    return _connected;
}

std::string QtWebSocketBackend::sendSync(const std::string& msg) {
    // A synchronous send parks a nested event loop until the reply arrives. Only
    // one may be parked at a time: nested/reentrant sync calls would clobber the
    // single _syncLoop pointer and cross their replies. Callers (register) run on
    // the Qt thread and never re-enter sendSync from within a parked loop, but we
    // fail loudly rather than corrupt state if that assumption is ever violated.
    if (_syncLoop != nullptr) {
        throw std::runtime_error("sendSync: a synchronous call is already in flight (reentrant use)");
    }
    if (!_connected) {
        throw std::runtime_error("disconnected");
    }
    _pendingReply.clear();
    _socket.sendTextMessage(QString::fromStdString(msg));
    QEventLoop loop;
    _syncLoop = &loop;
    loop.exec();
    _syncLoop = nullptr;
    // A disconnect (or a timeout with no reply) leaves _pendingReply empty; the
    // disconnected slot quits the loop with the buffer cleared. Surface that as a
    // disconnect error so callers throw a clear message instead of decoding "".
    if (_pendingReply.empty()) {
        throw std::runtime_error("disconnected");
    }
    return _pendingReply;
}

::morph::exec::detail::ModelId QtWebSocketBackend::registerModel(
    const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> /*factory*/) {
    auto env = ::morph::wire::makeRegister(typeId);
    env.session = _session;
    std::string replyJson;
    try {
        replyJson = sendSync(::morph::wire::encode(env));
    } catch (const std::exception& exc) {
        // sendSync throws "disconnected" if the socket drops (or was never
        // connected) while the register reply was outstanding — the nested event
        // loop is now quit rather than hung. Surface it as a register failure.
        throw std::runtime_error(std::string{"register failed: "} + exc.what());
    }
    auto reply = ::morph::wire::decode(replyJson);
    if (reply.kind == "ok") {
        return ::morph::exec::detail::ModelId{reply.modelId};
    }
    throw std::runtime_error("register failed: " + reply.message);
}

bool QtWebSocketBackend::registerModelAsync(
    const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> /*factory*/,
    std::string_view contextKey, std::function<void(::morph::exec::detail::ModelId)> onRegistered,
    std::function<void(const std::string&)> onError) {
    if (!_cfg.asyncRegistrationEnabled) {
        // Opt-in only (see QtWebSocketBackendConfig::asyncRegistrationEnabled):
        // returning false here makes Bridge::registerHandler() fall back to
        // the synchronous registerModel(), preserving every existing
        // embedder's behavior unless it explicitly asks for the async path.
        return false;
    }
    if (!_connected) {
        // Queue rather than fail: this is exactly the ordering a
        // single-threaded WASM client must use, since it can never block
        // waiting for the connection to settle (see issue #54). The queued
        // request is sent -- with a call-id assigned then, not now -- the
        // moment `connected` fires next (first connect included), from
        // flushQueuedRegistrations(). No call-id is assigned yet; if the
        // backend is torn down (or the socket disconnects) before that
        // happens, cancelPending() drains this queue too and still invokes
        // onError exactly once.
        std::scoped_lock const lock{_pendingMtx};
        _queuedRegistrations.push_back(
            QueuedRegistration{typeId, std::string{contextKey}, std::move(onRegistered), std::move(onError)});
        return true;
    }
    sendRegisterAsync(typeId, contextKey, std::move(onRegistered), std::move(onError));
    return true;
}

void QtWebSocketBackend::sendRegisterAsync(const std::string& typeId, std::string_view contextKey,
                                           std::function<void(::morph::exec::detail::ModelId)> onRegistered,
                                           std::function<void(const std::string&)> onError) {
    uint64_t const callId = ++_nextCallId;
    {
        std::scoped_lock const lock{_pendingMtx};
        _pendingRegistrations[callId] = PendingRegistration{std::move(onRegistered), std::move(onError)};
    }
    auto env = ::morph::wire::makeRegister(typeId, std::string{contextKey});
    env.callId = callId;
    env.session = _session;
    _socket.sendTextMessage(QString::fromStdString(::morph::wire::encode(env)));
}

void QtWebSocketBackend::flushQueuedRegistrations() {
    std::vector<QueuedRegistration> queued;
    {
        std::scoped_lock const lock{_pendingMtx};
        queued.swap(_queuedRegistrations);
    }
    for (auto& entry : queued) {
        sendRegisterAsync(entry.typeId, entry.contextKey, std::move(entry.onRegistered), std::move(entry.onError));
    }
}

bool QtWebSocketBackend::registerModelSharedAsync(
    const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> /*factory*/,
    ::morph::backend::detail::InstanceIdentity identity,
    std::function<void(::morph::exec::detail::ModelId)> onRegistered,
    std::function<void(const std::string&)> onError) {
    if (!_cfg.asyncRegistrationEnabled) {
        return false;
    }
    if (identity.primary.empty()) {
        // Degrades to the private (non-shared) path, exactly like the
        // synchronous registerModelShared below -- and that path already
        // has an async form: this class's own registerModelAsync.
        return registerModelAsync(typeId, nullptr, identity.contextKey, std::move(onRegistered), std::move(onError));
    }
    if (!_connected) {
        onError("disconnected");
        return true;
    }
    uint64_t const callId = ++_nextCallId;
    {
        std::scoped_lock const lock{_pendingMtx};
        _pendingRegistrations[callId] =
            PendingRegistration{.onRegistered = std::move(onRegistered), .onError = std::move(onError)};
    }
    auto env =
        ::morph::wire::makeRegisterShared(typeId, std::string{identity.primary}, std::string{identity.contextKey});
    env.callId = callId;
    _socket.sendTextMessage(QString::fromStdString(::morph::wire::encode(env)));
    return true;
}

bool QtWebSocketBackend::attachModelAsync(
    const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> /*factory*/,
    ::morph::backend::detail::InstanceIdentity identity, ::morph::exec::detail::ModelId current,
    std::function<void(::morph::exec::detail::ModelId)> onRegistered,
    std::function<void(const std::string&)> onError) {
    if (!_cfg.asyncRegistrationEnabled) {
        return false;
    }
    if (identity.primary.empty()) {
        // Mirrors the synchronous attachModel's empty-primary branch: release
        // the current instance (fire-and-forget, as deregisterModel already
        // is) and degrade to a private async registration.
        if (current.v != 0U) {
            deregisterModel(current);
        }
        return registerModelAsync(typeId, nullptr, identity.contextKey, std::move(onRegistered), std::move(onError));
    }
    if (!_connected) {
        onError("disconnected");
        return true;
    }
    uint64_t const callId = ++_nextCallId;
    {
        std::scoped_lock const lock{_pendingMtx};
        _pendingRegistrations[callId] =
            PendingRegistration{.onRegistered = std::move(onRegistered), .onError = std::move(onError)};
    }
    auto env =
        ::morph::wire::makeAttach(typeId, std::string{identity.primary}, current.v, std::string{identity.contextKey});
    env.callId = callId;
    _socket.sendTextMessage(QString::fromStdString(::morph::wire::encode(env)));
    return true;
}

::morph::wire::ProtocolNegotiationResult QtWebSocketBackend::negotiateProtocolVersion() {
    std::string replyJson;
    try {
        replyJson = sendSync(::morph::wire::encode(::morph::wire::makeHello()));
    } catch (const std::exception& exc) {
        throw std::runtime_error(std::string{"protocol negotiation failed: "} + exc.what());
    }
    return ::morph::wire::interpretHelloReply(::morph::wire::decode(replyJson));
}

namespace {

/// @brief Decodes a synchronous control reply and returns its `modelId`.
/// @param replyJson Raw reply text.
/// @param what      Verb name used in the error message.
/// @return The replied `ModelId`.
/// @throws std::runtime_error if the reply is an `err`.
::morph::exec::detail::ModelId modelIdFromReply(const std::string& replyJson, std::string_view what) {
    auto reply = ::morph::wire::decode(replyJson);
    if (reply.kind == "ok") {
        return ::morph::exec::detail::ModelId{reply.modelId};
    }
    throw std::runtime_error(std::string{what} + " failed: " + reply.message);
}

}  // namespace

::morph::exec::detail::ModelId QtWebSocketBackend::registerModelShared(
    const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
    ::morph::backend::detail::InstanceIdentity identity) {
    if (identity.primary.empty()) {
        return registerModelWithContext(typeId, std::move(factory), identity.contextKey);
    }
    auto env = ::morph::wire::makeRegisterShared(typeId, std::string{identity.primary},
                                                   std::string{identity.contextKey});
    env.session = _session;
    return modelIdFromReply(sendSync(::morph::wire::encode(env)), "register");
}

::morph::exec::detail::ModelId QtWebSocketBackend::attachModel(
    const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
    ::morph::backend::detail::InstanceIdentity identity, ::morph::exec::detail::ModelId current) {
    if (identity.primary.empty()) {
        if (current.v != 0U) {
            deregisterModel(current);
        }
        return registerModelWithContext(typeId, std::move(factory), identity.contextKey);
    }
    auto env = ::morph::wire::makeAttach(typeId, std::string{identity.primary}, current.v,
                                          std::string{identity.contextKey});
    env.session = _session;
    return modelIdFromReply(sendSync(::morph::wire::encode(env)), "attach");
}

void QtWebSocketBackend::assignPrimary(::morph::exec::detail::ModelId mid, const std::string& typeId,
                                       std::string_view primary) {
    if (primary.empty() || mid.v == 0U) {
        return;
    }
    auto env = ::morph::wire::makeAssign(typeId, std::string{primary}, mid.v);
    env.session = _session;
    (void)modelIdFromReply(sendSync(::morph::wire::encode(env)), "assign");
}

bool QtWebSocketBackend::assignPrimaryAsync(::morph::exec::detail::ModelId mid, const std::string& typeId,
                                            std::string_view primary,
                                            std::function<void(::morph::exec::detail::ModelId)> onRegistered,
                                            std::function<void(const std::string&)> onError) {
    if (primary.empty() || mid.v == 0U) {
        // Same no-op contract as the synchronous assignPrimary: nothing to
        // promote. Resolve onRegistered as a no-op success, echoing mid back,
        // rather than treating it as a failure.
        onRegistered(mid);
        return true;
    }
    if (!_connected) {
        onError("disconnected");
        return true;
    }
    uint64_t const callId = ++_nextCallId;
    {
        std::scoped_lock const lock{_pendingMtx};
        _pendingAssigns[callId] = PendingAssign{std::move(onRegistered), std::move(onError)};
    }
    auto env = ::morph::wire::makeAssign(typeId, std::string{primary}, mid.v);
    env.callId = callId;
    _socket.sendTextMessage(QString::fromStdString(::morph::wire::encode(env)));
    return true;
}

std::vector<std::string> QtWebSocketBackend::listInstances(const std::string& typeId) {
    auto reply = ::morph::wire::decode(sendSync(::morph::wire::encode(::morph::wire::makeInstances(typeId))));
    if (reply.kind != "ok") {
        throw std::runtime_error("instances failed: " + reply.message);
    }
    std::vector<std::string> keys;
    if (auto errCode = glz::read_json(keys, reply.body)) {
        throw std::runtime_error("instances decode failed: " + glz::format_error(errCode, reply.body));
    }
    return keys;
}

void QtWebSocketBackend::deregisterModel(::morph::exec::detail::ModelId mid) {
    // Fire-and-forget — avoids a nested QEventLoop during destructor which can
    // trigger Qt asserts. The server does no connection-scoped cleanup, so an
    // undelivered deregister leaves the model registered there indefinitely.
    if (!_connected) {
        return;
    }
    // A real, non-zero callId (issue #65): callId == 0 is the wire's
    // "parked sendSync waiter" sentinel, and this request's reply -- though
    // nobody waits on it -- would otherwise be indistinguishable from one a
    // synchronous register/attach/assign/instances call is genuinely parked
    // on. Tracked in _pendingDeregisters purely so onTextMessage can
    // recognise and drop it explicitly instead of routing it to whichever
    // sendSync loop happens to be parked when it arrives.
    uint64_t const callId = ++_nextCallId;
    {
        std::scoped_lock const lock{_pendingMtx};
        _pendingDeregisters.insert(callId);
    }
    auto env = ::morph::wire::makeDeregister(mid.v);
    env.callId = callId;
    env.session = _session;
    _socket.sendTextMessage(QString::fromStdString(::morph::wire::encode(env)));
}

::morph::async::Completion<std::shared_ptr<void>> QtWebSocketBackend::execute(
    ::morph::exec::detail::ModelId mid, ::morph::backend::detail::ActionCall call, ::morph::exec::IExecutor* cbExec) {
    auto compState = std::make_shared<::morph::async::detail::CompletionState<std::shared_ptr<void>>>();
    ::morph::async::Completion<std::shared_ptr<void>> comp{compState, cbExec};

    if (!_connected) {
        compState->setException(std::make_exception_ptr(::morph::backend::DisconnectedError{}));
        return comp;
    }

    uint64_t callId = ++_nextCallId;
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

    _socket.sendTextMessage(QString::fromStdString(::morph::wire::encode(env)));
    return comp;
}

void QtWebSocketBackend::cancelPending(const std::exception_ptr& exc) {
    std::unordered_map<uint64_t, PendingExecute> drainedExecutes;
    std::unordered_map<uint64_t, PendingRegistration> drainedRegistrations;
    std::vector<QueuedRegistration> drainedQueue;
    std::unordered_map<uint64_t, PendingAssign> drainedAssigns;
    {
        std::scoped_lock lock{_pendingMtx};
        drainedExecutes.swap(_pending);
        drainedRegistrations.swap(_pendingRegistrations);
        drainedQueue.swap(_queuedRegistrations);
        drainedAssigns.swap(_pendingAssigns);
        // _pendingDeregisters tracks fire-and-forget requests nobody awaits --
        // just drop the bookkeeping, there is no callback to invoke.
        _pendingDeregisters.clear();
    }
    for (auto& [_, pending] : drainedExecutes) {
        if (pending.state) {
            pending.state->setException(exc);
        }
    }
    std::string message = "disconnected";
    try {
        std::rethrow_exception(exc);
    } catch (const std::exception& concrete) {
        message = concrete.what();
    } catch (...) {
        // Non-std::exception thrown in: keep the "disconnected" fallback above.
    }
    for (auto& [_, pending] : drainedRegistrations) {
        if (pending.onError) {
            pending.onError(message);
        }
    }
    // A registerModelAsync request queued while the socket had never yet
    // connected (issue #54) never got a call-id, so it cannot be found in
    // _pendingRegistrations above -- drain it here instead, on the same
    // cancelPending path that already handles a connection that goes away
    // (or never comes up) before a queued reply, so its onError still fires
    // exactly once rather than leaving the caller waiting forever.
    for (auto& entry : drainedQueue) {
        if (entry.onError) {
            entry.onError(message);
        }
    }
    for (auto& [_, pending] : drainedAssigns) {
        if (pending.onError) {
            pending.onError(message);
        }
    }
}

void QtWebSocketBackend::setReconnectHandler(const std::function<void()>& handler) { _reconnectHandler = handler; }

void QtWebSocketBackend::setConnectHandler(const std::function<void()>& handler) { _connectHandler = handler; }

void QtWebSocketBackend::setDisconnectHandler(const std::function<void()>& handler) { _disconnectHandler = handler; }

void QtWebSocketBackend::setSession(::morph::session::Context session) { _session = std::move(session); }

void QtWebSocketBackend::scheduleReconnect() {
    _reconnectTimer.start(static_cast<int>(_currentReconnectDelay.count()));
    // Pre-compute the next backoff so the timer above used the *current* one.
    auto next = std::chrono::milliseconds{
        static_cast<std::chrono::milliseconds::rep>(_currentReconnectDelay.count() * _cfg.backoffMultiplier)};
    _currentReconnectDelay = std::min(next, _cfg.maxReconnectDelay);
}

void QtWebSocketBackend::attemptReconnect() {
    if (_shuttingDown || _connected) {
        return;
    }
    _socket.open(_serverUrl);
    // If this attempt fails, QWebSocket fires `disconnected` again and our slot
    // schedules the next attempt with the updated backoff.
}

void QtWebSocketBackend::onTextMessage(const QString& message) {
    std::string msg = message.toStdString();
    ::morph::wire::Envelope env;
    try {
        env = ::morph::wire::decode(msg);
    } catch (const std::exception&) {
        // Malformed reply — route as a sync error if a waiter is parked. The raw
        // text is handed over rather than discarded so the parked caller can
        // report something better than "disconnected".
        _pendingReply = msg;
        if (_syncLoop) {
            _syncLoop->quit();
            return;
        }
        // No sync waiter: with the callId unreadable, this reply cannot be
        // matched to the execute it belongs to. Dropping it silently — the
        // previous behavior — left that execute's Completion unsettled forever,
        // firing neither .then() nor .onError(); a caller awaiting it simply
        // hangs. Since every message on this socket is required to be one
        // envelope, an undecodable one means the peer's framing can no longer be
        // trusted, so fail the pending calls rather than wait on a stream that
        // may never produce a matching reply. A spurious error is recoverable by
        // the caller; a permanent hang is not.
        cancelPending(std::make_exception_ptr(
            std::runtime_error("protocol error: server sent a message that is not a valid envelope")));
        return;
    }

    // Async execute replies and async registerModelAsync replies both carry a
    // non-zero callId (the two share one counter/namespace, but land in
    // separate maps below since their reply shapes differ); sync replies
    // (registerModel/deregister/etc's sendSync calls) carry callId == 0 and
    // resume the parked nested event loop.
    if (env.callId != 0U) {
        PendingExecute execPending;
        bool foundExecute = false;
        {
            std::scoped_lock lock{_pendingMtx};
            auto iter = _pending.find(env.callId);
            if (iter != _pending.end()) {
                execPending = std::move(iter->second);
                _pending.erase(iter);
                foundExecute = true;
            }
        }
        if (foundExecute) {
            if (env.kind == "ok") {
                try {
                    execPending.state->setValue(execPending.deserialize(env.body));
                } catch (...) {
                    execPending.state->setException(std::current_exception());
                }
            } else if (env.message == "timeout") {
                execPending.state->setException(std::make_exception_ptr(::morph::backend::TimeoutError{}));
            } else {
                execPending.state->setException(std::make_exception_ptr(std::runtime_error(env.message)));
            }
            return;
        }

        PendingRegistration regPending;
        bool foundRegistration = false;
        {
            std::scoped_lock lock{_pendingMtx};
            auto iter = _pendingRegistrations.find(env.callId);
            if (iter != _pendingRegistrations.end()) {
                regPending = std::move(iter->second);
                _pendingRegistrations.erase(iter);
                foundRegistration = true;
            }
        }
        if (foundRegistration) {
            if (env.kind == "ok") {
                regPending.onRegistered(::morph::exec::detail::ModelId{env.modelId});
            } else {
                regPending.onError(env.message);
            }
            return;
        }

        PendingAssign assignPending;
        bool foundAssign = false;
        {
            std::scoped_lock lock{_pendingMtx};
            auto iter = _pendingAssigns.find(env.callId);
            if (iter != _pendingAssigns.end()) {
                assignPending = std::move(iter->second);
                _pendingAssigns.erase(iter);
                foundAssign = true;
            }
        }
        if (foundAssign) {
            if (env.kind == "ok") {
                assignPending.onRegistered(::morph::exec::detail::ModelId{env.modelId});
            } else {
                assignPending.onError(env.message);
            }
            return;
        }

        {
            // A fire-and-forget deregister's reply (see issue #65): assigned a
            // real callId purely so it lands here instead of falling through
            // to the callId==0 branch below and being handed to whichever
            // sendSync waiter happens to be parked. Nobody observes the
            // reply either way -- ok or err, drop it.
            std::scoped_lock lock{_pendingMtx};
            auto iter = _pendingDeregisters.find(env.callId);
            if (iter != _pendingDeregisters.end()) {
                _pendingDeregisters.erase(iter);
                return;
            }
        }
        // No map matched: an already-resolved or already-cancelled callId's
        // late reply, dropped silently -- same as before this feature for an
        // execute reply.
        return;
    }

    _pendingReply = std::move(msg);
    if (_syncLoop) {
        _syncLoop->quit();
    }
}

}  // namespace morph::qt

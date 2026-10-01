// SPDX-License-Identifier: Apache-2.0

#include <QCoreApplication>
#include <QEventLoop>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <cassert>
#include <cctype>
#include <morph/core/detail/reply_router.hpp>
#include <morph/core/wire.hpp>
#include <morph/qt/qt_websocket_backend.hpp>
#include <optional>
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
        // Fires on every successful connect, first included -- the general
        // "transport is up" notification a status indicator wants.
        if (_connectHandler) {
            _connectHandler();
        }
        // Send every bind request that arrived before this connect (the first
        // connect included). Runs before the reconnect handler
        // below so a caller that gates UI on the bind's continuation sees it
        // fire promptly on first connect too.
        flushQueuedRegistrations();
        // Fire the reconnect handler only on subsequent connects, never on the
        // first one — initial registration is handled by the BridgeHandler ctors.
        if (isReconnect && _reconnectHandler && _reconnectExec != nullptr) {
            _reconnectExec->post(_reconnectHandler);
        }
    });
    QObject::connect(&_socket, &QWebSocket::disconnected, [this]() {
        _connected = false;
        // Fires before reconnect scheduling below, so an observer sees the
        // disconnected state even when a retry follows immediately.
        if (_disconnectHandler) {
            _disconnectHandler();
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

bool QtWebSocketBackend::waitForConnected(int timeoutMs) {
    if (_connected) {
        return true;
    }
    QEventLoop loop;
    // Connected after the constructor's own `connected` slot, so `_connected`
    // is already set when this one quits the loop.
    QObject::connect(&_socket, &QWebSocket::connected, &loop, &QEventLoop::quit);
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    return _connected;
}

::morph::exec::detail::ModelId QtWebSocketBackend::registerModel(
    const std::string& /*typeId*/,
    std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> /*factory*/) {
    throw std::logic_error("QtWebSocketBackend::registerModel: registration is a round trip; use bindModel");
}

QtWebSocketBackend::PendingControl QtWebSocketBackend::modelIdReply(
    ::morph::async::Completion<::morph::exec::detail::ModelId>::Promise promise) {
    auto shared =
        std::make_shared<::morph::async::Completion<::morph::exec::detail::ModelId>::Promise>(std::move(promise));
    return PendingControl{.reply =
                              [shared](const ::morph::wire::Envelope& env) {
                                  if (env.kind == "ok") {
                                      shared->resolve(::morph::exec::detail::ModelId{env.modelId});
                                  } else {
                                      shared->reject(std::make_exception_ptr(std::runtime_error{env.message}));
                                  }
                              },
                          .fail = [shared](const std::exception_ptr& exc) { shared->reject(exc); }};
}

::morph::async::Completion<::morph::exec::detail::ModelId> QtWebSocketBackend::bindModel(
    ::morph::backend::detail::BindRequest request, ::morph::exec::IExecutor& cbExec) {
    checkThread("QtWebSocketBackend::bindModel");
    auto [completion, promise] = ::morph::async::Completion<::morph::exec::detail::ModelId>::makeSettleable(&cbExec);

    if (request.primary.empty()) {
        // An empty primary with a live instance: release
        // the instance currently held (fire-and-forget, as deregisterModel
        // already is) and degrade to a private registration.
        if (request.current.v != 0U) {
            deregisterModel(request.current);
        }
        if (!_connected) {
            // Queue rather than fail: this is exactly the ordering a
            // single-threaded WASM client must use, since it can never block
            // waiting for the connection to settle. The queued
            // request is sent -- with a call-id assigned then, not now -- the
            // moment `connected` fires next (first connect included), from
            // flushQueuedRegistrations(). No call-id is assigned yet; if the
            // backend is torn down (or the socket disconnects) before that
            // happens, cancelPending() drains this queue too and still settles
            // the promise exactly once.
            _queuedRegistrations.push_back(QueuedRegistration{.typeId = std::move(request.typeId),
                                                              .contextKey = std::move(request.contextKey),
                                                              .promise = std::move(promise)});
            return std::move(completion);
        }
        sendControl(::morph::wire::makeRegister(request.typeId, request.contextKey), modelIdReply(std::move(promise)));
        return std::move(completion);
    }

    if (!_connected) {
        // A keyed bind carries no queue: unlike a private registration it may
        // be a re-point of a live instance, and replaying that against a
        // connection that has since been re-established would attach from a
        // `current` the new connection never issued.
        promise.reject(std::make_exception_ptr(std::runtime_error{"disconnected"}));
        return std::move(completion);
    }

    // `registerShared` for a first bind, `attach` when re-pointing from a live
    // instance.
    auto env = request.current.v == 0U
                   ? ::morph::wire::makeRegisterShared(request.typeId, request.primary, request.contextKey)
                   : ::morph::wire::makeAttach(request.typeId, request.primary, request.current.v, request.contextKey);
    sendControl(std::move(env), modelIdReply(std::move(promise)));
    return std::move(completion);
}

void QtWebSocketBackend::sendControl(::morph::wire::Envelope env, PendingControl pending) {
    checkThread("QtWebSocketBackend::sendControl");
    uint64_t const callId = ++_nextCallId;
    env.callId = callId;
    // RemoteServer authenticates and authorizes from env.session; this is the
    // only place a request other than execute is sent from, so stamping it
    // here covers every one of them.
    env.session = _session;
    QString encoded;
    try {
        // Encoded before filing: a throw after filing would leave the entry
        // waiting for a reply to a message that was never sent.
        encoded = QString::fromStdString(::morph::wire::encode(env));
    } catch (...) {
        // Rejected rather than rethrown: every request verb gives its caller
        // one failure channel, the returned `Completion`.
        pending.fail(std::current_exception());
        return;
    }
    _pendingControl.insert_or_assign(callId, std::move(pending));
    _socket.sendTextMessage(encoded);
}

void QtWebSocketBackend::flushQueuedRegistrations() {
    checkThread("QtWebSocketBackend::flushQueuedRegistrations");
    auto queued = std::exchange(_queuedRegistrations, {});
    for (auto& entry : queued) {
        sendControl(::morph::wire::makeRegister(entry.typeId, entry.contextKey),
                    modelIdReply(std::move(entry.promise)));
    }
}

::morph::async::Completion<::morph::wire::ProtocolNegotiationResult> QtWebSocketBackend::negotiateProtocolVersion(
    ::morph::exec::IExecutor& replyExec) {
    checkThread("QtWebSocketBackend::negotiateProtocolVersion");
    using Result = ::morph::wire::ProtocolNegotiationResult;
    auto [completion, promise] = ::morph::async::Completion<Result>::makeSettleable(&replyExec);
    if (!_connected) {
        promise.reject(std::make_exception_ptr(std::runtime_error{"protocol negotiation failed: disconnected"}));
        return std::move(completion);
    }
    auto shared = std::make_shared<::morph::async::Completion<Result>::Promise>(std::move(promise));
    sendControl(::morph::wire::makeHello(),
                PendingControl{.reply =
                                   [shared](const ::morph::wire::Envelope& env) {
                                       try {
                                           shared->resolve(::morph::wire::interpretHelloReply(env));
                                       } catch (...) {
                                           shared->reject(std::current_exception());
                                       }
                                   },
                               .fail = [shared](const std::exception_ptr& exc) { shared->reject(exc); }});
    return std::move(completion);
}

void QtWebSocketBackend::checkThread(char const* site) const noexcept {
    // A QObject's state belongs to the thread it lives on.
    static_cast<void>(site);
    assert(QThread::currentThread() == _socket.thread() && "QtWebSocketBackend used off its socket's thread");
}

void QtWebSocketBackend::assignPrimary(::morph::exec::detail::ModelId /*mid*/, const std::string& /*typeId*/,
                                       std::string_view /*primary*/) {
    throw std::logic_error("QtWebSocketBackend::assignPrimary: an assign is a round trip; use promoteModel");
}

::morph::async::Completion<::morph::exec::detail::ModelId> QtWebSocketBackend::promoteModel(
    ::morph::backend::detail::PromoteRequest request, ::morph::exec::IExecutor& cbExec) {
    auto [completion, promise] = ::morph::async::Completion<::morph::exec::detail::ModelId>::makeSettleable(&cbExec);

    if (request.primary.empty() || request.mid.v == 0U) {
        // Same no-op contract as the synchronous assignPrimary: nothing to
        // promote. Resolve, echoing mid back, rather than treating it as a
        // failure.
        promise.resolve(request.mid);
        return std::move(completion);
    }
    if (!_connected) {
        promise.reject(std::make_exception_ptr(std::runtime_error{"disconnected"}));
        return std::move(completion);
    }
    sendControl(::morph::wire::makeAssign(request.typeId, request.primary, request.mid.v),
                modelIdReply(std::move(promise)));
    return std::move(completion);
}

std::vector<std::string> QtWebSocketBackend::listInstances(const std::string& /*typeId*/) {
    throw std::logic_error("QtWebSocketBackend::listInstances: a listing is a round trip; use instances");
}

::morph::async::Completion<std::vector<std::string>> QtWebSocketBackend::instances(const std::string& typeId,
                                                                                   ::morph::exec::IExecutor& cbExec) {
    checkThread("QtWebSocketBackend::instances");
    using Keys = std::vector<std::string>;
    auto [completion, promise] = ::morph::async::Completion<Keys>::makeSettleable(&cbExec);
    if (!_connected) {
        promise.reject(std::make_exception_ptr(std::runtime_error{"disconnected"}));
        return std::move(completion);
    }
    auto shared = std::make_shared<::morph::async::Completion<Keys>::Promise>(std::move(promise));
    sendControl(::morph::wire::makeInstances(typeId),
                PendingControl{.reply =
                                   [shared](const ::morph::wire::Envelope& env) {
                                       if (env.kind != "ok") {
                                           shared->reject(std::make_exception_ptr(
                                               std::runtime_error{"instances failed: " + env.message}));
                                           return;
                                       }
                                       Keys keys;
                                       if (auto errCode = glz::read_json(keys, env.body)) {
                                           shared->reject(std::make_exception_ptr(std::runtime_error{
                                               "instances decode failed: " + glz::format_error(errCode, env.body)}));
                                           return;
                                       }
                                       shared->resolve(std::move(keys));
                                   },
                               .fail = [shared](const std::exception_ptr& exc) { shared->reject(exc); }});
    return std::move(completion);
}

void QtWebSocketBackend::deregisterModel(::morph::exec::detail::ModelId mid) {
    // Fire-and-forget: nothing waits, so this is safe from a destructor. The
    // server does no connection-scoped cleanup, so an undelivered deregister
    // leaves the model registered there indefinitely.
    if (!_connected) {
        return;
    }
    checkThread("QtWebSocketBackend::deregisterModel");
    uint64_t const callId = ++_nextCallId;
    _pendingDeregisters.insert(callId);
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

    uint64_t const callId = ++_nextCallId;
    ::morph::wire::Envelope env;
    env.kind = "execute";
    env.callId = callId;
    env.modelId = mid.v;
    env.modelType = call.modelTypeId;
    env.actionType = call.actionTypeId;
    env.body = call.serializeBody();
    env.session = std::move(call.session);

    checkThread("QtWebSocketBackend::execute");
    _pending[callId] =
        PendingExecute{.state = compState, .deserialize = std::move(call.deserializeResult), .cbExec = cbExec};

    _socket.sendTextMessage(QString::fromStdString(::morph::wire::encode(env)));
    return comp;
}

void QtWebSocketBackend::cancelPending(const std::exception_ptr& exc) {
    checkThread("QtWebSocketBackend::cancelPending");
    // Taken out before any settle: a settle that re-enters this backend files
    // into empty tables.
    auto drainedExecutes = std::exchange(_pending, {});
    auto drainedControl = std::exchange(_pendingControl, {});
    auto drainedQueue = std::exchange(_queuedRegistrations, {});
    // _pendingDeregisters tracks fire-and-forget requests nobody awaits --
    // just drop the bookkeeping, there is no callback to invoke.
    _pendingDeregisters.clear();
    for (auto& [ignoredCallId, pending] : drainedExecutes) {
        if (pending.state) {
            pending.state->setException(exc);
        }
    }
    for (auto& [ignoredCallId, pending] : drainedControl) {
        // The exception itself, not a message rebuilt from it: a request
        // rejected by a dropped socket delivers the very
        // `backend::DisconnectedError` an execute() call delivers, so a caller
        // can catch one type for both.
        pending.fail(exc);
    }
    // A private bind queued while the socket had never yet connected never got
    // a call-id, so it cannot be found in _pendingControl
    // above -- drain it here instead, on the same cancelPending path that
    // already handles a connection that goes away (or never comes up) before a
    // queued reply, so its continuation still fires exactly once rather than
    // leaving the caller waiting forever.
    for (auto& entry : drainedQueue) {
        entry.promise.reject(exc);
    }
}

void QtWebSocketBackend::setReconnectHandler(std::function<void()> handler, ::morph::exec::IExecutor* exec) {
    _reconnectHandler = std::move(handler);
    _reconnectExec = exec;
}

void QtWebSocketBackend::setConnectHandler(const std::function<void()>& handler) { _connectHandler = handler; }

void QtWebSocketBackend::setDisconnectHandler(const std::function<void()>& handler) { _disconnectHandler = handler; }

void QtWebSocketBackend::setSession(::morph::session::Context session) { _session = std::move(session); }

void QtWebSocketBackend::scheduleReconnect() {
    _reconnectTimer.start(static_cast<int>(_currentReconnectDelay.count()));
    // Pre-compute the next backoff so the timer above used the *current* one.
    // Cast up to double first so the multiplication is openly floating-point.
    // Written as `count() * backoffMultiplier` the integral `rep` is narrowed to
    // double *inside* the expression, which the narrowing-conversions checks
    // flag separately from the explicit cast back. Same arithmetic,
    // same result -- only the one deliberate narrowing is left, on the outside.
    auto next = std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(
        static_cast<double>(_currentReconnectDelay.count()) * _cfg.backoffMultiplier)};
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

bool QtWebSocketBackend::tryRouteExecuteReply(const ::morph::wire::Envelope& env) {
    checkThread("QtWebSocketBackend::tryRouteExecuteReply");
    auto iter = _pending.find(env.callId);
    if (iter == _pending.end()) {
        return false;
    }
    PendingExecute execPending = std::move(iter->second);
    _pending.erase(iter);

    // Triage shared with net::SocketBackend and SimulatedRemoteBackend
    // (core/detail/reply_router.hpp). This site used to match a
    // hand-typed "timeout" literal instead of
    // wire::kExecuteTimeoutMessage -- the exact typo-drift that
    // constant exists to prevent, found while extracting the router.
    switch (::morph::backend::detail::classifyExecuteReply(env)) {
        case ::morph::backend::detail::ExecuteReplyKind::Value:
            try {
                execPending.state->setValue(execPending.deserialize(env.body));
            } catch (...) {
                execPending.state->setException(std::current_exception());
            }
            break;
        case ::morph::backend::detail::ExecuteReplyKind::Timeout:
            execPending.state->setException(std::make_exception_ptr(::morph::backend::TimeoutError{}));
            break;
        case ::morph::backend::detail::ExecuteReplyKind::Error:
        default:
            // `default:` only because the project builds with
            // -Wswitch-default; every enumerator of the closed
            // ExecuteReplyKind is handled explicitly above.
            execPending.state->setException(std::make_exception_ptr(std::runtime_error(env.message)));
            break;
    }
    return true;
}

bool QtWebSocketBackend::tryRouteControlReply(const ::morph::wire::Envelope& env) {
    checkThread("QtWebSocketBackend::tryRouteControlReply");
    auto iter = _pendingControl.find(env.callId);
    if (iter == _pendingControl.end()) {
        return false;
    }
    PendingControl pending = std::move(iter->second);
    // Erased before the settle: a continuation delivered inline may re-enter
    // this backend.
    _pendingControl.erase(iter);
    pending.reply(env);
    return true;
}

bool QtWebSocketBackend::tryRouteDeregisterReply(const ::morph::wire::Envelope& env) {
    // A fire-and-forget deregister's reply: nobody observes it, ok or err.
    auto iter = _pendingDeregisters.find(env.callId);
    if (iter == _pendingDeregisters.end()) {
        return false;
    }
    _pendingDeregisters.erase(iter);
    return true;
}

void QtWebSocketBackend::routeReply(const ::morph::wire::Envelope& env) {
    if (tryRouteExecuteReply(env)) {
        return;
    }
    if (tryRouteControlReply(env)) {
        return;
    }
    // No table matched (the deregister drop included): an already-cancelled
    // call's late reply, or one this backend never asked for (callId 0).
    static_cast<void>(tryRouteDeregisterReply(env));
}

void QtWebSocketBackend::onTextMessage(const QString& message) {
    ::morph::wire::Envelope env;
    try {
        env = ::morph::wire::decode(message.toStdString());
    } catch (const std::exception&) {
        // With the callId unreadable, this reply cannot be matched to the
        // request it belongs to, and every message on this socket is one
        // envelope: an undecodable one means the peer's framing can no longer
        // be trusted. Fail every pending request rather than leave one waiting
        // for a reply that may never come; a spurious error is recoverable by
        // the caller, a permanent hang is not.
        cancelPending(std::make_exception_ptr(
            std::runtime_error("protocol error: server sent a message that is not a valid envelope")));
        return;
    }
    routeReply(env);
}

}  // namespace morph::qt

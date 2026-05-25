// SPDX-License-Identifier: Apache-2.0

#include <QCoreApplication>
#include <QTimer>
#include <algorithm>
#include <morph/qt/qt_websocket_backend.hpp>
#include <morph/wire.hpp>
#include <cctype>
#include <stdexcept>
#include <utility>

namespace morph::qt {

QtWebSocketBackend::QtWebSocketBackend(QUrl serverUrl, ::morph::model::detail::ActionDispatcher& /*dispatcher*/,
                                       ::morph::model::detail::ModelRegistryFactory& /*registry*/,
                                       std::optional<QSslConfiguration> tls, Config cfg)
    : _serverUrl{std::move(serverUrl)},
      _tls{std::move(tls)},
      _cfg{cfg},
      _currentReconnectDelay{cfg.initialReconnectDelay} {
    if (_tls.has_value()) {
        _socket.setSslConfiguration(*_tls);
    }
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
        // Fire the reconnect handler only on subsequent connects, never on the
        // first one — initial registration is handled by the BridgeHandler ctors.
        if (isReconnect && _reconnectHandler) {
            _reconnectHandler();
        }
    });
    QObject::connect(&_socket, &QWebSocket::disconnected, [this]() {
        _connected = false;
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
    _socket.sendTextMessage(QString::fromStdString(msg));
    QEventLoop loop;
    _syncLoop = &loop;
    loop.exec();
    _syncLoop = nullptr;
    return _pendingReply;
}

::morph::exec::detail::ModelId QtWebSocketBackend::registerModel(
    const std::string& typeId,
    std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> /*factory*/) {
    auto replyJson = sendSync(::morph::wire::encode(::morph::wire::makeRegister(typeId)));
    auto reply = ::morph::wire::decode(replyJson);
    if (reply.kind == "ok") {
        return ::morph::exec::detail::ModelId{reply.modelId};
    }
    throw std::runtime_error("register failed: " + reply.message);
}

void QtWebSocketBackend::deregisterModel(::morph::exec::detail::ModelId mid) {
    // Fire-and-forget — server cleans up remaining models when connection closes.
    // Avoids a nested QEventLoop during destructor which can trigger Qt asserts.
    if (_connected) {
        _socket.sendTextMessage(QString::fromStdString(
            ::morph::wire::encode(::morph::wire::makeDeregister(mid.v))));
    }
}

::morph::async::Completion<std::shared_ptr<void>> QtWebSocketBackend::execute(
    ::morph::exec::detail::ModelId mid, ::morph::backend::detail::ActionCall call,
    ::morph::exec::IExecutor* cbExec) {
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
    std::unordered_map<uint64_t, PendingExecute> drained;
    {
        std::scoped_lock lock{_pendingMtx};
        drained.swap(_pending);
    }
    for (auto& [_, pending] : drained) {
        if (pending.state) {
            pending.state->setException(exc);
        }
    }
}

void QtWebSocketBackend::setReconnectHandler(std::function<void()> handler) {
    _reconnectHandler = std::move(handler);
}

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
        // Malformed reply — route as a sync error if a waiter is parked.
        _pendingReply = msg;
        if (_syncLoop) {
            _syncLoop->quit();
        }
        return;
    }

    // Async execute replies carry a non-zero callId; sync replies (register /
    // deregister) carry callId == 0 and resume the parked nested event loop.
    if (env.callId != 0U) {
        PendingExecute pending;
        {
            std::scoped_lock lock{_pendingMtx};
            auto iter = _pending.find(env.callId);
            if (iter == _pending.end()) {
                return;
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

    _pendingReply = std::move(msg);
    if (_syncLoop) {
        _syncLoop->quit();
    }
}

}  // namespace morph::qt

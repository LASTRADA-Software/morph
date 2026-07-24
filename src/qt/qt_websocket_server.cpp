// SPDX-License-Identifier: Apache-2.0

#include <QCoreApplication>
#include <QHostAddress>
#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <algorithm>
#include <chrono>
#include <morph/core/logger.hpp>
#include <morph/qt/qt_websocket_server.hpp>
#include <vector>

namespace morph::qt {

QtWebSocketServer::QtWebSocketServer(::morph::backend::RemoteServer& server, quint16 port,
                                     std::optional<QSslConfiguration> tls, QtWebSocketServerConfig cfg,
                                     QObject* parent)
    : QObject{parent},
      _server{server},
      _requestedPort{port},
      _cfg{cfg},
      _wsServer{QStringLiteral("morph"),
                tls.has_value() ? QWebSocketServer::SecureMode : QWebSocketServer::NonSecureMode, this},
      _housekeepingTimer{this} {
    if (tls.has_value()) {
        _wsServer.setSslConfiguration(*tls);
    }
    connect(&_wsServer, &QWebSocketServer::newConnection, this, &QtWebSocketServer::onNewConnection);
    if (_cfg.idleTimeout.count() > 0) {
        connect(&_housekeepingTimer, &QTimer::timeout, this, &QtWebSocketServer::onHousekeepingTick);
        _housekeepingTimer.start(1000);  // 1s sweep granularity — see ClientState::lastActivity doc comment
    }
}

QtWebSocketServer::~QtWebSocketServer() { close(); }

bool QtWebSocketServer::listen() {
    const bool hasTls = _wsServer.secureMode() == QWebSocketServer::SecureMode;
    if (!_cfg.bindAddress.isLoopback() && !hasTls && !_cfg.allowPlaintextExposure) {
        ::morph::log::logError(
            "[qt_websocket_server] listen() refused: bindAddress=" + _cfg.bindAddress.toString().toStdString() +
            " is not loopback and no TLS configuration was supplied. Pass a QSslConfiguration, or set "
            "QtWebSocketServerConfig::allowPlaintextExposure = true to deliberately serve plaintext off-host.");
        return false;
    }
    return _wsServer.listen(_cfg.bindAddress, _requestedPort);
}

quint16 QtWebSocketServer::port() const { return _wsServer.serverPort(); }

void QtWebSocketServer::close() {
    _wsServer.close();
    _housekeepingTimer.stop();
    // Disconnect the onDisconnected slot first to prevent re-entrant modification of _clients
    // during the abort/deleteLater sequence below.
    for (auto& [socket, state] : _clients) {
        socket->disconnect(this);
        _server.closeConnection(state.cid);
        if (state.handshakeTimer) {
            state.handshakeTimer->stop();
            state.handshakeTimer->deleteLater();
        }
        socket->abort();
        socket->deleteLater();
    }
    _clients.clear();
}

bool QtWebSocketServer::closeGracefully(std::chrono::milliseconds deadline) {
    auto const absoluteDeadline = std::chrono::steady_clock::now() + deadline;

    // Step 1: stop taking new connections.
    _wsServer.pauseAccepting();
    // Step 2: new register/execute envelopes now fail fast on every existing
    // connection.
    _server.beginShutdown();

    // Step 3: wait for in-flight replies without blocking the event loop.
    // drainedWithin(0) is a non-blocking poll of the current state; pumping
    // processEvents between polls lets the reply callbacks RemoteServer
    // already queued via QMetaObject::invokeMethod (see onTextMessage) run,
    // which is what actually delivers those replies over the still-open
    // sockets.
    bool drained = _server.drainedWithin(std::chrono::milliseconds{0});
    while (!drained && std::chrono::steady_clock::now() < absoluteDeadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        drained = _server.drainedWithin(std::chrono::milliseconds{0});
    }

    // The in-flight counter can reach zero a hair before the strand task's
    // reply callback actually reaches the Qt event queue (dispatchExecute's
    // `complete` decrements the counter, then invokes `reply`), and actually
    // delivering that reply over the socket needs a couple more event-loop
    // round trips beyond that (this side's write, then the peer's read). Give
    // a reply that just landed a short, bounded window to flush before this
    // method starts sending close frames out from under it — bounded by
    // whatever is left of `deadline` so a caller's budget is never exceeded.
    auto const settleDeadline =
        std::min(absoluteDeadline, std::chrono::steady_clock::now() + std::chrono::milliseconds{50});
    while (std::chrono::steady_clock::now() < settleDeadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }

    // Step 4: whatever is still connected gets a proper close frame instead
    // of an abort, so the client can tell an orderly stop from a crash.
    // Snapshot the sockets first (same reason as onHousekeepingTick's toClose
    // vector below): closing an idle, already-primed loopback socket can
    // complete synchronously, reentrantly firing onDisconnected -- which
    // erases from _clients -- while this loop is still iterating it.
    std::vector<QWebSocket*> toClose;
    toClose.reserve(_clients.size());
    for (const auto& entry : _clients) {
        toClose.push_back(entry.first);
    }
    for (auto* socket : toClose) {
        socket->close(QWebSocketProtocol::CloseCodeGoingAway, QStringLiteral("server shutting down"));
    }

    // Let whatever remains of the deadline flush the close handshake over the
    // event loop before the hard stop below reclaims any stragglers. If the
    // drain step above already consumed the whole deadline, this loop body
    // never runs and close() below fires immediately.
    while (!_clients.empty() && std::chrono::steady_clock::now() < absoluteDeadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }

    // Step 5: hard stop for stragglers — identical to today's close() (also
    // reclaims each remaining client's connection scope).
    close();
    return drained;
}

void QtWebSocketServer::onNewConnection() {
    QWebSocket* socket = _wsServer.nextPendingConnection();
    if (!socket) {
        return;
    }
    if (_cfg.maxConnections != 0 && _clients.size() >= _cfg.maxConnections) {
        socket->close();
        socket->deleteLater();
        return;
    }
    connect(socket, &QWebSocket::textMessageReceived, this, &QtWebSocketServer::onTextMessage);
    connect(socket, &QWebSocket::disconnected, this, &QtWebSocketServer::onDisconnected);

    ClientState state;
    state.socket = socket;
    state.tokens = static_cast<double>(_cfg.messagesPerSecond);  // full bucket: an immediate burst is allowed
    state.lastRefill = std::chrono::steady_clock::now();
    state.lastActivity = state.lastRefill;
    state.cid = _server.openConnection();

    if (_cfg.handshakeTimeout.count() > 0) {
        auto* timer = new QTimer(this);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, this, [this, socket] {
            if (_clients.contains(socket)) {
                socket->close();
            }
        });
        timer->start(static_cast<int>(_cfg.handshakeTimeout.count()));
        state.handshakeTimer = timer;
    }

    _clients.emplace(socket, state);
}

bool QtWebSocketServer::consumeToken(ClientState& state) {
    if (_cfg.messagesPerSecond == 0) {
        return true;  // unbounded (today's behavior)
    }
    auto const now = std::chrono::steady_clock::now();
    double const elapsedSeconds = std::chrono::duration<double>(now - state.lastRefill).count();
    state.lastRefill = now;
    double const capacity = static_cast<double>(_cfg.messagesPerSecond);
    state.tokens = std::min(capacity, state.tokens + elapsedSeconds * capacity);
    if (state.tokens < 1.0) {
        return false;
    }
    state.tokens -= 1.0;
    return true;
}

void QtWebSocketServer::onTextMessage(const QString& message) {
    auto* socket = qobject_cast<QWebSocket*>(sender());
    if (!socket) {
        return;
    }
    auto iter = _clients.find(socket);
    if (iter == _clients.end()) {
        return;  // disconnected/untracked socket; drop
    }
    ClientState& state = iter->second;
    state.lastActivity = std::chrono::steady_clock::now();
    if (state.handshakeTimer) {
        state.handshakeTimer->stop();
        state.handshakeTimer->deleteLater();
        state.handshakeTimer = nullptr;
    }

    if (static_cast<std::size_t>(message.toUtf8().size()) > _cfg.maxMessageBytes) {
        socket->sendTextMessage(
            QString::fromStdString(::morph::wire::encode(::morph::wire::makeErr("message exceeds maxMessageBytes"))));
        return;
    }

    if (!consumeToken(state)) {
        // Over the per-connection rate limit: drop the frame silently rather
        // than reply or close the connection (see docs/spec/core/backend.md,
        // QtWebSocketServerConfig::messagesPerSecond). A pending client
        // Completion for a dropped `execute` will not resolve on its own; pair
        // messagesPerSecond with LimitPolicy::executeTimeout for a bounded wait.
        return;
    }

    QPointer<QWebSocket> weakSocket{socket};
    _server.handle(
        message.toStdString(),
        [weakSocket](const std::string& reply) {
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [weakSocket, reply]() {
                    if (weakSocket) {
                        weakSocket->sendTextMessage(QString::fromStdString(reply));
                    }
                },
                Qt::QueuedConnection);
        },
        state.cid);
}

void QtWebSocketServer::onDisconnected() {
    auto* socket = qobject_cast<QWebSocket*>(sender());
    if (!socket) {
        return;
    }
    auto iter = _clients.find(socket);
    if (iter != _clients.end()) {
        _server.closeConnection(iter->second.cid);
        if (iter->second.handshakeTimer) {
            iter->second.handshakeTimer->stop();
            iter->second.handshakeTimer->deleteLater();
        }
        _clients.erase(iter);
        socket->deleteLater();
    }
}

void QtWebSocketServer::onHousekeepingTick() {
    if (_cfg.idleTimeout.count() <= 0) {
        return;
    }
    auto const now = std::chrono::steady_clock::now();
    std::vector<QWebSocket*> toClose;
    for (auto& [socket, state] : _clients) {
        if (now - state.lastActivity > _cfg.idleTimeout) {
            toClose.push_back(socket);
        }
    }
    for (auto* socket : toClose) {
        socket->close();
    }
}

}  // namespace morph::qt

// SPDX-License-Identifier: Apache-2.0

#include <QCoreApplication>
#include <QHostAddress>
#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <algorithm>
#include <morph/qt/qt_websocket_server.hpp>

namespace morph::qt {

QtWebSocketServer::QtWebSocketServer(::morph::backend::RemoteServer& server, quint16 port,
                                     std::optional<QSslConfiguration> tls, QtWebSocketServerConfig cfg,
                                     QObject* parent)
    : QObject{parent},
      _server{server},
      _requestedPort{port},
      _cfg{cfg},
      _wsServer{QStringLiteral("morph"),
                tls.has_value() ? QWebSocketServer::SecureMode : QWebSocketServer::NonSecureMode, this} {
    if (tls.has_value()) {
        _wsServer.setSslConfiguration(*tls);
    }
    connect(&_wsServer, &QWebSocketServer::newConnection, this, &QtWebSocketServer::onNewConnection);
}

QtWebSocketServer::~QtWebSocketServer() { close(); }

bool QtWebSocketServer::listen() { return _wsServer.listen(QHostAddress::LocalHost, _requestedPort); }

quint16 QtWebSocketServer::port() const { return _wsServer.serverPort(); }

void QtWebSocketServer::close() {
    _wsServer.close();
    // Disconnect the onDisconnected slot first to prevent re-entrant modification of _clients
    // during the abort/deleteLater sequence below.
    for (auto& [socket, state] : _clients) {
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
    _clients.clear();
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
    _server.handle(message.toStdString(), [weakSocket](const std::string& reply) {
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [weakSocket, reply]() {
                if (weakSocket) {
                    weakSocket->sendTextMessage(QString::fromStdString(reply));
                }
            },
            Qt::QueuedConnection);
    });
}

void QtWebSocketServer::onDisconnected() {
    auto* socket = qobject_cast<QWebSocket*>(sender());
    if (!socket) {
        return;
    }
    auto iter = _clients.find(socket);
    if (iter != _clients.end()) {
        _clients.erase(iter);
        socket->deleteLater();
    }
}

}  // namespace morph::qt

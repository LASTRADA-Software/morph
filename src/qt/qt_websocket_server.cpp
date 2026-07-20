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
    for (QWebSocket* socket : _clients) {
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
        // Over the connection cap: refuse before wiring any signal, so this
        // socket is never tracked and never reaches RemoteServer.
        socket->close();
        socket->deleteLater();
        return;
    }
    connect(socket, &QWebSocket::textMessageReceived, this, &QtWebSocketServer::onTextMessage);
    connect(socket, &QWebSocket::disconnected, this, &QtWebSocketServer::onDisconnected);
    _clients.push_back(socket);
}

void QtWebSocketServer::onTextMessage(const QString& message) {
    auto* socket = qobject_cast<QWebSocket*>(sender());
    if (!socket) {
        return;
    }

    if (static_cast<std::size_t>(message.toUtf8().size()) > _cfg.maxMessageBytes) {
        socket->sendTextMessage(
            QString::fromStdString(::morph::wire::encode(::morph::wire::makeErr("message exceeds maxMessageBytes"))));
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
    auto iter = std::find(_clients.begin(), _clients.end(), socket);
    if (iter != _clients.end()) {
        _clients.erase(iter);
        socket->deleteLater();
    }
}

}  // namespace morph::qt

// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <QObject>
#include <QSslConfiguration>
#include <QWebSocket>
#include <QWebSocketServer>
#include <morph/core/remote.hpp>
#include <optional>
#include <vector>

namespace morph::qt {

/// @brief Qt WebSocket server that bridges incoming connections to a `RemoteServer`.
///
/// Listens for WebSocket clients, receives their text messages, and forwards each
/// one to `RemoteServer::handle()`. Replies are sent back to the originating client.
///
/// @par TLS
/// Pass a `QSslConfiguration` to enable `wss://`. The configuration should be built
/// from a certificate file, a Qt resource, or a byte array before being passed in.
///
/// @par Usage
/// Call `listen()` to start accepting connections, and `port()` to discover the
/// bound port (useful when @p port was 0 — let the OS assign a free port).
/// Call `close()` or destroy the object to stop the server.
class QtWebSocketServer : public QObject {
    Q_OBJECT
public:
    /// @brief Constructs the server and prepares it to listen on @p port.
    ///
    /// The server does not start accepting connections until `listen()` is called.
    ///
    /// @param server  `RemoteServer` instance that processes incoming messages.
    /// @param port    TCP port to listen on. Pass 0 to let the OS pick a free port.
    /// @param tls     If non-null, enables TLS (`wss://`) with this configuration.
    /// @param parent  Optional Qt parent object.
    explicit QtWebSocketServer(::morph::backend::RemoteServer& server, quint16 port = 0,
                               std::optional<QSslConfiguration> tls = std::nullopt, QObject* parent = nullptr);

    /// @brief Closes the server and disconnects all clients.
    ~QtWebSocketServer() override;

    /// @brief Starts listening for incoming WebSocket connections.
    ///
    /// @return `true` if the server successfully bound to the requested port.
    bool listen();

    /// @brief Returns the port the server is currently bound to.
    ///
    /// When constructed with port 0, this returns the OS-assigned port after
    /// a successful `listen()` call.
    /// @return Bound TCP port number.
    [[nodiscard]] quint16 port() const;

    /// @brief Stops accepting new connections and closes the server socket.
    void close();

    /// @brief Qt slot called when a new client connects.
    Q_SLOT void onNewConnection();

    /// @brief Qt slot called when a connected client sends a text message.
    /// @param message Raw text frame received from the client.
    Q_SLOT void onTextMessage(const QString& message);

    /// @brief Qt slot called when a client disconnects.
    Q_SLOT void onDisconnected();

private:
    ::morph::backend::RemoteServer& _server;
    quint16 _requestedPort;
    QWebSocketServer _wsServer;
    std::vector<QWebSocket*> _clients;
};

}  // namespace morph::qt

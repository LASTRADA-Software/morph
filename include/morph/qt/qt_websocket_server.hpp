// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <QObject>
#include <QSslConfiguration>
#include <QWebSocket>
#include <QWebSocketServer>
#include <chrono>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <optional>
#include <vector>

namespace morph::qt {

/// @brief Per-connection resource limits enforced by `QtWebSocketServer`.
///
/// Declared outside `QtWebSocketServer` so its default member initialisers are
/// fully parsed before any constructor default argument that names `Config{}`
/// is evaluated (same rationale as `morph::qt::QtWebSocketBackendConfig` and
/// `morph::offline::NetworkMonitorConfig`).
///
/// @note This struct is also the target of the independent TLS / peer-verification
/// hardening work (`docs/planned/tls_peer_verification.md`), which adds
/// `bindAddress` and `allowPlaintextExposure` fields here. If that work lands in
/// the same tree as this one, both sets of fields belong on this *same* struct
/// — whichever change lands second should extend this definition rather than
/// declaring a second, differently-named config type.
struct QtWebSocketServerConfig {
    /// @brief Max simultaneous live client connections. `0` = unbounded (today's behavior).
    ///
    /// A new connection beyond this count is closed immediately in `onNewConnection`,
    /// before any message exchange and before it is tracked internally.
    std::size_t maxConnections = 0;

    /// @brief Per-frame size cap enforced before a message reaches `RemoteServer::handle()`.
    ///
    /// Defaults to `morph::wire::kMaxEnvelopeBytes` (the wire layer's own bound), so
    /// an unconfigured server behaves exactly as today: the wire-layer cap is the
    /// only one in effect. Set lower to reject oversized frames earlier, before the
    /// cost of a pool round-trip and JSON decode.
    std::size_t maxMessageBytes = ::morph::wire::kMaxEnvelopeBytes;

    /// @brief Per-connection token-bucket rate limit, in messages per second. `0` = unbounded.
    ///
    /// The bucket capacity equals `messagesPerSecond` (an immediate one-second
    /// burst is allowed right after connecting), refilling continuously at that
    /// rate. A frame that arrives with an empty bucket is dropped — not queued,
    /// not replied to. See `docs/spec/core/backend.md`.
    std::size_t messagesPerSecond = 0;

    /// @brief Time allowed for a newly-accepted connection to send its first text
    ///        frame before it is closed. `0` = disabled (today's behavior).
    ///
    /// `QWebSocketServer::newConnection()` fires only after the WebSocket (and, in
    /// `SecureMode`, TLS) opening handshake has already completed, so in practice
    /// this bounds time-to-first-frame after that point, not the handshake itself
    /// — Qt exposes no earlier hook at this layer.
    std::chrono::milliseconds handshakeTimeout{0};

    /// @brief Time a connection may go without sending any frame before it is
    ///        closed. `0` = disabled (today's behavior).
    ///
    /// Checked by a periodic housekeeping sweep (roughly once per second), so the
    /// actual close can lag the configured value by up to that sweep interval.
    std::chrono::milliseconds idleTimeout{0};
};

/// @brief Qt WebSocket server that bridges incoming connections to a `RemoteServer`.
///
/// Listens for WebSocket clients, receives their text messages, and forwards each
/// one to `RemoteServer::handle()`. Replies are sent back to the originating client.
///
/// @par TLS
/// Pass a `QSslConfiguration` to enable `wss://`. The configuration should be built
/// from a certificate file, a Qt resource, or a byte array before being passed in.
///
/// @par Resource limits
/// Pass a `QtWebSocketServerConfig` to bound connection count, per-frame size,
/// per-connection message rate, and handshake/idle time. All fields default to
/// unbounded (except `maxMessageBytes`, which defaults to the wire-layer cap),
/// reproducing today's behavior when omitted.
///
/// @par Usage
/// Call `listen()` to start accepting connections, and `port()` to discover the
/// bound port (useful when @p port was 0 — let the OS assign a free port).
/// Call `close()` or destroy the object to stop the server.
class QtWebSocketServer : public QObject {
    Q_OBJECT
public:
    /// @brief Alias for the per-connection limits configuration struct.
    using Config = QtWebSocketServerConfig;

    /// @brief Constructs the server and prepares it to listen on @p port.
    ///
    /// The server does not start accepting connections until `listen()` is called.
    ///
    /// @param server  `RemoteServer` instance that processes incoming messages.
    /// @param port    TCP port to listen on. Pass 0 to let the OS pick a free port.
    /// @param tls     If non-null, enables TLS (`wss://`) with this configuration.
    /// @param cfg     Per-connection resource limits. Default: everything unbounded
    ///                (today's behavior) except `maxMessageBytes`, which defaults to
    ///                the wire-layer cap.
    /// @param parent  Optional Qt parent object.
    explicit QtWebSocketServer(::morph::backend::RemoteServer& server, quint16 port = 0,
                               std::optional<QSslConfiguration> tls = std::nullopt,
                               QtWebSocketServerConfig cfg = QtWebSocketServerConfig{}, QObject* parent = nullptr);

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
    QtWebSocketServerConfig _cfg;
    QWebSocketServer _wsServer;
    std::vector<QWebSocket*> _clients;
};

}  // namespace morph::qt

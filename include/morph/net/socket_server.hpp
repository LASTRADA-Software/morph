// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <morph/core/remote.hpp>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "detail/tcp_socket.hpp"
#include "detail/ws_frame.hpp"
#include "detail/ws_handshake.hpp"

namespace morph::net {

/// @brief Configuration for `SocketServer`.
struct SocketServerConfig {
    /// @brief Pending-connection backlog passed to the listening socket.
    int backlog = 64;
};

/// @brief Raw-socket WebSocket server front for `morph::backend::RemoteServer`.
///
/// Mirrors `morph::qt::QtWebSocketServer`'s behavior without Qt: for each
/// accepted connection it performs the RFC 6455 handshake, then reads framed
/// text messages and forwards each to `RemoteServer::handle()`; the reply is
/// written back to the originating socket, or dropped if the connection is
/// gone by the time the reply is ready.
///
/// @par Threading
/// Owns one accept thread plus one thread per accepted connection — there is
/// no shared event loop. `RemoteServer::handle()` replies arrive on the
/// server's worker-pool thread and are marshalled back onto the owning
/// connection's own write path, serialized by a per-connection mutex.
///
/// @par Lifetime
/// Holds `RemoteServer& _server` by reference, exactly like
/// `QtWebSocketServer` — the server's owning `shared_ptr` must outlive this
/// object (see `docs/spec/core/backend.md`'s Lifetime & ownership section).
class SocketServer {
public:
    /// @brief Alias for the config struct.
    using Config = SocketServerConfig;

    /// @brief Constructs the server; does not start listening.
    /// @param server `RemoteServer` instance that processes incoming messages.
    /// @param port   TCP port to listen on. Pass 0 to let the OS pick a free port.
    /// @param cfg    Backlog tuning. Default: 64-connection backlog.
    // Copy/move are implicitly deleted by the non-copyable/non-movable
    // std::mutex/std::thread members below — no explicit `= delete` needed
    // (matches the rest of the codebase's convention, e.g. `LocalBackend`).
    SocketServer(::morph::backend::RemoteServer& server, std::uint16_t port, Config cfg = {})
        : _server{server}, _requestedPort{port}, _cfg{cfg} {}

    /// @brief Stops accepting and closes every client connection.
    ~SocketServer() { close(); }

    /// @brief Starts listening for incoming WebSocket connections.
    /// @return `true` if the server successfully bound to the requested port.
    bool listen() {
        try {
            _listenSocket = ::morph::net::detail::TcpSocket::listen(_requestedPort, _cfg.backlog);
        } catch (const std::exception&) {
            return false;
        }
        _closing.store(false);
        _acceptThread = std::thread{[this] { acceptLoop(); }};
        return true;
    }

    /// @brief Returns the port the server is currently bound to.
    /// @return Bound TCP port (OS-assigned when constructed with port 0), or `0` before `listen()` succeeds.
    [[nodiscard]] std::uint16_t port() const { return _listenSocket.boundPort(); }

    /// @brief Stops accepting new connections and closes every client connection.
    ///
    /// Idempotent: safe to call more than once (including implicitly, via the
    /// destructor, after an explicit call).
    void close() {
        bool const wasAlreadyClosing = _closing.exchange(true);
        if (wasAlreadyClosing && !_acceptThread.joinable()) {
            return;
        }
        _listenSocket.shutdownBoth();
        if (_acceptThread.joinable()) {
            _acceptThread.join();
        }
        std::vector<std::shared_ptr<ClientConnection>> clients;
        std::vector<std::thread> threads;
        {
            std::scoped_lock lock{_clientsMtx};
            clients = _clients;
            threads.swap(_clientThreads);
            _clients.clear();
        }
        for (auto& client : clients) {
            client->closed.store(true);
            std::scoped_lock lock{client->writeMtx};
            client->socket.shutdownBoth();
        }
        for (auto& t : threads) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

private:
    struct ClientConnection {
        explicit ClientConnection(::morph::net::detail::TcpSocket s) : socket{std::move(s)} {}
        ::morph::net::detail::TcpSocket socket;
        std::mutex writeMtx;
        std::atomic<bool> closed{false};

        void sendText(const std::string& payload) {
            std::scoped_lock lock{writeMtx};
            if (closed.load() || !socket.valid()) {
                return;
            }
            try {
                std::string frame = ::morph::net::detail::encodeWsFrame(::morph::net::detail::WsOpcode::kText, payload,
                                                                        /*mask=*/false);
                socket.sendAll(frame.data(), frame.size());
            } catch (const std::exception&) {
                closed.store(true);
            }
        }
    };

    void acceptLoop() {
        for (;;) {
            ::morph::net::detail::TcpSocket clientSocket;
            try {
                clientSocket = _listenSocket.accept();
            } catch (const std::exception&) {
                return;  // listener was shut down (server closing)
            }
            if (_closing.load()) {
                return;
            }
            auto conn = std::make_shared<ClientConnection>(std::move(clientSocket));
            std::thread clientThread{[this, conn] { clientLoop(conn); }};
            {
                std::scoped_lock lock{_clientsMtx};
                _clients.push_back(conn);
                _clientThreads.push_back(std::move(clientThread));
            }
        }
    }

    void clientLoop(const std::shared_ptr<ClientConnection>& conn) {
        std::string leftover;
        try {
            leftover = ::morph::net::detail::performServerHandshake(conn->socket);
        } catch (const std::exception&) {
            conn->closed.store(true);
            return;
        }
        ::morph::net::detail::WsFrameReader reader;
        reader.feed(leftover);
        char buf[4096];
        for (;;) {
            if (!drainFrames(conn, reader)) {
                return;
            }
            std::size_t got = 0;
            try {
                got = conn->socket.recvSome(buf, sizeof(buf));
            } catch (const std::exception&) {
                conn->closed.store(true);
                return;
            }
            if (got == 0) {
                conn->closed.store(true);
                return;
            }
            reader.feed(std::string_view{buf, got});
        }
    }

    // Returns false when the connection should stop reading (peer sent
    // Close, or a protocol error was detected).
    bool drainFrames(const std::shared_ptr<ClientConnection>& conn, ::morph::net::detail::WsFrameReader& reader) {
        using ::morph::net::detail::WsOpcode;
        for (;;) {
            std::optional<::morph::net::detail::WsFrame> frame;
            try {
                frame = reader.tryExtractFrame();
            } catch (const std::exception&) {
                conn->closed.store(true);
                return false;
            }
            if (!frame) {
                return true;
            }
            if (frame->opcode == WsOpcode::kClose) {
                {
                    std::scoped_lock lock{conn->writeMtx};
                    if (!conn->closed.load() && conn->socket.valid()) {
                        try {
                            std::string closeFrame = ::morph::net::detail::encodeWsFrame(WsOpcode::kClose, "", false);
                            conn->socket.sendAll(closeFrame.data(), closeFrame.size());
                        } catch (const std::exception&) {
                        }
                    }
                }
                conn->closed.store(true);
                return false;
            }
            if (frame->opcode == WsOpcode::kPing) {
                std::scoped_lock lock{conn->writeMtx};
                if (!conn->closed.load() && conn->socket.valid()) {
                    try {
                        std::string pong = ::morph::net::detail::encodeWsFrame(WsOpcode::kPong, frame->payload, false);
                        conn->socket.sendAll(pong.data(), pong.size());
                    } catch (const std::exception&) {
                    }
                }
                continue;
            }
            if (frame->opcode == WsOpcode::kPong) {
                continue;
            }
            if (frame->opcode == WsOpcode::kText) {
                std::weak_ptr<ClientConnection> weak = conn;
                _server.handle(frame->payload, [weak](const std::string& reply) {
                    if (auto locked = weak.lock()) {
                        locked->sendText(reply);
                    }
                });
            }
        }
    }

    ::morph::backend::RemoteServer& _server;
    std::uint16_t _requestedPort;
    Config _cfg;
    ::morph::net::detail::TcpSocket _listenSocket;
    std::atomic<bool> _closing{true};
    std::thread _acceptThread;
    std::mutex _clientsMtx;
    std::vector<std::shared_ptr<ClientConnection>> _clients;
    std::vector<std::thread> _clientThreads;
};

}  // namespace morph::net

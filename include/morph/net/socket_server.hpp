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

#include "../attributes.hpp"
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
    ///               Borrowed, not owned: it must outlive this server (see
    ///               `docs/spec/concurrency_and_lifetimes.md`, "Destruction
    ///               ordering").
    /// @param port   TCP port to listen on. Pass 0 to let the OS pick a free port.
    /// @param cfg    Backlog tuning. Default: 64-connection backlog.
    // Copy/move are implicitly deleted by the non-copyable/non-movable
    // std::mutex/std::thread members below — no explicit `= delete` needed
    // (matches the rest of the codebase's convention, e.g. `LocalBackend`).
    SocketServer(::morph::backend::RemoteServer& server MORPH_LIFETIMEBOUND, std::uint16_t port, Config cfg = {})
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
    /// destructor, after an explicit call), and safe to call from several
    /// threads at once **on a live object** — concurrent callers are
    /// serialized, and every one of them returns only once the teardown is
    /// complete. Racing a call against the *destructor* is still the caller's
    /// problem (see `docs/spec/concurrency_and_lifetimes.md`, "Destruction
    /// ordering"): no lock inside this object can outlive the object holding
    /// it.
    void close() {
        // Serialize the whole body, not just the guard below (morph#451).
        // `_closing.exchange` alone cannot exclude a second caller: the loser
        // still sees a *joinable* accept thread — the winner has not joined it
        // yet and cannot have, since that thread is blocked in accept(2) until
        // the winner's shutdownBoth() releases it — so it falls through and
        // calls join() on the same std::thread the winner is joining. That is
        // UB, and it is not a theoretical one: on Linux/glibc the loser parks
        // forever in pthread_join's futex on a descriptor the winner already
        // reaped; on macOS/libc++ it throws std::system_error. Holding the
        // mutex across the join makes the second caller wait for the first and
        // then observe !joinable(), which is exactly the outcome the guard was
        // written to produce.
        //
        // Deadlock-free because nothing inside this class calls close():
        // neither acceptLoop() nor a per-client thread does, so no thread this
        // function joins can be waiting on this mutex.
        std::scoped_lock const teardownLock{_closeMtx};
        // Kept as-is despite the mutex: acceptLoop() reads `_closing` from the
        // accept thread, which never takes `_closeMtx`.
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
            // `closed` first, so no client thread starts a *new* send; then
            // shutdown without taking writeMtx. Taking it here would deadlock
            // exactly when the shutdown is most needed: a client thread blocked
            // in sendAll against a stalled peer's full socket buffer holds
            // writeMtx for as long as that send is stuck, and close() has no
            // timeout -- it is reached from the destructor. shutdownBoth() is
            // documented safe from any thread and is itself the mechanism that
            // unblocks that send (it fails, sendText catches, the thread exits
            // and releases the lock). Locking to "protect" the socket would
            // therefore wait on the very thing it is trying to interrupt.
            client->closed.store(true);
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
        ClientConnection(::morph::net::detail::TcpSocket sock, ::morph::backend::ConnectionId connectionId)
            : socket{std::move(sock)}, cid{connectionId} {}
        ::morph::net::detail::TcpSocket socket;
        /// Scope every `register` on this connection belongs to, so dropping
        /// the connection reclaims its models. See `RemoteServer::openConnection`.
        ::morph::backend::ConnectionId cid{0};
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
            auto conn = std::make_shared<ClientConnection>(std::move(clientSocket), _server.openConnection());
            std::thread clientThread{[this, conn] { clientLoop(conn); }};
            {
                std::scoped_lock lock{_clientsMtx};
                _clients.push_back(conn);
                _clientThreads.push_back(std::move(clientThread));
            }
        }
    }

    void clientLoop(const std::shared_ptr<ClientConnection>& conn) {
        // Reclaim this connection's models however the loop exits — failed
        // handshake, peer close, read error, or shutdown via close(). Without
        // it every model registered over this transport outlived its connection
        // forever: the scope machinery was wired into QtWebSocketServer only,
        // and this server dispatched through the unscoped two-argument
        // handle(). closeConnection() is idempotent, so the redundant call
        // during close() (which joins these threads) is harmless.
        struct ScopeGuard {
            ScopeGuard(::morph::backend::RemoteServer& srv MORPH_LIFETIMEBOUND,
                       ::morph::backend::ConnectionId connectionId)
                : server{srv}, cid{connectionId} {}
            ~ScopeGuard() { server.closeConnection(cid); }
            ScopeGuard(const ScopeGuard&) = delete;
            ScopeGuard& operator=(const ScopeGuard&) = delete;
            ScopeGuard(ScopeGuard&&) = delete;
            ScopeGuard& operator=(ScopeGuard&&) = delete;

            ::morph::backend::RemoteServer& server;
            ::morph::backend::ConnectionId cid;
        } const guard{_server, conn->cid};

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

    /// Best-effort control-frame write (a Close echo or a Pong). Failure to
    /// send one is never worth propagating: the connection is either already
    /// going away or will be noticed as gone by the next read.
    static void sendControlFrame(const std::shared_ptr<ClientConnection>& conn, ::morph::net::detail::WsOpcode opcode,
                                 std::string_view payload) {
        std::scoped_lock const lock{conn->writeMtx};
        if (conn->closed.load() || !conn->socket.valid()) {
            return;
        }
        try {
            std::string const frame = ::morph::net::detail::encodeWsFrame(opcode, payload, /*mask=*/false);
            conn->socket.sendAll(frame.data(), frame.size());
        } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch) — see the doc comment above
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
                sendControlFrame(conn, WsOpcode::kClose, "");
                conn->closed.store(true);
                return false;
            }
            if (frame->opcode == WsOpcode::kPing) {
                sendControlFrame(conn, WsOpcode::kPong, frame->payload);
                continue;
            }
            if (frame->opcode == WsOpcode::kPong) {
                continue;
            }
            if (frame->opcode == WsOpcode::kText) {
                std::weak_ptr<ClientConnection> weak = conn;
                _server.handle(
                    frame->payload,
                    [weak](const std::string& reply) {
                        if (auto locked = weak.lock()) {
                            locked->sendText(reply);
                        }
                    },
                    conn->cid);
            }
        }
    }

    ::morph::backend::RemoteServer& _server;
    std::uint16_t _requestedPort;
    Config _cfg;
    ::morph::net::detail::TcpSocket _listenSocket;
    /// Serializes `close()` against itself so only one caller ever reaches
    /// `_acceptThread.join()` (morph#451). Not taken anywhere else.
    std::mutex _closeMtx;
    std::atomic<bool> _closing{true};
    std::thread _acceptThread;
    std::mutex _clientsMtx;
    std::vector<std::shared_ptr<ClientConnection>> _clients;
    std::vector<std::thread> _clientThreads;
};

}  // namespace morph::net

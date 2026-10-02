// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IListener.hpp>
#include <core/net/Sockets.hpp>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <morph/core/detail/owner_probe.hpp>
#include <morph/core/io_loop.hpp>
#include <morph/core/remote.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../attributes.hpp"
#include "detail/tcp_socket.hpp"
#include "detail/ws_connection.hpp"
#include "detail/ws_frame.hpp"
#include "detail/ws_handshake.hpp"

namespace morph::net {

/// @brief Configuration for `SocketServer`.
struct SocketServerConfig {
    /// @brief Pending-connection backlog passed to the listening socket.
    int backlog = 64;

    /// @brief Bound on completing the RFC 6455 Upgrade handshake, from
    /// accept to the request's terminating `\r\n\r\n`, as a single deadline
    /// across the whole read. Zero disables it.
    std::chrono::milliseconds handshakeTimeout{10000};

    /// @brief Bound on writing one chunk (up to 64 KiB) of a reply frame.
    ///
    /// A loop timer armed around each chunk's write closes the connection when
    /// it runs out, so a peer that stops reading is retired — its models
    /// reclaimed, its requests no longer dispatched — instead of accumulating
    /// replies it will never read. Zero disables it.
    std::chrono::milliseconds sendTimeout{30000};
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
/// Owns no thread. The listener, every accepted connection and every reply
/// write live on an `exec::IoLoop` — the application's one loop, shared with
/// every other socket and timer built on it. `RemoteServer::handle()` replies
/// arrive on the server's worker pool and are posted to the loop, which queues
/// them on the connection's writer.
///
/// Cross-thread surface: `listen()` and `close()`, which run on the loop and
/// wait for it (inline when already there), and `port()`, an atomic read.
///
/// @par Lifetime
/// Holds `RemoteServer& _server` by reference, exactly like
/// `QtWebSocketServer` — the server's owning `shared_ptr` must outlive this
/// object (see `docs/spec/core/backend.md`'s Lifetime & ownership section).
class SocketServer {
public:
    /// @brief Alias for the config struct.
    using Config = SocketServerConfig;

    /// @brief Constructs the server on @p loop; does not start listening.
    /// @param loop   The application's I/O loop. Borrowed: it must outlive this
    ///               server.
    /// @param server `RemoteServer` instance that processes incoming messages.
    ///               Borrowed, not owned: it must outlive this server (see
    ///               `docs/spec/concurrency_and_lifetimes.md`, "Destruction
    ///               ordering").
    /// @param port   TCP port to listen on. Pass 0 to let the OS pick a free port.
    /// @param cfg    Backlog and timeout tuning. Default: 64-connection
    ///               backlog, 10s handshake timeout, 30s send timeout.
    SocketServer(::morph::exec::IoLoop& loop MORPH_LIFETIMEBOUND,
                 ::morph::backend::RemoteServer& server MORPH_LIFETIMEBOUND, std::uint16_t port, Config cfg = {})
        : _loop{&loop}, _core{std::make_shared<Core>(loop, server, cfg)}, _requestedPort{port} {}

    /// @brief Constructs the server on a loop of its own, created by the first
    ///        `listen()`; does not start listening.
    /// @param server `RemoteServer` instance that processes incoming messages.
    ///               Borrowed, not owned: it must outlive this server.
    /// @param port   TCP port to listen on. Pass 0 to let the OS pick a free port.
    /// @param cfg    Backlog and timeout tuning.
    SocketServer(::morph::backend::RemoteServer& server MORPH_LIFETIMEBOUND, std::uint16_t port, Config cfg = {})
        : _server{&server}, _requestedPort{port}, _cfg{cfg} {}

    SocketServer(const SocketServer&) = delete;
    SocketServer& operator=(const SocketServer&) = delete;
    SocketServer(SocketServer&&) = delete;
    SocketServer& operator=(SocketServer&&) = delete;

    /// @brief Stops accepting and closes every client connection.
    ///
    /// Closes on the loop and waits for it (inline when already there), then
    /// cuts the loop's tie to `RemoteServer`, so a connection's flow that
    /// resumes later touches neither this object nor the server.
    // NOLINTNEXTLINE(bugprone-exception-escape): teardown has no recovery if handing the close to the loop fails, so terminating is the outcome.
    ~SocketServer() {
        if (_core) {
            _loop->runAndWait([core = _core] {
                core->close();
                core->server = nullptr;
            });
        }
    }

    /// @brief Starts listening for incoming WebSocket connections.
    ///
    /// Binds `127.0.0.1:port` and starts the accept flow, on the loop. With no
    /// loop of its own yet, creates one first; a process out of descriptors
    /// cannot, and gets `false`.
    /// @return `true` if the server bound the requested port and is accepting.
    bool listen() {
        if (!_core) {
            try {
                _ownedLoop = std::make_unique<::morph::exec::IoLoop>();
            } catch (const std::exception&) {
                return false;
            }
            _loop = _ownedLoop.get();
            _core = std::make_shared<Core>(*_ownedLoop, *_server, _cfg);
        }
        bool listening = false;
        _loop->runAndWait([core = _core, port = _requestedPort, &listening] { listening = core->listen(port); });
        return listening;
    }

    /// @brief Returns the port the server is currently bound to.
    /// @return Bound TCP port (OS-assigned when constructed with port 0), or
    ///         `0` when not listening.
    [[nodiscard]] std::uint16_t port() const { return _core ? _core->port.load() : std::uint16_t{0}; }

    /// @brief Stops accepting new connections and closes every client connection.
    ///
    /// Releases the listening socket, so `port()` reads `0` afterwards and the
    /// bound port is free to rebind — the same post-close observation
    /// `QtWebSocketServer` makes. Reclaims every connection's models
    /// (`RemoteServer::closeConnection`) before it returns.
    ///
    /// Runs on the loop and waits for it, inline when already there.
    /// Idempotent, and safe to call from several threads at once on a live
    /// object: each call is one loop task, and the loop runs them one at a
    /// time. Racing a call against the *destructor* is still the caller's
    /// problem (see `docs/spec/concurrency_and_lifetimes.md`, "Destruction
    /// ordering").
    void close() {
        if (_core) {
            _loop->runAndWait([core = _core] { core->close(); });
        }
    }

private:
    /// One accepted connection.
    struct Client {
        std::shared_ptr<::morph::net::detail::LoopConnection> conn;
        /// Scope every `register` on this connection belongs to, so dropping
        /// the connection reclaims its models. See `RemoteServer::openConnection`.
        ::morph::backend::ConnectionId cid{0};
        /// Set once its models are reclaimed; the flow touches nothing after.
        bool done{false};
    };

    /// @brief Everything the loop owns, touched only in its tasks.
    ///
    /// Held by `shared_ptr` from the server and from every flow, so a flow
    /// that resumes after the server is gone finds it closed and ends.
    struct Core : std::enable_shared_from_this<Core> {
        Core(::morph::exec::IoLoop& ioLoop, ::morph::backend::RemoteServer& remote, Config config)
            : loop{ioLoop}, server{&remote}, cfg{config} {}

        void note(char const* site) const noexcept {
            ::morph::exec::detail::noteOwner(site, loop.loop(), loop.runningHere());
        }

        bool listen(std::uint16_t requestedPort) {
            note("SocketServer::listen");
            if (listener || server == nullptr) {
                return false;
            }
            ::morph::net::detail::TcpSocket bound;
            try {
                bound = ::morph::net::detail::TcpSocket::listen(requestedPort, cfg.backlog);
            } catch (const std::exception&) {
                return false;
            }
            if (!bound.setNonBlocking()) {
                return false;
            }
            auto adopted = ::core::net::adoptListener(loop.loop(), bound.nativeHandle());
            if (!adopted) {
                return false;  // `bound` still owns the descriptor and closes it
            }
            static_cast<void>(bound.release());
            listener = std::move(*adopted);
            port.store(listener->boundPort());
            ++generation;
            loop.loop().spawn(acceptFlow(shared_from_this(), listener.get(), generation));
            return true;
        }

        void close() {
            note("SocketServer::close");
            ++generation;
            if (listener) {
                // The accept flow parked on it resumes, in a later turn, with
                // `Cancelled`, sees the generation moved on, and ends without
                // touching the listener it no longer owns.
                listener->close();
                listener.reset();
            }
            port.store(0);
            for (auto const& client : std::exchange(clients, {})) {
                finish(*client, /*flush=*/false);
            }
        }

        /// Reclaims @p client's models and closes its socket. Idempotent.
        void finish(Client& client, bool flush) const {
            if (client.done) {
                return;
            }
            client.done = true;
            if (server != nullptr) {
                server->closeConnection(client.cid);
            }
            if (flush) {
                ::morph::net::detail::closeAfterFlush(client.conn);
            } else {
                ::morph::net::detail::closeConnection(*client.conn);
            }
        }

        void forget(Client const& client) {
            std::erase_if(clients, [&client](std::shared_ptr<Client> const& entry) { return entry.get() == &client; });
        }

        void accepted(std::unique_ptr<::core::net::ISocket> socket) {
            note("SocketServer::accept");
            if (server == nullptr) {
                return;
            }
            auto client =
                std::make_shared<Client>(Client{.conn = std::make_shared<::morph::net::detail::LoopConnection>(
                                                    loop.loop(), std::move(socket), cfg.sendTimeout),
                                                .cid = server->openConnection(),
                                                .done = false});
            clients.push_back(client);
            loop.loop().spawn(clientFlow(shared_from_this(), client));
        }

        /// Accepts until the listener it was started with is closed.
        static ::core::async::Task<void> acceptFlow(std::shared_ptr<Core> self, ::core::net::IListener* accepting,
                                                    std::uint64_t startedAt) {
            for (;;) {
                auto next = co_await accepting->accept();
                if (self->generation != startedAt) {
                    co_return;
                }
                if (!next) {
                    if (next.error().code == ::core::net::NetErrorCode::Cancelled) {
                        co_return;
                    }
                    // Out of descriptors, or another failure the next attempt
                    // may not repeat: wait a little rather than spin on a
                    // listener that stays readable.
                    co_await self->loop.loop().delay(std::chrono::milliseconds{50});
                    if (self->generation != startedAt) {
                        co_return;
                    }
                    continue;
                }
                self->accepted(std::move(*next));
            }
        }

        /// One connection: handshake, then read frames until it ends.
        static ::core::async::Task<void> clientFlow(std::shared_ptr<Core> self, std::shared_ptr<Client> client) {
            auto const connection = client->conn;
            auto header = co_await ::morph::net::detail::readHeaderBlockAsync(connection, self->cfg.handshakeTimeout);
            if (client->done) {
                co_return;
            }
            std::optional<std::string> leftover;
            if (header) {
                try {
                    auto const request = ::morph::net::detail::parseClientHandshakeRequest(header->header);
                    static_cast<void>(::morph::net::detail::enqueueFrame(
                        connection, ::morph::net::detail::buildServerHandshakeResponse(request.key)));
                    leftover = std::move(header->leftover);
                } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
                    // A malformed Upgrade request: the connection ends below.
                }
            }
            bool flush = false;
            if (leftover) {
                // Server role: RFC 6455 §5.1 requires a client to mask every
                // frame it sends, so this reader must reject an unmasked one.
                ::morph::net::detail::WsFrameReader reader{/*expectMasked=*/true};
                reader.feed(*leftover);
                std::array<std::byte, 4096> buf{};
                for (;;) {
                    auto const outcome = self->drainFrames(client, reader);
                    if (outcome != Drain::More) {
                        flush = outcome == Drain::Closed;
                        break;
                    }
                    auto const got = co_await connection->socket->read(buf);
                    if (client->done || !got || *got == 0) {
                        break;
                    }
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the socket yields bytes, the reader takes text; same object representation.
                    reader.feed(std::string_view{reinterpret_cast<char const*>(buf.data()), *got});
                }
            }
            if (client->done) {
                co_return;
            }
            self->finish(*client, flush);
            self->forget(*client);
        }

        enum class Drain : std::uint8_t {
            More,    ///< Read on.
            Closed,  ///< The peer sent Close; the echo is queued.
            Failed,  ///< A protocol error, or the connection is done.
        };

        Drain drainFrames(std::shared_ptr<Client> const& client, ::morph::net::detail::WsFrameReader& reader) {
            using ::morph::net::detail::WsOpcode;
            for (;;) {
                std::optional<::morph::net::detail::WsFrame> frame;
                try {
                    frame = reader.tryExtractFrame();
                } catch (const std::exception&) {
                    return Drain::Failed;
                }
                if (!frame) {
                    return Drain::More;
                }
                if (frame->opcode == WsOpcode::kClose) {
                    static_cast<void>(::morph::net::detail::enqueueFrame(
                        client->conn, ::morph::net::detail::encodeWsFrame(WsOpcode::kClose, frame->payload, false)));
                    return Drain::Closed;
                }
                if (frame->opcode == WsOpcode::kPing) {
                    static_cast<void>(::morph::net::detail::enqueueFrame(
                        client->conn, ::morph::net::detail::encodeWsFrame(WsOpcode::kPong, frame->payload, false)));
                    continue;
                }
                if (frame->opcode == WsOpcode::kText && server != nullptr) {
                    dispatch(client, frame->payload);
                    if (client->done) {
                        return Drain::Failed;
                    }
                }
            }
        }

        /// Hands one request to the server. Its reply comes back on whichever
        /// thread produces it — the server's pool, usually — and is posted
        /// here, to be queued on this connection if it is still open.
        void dispatch(std::shared_ptr<Client> const& client, std::string const& payload) {
            std::weak_ptr<Client> weak = client;
            server->handle(
                payload,
                [loopHandle = loop.weak(), weak](const std::string& reply) {
                    // Encoded on the replying thread, so the loop only queues it.
                    std::string frame = ::morph::net::detail::encodeWsFrame(::morph::net::detail::WsOpcode::kText,
                                                                            reply, /*mask=*/false);
                    static_cast<void>(loopHandle.post([weak, frame = std::move(frame)]() mutable {
                        auto const target = weak.lock();
                        if (!target || target->done) {
                            return;
                        }
                        ::morph::exec::detail::noteOwner("SocketServer::reply", target->conn->loop,
                                                         ::morph::exec::runningOn(target->conn->loop));
                        static_cast<void>(::morph::net::detail::enqueueFrame(target->conn, std::move(frame)));
                    }));
                },
                client->cid);
        }

        ::morph::exec::IoLoop& loop;
        /// The server, until `~SocketServer`.
        ::morph::backend::RemoteServer* server;
        Config cfg;
        std::unique_ptr<::core::net::IListener> listener;
        std::vector<std::shared_ptr<Client>> clients;
        /// Moves on with every `listen()` and `close()`, so an accept flow can
        /// tell that the listener it was started with is gone.
        std::uint64_t generation{0};
        /// Read by `port()` from any thread.
        std::atomic<std::uint16_t> port{0};
    };

    /// Present only for the owning constructor, from the first `listen()`;
    /// destroyed after the core has been closed on it.
    std::unique_ptr<::morph::exec::IoLoop> _ownedLoop;
    ::morph::exec::IoLoop* _loop{nullptr};
    std::shared_ptr<Core> _core;
    ::morph::backend::RemoteServer* _server{nullptr};
    std::uint16_t _requestedPort;
    Config _cfg{};
};

}  // namespace morph::net

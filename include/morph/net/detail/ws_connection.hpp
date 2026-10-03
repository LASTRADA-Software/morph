// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "../../core/profiler.hpp"
#include "ws_handshake.hpp"

/// @file
/// @brief One WebSocket connection on an I/O loop: its socket, the frames
///        waiting to be written, and the two flows that move bytes.
///
/// Shared by `SocketBackend` (client role) and `SocketServer` (server role).
/// Everything here runs on the loop's thread and is touched nowhere else; the
/// `LoopConnection` itself is held by `shared_ptr` from each flow that uses it,
/// so a flow that resumes after its owner has let go of the connection still
/// finds the socket object alive.

namespace morph::net::detail {

/// @brief A connected socket on the loop, plus its outgoing frame queue.
struct LoopConnection {
    /// @param eventLoop    The loop the socket is registered with.
    /// @param connected    The socket; owned from here on.
    /// @param writeTimeout Bound on writing one chunk of a frame; zero disables it.
    LoopConnection(::core::net::EventLoop& eventLoop, std::unique_ptr<::core::net::ISocket> connected,
                   std::chrono::milliseconds writeTimeout)
        : loop{eventLoop}, socket{std::move(connected)}, sendTimeout{writeTimeout} {}

    LoopConnection(const LoopConnection&) = delete;
    LoopConnection& operator=(const LoopConnection&) = delete;
    LoopConnection(LoopConnection&&) = delete;
    LoopConnection& operator=(LoopConnection&&) = delete;
    ~LoopConnection() = default;

    /// @brief The loop the socket belongs to.
    ::core::net::EventLoop& loop;
    /// @brief The socket. Closed, never replaced, for the connection's life.
    std::unique_ptr<::core::net::ISocket> socket;
    /// @brief Bound on one chunk's write; see `writerFlow`.
    std::chrono::milliseconds sendTimeout;
    /// @brief Whole frames waiting for the writer, oldest first.
    std::deque<std::string> outbox;
    /// @brief Whether a writer flow is running.
    bool writing{false};
    /// @brief Whether the socket has been closed.
    bool closed{false};
    /// @brief Close once the outbox has drained (a Close echo, say).
    bool closeWhenFlushed{false};
};

/// @brief Closes @p conn's socket now, dropping any unwritten frames.
///
/// A read or write parked on it resumes with `NetErrorCode::Cancelled` in a
/// later drain step of the loop, which is how the flows learn of it.
/// @param conn The connection; idempotent.
inline void closeConnection(LoopConnection& conn) noexcept {
    if (conn.closed) {
        return;
    }
    conn.closed = true;
    conn.outbox.clear();
    conn.socket->close();
}

/// @brief The largest single write the writer issues, so that `sendTimeout`
///        bounds a write making no progress rather than a large frame on a
///        slow link.
inline constexpr std::size_t kWriteChunk = std::size_t{64} * 1024;

/// @brief Timer callback: a write, or a handshake read, overran its bound.
/// @param connPtr The `LoopConnection`.
inline void closeOnDeadline(void* connPtr) { closeConnection(*static_cast<LoopConnection*>(connPtr)); }

/// @brief Writes every queued frame, in order, one chunk at a time.
///
/// Each chunk's write is bounded by `sendTimeout`: a loop timer that closes
/// the socket if the write has not finished, so a peer that stops reading
/// retires the connection instead of growing its queue forever.
/// @param conn The connection; the flow's own share keeps its socket alive.
/// @return The flow.
inline ::core::async::Task<void> writerFlow(std::shared_ptr<LoopConnection> conn) {
    while (!conn->closed && !conn->outbox.empty()) {
        std::string const frame = std::move(conn->outbox.front());
        conn->outbox.pop_front();
        std::size_t offset = 0;
        while (offset < frame.size() && !conn->closed) {
            std::size_t const length = std::min(kWriteChunk, frame.size() - offset);
            ::core::net::TimerId timer{};
            if (conn->sendTimeout.count() > 0) {
                timer =
                    conn->loop.addTimer(conn->loop.clock().now() + conn->sendTimeout, &closeOnDeadline, conn.get());
            }
            auto const written =
                co_await conn->socket->write(std::as_bytes(std::span<char const>{frame}.subspan(offset, length)));
            static_cast<void>(conn->loop.cancelTimer(timer));
            if (!written || *written == 0) {
                closeConnection(*conn);
                break;
            }
            offset += *written;
        }
    }
    conn->writing = false;
    if (conn->closeWhenFlushed) {
        closeConnection(*conn);
    }
}

/// @brief Queues @p frame on @p conn and starts the writer if it is idle.
/// @param conn  The connection.
/// @param frame One complete, encoded WebSocket frame.
/// @return `false` if the connection is closed and the frame was dropped.
inline bool enqueueFrame(std::shared_ptr<LoopConnection> const& conn, std::string frame) {
    // The send side's zone is here and not in `writerFlow`: a zone must not
    // stay open across a `co_await`, where the loop runs other work on the
    // same thread before the writer resumes.
    MORPH_ZONE("ws::enqueueFrame");
    if (conn->closed || conn->closeWhenFlushed) {
        return false;
    }
    conn->outbox.push_back(std::move(frame));
    if (!conn->writing) {
        conn->writing = true;
        conn->loop.spawn(writerFlow(conn));
    }
    return true;
}

/// @brief Closes @p conn once every queued frame is written, or now if none is.
/// @param conn The connection.
inline void closeAfterFlush(std::shared_ptr<LoopConnection> const& conn) {
    if (conn->writing) {
        conn->closeWhenFlushed = true;
        return;
    }
    closeConnection(*conn);
}

/// @brief Reads an HTTP header block — up to and including `\r\n\r\n` — off
///        @p conn, as the WebSocket handshake needs.
///
/// @p timeout bounds the whole read, from the first byte to the terminator:
/// a loop timer closes the socket when it runs out, so a peer that dribbles
/// one byte at a time cannot stretch the handshake past it. The header is
/// capped at 64 KiB, checked before each read, like `readHttpHeaderBlock`.
/// @param conn    The connection; must have no other read outstanding.
/// @param timeout Bound on the whole read; zero disables it.
/// @return The header and any bytes read past it, or `std::nullopt` if the
///         peer closed, the read failed, the cap was reached or the time ran out.
inline ::core::async::Task<std::optional<HandshakeReadResult>> readHeaderBlockAsync(
    std::shared_ptr<LoopConnection> conn, std::chrono::milliseconds timeout) {
    ::core::net::TimerId timer{};
    if (timeout.count() > 0) {
        timer = conn->loop.addTimer(conn->loop.clock().now() + timeout, &closeOnDeadline, conn.get());
    }
    std::string buf;
    std::array<std::byte, 4096> chunk{};
    std::optional<HandshakeReadResult> result;
    for (;;) {
        if (auto const pos = buf.find("\r\n\r\n"); pos != std::string::npos) {
            result = HandshakeReadResult{.header = buf.substr(0, pos), .leftover = buf.substr(pos + 4)};
            break;
        }
        if (buf.size() > std::size_t{64} * 1024 || conn->closed) {
            break;
        }
        auto const got = co_await conn->socket->read(chunk);
        if (!got || *got == 0) {
            break;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the socket yields bytes, the buffer holds text; same object representation.
        buf.append(reinterpret_cast<char const*>(chunk.data()), *got);
    }
    static_cast<void>(conn->loop.cancelTimer(timer));
    co_return result;
}

}  // namespace morph::net::detail

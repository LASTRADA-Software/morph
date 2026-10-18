// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

/// @file
/// @brief What a transport hands `RemoteServer` to write to one connection,
///        and the backlog bounds both sides apply.
///
/// Kept apart from `remote.hpp` so a transport's connection code, which both
/// the client and the server role share, can use the bounds without compiling
/// the server.

namespace morph::backend {

/// @brief Opaque id a transport uses to scope model registrations to one
///        connection so `RemoteServer::closeConnection` can reclaim them.
///
/// `0` is reserved and means *unscoped*: it is what the two-argument
/// `RemoteServer::handle()`/`handleInline()` overloads pass. Non-zero values
/// are minted by `RemoteServer::openConnection()`.
using ConnectionId = std::uint64_t;

/// @brief A transport's outbound side of one connection, for what the server
///        sends unasked: events.
///
/// Handed to `RemoteServer::setConnectionWriter`. Both callables run on the
/// server strand, so neither may block: `write` queues the frame on the
/// transport's own thread, as a reply is queued.
struct ConnectionWriter {
    /// @brief Queues one encoded envelope for the connection. May throw when
    ///        the connection can no longer be written; the server catches it.
    std::function<void(std::string)> write;
    /// @brief Bytes of encoded envelopes handed to the transport for this
    ///        connection, replies and events alike, and not yet written.
    ///        Empty reads as `0`. May throw; the server catches it and writes
    ///        nothing.
    std::function<std::size_t()> backlog;
};

/// @brief Backlog above which `RemoteServer::post` holds a connection's events
///        back and owes it one `resync` instead: 1 MiB.
inline constexpr std::size_t kEventBacklogHigh = std::size_t{1} << 20;

/// @brief Backlog below which a transport reports a connection drained, by
///        calling `RemoteServer::connectionDrained`: 256 KiB.
///
/// Well under `kEventBacklogHigh`, so a reader hovering at the bound does not
/// flip between held back and drained on every frame.
inline constexpr std::size_t kEventBacklogLow = std::size_t{256} << 10;

}  // namespace morph::backend

// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>

#include "../wire.hpp"

/// @file
/// @brief The callId-multiplexed `execute` reply router, shared by every
///        backend that talks to a `RemoteServer` over a real (or simulated)
///        connection.
///
/// Two pieces live here, both extracted out of `net::SocketBackend` because
/// they are a pure function of a decoded `wire::Envelope` and an in-memory
/// map — no socket, no thread, no server — and so are worth testing directly
/// rather than through a bound TCP port:
///
///   * `classifyExecuteReply` — the ok/timeout/err triage. It had been
///     open-coded three times (`net/socket_backend.hpp`,
///     `core/remote.hpp`'s `SimulatedRemoteBackend`, `src/qt/
///     qt_websocket_backend.cpp`) and the Qt copy had already drifted to a
///     hand-typed `"timeout"` literal, which is exactly the drift
///     `wire::kExecuteTimeoutMessage` exists to prevent.
///   * `PendingCallTable` — call-id allocation plus the pending-completion
///     map, owned by the backend's I/O loop.
///
/// @par Why `core/detail/` and not `net/detail/`
/// `core/remote.hpp` is one of the consumers and ships in the base `morph`
/// target's installed header set, whereas everything under `include/morph/
/// net/` belongs to the optional `morph_net` target (`MORPH_BUILD_NET`,
/// default `OFF`). A `core/` header including a `net/` one would therefore
/// install a `remote.hpp` that cannot find its own include whenever net is
/// off. Living under
/// `core/detail/` keeps the dependency pointing the one direction it
/// already points (`net` → `core`).

namespace morph::backend::detail {

/// @brief How a callId-matched `execute` reply should settle its `Completion`.
enum class ExecuteReplyKind : std::uint8_t {
    /// @brief An `ok` reply: deserialize `Envelope::body` and set the value.
    Value,
    /// @brief The server's own `LimitPolicy::executeTimeout` reply: fail with
    ///        `backend::TimeoutError` specifically.
    Timeout,
    /// @brief Any other reply: fail with a generic error carrying
    ///        `Envelope::message`.
    Error,
};

/// @brief Triages one decoded `execute` reply envelope into the three ways it
///        can settle a pending `Completion`.
///
/// Order matters and is part of the contract: `kind == "ok"` is decided
/// first, so an `ok` reply whose (unused) `message` field happens to read
/// `"timeout"` still settles as a value, not as a timeout.
///
/// The timeout arm matches `wire::kExecuteTimeoutMessage` rather than a
/// hand-typed literal. As that constant's own documentation is careful to
/// say, this does not make the classification airtight — `Envelope::message`
/// is a free-text field that can carry a caught exception's `what()`, so an
/// application exception whose text is exactly `"timeout"` is
/// indistinguishable from the server's timeout reply. That ambiguity is
/// inherent to the documented wire contract and is not something this
/// function can close; using the shared constant only keeps the one producer
/// and its consumers from drifting apart by typo.
///
/// @param env Decoded reply envelope, already matched to a pending call.
/// @return Which of the three arms the caller should take.
[[nodiscard]] inline ExecuteReplyKind classifyExecuteReply(const ::morph::wire::Envelope& env) {
    if (env.kind == "ok") {
        return ExecuteReplyKind::Value;
    }
    if (env.message == ::morph::wire::kExecuteTimeoutMessage) {
        return ExecuteReplyKind::Timeout;
    }
    return ExecuteReplyKind::Error;
}

/// @brief The map of in-flight calls, keyed by call id, owned by one executor.
///
/// @tparam PendingT Backend-specific per-call state (the `Completion` state,
///         the result deserializer, the callback executor). Must be movable.
///
/// Owns the call-id counter as well as the map, so the two cannot be
/// allocated and stored by different owners by accident. Neither is
/// synchronised: the table belongs to its backend's owner — for
/// `net::SocketBackend`, the I/O loop — and every call is made in a task that
/// owner runs. Allocating an id, filing the record, writing the request, and
/// finding the record again when the reply arrives are all loop tasks, so a
/// disconnect sweep cannot fall between a connected check and an insert: they
/// are one task, and the sweep is another.
template <class PendingT>
class PendingCallTable {
public:
    /// @brief The drained-map type returned by `drain()`.
    using Map = std::unordered_map<std::uint64_t, PendingT>;

    /// @brief Allocates the next call id.
    ///
    /// Starts at 1: `callId == 0` is reserved on the wire for "this is a
    /// synchronous control reply, not an execute reply".
    /// @return A call id not previously returned by this table.
    [[nodiscard]] std::uint64_t nextCallId() noexcept { return ++_nextCallId; }

    /// @brief Files @p pending under @p callId, replacing any existing entry.
    /// @param callId  Id to file @p pending under.
    /// @param pending Per-call state to store.
    void insert(std::uint64_t callId, PendingT pending) { _map.insert_or_assign(callId, std::move(pending)); }

    /// @brief Removes the entry for @p callId and returns it.
    /// @param callId Call id carried by an incoming reply.
    /// @return The stored state, or `std::nullopt` if no such call is in flight
    ///         (a late or already-cancelled reply).
    [[nodiscard]] std::optional<PendingT> take(std::uint64_t callId) {
        auto node = _map.extract(callId);
        if (node.empty()) {
            return std::nullopt;
        }
        return std::optional<PendingT>{std::move(node.mapped())};
    }

    /// @brief Removes every entry and returns them, leaving the table empty.
    ///
    /// Returns rather than resolves so the caller settles the drained
    /// completions with the table already empty: settling one may re-enter
    /// the backend and file a new call.
    /// @return Every entry that was in flight, keyed by call id.
    [[nodiscard]] Map drain() { return std::exchange(_map, {}); }

    /// @brief Number of calls currently in flight.
    /// @return The number of filed entries.
    [[nodiscard]] std::size_t size() const noexcept { return _map.size(); }

private:
    std::uint64_t _nextCallId{0};
    Map _map;
};

}  // namespace morph::backend::detail

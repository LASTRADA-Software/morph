// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
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
///     map, with the admit-under-lock insert that closes the
///     insert/disconnect-sweep race.
///
/// @par Why `core/detail/` and not `net/detail/`
/// `core/remote.hpp` is one of the consumers and ships in the base `morph`
/// target's installed header set, whereas everything under `include/morph/
/// net/` belongs to the optional `morph_net` target (`MORPH_BUILD_NET`,
/// default `OFF`). A `core/` header including a `net/` one would therefore
/// install a `remote.hpp` that cannot find its own include whenever net is
/// off — the same breakage morph#232 fixed for `quantity.hpp`. Living under
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

/// @brief Thread-safe map of in-flight `execute` calls, keyed by call id.
///
/// @tparam PendingT Backend-specific per-call state (the `Completion` state,
///         the result deserializer, the callback executor). Must be movable
///         *and* default-constructible: `insertIf` files entries through
///         `operator[]`, which value-initializes before assigning.
///
/// Owns the call-id counter as well as the map, so the two cannot be
/// allocated and stored by different owners by accident. Note the counter is a
/// lock-free `std::atomic` read *outside* `_mtx`, on purpose: allocating an id
/// and inserting its entry are deliberately not one atomic step -- `insertIf`'s
/// admit predicate is what makes the insert safe, not a shared lock.
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
    [[nodiscard]] std::uint64_t nextCallId() { return ++_nextCallId; }

    /// @brief Inserts @p pending under @p callId, but only if @p admit — called
    ///        while this table's mutex is held — approves it.
    ///
    /// The admit predicate is the whole point of this overload rather than a
    /// plain `insert`. Backends check "am I still connected?" before building
    /// the envelope, but a disconnect sweep (`drain()` + resolve-with-error)
    /// can run strictly between that check and the insert; the entry would
    /// then land in the map *after* the sweep already emptied it, and nothing
    /// would ever resolve its `Completion`. Re-checking under the same mutex
    /// the sweep takes closes that window: either the insert happens-before
    /// the sweep (and the sweep cancels it), or it happens-after and the
    /// predicate observes the disconnect and rejects it here.
    ///
    /// @param callId  Id to file @p pending under; overwrites any existing entry.
    /// @param pending Per-call state to store.
    /// @param admit   Predicate evaluated under the lock; `false` means "do not insert".
    /// @return `true` if the entry was stored, `false` if @p admit rejected it.
    template <class AdmitFn>
    bool insertIf(std::uint64_t callId, PendingT pending, AdmitFn&& admit) {
        std::scoped_lock const lock{_mtx};
        // Forwarded, not called as a plain lvalue: `admit` is a forwarding
        // reference and is invoked exactly once, so preserving the caller's
        // value category is both safe and what lets an rvalue-qualified or
        // move-only predicate be passed.
        if (!std::forward<AdmitFn>(admit)()) {
            return false;
        }
        _map[callId] = std::move(pending);
        return true;
    }

    /// @brief Removes the entry for @p callId and returns it.
    /// @param callId Call id carried by an incoming reply.
    /// @return The stored state, or `std::nullopt` if no such call is in flight
    ///         (a late or already-cancelled reply).
    [[nodiscard]] std::optional<PendingT> take(std::uint64_t callId) {
        std::scoped_lock const lock{_mtx};
        auto iter = _map.find(callId);
        if (iter == _map.end()) {
            return std::nullopt;
        }
        std::optional<PendingT> taken{std::move(iter->second)};
        _map.erase(iter);
        return taken;
    }

    /// @brief Removes every entry and returns them, leaving the table empty.
    ///
    /// Returns rather than resolves so the caller settles the drained
    /// completions *outside* this table's lock — a completion callback may
    /// re-enter the backend.
    /// @return Every entry that was in flight, keyed by call id.
    [[nodiscard]] Map drain() {
        Map drained;
        {
            std::scoped_lock const lock{_mtx};
            drained.swap(_map);
        }
        return drained;
    }

    /// @brief Number of calls currently in flight.
    ///
    /// Test-only observability. The count is stale the moment the lock is
    /// released, so it must not drive a check-then-act decision — use
    /// `insertIf`'s admit predicate (which runs *inside* the lock) for
    /// anything that has to be atomic with respect to the table.
    [[nodiscard]] std::size_t size() const {
        std::scoped_lock const lock{_mtx};
        return _map.size();
    }

private:
    mutable std::mutex _mtx;
    std::atomic<std::uint64_t> _nextCallId{0};
    Map _map;
};

}  // namespace morph::backend::detail

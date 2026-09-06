// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>

#include "../strand.hpp"

/// @file
/// @brief Per-model execute-ordering gate, extracted out of `RemoteServer`.
///
/// `RemoteServer`'s per-model execute-ordering gate (`ExecuteGate` before this
/// extraction, `takeExecuteTicket`/`awaitExecuteTurn`/`releaseExecuteTicket`,
/// and `ExecuteTicketGuard` — all `private`) is a standalone concurrency
/// primitive that was trapped inside the server. Its contract — hand out
/// monotonic tickets per `ModelId`; block a ticket until its predecessor
/// releases; tolerate releases arriving out of order; erase the gate when
/// drained; tolerate a gate already erased — touches no wire format, no
/// authorizer, no model, no executor, no strand, and is expressible without
/// `RemoteServer` at all.
///
/// This is a faithful, behavior-preserving port of the logic that used to
/// live directly on `RemoteServer`: same fields, same locking, same
/// out-of-order-release handling (`releasedOutOfOrder`, closing issue #449 --
/// already fixed upstream before this extraction; see that field's own doc
/// comment below for the full history). Nothing here is new logic.
namespace morph::backend::detail {

/// @brief Hands out monotonic per-`ModelId` tickets and lets callers block
///        until their ticket's turn comes, tolerating releases that arrive
///        out of ticket order.
///
/// Keyed by `ModelId`, not held forever: a model with no outstanding tickets
/// has no entry in the internal map at all (erased once its last ticket is
/// released), so this never grows unbounded across the gate's lifetime the
/// way a per-model map with no cleanup would.
class ExecuteOrderGate {
public:
    /// @brief Hands out the next ticket for @p mid, in call order.
    ///
    /// Intended to be called synchronously, on whatever thread received the
    /// request, before anything asynchronous happens -- the ticket numbers
    /// two calls receive for the same `mid` are therefore always in the
    /// order this was called in.
    /// @param mid The model the upcoming turn is for.
    /// @return This call's ticket number.
    [[nodiscard]] std::uint64_t take(::morph::exec::detail::ModelId mid) {
        std::scoped_lock const lock{_mtx};
        auto& gate = _gates[mid];
        if (!gate) {
            gate = std::make_shared<Gate>();
        }
        return gate->nextTicket++;
    }

    /// @brief Blocks until @p ticket is next in line for @p mid, then returns.
    /// @param mid    The model @p ticket was taken for.
    /// @param ticket This call's ticket, from `take`.
    void awaitTurn(::morph::exec::detail::ModelId mid, std::uint64_t ticket) {
        std::unique_lock lock{_mtx};
        auto iter = _gates.find(mid);
        if (iter == _gates.end()) {
            return;  // Nothing left to wait for -- every ticket for mid already released.
        }
        auto gate = iter->second;  // Keep it alive even if release() erases the map entry mid-wait.
        gate->cv.wait(lock, [&gate, ticket] { return gate->nextToRun == ticket; });
    }

    /// @brief Releases @p ticket for @p mid, letting the next ticket (if any) proceed.
    ///
    /// Must be called exactly once per ticket taken. A ticket that is taken
    /// but never released would permanently stall every later ticket for the
    /// same `mid`, since `awaitTurn` waits with no deadline. (`RemoteServer`
    /// enforces the "exactly once" half of that contract structurally, via
    /// `ExecuteTicketGuard` below -- see its own doc comment.)
    /// @param mid    The model @p ticket was taken for.
    /// @param ticket The ticket to release.
    void release(::morph::exec::detail::ModelId mid, std::uint64_t ticket) {
        std::scoped_lock const lock{_mtx};
        auto iter = _gates.find(mid);
        if (iter == _gates.end()) {
            return;  // Defensive; should not happen (this ticket's own take() created the entry).
        }
        auto& gate = *iter->second;
        // `nextToRun` is the lowest ticket that has *not* released yet, which
        // is exactly what `awaitTurn`'s `nextToRun == ticket` predicate needs
        // it to be. Advancing it to `ticket + 1` unconditionally was only
        // correct if tickets released in order, and callers deliberately do
        // not guarantee that: a caller may need to release a ticket
        // immediately, without ever waiting for its turn, precisely so that
        // ticket cannot hold up the live work behind it (`RemoteServer`'s
        // rejection paths -- model not found, unauthorized, over limit, a
        // shutdown gate -- all do exactly this). A later ticket releasing
        // first therefore used to push `nextToRun` straight past an earlier
        // ticket's number, whose waiter then had a predicate that could never
        // become true again -- a caller parked forever (issue #449, the third
        // occurrence of the stranded-ticket class #348 and #351 closed from
        // the other end by making the release itself unmissable).
        //
        // So an out-of-order release is recorded rather than applied, and
        // `nextToRun` walks forward only over a contiguous run of released
        // tickets. `ticket < nextToRun` cannot happen: every ticket is
        // released exactly once, and `nextToRun` only moves past tickets that
        // have already released.
        if (ticket == gate.nextToRun) {
            ++gate.nextToRun;
            while (!gate.releasedOutOfOrder.empty() && *gate.releasedOutOfOrder.begin() == gate.nextToRun) {
                gate.releasedOutOfOrder.erase(gate.releasedOutOfOrder.begin());
                ++gate.nextToRun;
            }
        } else {
            gate.releasedOutOfOrder.insert(ticket);
        }
        if (gate.nextToRun == gate.nextTicket) {
            // No ticket is currently waiting and none can arrive for a ticket
            // number already handed out -- safe to drop the entry so a model
            // with no in-flight tickets leaves no trace in this map. Reaching
            // `nextTicket` this way means every ticket handed out has
            // released, so `releasedOutOfOrder` is necessarily empty here.
            _gates.erase(iter);
        } else {
            gate.cv.notify_all();
        }
    }

    /// @brief Number of models with at least one outstanding (unreleased) ticket.
    ///
    /// Test-only observability, mirroring `PendingCallTable::size()` --
    /// stale the moment the lock is released, so it must not drive a
    /// check-then-act decision.
    /// @return Current map size.
    [[nodiscard]] std::size_t gateCount() const {
        std::scoped_lock const lock{_mtx};
        return _gates.size();
    }

private:
    struct Gate {
        std::uint64_t nextTicket = 0;
        std::uint64_t nextToRun = 0;
        // Tickets released *before* their turn came, held aside until every
        // ticket ahead of them has released too. Not an optimisation: it is
        // what makes `nextToRun` mean "the lowest ticket not yet released"
        // rather than "one past whichever ticket released last". See
        // `release` (issue #449). Ordered, because the release loop consumes
        // it from the front; small by construction (it holds at most the
        // tickets in flight for one model, minus one).
        std::set<std::uint64_t> releasedOutOfOrder;
        std::condition_variable cv;
    };

    mutable std::mutex _mtx;
    std::unordered_map<::morph::exec::detail::ModelId, std::shared_ptr<Gate>, ::morph::exec::detail::ModelIdHash>
        _gates;
};

/// @brief Owns a taken `ExecuteOrderGate` ticket and releases it on every exit
///        path, unless ownership was explicitly handed on.
///
/// A ticket handed out by `ExecuteOrderGate::take` must be released exactly
/// once by whatever path took it: an unreleased ticket permanently stalls
/// every later ticket for the same model, because `awaitTurn` is a `cv.wait`
/// with no deadline. In `RemoteServer` (the sole caller today) that rule used
/// to be a per-call-site convention, and the convention was missed twice: by
/// a shutdown gate that returns before the one place that released a ticket
/// (issue #348), and by every exception that unwinds past a dispatch
/// function's early returns (issue #351). This holder makes the rule
/// structural instead of remembered: the ticket is owned from the moment it
/// is taken until the guard dies, so every exit path -- `return`, `throw`,
/// and any branch a later change adds -- releases it. Two members opt out
/// deliberately:
///
/// - `release()`, for the paths that want the ticket freed *before* some
///   later, possibly-slow step, so that step never holds a later same-model
///   ticket up; and
/// - `disarm()`, for the one place ownership is genuinely handed on to
///   another owner (e.g. a task posted to a different executor).
///
/// Neither copyable nor movable: it is only ever a local of the frame that
/// owns the ticket.
class ExecuteTicketGuard {
public:
    /// @brief Adopts @p ticket on behalf of @p gate.
    /// @param gate   Gate @p ticket belongs to. Borrowed: a guard is always a
    ///               local of a frame that does not outlive the gate.
    /// @param ticket This call's ticket from `ExecuteOrderGate::take`, or
    ///               `std::nullopt` if it took none, in which case the guard
    ///               is inert.
    ExecuteTicketGuard(ExecuteOrderGate& gate,
                       std::optional<std::pair<::morph::exec::detail::ModelId, std::uint64_t>> ticket) noexcept
        : _gate{gate}, _ticket{std::move(ticket)} {}

    ExecuteTicketGuard(const ExecuteTicketGuard&) = delete;
    ExecuteTicketGuard(ExecuteTicketGuard&&) = delete;
    ExecuteTicketGuard& operator=(const ExecuteTicketGuard&) = delete;
    ExecuteTicketGuard& operator=(ExecuteTicketGuard&&) = delete;

    /// @brief Releases the ticket if it is still held -- the backstop that
    ///        covers every path which did not release explicitly.
    ~ExecuteTicketGuard() { release(); }

    /// @brief Releases the held ticket now, letting the next ticket for the
    ///        same model proceed. Idempotent: a no-op if no ticket was taken,
    ///        or if it has already been released or disarmed.
    void release() {
        if (!_ticket) {
            return;
        }
        auto const held = *_ticket;
        // Cleared before the release, not after: this guard owes nothing
        // further from here on, so the destructor cannot release twice even
        // if `ExecuteOrderGate::release` itself exits by exception.
        _ticket.reset();
        _gate.release(held.first, held.second);
    }

    /// @brief Gives up ownership *without* releasing, for the one path that
    ///        hands the ticket on to a new owner.
    void disarm() noexcept { _ticket.reset(); }

    /// @brief Blocks until the held ticket's turn comes (see
    ///        `ExecuteOrderGate::awaitTurn`). A no-op if no ticket is held.
    void awaitTurn() {
        if (_ticket) {
            _gate.awaitTurn(_ticket->first, _ticket->second);
        }
    }

private:
    ExecuteOrderGate& _gate;
    std::optional<std::pair<::morph::exec::detail::ModelId, std::uint64_t>> _ticket;
};

}  // namespace morph::backend::detail

// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>

#include "../../attributes.hpp"
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
private:
    struct Gate;  // Forward-declared; full definition is below, in the private section.

public:
    /// @brief Opaque handle to one outstanding ticket, bound to the specific
    ///        `Gate` instance it was issued from.
    ///
    /// `take(mid)` plus a later `awaitTurn(mid, ticket)`/`release(mid, ticket)`
    /// re-derive "the gate for `mid`" by a fresh map lookup each time — correct
    /// only if the entry found is still the same `Gate` the ticket came from.
    /// It might not be: a model's entry is erased once it fully drains (see
    /// `releaseOnGateLocked`), and a *later* ticket for the same `mid`, still
    /// in-flight (its dispatch task enqueued but not yet reached by a worker --
    /// easy to hit under load once `takeAndPost` lets the producer run well
    /// ahead of the workers), can find a *newer* `Gate` already sitting where
    /// the old one used to be. Its `awaitTurn` then waits on that new gate's
    /// independent counter for a ticket number it will never produce --
    /// permanently (morph#519's own fix introduced this: the old two-step
    /// `take()`-then-post() throttled the producer just enough that a gate
    /// essentially never drained mid-burst; the atomic `takeAndPost` removes
    /// that throttle). `Ticket` closes this by carrying the exact `Gate`
    /// `shared_ptr` a ticket was issued from, so `awaitTurn(Ticket)` and
    /// `release(Ticket)` operate on that object directly -- never a fresh
    /// lookup, so a drain-and-recreate of the map entry cannot redirect them.
    ///
    /// Default-constructed to the empty state (`empty()` true), mirroring a
    /// disengaged `std::optional` -- `handleImpl`'s non-`execute` path passes
    /// one of these to mean "no ticket was ever taken".
    class Ticket {
    public:
        /// @brief Constructs the empty ticket (no ticket taken).
        Ticket() = default;

        /// @brief True if this handle carries no ticket.
        [[nodiscard]] bool empty() const noexcept { return _gate == nullptr; }

    private:
        friend class ExecuteOrderGate;

        Ticket(::morph::exec::detail::ModelId mid, std::uint64_t number, std::shared_ptr<Gate> gate)
            : _mid{mid}, _number{number}, _gate{std::move(gate)} {}

        ::morph::exec::detail::ModelId _mid{};
        std::uint64_t _number{0};
        std::shared_ptr<Gate> _gate;
    };

    /// @brief Hands out the next ticket for @p mid, in call order.
    ///
    /// Intended to be called synchronously, on whatever thread received the
    /// request, before anything asynchronous happens -- the ticket numbers
    /// two calls receive for the same `mid` are therefore always in the
    /// order this was called in.
    ///
    /// This bare-number ticket, and the by-`ModelId` `awaitTurn`/`release`
    /// overloads below that go with it, still carry the exact erase-and-
    /// recreate hazard `Ticket`'s own doc comment describes: nothing here
    /// stops @p mid's gate from fully draining and being replaced by a new
    /// one between this call and a later `awaitTurn(mid, ticket)` or
    /// `release(mid, ticket)` on the same number. Do not use this trio to
    /// drive a ticket across an asynchronous gap -- take it via `takeTicket`
    /// or `takeAndPost` instead, and drive it through the `Ticket`-typed
    /// overloads. This overload exists for tests that exercise the gate's
    /// raw counter/out-of-order-release state machine directly, and for
    /// `takeAndPost`'s own internals, which never let this gap open.
    /// @param mid The model the upcoming turn is for.
    /// @return This call's ticket number.
    [[nodiscard]] std::uint64_t take(::morph::exec::detail::ModelId mid) {
        std::scoped_lock const lock{_mtx};
        return getOrCreateGateLocked(mid)->nextTicket++;
    }

    /// @brief Hands out the next ticket for @p mid, in call order, as a
    ///        `Ticket` bound to the gate it came from.
    ///
    /// Not atomic with any enqueue step the way `takeAndPost` is -- this is
    /// for callers that only need a valid, correctly-bound `Ticket` to test
    /// or drive `awaitTurn(Ticket)`/`release(Ticket)`/`ExecuteTicketGuard`
    /// directly, without a real dispatch pipeline behind it.
    /// @param mid The model the upcoming turn is for.
    /// @return This call's ticket, bound to @p mid's current gate.
    [[nodiscard]] Ticket takeTicket(::morph::exec::detail::ModelId mid) {
        std::shared_ptr<Gate> gate;
        std::uint64_t number = 0;
        {
            std::scoped_lock const lock{_mtx};
            gate = getOrCreateGateLocked(mid);
            number = gate->nextTicket++;
        }
        return Ticket{mid, number, std::move(gate)};
    }

    /// @brief Atomically hands out the next ticket for @p mid and invokes
    ///        @p postFn with a `Ticket` bound to it, holding @p mid's own
    ///        take-then-enqueue window open (via `Gate::enqueueMtx`) for the
    ///        whole call.
    ///
    /// `take()` followed by a separate, unlocked enqueue (e.g. to a worker
    /// pool) lets two concurrent callers' ticket numbers and enqueue order
    /// diverge: caller A can take ticket 0 and then be pre-empted before
    /// enqueueing, while caller B takes ticket 1 and enqueues immediately — so
    /// a pool worker picks up ticket 1 first, blocks in `awaitTurn` waiting for
    /// ticket 0, and A's own enqueued work never gets a worker to run on
    /// (morph#519: exactly this, with `RemoteServer::handleImpl` and its worker
    /// pool). Folding the enqueue into the same critical section as `take`
    /// makes that divergence impossible: whichever caller's `takeAndPost` runs
    /// first for a given model gets both the lower ticket number and the
    /// earlier enqueue slot, for any thread scheduling, because a second
    /// concurrent caller *for that same model* cannot even take its ticket
    /// until the first has finished enqueueing.
    ///
    /// Deliberately per-model (`Gate::enqueueMtx`) rather than one mutex shared
    /// across every model, and deliberately separate from `_mtx`. Both choices
    /// exist for the same reason: `postFn` runs while `enqueueMtx` is held, and
    /// with a synchronous (inline) executor `postFn`'s `pool.post(...)` call
    /// can run the *whole* dispatch chain before returning — including a
    /// re-entrant call back into this gate. A re-entrant `awaitTurn`/`release`
    /// for the *same* model only ever touches `_mtx` (separate from
    /// `enqueueMtx`, so no self-relock there), and a re-entrant `takeAndPost`
    /// for a *different* model locks that other model's own `enqueueMtx` (not
    /// this one) precisely because the lock lives per-`Gate` rather than on
    /// the gate object as a whole.
    ///
    /// @param mid    The model the upcoming turn is for.
    /// @param postFn Invoked synchronously, with a `Ticket` for this call,
    ///               while @p mid's own take-then-enqueue window is held open
    ///               (via that model's `Gate::enqueueMtx`) — but *not* while
    ///               `_mtx` (the ticket-bookkeeping lock `take`, `awaitTurn`
    ///               and `release` use) is held. If it throws, the ticket is
    ///               released before the exception propagates, exactly as if
    ///               it had never been taken.
    template <typename PostFn>
    void takeAndPost(::morph::exec::detail::ModelId mid, PostFn&& postFn) {
        std::shared_ptr<Gate> gate;
        {
            std::scoped_lock const lock{_mtx};
            gate = getOrCreateGateLocked(mid);
        }
        // Per-model, not global: a synchronous (inline) executor can run
        // postFn's whole dispatch chain before returning, and a handler that
        // makes a nested execute call against a *different* model must not
        // contend with -- let alone re-lock -- the same mutex this call is
        // still holding. Keeping `gate` alive locally means this lock stays
        // valid even if `_gates[mid]`'s entry is (much later) erased.
        std::scoped_lock const enqueueLock{gate->enqueueMtx};
        std::uint64_t number = 0;
        {
            std::scoped_lock const lock{_mtx};
            number = gate->nextTicket++;
        }
        Ticket const ticket{mid, number, gate};
        try {
            std::forward<PostFn>(postFn)(ticket);
        } catch (...) {
            release(ticket);
            throw;
        }
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

    /// @brief Blocks until @p ticket's turn comes, on the exact `Gate` it was
    ///        issued from -- immune to that model's map entry being erased
    ///        and recreated while this ticket was in flight (see `Ticket`'s
    ///        own doc comment). A no-op if @p ticket is empty.
    /// @param ticket A ticket from `takeAndPost`.
    void awaitTurn(const Ticket& ticket) {
        if (ticket.empty()) {
            return;
        }
        std::unique_lock lock{_mtx};
        auto& gate = *ticket._gate;
        gate.cv.wait(lock, [&gate, &ticket] { return gate.nextToRun == ticket._number; });
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
            // A supported call, not an impossibility: the file-level contract
            // above lists "tolerate a gate already erased" as a first-class
            // element, and tests/test_execute_order_gate.cpp names the case
            // ("releasing a ticket for a model with no gate entry is a
            // harmless no-op").
            return;
        }
        // `iter` already names the exact entry `gate` came from, so pass it
        // straight through -- no need to re-derive it by another `_gates.find`
        // the way `release(const Ticket&)` has to (there, `ticket._gate` may
        // no longer be the current entry at all; see `releaseOnGateLocked`).
        releaseOnGateLocked(iter, ticket);
    }

    /// @brief Releases @p ticket on the exact `Gate` it was issued from --
    ///        immune to that model's map entry being erased and recreated
    ///        while this ticket was in flight (see `Ticket`'s own doc
    ///        comment). A no-op if @p ticket is empty.
    /// @param ticket A ticket from `takeAndPost`.
    void release(const Ticket& ticket) {
        if (ticket.empty()) {
            return;
        }
        std::scoped_lock const lock{_mtx};
        Gate& gate = *ticket._gate;
        if (!advanceOnReleaseLocked(gate, ticket._number)) {
            return;
        }
        // Unlike `release(mid, ticket)`, which already holds the exact
        // iterator `gate` came from, `ticket._gate` may no longer be the
        // entry currently sitting at `_gates[ticket._mid]` -- it is immune to
        // being *misdirected* by an erase-and-recreate (that is the whole
        // point of `Ticket`), but that immunity means it must independently
        // re-check whether it is still current before erasing it. Comparing
        // the stored pointer is a cheap, explicit invariant check rather than
        // a silent assumption.
        auto iter = _gates.find(ticket._mid);
        if (iter != _gates.end() && iter->second.get() == &gate) {
            _gates.erase(iter);
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
        // `releaseOnGateLocked` (issue #449). Ordered, because the release
        // loop consumes it from the front; small by construction (it holds at
        // most the tickets in flight for one model, minus one).
        std::set<std::uint64_t> releasedOutOfOrder;
        std::condition_variable cv;
        // Serialises `takeAndPost` calls for *this* model only (morph#519).
        // Per-model rather than a single mutex shared across every `Gate`, so
        // that a synchronous executor's re-entrant `takeAndPost` call for a
        // *different* model contends for a different lock rather than trying
        // to re-lock this same one from the same thread. See `takeAndPost`'s
        // own doc comment for the full reasoning.
        std::mutex enqueueMtx;
    };

    /// @brief Returns @p mid's `Gate`, creating it if this is its first ticket.
    ///        Assumes `_mtx` is already held by the caller.
    /// @param mid The model to find or create a gate for.
    /// @return The (possibly just-created) gate for @p mid.
    [[nodiscard]] std::shared_ptr<Gate>& getOrCreateGateLocked(::morph::exec::detail::ModelId mid) {
        auto& slot = _gates[mid];
        if (!slot) {
            slot = std::make_shared<Gate>();
        }
        return slot;
    }

    /// @brief Advances @p gate's ticket bookkeeping for @p ticket's release,
    ///        assuming `_mtx` is already held by the caller. Shared by both
    ///        `release()` overloads.
    /// @param gate   The gate @p ticket was issued from.
    /// @param ticket The ticket to release.
    /// @return `true` if this was the release that leaves every ticket
    ///         `gate` has ever handed out released (i.e. `gate` is now fully
    ///         drained and its map entry, if it still has one, may be
    ///         erased); `false` otherwise, in which case any waiter is
    ///         notified instead.
    [[nodiscard]] static bool advanceOnReleaseLocked(Gate& gate, std::uint64_t ticket) {
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
            return true;
        }
        gate.cv.notify_all();
        return false;
    }

    /// @brief `release(mid, ticket)`'s implementation, assuming `_mtx` is
    ///        already held by the caller and @p iter names the exact entry
    ///        `iter->second` was issued from -- so, unlike
    ///        `release(const Ticket&)`, no identity re-check is needed before
    ///        erasing: nothing could have replaced `*iter` since the caller
    ///        looked it up, still under the same `_mtx` critical section.
    /// @param iter   Iterator into `_gates`, naming @p ticket's gate.
    /// @param ticket The ticket to release.
    void releaseOnGateLocked(std::unordered_map<::morph::exec::detail::ModelId, std::shared_ptr<Gate>,
                                                ::morph::exec::detail::ModelIdHash>::iterator iter,
                             std::uint64_t ticket) {
        if (advanceOnReleaseLocked(*iter->second, ticket)) {
            _gates.erase(iter);
        }
    }

    mutable std::mutex _mtx;
    std::unordered_map<::morph::exec::detail::ModelId, std::shared_ptr<Gate>, ::morph::exec::detail::ModelIdHash>
        _gates;
};

/// @brief Owns a taken `ExecuteOrderGate` ticket and releases it on every exit
///        path, unless ownership was explicitly handed on.
///
/// A ticket handed out by `ExecuteOrderGate::takeAndPost` must be released
/// exactly once by whatever path took it: an unreleased ticket permanently
/// stalls every later ticket for the same model, because `awaitTurn` is a
/// `cv.wait` with no deadline. In `RemoteServer` (the sole caller today) that
/// rule used to be a per-call-site convention, and the convention was missed
/// twice: by a shutdown gate that returns before the one place that released
/// a ticket (issue #348), and by every exception that unwinds past a dispatch
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
    /// @param ticket This call's ticket from `ExecuteOrderGate::takeAndPost`,
    ///               or the empty `Ticket` if it took none, in which case the
    ///               guard is inert.
    ExecuteTicketGuard(ExecuteOrderGate& gate MORPH_LIFETIMEBOUND, ExecuteOrderGate::Ticket ticket) noexcept
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
        if (_ticket.empty()) {
            return;
        }
        // Cleared before the release, not after: this guard owes nothing
        // further from here on, so the destructor cannot release twice even
        // if `ExecuteOrderGate::release` itself exits by exception.
        auto held = std::exchange(_ticket, ExecuteOrderGate::Ticket{});
        _gate.release(held);
    }

    /// @brief Gives up ownership *without* releasing, for the one path that
    ///        hands the ticket on to a new owner.
    void disarm() noexcept { _ticket = ExecuteOrderGate::Ticket{}; }

    /// @brief Blocks until the held ticket's turn comes (see
    ///        `ExecuteOrderGate::awaitTurn`). A no-op if no ticket is held.
    void awaitTurn() {
        if (!_ticket.empty()) {
            _gate.awaitTurn(_ticket);
        }
    }

private:
    ExecuteOrderGate& _gate;
    ExecuteOrderGate::Ticket _ticket;
};

}  // namespace morph::backend::detail

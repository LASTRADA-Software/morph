// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <atomic>
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
/// The per-model execute-ordering gate `RemoteServer` dispatches through. Its
/// contract — hand out monotonic tickets per `ModelId`; block a ticket until
/// its predecessor releases; tolerate releases arriving out of order; erase the
/// gate when drained; tolerate a gate already erased — touches no wire format,
/// no authorizer, no model, no executor and no strand, so it lives here rather
/// than inside the server.
///
/// `takeAndPost` takes a ticket and enqueues the work in one atomic step, the
/// `Ticket` it returns is bound to the exact `Gate` it was issued from (so a
/// drain-and-recreate cannot redirect a later `awaitTurn`), and a gate-wide
/// `std::recursive_mutex` is held across the caller's `postFn`. Read
/// `takeAndPost`'s doc before reasoning about lock order here: it is
/// `_enqueueMtx` then `_mtx`, never the reverse. One mutex *per model* instead
/// deadlocks two threads against each other.
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
    /// permanently. The atomic `takeAndPost` is what makes this reachable: a
    /// two-step `take()`-then-post() throttles the producer just enough that a
    /// gate essentially never drains mid-burst, and `takeAndPost` removes that
    /// throttle. `Ticket` closes the hole by carrying the exact `Gate`
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

        /// @param mid      The model this ticket orders against.
        /// @param number    The ticket number minted for it.
        /// @param gate      The exact `Gate` it was issued from.
        /// @param released  The shared "already released" flag. Passed in rather
        ///        than allocated here so the caller can allocate it *before*
        ///        minting @p number: `make_shared` can throw `bad_alloc`, and a
        ///        throw between `nextTicket++` and the ticket becoming owned
        ///        strands that number forever -- `nextToRun` can then never
        ///        reach `nextTicket`, the gate entry is never erased, and every
        ///        later ticket for the model parks in the deadline-less
        ///        `awaitTurn`.
        Ticket(::morph::exec::detail::ModelId mid, std::uint64_t number, std::shared_ptr<Gate> gate,
               std::shared_ptr<std::atomic<bool>> released)
            : _mid{mid}, _number{number}, _gate{std::move(gate)}, _released{std::move(released)} {}

        ::morph::exec::detail::ModelId _mid{};
        std::uint64_t _number{0};
        std::shared_ptr<Gate> _gate;
        // Shared across every copy of this logical ticket -- `Ticket` is
        // freely copyable and both `ExecuteTicketGuard` (via `dispatchMessage`)
        // and `takeAndPost`'s own exception path can independently decide to
        // release the "same" ticket (a non-`std::exception` throw escaping
        // dispatchMessage's `catch (const std::exception&)` unwinds through
        // both). `release(Ticket)` claims this flag with `exchange(true)`
        // before doing any work, so only the first caller actually runs the
        // release logic -- a second call is a silent no-op instead of
        // inserting an already-passed ticket number into `releasedOutOfOrder`
        // a second time, which would sit there as the set's permanent
        // minimum and quietly break every future out-of-order release for
        // this gate.
        std::shared_ptr<std::atomic<bool>> _released;
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
        // Allocated before the number is minted -- same reason as `takeAndPost`:
        // a throw between `nextTicket++` and the owning `Ticket` strands that
        // number, and nothing can ever release it.
        auto released = std::make_shared<std::atomic<bool>>();
        std::shared_ptr<Gate> gate;
        std::uint64_t number = 0;
        {
            std::scoped_lock const lock{_mtx};
            gate = getOrCreateGateLocked(mid);
            number = gate->nextTicket++;
        }
        return Ticket{mid, number, std::move(gate), std::move(released)};
    }

    /// @brief Atomically hands out the next ticket for @p mid and invokes
    ///        @p postFn with a `Ticket` bound to it, holding the gate's
    ///        take-then-enqueue window open for the whole call.
    ///
    /// `take()` followed by a separate, unlocked enqueue (e.g. to a worker
    /// pool) lets two concurrent callers' ticket numbers and enqueue order
    /// diverge: caller A can take ticket 0 and then be pre-empted before
    /// enqueueing, while caller B takes ticket 1 and enqueues immediately — so
    /// a pool worker picks up ticket 1 first, blocks in `awaitTurn` waiting for
    /// ticket 0, and A's own enqueued work never gets a worker to run on --
    /// reachable with `RemoteServer::handleImpl` and its worker pool.
    /// Folding the enqueue into the same critical section as `take`
    /// makes that divergence impossible: whichever caller's `takeAndPost` runs
    /// first for a given model gets both the lower ticket number and the
    /// earlier enqueue slot, for any thread scheduling, because a second
    /// concurrent caller *for that same model* cannot even take its ticket
    /// until the first has finished enqueueing.
    ///
    /// The gate's **one** enqueue mutex is acquired first, with no other lock
    /// held, and `_mtx` only after — never the other way round.
    ///
    /// @par Why one mutex and not one per model
    /// `enqueueLock` is deliberately held across `postFn`, and `postFn` is
    /// opaque: on a `ThreadPoolExecutor` it only enqueues, but on a synchronous
    /// executor it runs the entire dispatch chain inline, including any
    /// re-entrant `takeAndPost` that chain triggers. With a mutex *per model*
    /// that gave two threads two locks to take in opposite orders — thread A
    /// inside model 1's callback reaching for model 2, thread B the mirror —
    /// and they deadlocked. Measured, not theorised: a two-thread probe against
    /// the per-model version hung every time, while `master` (which held no
    /// lock across a caller callback at all) could not exhibit it.
    ///
    /// One mutex cannot form a cycle with itself, so that class of deadlock is
    /// gone by construction. `std::recursive_mutex` because a synchronous
    /// executor can re-enter `takeAndPost` on the *same* thread, which a plain
    /// mutex would self-deadlock on.
    ///
    /// The cost is real and accepted: `takeAndPost` now serialises across every
    /// model rather than per model, and on a synchronous executor that means
    /// serialising the dispatch those callbacks perform.
    ///
    /// @par What this buys back
    /// Because the enqueue mutex is taken *before* `_mtx` and never inside it,
    /// the gate fetch and the ticket increment can share one continuous `_mtx`
    /// hold again. That closes the window a split hold left open — a concurrent
    /// `release()` draining and erasing this exact gate between "fetched" and
    /// "ticket assigned", minting this ticket on an orphaned generation while a
    /// racing `takeAndPost(mid)` gets a fresh one — with no re-check or retry
    /// loop needed.
    ///
    /// @param mid    The model the upcoming turn is for.
    /// @param postFn Invoked synchronously, with a `Ticket` for this call,
    ///               while the enqueue mutex is held — but *not* while `_mtx`
    ///               (the ticket-bookkeeping lock `awaitTurn` and `release`
    ///               use) is held. If it throws, the ticket is released before
    ///               the exception propagates, exactly as if it had never been
    ///               taken.
    template <typename PostFn>
    void takeAndPost(::morph::exec::detail::ModelId mid, PostFn&& postFn) {
        // Allocated before a ticket number exists, so nothing between the mint
        // and the owning `Ticket` can throw. See `Ticket`'s constructor.
        auto released = std::make_shared<std::atomic<bool>>();

        // First, and with no other lock held. Held across postFn, including any
        // re-entrant takeAndPost it triggers.
        std::scoped_lock const enqueueLock{_enqueueMtx};

        std::shared_ptr<Gate> gate;
        std::uint64_t number = 0;
        {
            std::scoped_lock const lock{_mtx};
            gate = getOrCreateGateLocked(mid);
            number = gate->nextTicket++;
        }
        Ticket const ticket{mid, number, gate, std::move(released)};
        try {
            std::forward<PostFn>(postFn)(ticket);
        } catch (...) {
            release(ticket);
            throw;
        }
        // `enqueueLock` releases here, at the end of scope -- after postFn,
        // including any reentrant takeAndPost(mid) it triggered, is done.
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
        // `>=`, not `==`. `nextToRun` is the lowest ticket not yet released, so
        // once it has moved *past* this ticket's number the turn has come and
        // gone and there is nothing left to wait for. An exact match parks
        // forever in that case, and it is reachable: `Ticket` is deliberately
        // copyable with two owners that can each release it, so if
        // `takeAndPost`'s `catch (...)` releases after `postFn` already enqueued
        // a task holding its own copy, that task's `awaitTurn` would wait on a
        // number `nextToRun` has already passed. There is no deadline here, so
        // "wait forever" means a parked pool worker for the life of the process.
        // Mirrors the by-ModelId overload, which returns immediately once the
        // gate entry is gone.
        gate.cv.wait(lock, [&gate, &ticket] { return gate.nextToRun >= ticket._number; });
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
    ///        comment). A no-op if @p ticket is empty **or already released**
    ///        -- `Ticket` is copyable and two independent owners of the same
    ///        logical ticket (`ExecuteTicketGuard` and `takeAndPost`'s own
    ///        exception path both can be, when a non-`std::exception` throw
    ///        escapes `dispatchMessage`'s narrower catch) can each decide to
    ///        release it; only the first one actually does (see `Ticket`'s
    ///        own doc comment on `_released`).
    /// @param ticket A ticket from `takeAndPost`.
    void release(const Ticket& ticket) {
        if (ticket.empty()) {
            return;
        }
        std::scoped_lock const lock{_mtx};
        // Claimed *under* `_mtx` and *after* the bookkeeping, not before it.
        // Every release serialises on this mutex, so a plain load/store here is
        // still exactly-once -- but claiming the flag first meant a throw in the
        // locked section below (the `releasedOutOfOrder` node allocation can
        // throw `bad_alloc`) left the ticket permanently un-releasable by every
        // owner, with `nextToRun` never reaching `nextTicket`, the gate entry
        // never erased, and every later `awaitTurn` for that model blocked
        // forever. Leaving the flag clear on a throw lets the other owner retry.
        if (ticket._released->load()) {
            return;
        }
        Gate& gate = *ticket._gate;
        bool const drained = advanceOnReleaseLocked(gate, ticket._number);
        ticket._released->store(true);
        if (!drained) {
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
        // `releaseOnGateLocked`. Ordered, because the release
        // loop consumes it from the front; small by construction (it holds at
        // most the tickets in flight for one model, minus one).
        std::set<std::uint64_t> releasedOutOfOrder;
        std::condition_variable cv;
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
        // shutdown gate -- all do exactly this). Applying a later ticket's
        // release directly would push `nextToRun` straight past an earlier
        // ticket's number, leaving that ticket's waiter with a predicate that
        // can never become true again -- a caller parked forever.
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
        // Notify before the drained-path return, not only on the other branch.
        // "Fully drained" means every ticket handed out has released -- but a
        // waiter can still be parked on one of them (see `awaitTurn`'s note on
        // the two owners of a `Ticket`), and returning without a notify left it
        // asleep on a predicate that had already become true.
        gate.cv.notify_all();
        // True means: safe to drop the entry, so a model with no in-flight
        // tickets leaves no trace in this map. Reaching `nextTicket` this way
        // means every ticket handed out has released, so `releasedOutOfOrder`
        // is necessarily empty here.
        return gate.nextToRun == gate.nextTicket;
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

    // Serialises `takeAndPost` from first lock to after `postFn` returns, so a
    // ticket number and the enqueue that follows it cannot interleave with
    // another call's. **One mutex for the whole gate, not one per model** --
    // see `takeAndPost` for the two-thread cross-model deadlock a per-model
    // mutex allowed, and for what the single lock costs.
    //
    // Recursive because a synchronous executor can run `postFn`'s whole
    // dispatch chain inline, including a re-entrant `takeAndPost` on this same
    // thread, which a plain mutex would self-deadlock on. Same-thread
    // re-entrancy is never concurrent with itself, so it needs no ordering
    // against itself -- only against other threads, which is exactly what a
    // recursive mutex still provides.
    //
    // Lock order: this, then `_mtx`. Never the reverse.
    mutable std::recursive_mutex _enqueueMtx;
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
/// two ways: by a shutdown gate that returns before the one place that
/// releases a ticket, and by an exception that unwinds past a dispatch
/// function's early returns. This holder makes the rule
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

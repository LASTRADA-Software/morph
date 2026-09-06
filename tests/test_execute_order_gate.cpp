// SPDX-License-Identifier: Apache-2.0

// Direct tests for the per-model execute-ordering gate extracted out of
// RemoteServer (include/morph/core/detail/execute_order_gate.hpp).
//
// The point of the extraction, and of this file, is cost: `take`/`awaitTurn`/
// `release` are a pure function of an in-memory map and a couple of counters
// -- no socket, no ThreadPoolExecutor, no IAuthorizer, no wire envelopes, no
// RemoteServer at all. Before this extraction, reaching the "gate already
// erased" defensive branches and the out-of-order-release mechanism
// (`releasedOutOfOrder`, issue #449) cost `tests/test_remote_execute_ordering.cpp`
// a full ThreadPoolExecutor, bespoke IAuthorizer subclasses that force
// deterministic interleaving, a real register round-trip, and hand-encoded
// envelopes. Here they are a handful of synchronous calls.
//
// tests/test_remote_execute_ordering.cpp still exists alongside this file --
// it keeps the cases that need RemoteServer's real dispatch path (send-order
// preservation through handle()/dispatchExecute, and the shutdown-gate/throw
// interactions that only manifest through that real call sequence) -- but the
// gate's own internal state machine, including the #449 mechanism, now has
// its direct coverage here instead of only being reachable by forcing thread
// interleavings through the whole server.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <morph/core/detail/execute_order_gate.hpp>
#include <optional>
#include <thread>
#include <utility>

#include "test_support.hpp"

namespace {

using ::morph::backend::detail::ExecuteOrderGate;
using ::morph::backend::detail::ExecuteTicketGuard;
using ::morph::exec::detail::ModelId;

}  // namespace

// ── take / awaitTurn / release: normal ordering ─────────────────────────────

TEST_CASE("ExecuteOrderGate: a single ticket's turn is immediate and releasing it drains the gate",
          "[remote][execute-order-gate]") {
    ExecuteOrderGate gate;
    ModelId const mid{1};
    CHECK(gate.gateCount() == 0U);

    auto const ticket = gate.take(mid);
    CHECK(ticket == 0U);
    CHECK(gate.gateCount() == 1U);

    // nextToRun starts at 0, so ticket 0's turn has already come -- this must
    // return immediately, not block.
    gate.awaitTurn(mid, ticket);

    gate.release(mid, ticket);
    // The last outstanding ticket for mid released -- the entry is erased, not
    // merely emptied, so the gate never grows unbounded across a server's
    // lifetime.
    CHECK(gate.gateCount() == 0U);
}

TEST_CASE("ExecuteOrderGate: tickets for the same model are handed out in call order and released in that order",
          "[remote][execute-order-gate]") {
    ExecuteOrderGate gate;
    ModelId const mid{1};

    auto const first = gate.take(mid);
    auto const second = gate.take(mid);
    auto const third = gate.take(mid);
    CHECK(first == 0U);
    CHECK(second == 1U);
    CHECK(third == 2U);
    CHECK(gate.gateCount() == 1U);

    // Released strictly in ticket order: each awaitTurn call must be able to
    // return immediately, since nextToRun always matches the ticket about to
    // be awaited here.
    gate.awaitTurn(mid, first);
    gate.release(mid, first);
    gate.awaitTurn(mid, second);
    gate.release(mid, second);
    gate.awaitTurn(mid, third);
    gate.release(mid, third);
    CHECK(gate.gateCount() == 0U);
}

TEST_CASE("ExecuteOrderGate: different models are tracked independently", "[remote][execute-order-gate]") {
    ExecuteOrderGate gate;
    ModelId const midA{1};
    ModelId const midB{2};

    auto const a0 = gate.take(midA);
    auto const b0 = gate.take(midB);
    CHECK(a0 == 0U);
    CHECK(b0 == 0U);  // Independent counters -- midB's first ticket is also 0.
    CHECK(gate.gateCount() == 2U);

    gate.release(midA, a0);
    CHECK(gate.gateCount() == 1U);  // midA drained; midB still outstanding.
    gate.release(midB, b0);
    CHECK(gate.gateCount() == 0U);
}

// ── defensive branches: gate already erased ─────────────────────────────────

TEST_CASE("ExecuteOrderGate: awaitTurn against a model with no gate entry returns immediately",
          "[remote][execute-order-gate]") {
    // Mirrors what happens once every ticket for a model has released and the
    // map entry is gone: a caller that still holds a (now-meaningless) ticket
    // number must not block waiting on a gate that no longer exists.
    ExecuteOrderGate gate;
    ModelId const mid{1};
    CHECK(gate.gateCount() == 0U);
    gate.awaitTurn(mid, 0);  // Would hang forever if this dereferenced a missing entry.
    CHECK(gate.gateCount() == 0U);
}

TEST_CASE("ExecuteOrderGate: releasing a ticket for a model with no gate entry is a harmless no-op",
          "[remote][execute-order-gate]") {
    // The "should not happen" defensive branch: every ticket's own take()
    // creates the entry, but a caller can still observe it already gone if
    // some other ticket's release already erased it first (exactly the
    // interleaving the out-of-order test below forces deliberately).
    ExecuteOrderGate gate;
    ModelId const mid{1};
    gate.release(mid, 0);  // No entry exists at all; must not throw or crash.
    CHECK(gate.gateCount() == 0U);
}

TEST_CASE("ExecuteOrderGate: awaitTurn and release both cope once a gate has fully drained mid-sequence",
          "[remote][execute-order-gate]") {
    // A two-ticket version of the "already gone" scenario: ticket 0 is taken
    // but never awaited/released until after ticket 1 has already drained the
    // gate (releasing in order, so no out-of-order recording is involved
    // here -- that mechanism gets its own dedicated test below).
    ExecuteOrderGate gate;
    ModelId const mid{1};
    auto const t0 = gate.take(mid);
    auto const t1 = gate.take(mid);
    CHECK(gate.gateCount() == 1U);

    gate.release(mid, t0);  // nextToRun 0 -> 1; gate stays (t1 still outstanding).
    CHECK(gate.gateCount() == 1U);
    gate.release(mid, t1);  // nextToRun 1 -> 2 == nextTicket; erased.
    CHECK(gate.gateCount() == 0U);

    // A caller that raced its own awaitTurn/release against the above and
    // only reaches them now must find "gate already gone" rather than crash.
    gate.awaitTurn(mid, t0);
    gate.release(mid, t0);
    CHECK(gate.gateCount() == 0U);
}

// ── out-of-order release (issue #449) ───────────────────────────────────────

TEST_CASE(
    "ExecuteOrderGate: an out-of-order release is recorded rather than applied, "
    "and resolves once the gap it was waiting on closes",
    "[remote][execute-order-gate][449]") {
    // Threadless reproduction of the #449 mechanism. Three tickets for one
    // model, released out of ticket order:
    //   0 (never released yet -- the "earlier ticket still outstanding")
    //   1 (released first  -- out of order relative to 0)
    //   2 (released second -- also out of order relative to 0)
    //
    // The naive, pre-#449 implementation unconditionally set
    // `nextToRun = ticket + 1` on every release. Under that logic, releasing
    // ticket 2 second would set nextToRun = 3, which equals nextTicket (3) --
    // and the gate's own "fully drained" check would erase the map entry
    // *before ticket 0 has ever released*, permanently stranding anything
    // still waiting on ticket 0's turn (and, per `awaitTurn`'s "gate already
    // gone" branch, silently letting ticket 0's own eventual wait return
    // immediately without ever having been genuinely resolved).
    //
    // The fix instead holds each out-of-order release aside and only walks
    // `nextToRun` forward over a *contiguous* run starting from the ticket
    // that is actually next. gateCount() is the observable that distinguishes
    // the two: correct behavior keeps the gate alive (ticket 0 is still
    // outstanding) after both out-of-order releases, whereas the naive
    // implementation would have erased it already.
    ExecuteOrderGate gate;
    ModelId const mid{1};
    auto const t0 = gate.take(mid);
    auto const t1 = gate.take(mid);
    auto const t2 = gate.take(mid);
    CHECK(gate.gateCount() == 1U);

    // t1 released first: out of order (nextToRun is still 0). Recorded, not
    // applied -- nextToRun does not move, and in particular does not jump to
    // 2 the way the pre-#449 code would have.
    gate.release(mid, t1);
    CHECK(gate.gateCount() == 1U);  // Still alive: had this "applied" (nextToRun=2), it would stay alive too here...

    // ...so the discriminating step is releasing t2 next, also out of order.
    // Naive `nextToRun = ticket + 1` would now set nextToRun = 3 == nextTicket
    // and erase the entry, even though t0 has never released. The fix must
    // instead keep recording and leave the gate alive.
    gate.release(mid, t2);
    CHECK(gate.gateCount() == 1U);  // Would be 0 here under the pre-#449 bug -- t0 is still outstanding.

    // Now release the first ticket: this closes the gap, so the release loop
    // must walk forward over both recorded out-of-order releases in one go,
    // reaching nextToRun == nextTicket and erasing the entry -- i.e. both t1's
    // and t2's releases "resolve" (take effect) together, right now, rather
    // than having been silently lost or double-counted earlier.
    gate.release(mid, t0);
    CHECK(gate.gateCount() == 0U);

    // And the resolution is real, not just "erased" bookkeeping: a fresh
    // ticket for the same model starts a new epoch at 0 again, proving no
    // stale state (e.g. a leftover releasedOutOfOrder entry) survived.
    auto const next = gate.take(mid);
    CHECK(next == 0U);
    gate.release(mid, next);
    CHECK(gate.gateCount() == 0U);
}

TEST_CASE(
    "ExecuteOrderGate: an out-of-order release unblocks a waiter parked on the ticket ahead of it, "
    "once that ticket releases",
    "[remote][execute-order-gate][449]") {
    // Same mechanism as above, but observed through a genuinely blocked
    // awaitTurn call rather than only through gateCount() -- the real-thread
    // half of the #449 regression, kept minimal (one waiter, one release)
    // since the pure state-machine behavior is already pinned above.
    ExecuteOrderGate gate;
    ModelId const mid{1};
    auto const t0 = gate.take(mid);
    auto const t1 = gate.take(mid);

    std::atomic<bool> t1Turn{false};
    std::thread waiter{[&] {
        gate.awaitTurn(mid, t1);
        t1Turn.store(true);
    }};

    // t1 is not next in line (t0 hasn't released) -- the waiter must still be
    // parked after a short, generous wait.
    REQUIRE_FALSE(morph::testing::waitUntil([&] { return t1Turn.load(); }, std::chrono::milliseconds{100}));

    gate.release(mid, t0);  // Closes the gap: nextToRun 0 -> 1, matching t1.
    REQUIRE(morph::testing::waitUntil([&] { return t1Turn.load(); }));
    waiter.join();

    gate.release(mid, t1);
    CHECK(gate.gateCount() == 0U);
}

// ── ExecuteTicketGuard ───────────────────────────────────────────────────────

TEST_CASE("ExecuteTicketGuard: releases its ticket on destruction", "[remote][execute-order-gate][guard]") {
    ExecuteOrderGate gate;
    ModelId const mid{1};
    {
        auto const ticket = gate.take(mid);
        ExecuteTicketGuard guard{gate, std::make_pair(mid, ticket)};
        CHECK(gate.gateCount() == 1U);
    }  // Destructor releases; nothing else does.
    CHECK(gate.gateCount() == 0U);
}

TEST_CASE("ExecuteTicketGuard: release() is explicit-then-idempotent", "[remote][execute-order-gate][guard]") {
    ExecuteOrderGate gate;
    ModelId const mid{1};
    auto const ticket = gate.take(mid);
    ExecuteTicketGuard guard{gate, std::make_pair(mid, ticket)};
    guard.release();
    CHECK(gate.gateCount() == 0U);
    guard.release();  // Second call: no ticket held, must not double-release.
    CHECK(gate.gateCount() == 0U);
}

TEST_CASE("ExecuteTicketGuard: disarm() gives up ownership without releasing", "[remote][execute-order-gate][guard]") {
    ExecuteOrderGate gate;
    ModelId const mid{1};
    auto const ticket = gate.take(mid);
    {
        ExecuteTicketGuard guard{gate, std::make_pair(mid, ticket)};
        guard.disarm();
    }  // Destructor: ticket already disarmed, must not release it.
    CHECK(gate.gateCount() == 1U);
    // The caller that adopted ownership via disarm() is responsible for the
    // eventual release; simulate that here so the gate is left clean.
    gate.release(mid, ticket);
    CHECK(gate.gateCount() == 0U);
}

TEST_CASE("ExecuteTicketGuard: constructed with no ticket is inert", "[remote][execute-order-gate][guard]") {
    ExecuteOrderGate gate;
    ExecuteTicketGuard guard{gate, std::nullopt};
    guard.awaitTurn();  // No-op: nothing to wait for.
    guard.release();    // No-op: nothing to release.
    CHECK(gate.gateCount() == 0U);
}

TEST_CASE("ExecuteTicketGuard: awaitTurn forwards to the gate for the held ticket",
          "[remote][execute-order-gate][guard]") {
    // A ticket whose turn has already come is not a discriminating case here:
    // a no-op `awaitTurn()` body would pass it identically (confirmed while
    // writing this test -- an earlier version used exactly that shape and
    // stayed green after `ExecuteTicketGuard::awaitTurn()`'s single call-through
    // line was temporarily replaced with a no-op). To actually prove
    // forwarding, hold the guard's ticket *behind* an unreleased earlier one,
    // same shape as the plain-gate blocking test above, and confirm the call
    // genuinely parks and then genuinely resolves.
    ExecuteOrderGate gate;
    ModelId const mid{1};
    auto const t0 = gate.take(mid);
    auto const t1 = gate.take(mid);
    ExecuteTicketGuard guard{gate, std::make_pair(mid, t1)};

    std::atomic<bool> t1Turn{false};
    std::thread waiter{[&] {
        guard.awaitTurn();
        t1Turn.store(true);
    }};

    // t1 is not next in line (t0 hasn't released) -- if awaitTurn() forwarded
    // to nothing (or returned immediately regardless of ticket), this would
    // already be true.
    REQUIRE_FALSE(morph::testing::waitUntil([&] { return t1Turn.load(); }, std::chrono::milliseconds{100}));

    gate.release(mid, t0);  // Closes the gap: nextToRun 0 -> 1, matching t1.
    REQUIRE(morph::testing::waitUntil([&] { return t1Turn.load(); }));
    waiter.join();

    guard.release();
    CHECK(gate.gateCount() == 0U);
}

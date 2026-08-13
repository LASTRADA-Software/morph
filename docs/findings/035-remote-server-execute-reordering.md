---
id: 035
title: "`RemoteServer::handle()` posted every envelope straight to the shared worker pool, so two `execute`s for the same model could reach the model's own strand out of send order"
subsystem: core/remote
severity: major
source: application-ladder CI hardening session (2026-08-11), found via a genuine (non-reproducible-locally) failure of `examples/common/testkit/test_fault_proxy.cpp`'s `FaultProxy::dropReply` test on the `clang-coverage` CI leg
disposition: fixed — a first attempt regressed a different pre-existing test and was reverted (see "Attempt 1"); the second attempt (a per-model execute-ordering ticket) is verified against both regression tests plus the full `morph_tests`/`morph_qt_tests`/ladder suites
test: `tests/test_remote_execute_ordering.cpp` (new, deterministic-by-construction reproduction of the bug); `examples/common/testkit/test_fault_proxy.cpp`'s `FaultProxy::dropReply` (the original, incidental catch — now expected to stop failing intermittently in CI); `tests/test_remote_connection_scope.cpp`'s `closeConnection` in-flight-execute test (the regression guard for attempt 1's mistake)
---

## How this was found

Not from a design review — from CI. The `clang-coverage` leg (the first CI
run this session that got far enough to actually execute the ladder's test
suite, after a string of unrelated build/configure fixes) failed one test
out of 942:

```
FaultProxy::dropReply loses exactly the reply frame of the targeted call
  CHECK( ::morph::ladder::testkit::awaitQt(handler.execute(FaultProbeAdd{100})) == 111 )
  with expansion:
  101 == 111
```

The test's own comment names exactly what a wrong value here means: `1 +
10 + 100` only equals `111` if the middle call (`FaultProbeAdd{10}`, call
2) actually reached the server and committed its effect before the third
call's reply came back. `101` (`1 + 100`) means call 2's effect was
**not yet applied** when call 3's already was — i.e., call 3 was processed
*before* call 2, even though the client issued them in the opposite order
on the same connection.

This did not reproduce locally: dozens of consecutive runs of the same
test binary on this machine (Windows, MSVC) all passed. That is consistent
with a genuine but narrow race window that a slower or more
heavily-loaded runner (a `clang-coverage`-instrumented build under CI,
competing for CPU with everything else GitHub Actions is running on that
host) is more likely to hit than a fast, quiet local machine — not
evidence there was no bug.

## Root cause

`RemoteServer::handle()` (both overloads, `include/morph/core/remote.hpp`)
used to post the **raw, undecoded** message straight to `_pool`, a
multi-worker `ThreadPoolExecutor`:

```cpp
void handle(std::string msg, std::function<void(std::string)> reply) {
    auto self = shared_from_this();
    _pool.post([self, msg = std::move(msg), reply = std::move(reply)]() mutable {
        self->dispatchMessage(msg, reply);
    });
}
```

`dispatchMessage` then did real work — decode, a shutdown check, and (for
`execute`) `dispatchExecute`'s own sequence (rate-limit shed, `authorize`,
`authenticate`, a registry lookup, per-instance `authorizeInstance`, an
in-flight-count reservation) — **before** finally reaching the one place
ordering was actually enforced: `_strand.post(mid, ...)`, a genuine
per-model FIFO queue (`StrandExecutor`, `include/morph/core/strand.hpp`).

`_pool.post()` only guarantees FIFO **dequeue** order across its worker
threads — it says nothing about the order in which two different worker
threads *finish* the pre-strand work ahead of a given task. Two `execute`
envelopes for the *same* model, sent back-to-back on one connection, are
two independent `_pool.post()` calls. With more than one pool worker free,
the second `handle()` call's worker thread could finish `dispatchMessage`
→ `dispatchExecute`'s pre-strand work faster than the first one's and win
the race to `_strand.post(mid, ...)` — reaching the actual per-model FIFO
queue *ahead* of the request the client sent first.

## Attempt 1: strand-route `execute` at `handle()`, reverted

The first fix tried: decode the envelope in `handle()` itself and, for any
`execute` with a known `modelId`, post the *entire*
`dispatchMessage`/`dispatchExecute` call straight to `_strand.post(mid,
...)` instead of `_pool`.

This closed the original race, but broke
`tests/test_remote_connection_scope.cpp`'s `"RemoteServer::closeConnection:
an in-flight execute completes safely across a disconnect"` test, which
deliberately blocks one `execute` inside the target model's `execute()`
body to hold the strand, then asserts a *second*, concurrent `execute` for
the same (now-closed-connection-reclaimed) `modelId` resolves
**immediately** with `"model not found"` — it must never wait on the
blocked model's strand. Attempt 1 moved the registry lookup that decides
"model not found" onto the strand too (since it moved the *whole*
pipeline), so the fast-reject path collapsed into the same queue as the
slow model's in-flight work and deadlocked. Caught locally (`morph_tests`,
never reached CI) and reverted in full.

## Attempt 2 (this fix): a per-model execute-ordering ticket

The real constraint attempt 1 missed: the registry lookup that decides
"model not found" **must** run before any strand involvement, on the pool,
exactly as before — a fast-reject that waits on an unrelated model's
strand is not "slower," it's a hang, per the connection-scope test's own
2-second polling budget racing a deliberately-forever-blocked model.
Ordering therefore cannot be achieved by routing the whole pipeline
through one decision; it has to be achieved by ordering only the *moment*
each call is allowed to make its own `_strand.post()` call, independent of
whether that call is ever reached at all.

The fix adds a lightweight per-model ticket gate (`RemoteServer`'s
`ExecuteGate`/`takeExecuteTicket`/`awaitExecuteTurn`/`releaseExecuteTicket`,
`include/morph/core/remote.hpp`):

- `handle()`'s shared body (`handleImpl`) does a cheap, best-effort decode
  of the incoming message — thrown away either way — and, for an `execute`
  naming a `modelId`, calls `takeExecuteTicket(mid)` **before** posting to
  `_pool`. `handleImpl` runs synchronously, on whatever single thread the
  transport calls `handle()` from, so two tickets for the same model are
  always handed out in the order `handle()` was called — send order.
- The ticket travels with the posted task into `dispatchMessage` →
  `dispatchExecute` as an `std::optional<std::pair<ModelId, ticket>>`
  parameter (never a shared mutable member — two pool threads running
  concurrently must never share mutable per-call state).
- Every early-return branch in `dispatchExecute` that follows the
  ticket-taking point (`server busy` twice, `unauthorized` twice, `model
  not found`) releases the ticket immediately, via a small
  `rejectAndRelease` helper, before replying. None of these ever touch the
  strand, so none of them can be blocked by, or block, anyone else's turn.
- Only immediately before the pre-existing `_strand.post(mid, ...)` call —
  the sole call site this fix actually changes the *timing* of — does the
  code call `awaitExecuteTurn(mid, ticket)`, which blocks (on this pool
  thread, never the strand, never any other model's strand) until every
  earlier ticket for the same model has already made its own
  `_strand.post()` call. It then posts, and releases its own ticket right
  after — not waiting for the strand task itself to run, only for the
  `_strand.post()` call to have happened, which is all the ordering
  guarantee ever needed.

This reconciles both properties: a model-not-found (or any other
early-reject) ticket releases immediately and can never stall anyone else,
while two live executes for the same model always call `_strand.post()` in
send order, regardless of which one's authorize/authenticate/lookup work
happens to finish first.

## Verification

- **New deterministic-by-construction test**,
  `tests/test_remote_execute_ordering.cpp`: real `ThreadPoolExecutor{2}`
  plus a custom `IAuthorizer` (`SlowFirstAuthorizer`) whose `authorize()`
  sleeps 200ms on its first invocation only — guaranteeing call B's
  pre-strand work finishes before call A's on every run, deterministically
  (not a timing hope). Confirmed this test genuinely exercises the bug: run
  against the pre-fix code, it failed 2 of 3 runs (the artificial delay
  makes the race very likely but, being real threads under a real OS
  scheduler, not perfectly deterministic pre-fix — the fix itself is what
  makes the *result* deterministic). Run against the fix, 5/5 clean.
  - A `DeterministicExecutor`-based version (single-threaded, step-driven,
    reusing the ladder's own `strand_interleaver.hpp` harness pattern) was
    tried first and does not work for this bug: it cannot model "B's pool
    thread blocks waiting for A to make progress" without a second real
    thread to make that progress, so a *correct* fix (which makes B
    legitimately wait for A) deadlocks it. `DeterministicExecutor` was
    ported into `tests/test_support.hpp` (`morph::testing`) as part of this
    work regardless — it's core-layer test infrastructure that had no
    business living only under `examples/common/testkit/`, and is now
    available to any future `tests/` regression test that needs a
    single-threaded, hand-stepped executor for a *different* kind of race
    (one that doesn't require two genuinely concurrent threads to
    reproduce).
- `tests/test_remote_connection_scope.cpp`'s full `[connection-scope]` tag
  (20 test cases, including the specific in-flight-execute-across-
  disconnect test attempt 1 broke): passes, completes in under a second —
  no hang.
- Full `morph_tests` suite: 868 test cases / 8631 assertions, all pass.
- `morph_qt_tests`: 63 test cases / 428 assertions, all pass.
- `ladder_pastebin_tests`, `ladder_polls_tests`, `ladder_common_tests`:
  all pass. `ladder_bookmarks_tests`: passes except one already-known,
  already-documented, unrelated pre-existing flake (a Windows temp-file-
  lock race in `test_app.cpp`, present since before this session and
  unrelated to `RemoteServer`).

## What's still open

- No dedicated unit test for the `ExecuteGate` mechanism in isolation
  (`takeExecuteTicket`/`awaitExecuteTurn`/`releaseExecuteTicket` as their
  own contract, independent of `RemoteServer`'s full dispatch pipeline) —
  the coverage here is entirely through `RemoteServer`'s public surface.
  Would be worth adding if this mechanism is ever reused elsewhere.
- The `SlowFirstAuthorizer` technique (sleep the first call to force a
  race) is a reasonable, common pattern for this class of test but is not
  perfectly deterministic pre-fix, as measured above (2/3, not 3/3) — a
  future hardening pass could look at whether a more direct hook (e.g. an
  injectable delay point inside `RemoteServer` itself, gated behind a
  test-only seam) would make the *pre-fix-failure* rate fully
  deterministic too, not just the *post-fix-pass* rate.

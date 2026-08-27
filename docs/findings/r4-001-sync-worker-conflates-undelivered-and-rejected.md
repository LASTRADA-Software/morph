---
id: r4-001
title: SyncWorker's replay budget cannot tell an undelivered replay from a rejected one, so reconnect flaps dead-letter work the server never saw
subsystem: offline
severity: major
source: kanban rung 4, README "Dead-letter is user-facing, not a log line" strain point
disposition: open
test: spec-cited
---

`SyncWorker::ReplayFunction` is `std::function<bool(const std::string&)>`
(`include/morph/offline/sync_worker.hpp:72`), and its contract has exactly two
outcomes: `true` removes the item, `false` (or a throw) counts one failed
attempt (`:60-64`). There is no third outcome for *"this never reached the
server"*.

`run()` acts on that single bit unconditionally
(`include/morph/offline/sync_worker.hpp:131-153`): every failure increments the
item's cumulative counter, writes it back through
`IOfflineQueue::setAttempts()` — durable on both shipped file and SQLite queues
(`sqlite_offline_queue.hpp:258`) — and dead-letters the item at
`kMaxAttempts = 5` (`:168`). A transport failure and a server-side rejection
are therefore charged to the same budget, and the budget survives process
restarts.

The two conditions that make this reachable rather than theoretical are both
documented framework behaviour:

- `ReconnectCoordinator::onOnline()` holds its mutex for the whole retry loop,
  so a flap back offline cannot preempt an in-progress replay; it can only take
  effect through `shouldContinue()` at the next poll
  (`docs/spec/offline/offline.md`, "`onOnline()` holds the mutex for the entire
  retry loop").
- `SyncWorker::stop()` exists but is one-shot and host-driven
  (`sync_worker.hpp:163`); nothing in the framework wires a
  `NetworkMonitor` transition to it, and kanban does not — the rung's only
  `stop()` call anywhere in `examples/kanban/gui_lib/` is its
  `GetEventsSince` poller's, in `board_qml_bridge.cpp`.

So five reconnect flaps — each one replaying into a connection that drops
before the server commits anything — exhaust the budget of every queued item
and drop them all, with the same `DeadLetterSink` call and the same
"N changes could not be synced" GUI state that a genuine server rejection
produces. In kanban this is the entire user-visible offline story: a dragged
card that the server never saw is reported to the user as a change that could
not be applied, and the payload is gone unless the host's sink persisted it
(`docs/spec/offline/offline.md`, Limitations: morph ships no dead-letter
store).

**What should happen instead.** A replay outcome richer than `bool` — enough
for the caller to say "not delivered, do not charge an attempt" — or a
framework-side rule that only a delivered-and-refused replay consumes the
budget. The retry *count* being hard-coded at 5 with no knob is a deliberate,
documented design decision (`offline.md`, Design decisions table) and is not
what this finding disputes; what is missing is the distinction the count is
spent on.

**Verification status.** Read at master `9371c1a0`:
`include/morph/offline/sync_worker.hpp` (the whole file),
`sqlite_offline_queue.hpp:258-263`, `docs/spec/offline/offline.md`
(SyncWorker, ReconnectCoordinator, Limitations, Design decisions), and
`examples/kanban/gui_lib/board_qml_bridge.cpp`'s `enableOfflineQueue` /
`replayMoveTaskPosition`, whose replay lambda returns `false` on any
`onError` and so cannot distinguish the cases either. **Nothing was built or
run**: the five-flap sequence is inferred from the code paths above, not
reproduced. A minimal repro would be an `InMemoryOfflineQueue` plus a
`ReplayFunction` that always returns `false` for transport reasons, asserting
`SyncResult::deadLettered == queue depth` after five `run()` calls — this
finding should carry that test before it is triaged.

**Prior statements of the same fact.** `examples/LADDER.md`'s status notes
already record that the cap "is still hard-coded and still dead-letters
legitimate writes after five flaky reconnects", and this rung's README names
it as an expected strain point. Neither names the *cause* — the missing
outcome distinction — and neither is a filed finding; this file is that.

**What would change the verdict.** Either seam is enough to close it: a
non-boolean replay outcome, or a documented obligation on the host to gate
`run()` on liveness (with `NetworkMonitor` wiring shipped, not left to each
app to rediscover). It should be triaged alongside morph#203's survey of the
offline stack — this is a failure-semantics asymmetry on the `SyncWorker`
path, where the one that survey already catalogued (folded into the
now-closed morph#201) was on the `Model::onBackendChanged()` path.

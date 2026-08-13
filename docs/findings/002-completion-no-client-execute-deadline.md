---
id: 002
title: Completion<T> has no client-side execute deadline
subsystem: core
severity: major
source: IMPLEMENTATION.md rule 3
disposition: fixed
test: tests/test_client_execute_deadline.cpp
---

`Completion<T>` (`include/morph/core/completion.hpp`) provides no timeout or deadline member for client-side execution. Actions dispatched through `BridgeHandler::execute()` have no built-in way for a caller to bound the time they are willing to wait for the result, leaving rung applications to implement their own timeouts via timer-and-callback patterns.

**What happens instead:** apps resort to lower-level mechanisms (QTimer, thread::sleep polling) to enforce their own deadlines, duplicating work that the framework could provide.

**Resolution (rung 3 framework prerequisite, Task 1 of
`docs/superpowers/plans/2026-08-07-ladder-rung3-framework-prereqs.md`).**
`Bridge::setExecuteDeadline(std::chrono::milliseconds)`
(`include/morph/core/bridge.hpp:821`, read back via `executeDeadline()`)
installs a client-side deadline for every subsequent `executeVia()`; when it
elapses first, the pending `Completion` fails with
`morph::backend::ClientTimeoutError` (`include/morph/core/backend.hpp:475`),
a distinct type from the server-raised `TimeoutError` precisely because the
two report different facts (see the table in
`docs/spec/core/completion.md`, "Client-side execute deadline"). As this
finding anticipated, `Completion`/`CompletionState` needed no API change:
the timer races a delayed `setException` against the real reply and
`setException`'s existing idempotence decides the winner. Opt-in and default
disabled (`0` = no deadline), so no existing caller changes behavior, and the
backing `TimeoutScheduler` is constructed lazily on first use.

Covered by `tests/test_client_execute_deadline.cpp`: the default never fires,
a missing reply fails with `ClientTimeoutError`, an on-time reply cancels the
deadline and releases the scheduler entry it pinned, and a real reply
arriving after the deadline is discarded rather than double-resolving.
Rung 3's `EventPoller` (`examples/common/gui/event_poller.hpp`) is the first
consumer — it treats `ClientTimeoutError` as its one retryable failure, which
is the "GetEventsSince on a client timer" case the rung README named as
untestable without this.

**Closed.** The disposition stays `fix-scheduled` only because
`examples/FINDINGS.md` defines no `closed` value; nothing further is
scheduled against it.

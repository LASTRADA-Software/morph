---
id: 014
title: Model::onBackendChanged() runs with no session, so the framework's documented rich-outcome replay seam cannot perform an authenticated replay
subsystem: offline
severity: major
source: lims rung 6, backend-mode matrix (found by driving replay through switchBackend instead of calling the hook directly)
disposition: open
test: examples/lims/tests/test_backend_matrix.cpp (case "onBackendChanged fires on switchBackend, and fails closed with no session")
---

`docs/spec/offline/offline.md` ("Conflict resolution on replay") names
`Model::onBackendChanged()` as *the* seam for replay outcomes richer than
success/failure:

> **Conflict detection and merge/discard are the model's responsibility**, and
> the seam the framework provides for them is `Model::onBackendChanged()`, not
> `SyncWorker`. … This is the path that supports **clean-replay / merge /
> discard** outcomes, because the model — not an opaque `bool` callback —
> decides what each item becomes.

`LocalBackend::notifyBackendChanged` **posts** that call onto the model's own
strand. `morph::session::current()` is a thread-local the *dispatcher*
establishes around an `execute`, and a posted `onBackendChanged` is not an
execute — so the hook runs with `session::current() == nullptr`.

Any replay that has to know *who* is replaying therefore cannot use it. That
is not a niche requirement: a queued write is a write, and every model in this
repo that authorizes anything does so by reading
`session::current()->principal` (`docs/spec/security.md`;
`IMPLEMENTATION.md` rule 1, "Models must re-check their own preconditions and
authorization").

## Measured

`examples/lims/tests/test_backend_matrix.cpp`, case *"onBackendChanged fires
on switchBackend, and fails closed with no session"*:

- A `SampleModel` is given an `IOfflineQueue` through a `HandlerBinding`'s
  `modelFactory` (the public `Bridge::registerHandler(binding)` seam).
- One `QueuedCapture` is enqueued, naming its capturing operator.
- `bridge.switchBackend(...)` fires `onBackendChanged()` on the fresh
  instance.
- The queue drains to empty (every item is `markDone`d, so nothing blocks),
  and **nothing is applied**: `ListResults` is empty, because
  `SampleModel::execute(QueuedCapture)` refused every item with
  `EmptyPrincipalError`.

The framework's own conflict-resolution tests
(`tests/test_conflict_resolution.cpp`) do not surface this because their
`OrderModel` authenticates nothing — it counts payloads.

## Why this is not simply "the app should not authenticate replay"

A queued lab reading carries the identity of the operator who took it, and a
21 CFR Part 11-style trail requires that the stored result name that person.
Accepting the client's asserted `capturedBy` unchecked would make the field
self-authenticating — anyone able to reach the server could file a result
under a colleague's name. So the check has to happen, and it needs a
trustworthy principal, which is exactly what the hook does not provide.

## What lims does instead, and its cost

The rung's supported replay path is a **re-dispatch**: the reconnecting client
drains its own queue and sends each `QueuedCapture` as an ordinary action
through its authenticated `Bridge`, where the session exists.
`execute(QueuedCapture)` is a registered action precisely so this works, and
`test_backend_matrix.cpp` runs it across all three deployment modes.

The cost is that the *drain* half is back in the application (see also
`docs/findings/008`, which reports the same for the enqueue half) — so
neither end of the offline round trip uses a framework seam, and the
`onBackendChanged` implementation `SampleModel` still carries is a
fail-closed backstop rather than the primary path.

## What should happen

Give the hook a session. The options, roughly in increasing order of cost:

1. **Pass one in.** `onBackendChanged(const session::Context&)` (detected the
   same structural way the no-argument form is), with `Bridge` supplying the
   session it last had installed via `setDefaultSession`. That is the
   identity the client authenticated as, which is precisely the identity a
   replay should run under.
2. **Establish the thread-local around the call.**
   `LocalBackend::notifyBackendChanged` (and the `RemoteServer` equivalent)
   wraps the posted call in a `session::detail::ScopedContext` built from the
   same source. No signature change; every existing model keeps working, and
   one that reads `session::current()` starts getting an answer.
3. **Document the limitation** in `docs/spec/offline/offline.md` — that the
   `onBackendChanged` path is for replays that need no identity, and that an
   authenticated replay must be dispatched — so the next application does not
   discover it the way this one did, by writing a suite that passed only
   because it called the hook directly from a thread that happened to have a
   session installed.

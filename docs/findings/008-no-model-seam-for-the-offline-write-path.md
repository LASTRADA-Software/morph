---
id: 008
title: The offline write path (enqueue-on-failure) has no model-side seam, so the ladder's "models are the application" rule cannot hold for it
subsystem: offline
severity: minor
source: lims rung 6, build order §7
disposition: open
test: spec-cited (examples/lims/include/lims/offline/field_outbox.hpp is the workaround)
---

`examples/IMPLEMENTATION.md` rule 1 is unconditional: *"All business logic,
all invariants, and all persistence access live in plain, single-threaded
model classes with typed actions — nothing domain-shaped may live in
presenters, QML, `main()`, or free functions. If logic can't be expressed in a
model, that is a finding."* This is that finding.

`docs/spec/offline/offline.md` ("Ownership: who enqueues") is equally explicit
that the framework provides nothing here:

> **The queue is passive.** … The framework supplies no transport layer that
> would notice a write failed and drop it into the queue, so **detecting an
> offline/failed `execute()` and calling `enqueue()` is the application's
> job.** … The seam is on the *write path*, not inside `morph::offline`.

Its worked example puts that code at the dispatch site — `if
(!monitor.isOnline()) queue.enqueue(serialise(action));` — i.e. in exactly the
place rule 1 forbids.

The read path has a proper model seam and lims uses it:
`SampleModel::onBackendChanged()` drains the queue, classifies each item
against the backend, and flags conflicts, all inside the model. There is no
counterpart for the write path.

## Why a model cannot host it anyway

Even setting the missing seam aside, the machine that has to make the
enqueue decision is the one machine with no model on it. A rung's models live
server-side behind Lightweight/ODBC (`IMPLEMENTATION.md` rule 4's WASM
clause: *"The ladder's WASM clients are remote clients — persistence lives
server-side, behind the model"*). A disconnected field client, by definition,
cannot reach that model — deciding "queue this instead of sending it" is the
first thing it must do and the last thing it can delegate.

So the logic is not merely *placed* outside a model; under the current
architecture it cannot be inside one.

## What lims does instead

`examples/lims/include/lims/offline/field_outbox.hpp` — a small client-side
class that stamps each capture with the sample version it was prepared
against, mints its idempotency key, enqueues it, and advances its own local
view of that version so the client's *next* offline edit chains onto its own
pending one rather than colliding with it (the ODK Central self-conflict
trap the rung README names).

That last part is the reason this is worth filing rather than shrugging at:
the write path is not glue. It carries a real invariant — *a client's own
successive offline edits must chain* — and getting it wrong produces a
client that conflicts with itself. Two rungs from now, kanban's and lims'
versions of that invariant will be two independent implementations of the
same non-obvious rule, in a place the ladder's rules say logic must not live.

## What should happen

Either:

1. A framework-level write-path seam — something like an `OutboxDispatcher`
   wrapping a `Bridge`/`BridgeHandler`, which attempts delivery, traps a
   failure into a supplied `IOfflineQueue`, and hands the application a
   documented hook to stamp per-item metadata (base version, idempotency
   key) before the item is queued. The base-version-chaining rule would then
   have one implementation instead of one per rung; or
2. An explicit disposition in `IMPLEMENTATION.md` rule 1 carving the offline
   write path out as app-layer by design, naming `docs/spec/offline/offline.md`
   as the reason — so the next rung does not re-litigate it and the rule keeps
   its teeth everywhere else.

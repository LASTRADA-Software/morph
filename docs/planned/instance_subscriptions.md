# Instance subscriptions — planned

**Status:** planned, not implemented. This document is a design proposal, not a
description of current behaviour. The authoritative present-tense specs are in
[`docs/spec/`](../spec).

This item **removes** the reactive-draft mechanism described in
[bridge.md](../spec/core/bridge.md) and
[ARCHITECTURE.md](../ARCHITECTURE.md) ("Subscriptions and fielded actions") and
replaces it. See [What this removes](#what-this-removes) for the blast radius.

## Contents

- [The gap this closes](#the-gap-this-closes)
- [The new meaning of `subscribe`](#the-new-meaning-of-subscribe)
- [Why keyed on the result type](#why-keyed-on-the-result-type)
- [Delivery semantics](#delivery-semantics)
- [Wire protocol changes](#wire-protocol-changes)
- [What this removes](#what-this-removes)
- [Rebuilding reactive forms on the new primitive](#rebuilding-reactive-forms-on-the-new-primitive)
- [Reworking `morph::flows`](#reworking-morphflows)
- [API reference](#api-reference)
- [Design decisions](#design-decisions)
- [Failure modes](#failure-modes)
- [Limitations](#limitations)
- [Cross-references](#cross-references)

## The gap this closes

Once instances are shared ([shared_model_instances.md](shared_model_instances.md)),
two handlers — possibly in two different client processes — operate on the same
stateful model. Nothing tells either of them that the other changed it.

morph has no server-initiated message at all. The `Envelope` protocol is
strictly request/reply, and `views.md` records "no live/push list updates" as a
non-goal. The only tool that *sounds* like a subscription,
`BridgeHandler::subscribe<A>()`, is a client-side draft mechanism: it streams
field values into a local draft, fires when `ActionValidator<A>::ready` passes,
and hands the caller that action's result. It never hears about anything anyone
else did.

So a shared instance is currently a shared secret: `a1` and `a2` both hold
account 42, `a2` deposits, and `a1` shows a stale balance until something makes
it ask again.

## The new meaning of `subscribe`

A subscription is keyed on the **result / state type**, and fires whenever a
value of that type is produced on the instance the handler is attached to — by
*any* handler attached to it, on any connection.

```cpp
BridgeHandler<AccountModel, AllowShared> a1{bridge, gui};
BridgeHandler<AccountModel, AllowShared> a2{bridge, gui};
a1.attach(42);
a2.attach(42);

a1.subscribe<AccountInfo>([](AccountInfo a) {
    showBalance(a.balanceMinor);              // fires for a2's work too
});

a2.execute(Deposit{.amountMinor = 5000});     // produces an AccountInfo
// → a1's callback runs, on a1's own gui executor
```

The subscriber names *what it wants to see*, not *what someone else must do to
produce it*. `a1` does not need to know that `Deposit` exists, that `a2` exists,
or that a deposit is what changed the balance — only that an `AccountInfo` is
the shape of the state it renders.

## Why keyed on the result type

Keying on the action type was the other candidate, and it is what the current
draft mechanism does. It is the wrong choice here for three reasons:

- **A subscriber is a renderer, not a caller.** A GUI panel showing a balance
  cares about `AccountInfo`. Requiring it to enumerate every action that might
  produce one (`Deposit`, `Withdraw`, `GetAccount`, `CloseAccount`, and every
  action added later) makes every new action a breaking change for every
  subscriber.
- **It composes with stateful models.** A keyed model's actions are mostly
  keyless mutations of one state; the state type is the stable, meaningful
  identity in that design, and the action set is the volatile part.
- **It is the shape the existing schema layer already assumes.**
  [views.md](../spec/forms/views.md) derives its columns from the query action's
  *row type*, and `AccountInfo` is described there as "the Account model's
  primary result type". The result type is already the thing morph's own
  generation layer treats as the model's public shape.

## Delivery semantics

- **Scope is the instance.** A subscription is bound to the instance the handler
  is attached to at the time it fires, not to the model *type*. Re-pointing the
  handler ([shared_model_instances.md](shared_model_instances.md#re-pointing-not-re-keying))
  moves its subscriptions to the new instance.
- **A handler with no primary receives only its own results.** Nothing else is
  attached, so there is nothing else to hear.
- **Callbacks run on the handler's executor**, exactly as `.then` does today.
  Two handlers in one process with different executors each get their callback
  where they asked for it.
- **The originating handler is notified too.** `a2` executing `Deposit` gets its
  ordinary `Completion` result *and*, if subscribed, its subscription callback.
  Suppressing the echo would force every subscriber to special-case "was this
  mine", which is exactly the bookkeeping the feature exists to remove.
- **Ordering is per instance.** Because every action on an instance runs on that
  instance's strand, notifications are naturally ordered and that order is
  guaranteed. No ordering is guaranteed *between* instances.
- **Delivery is best-effort and unbuffered.** A notification produced while a
  client is disconnected is lost. There is no replay, no cursor, no
  checkpointing. On reconnect a client re-reads state the ordinary way; the
  subscription resumes from then on. This is deliberate — see
  [Limitations](#limitations).
- **Failed actions notify nobody.** A notification is produced from a successful
  result only.
- **One callback per `(handler, result type)`.** Subscribing again replaces the
  previous callback, matching the current cardinality rule.

## Wire protocol changes

This introduces **the first server-initiated message in morph**. Until now every
frame a client receives is a reply to something it sent, and both transports,
the reconnect logic, and the fuzz harness assume it. That assumption ends here,
and every one of those places needs revisiting.

- **A new `subscribe` / `unsubscribe` request pair**, carrying a model id and a
  result type id. The server records `(modelId, resultTypeId) → set<connection>`.
- **A new `notify` server-initiated message**, carrying the model id, the result
  type id, and the result payload. It has no `callId`, because it answers
  nothing.
- **Client dispatch must gain an unsolicited-message path.** `QtWebSocketBackend`
  and `morph::net`'s `SocketBackend` both currently correlate every inbound
  frame to a pending call; an uncorrelated frame is presently an error and must
  become a routed notification.

Subscriptions are connection-scoped and die with the connection, so
`closeConnection` drops them alongside its instance references. They are gated
by `authorize` for the model type — a principal that may not execute against a
model may not subscribe to its results either, or the subscription becomes a
read channel that bypasses authorization.

## What this removes

The reactive-draft mechanism is deleted, not deprecated. Removed API:

| Removed | What it did |
|---|---|
| `subscribe<A>(cb)` *(old meaning)* | Registered a result callback for action `A`'s draft |
| `set<&A::field>(value)` | Streamed one field into the client-side draft |
| `unsubscribe<A>()` *(old meaning)* | Dropped the draft's callback |
| `reset<A>()` | Destroyed the draft |
| in-flight coalescing | Collapsed patches landing during a flight into one re-fire |
| draft persistence across `switchBackend` | Kept drafts alive over a backend swap |

`subscribe` keeps its name with new semantics. The break is loud rather than
silent: the callback's parameter changes from *the action's result* to *the
subscribed type itself*, so existing call sites fail to compile rather than
quietly changing behaviour.

`ActionValidator<A>::ready` **survives**. Its original purpose was gating a
draft fire, but A1 made it the server-side validation hook enforced in the
dispatcher runner and in `Bridge::executeVia`'s `localOp`
([registry.md](../spec/core/registry.md)). It keeps that role and loses the
draft one. Its documentation must be rewritten accordingly — the phrase
"decides whether a partially-built action draft is ready to execute" becomes
wrong.

**Blast radius** (from the current tree):

- `include/morph/core/bridge.hpp` — the draft storage and `set<>`/`reset<>` path.
- `include/morph/forms/flows.hpp` — `FlowSession` is built directly on
  `subscribe<A>` / `unsubscribe<A>`. See below.
- `tests/test_subscription.cpp` (~69 uses), `tests/test_coverage_gaps.cpp`
  (~16), `tests/test_computed_fields.cpp`, `tests/test_coverage_extra.cpp`,
  `tests/test_flows_apps.cpp`, `tests/test_example.cpp`,
  `examples/bank/tests/test_payee.cpp`.
- `src/qt/forms/` — `DynamicForm.qml`'s reactive path and
  `tst_DynamicFormReactive.qml`.
- Docs: `ARCHITECTURE.md`'s "Subscriptions and fielded actions",
  `spec/core/bridge.md`, `spec/forms/workflows_navigation.md`,
  `docs/superpowers/2026-07-06-reactive-forms-bridge.md`, and
  `examples/bank/README.md`.

morph is `0.1.0` and [VERSIONING.md](../spec/VERSIONING.md) reserves exactly
this latitude before 1.0. The removal should still land as one reviewable
change with its replacement, not as a bare deletion.

## Rebuilding reactive forms on the new primitive

The draft mechanism solved a real problem: a form where each widget edits one
field and the UI responds live. Dropping it is only defensible because stateful
models solve the same problem better — by putting the draft **in the model**
instead of in the client.

Before, the draft lived on the client and fired a whole action when a validator
said it was complete:

```cpp
handler.subscribe<ComputeDensity>([](Density d) { show(d); });
handler.set<&ComputeDensity::mass>(m);
handler.set<&ComputeDensity::volume>(v);      // validator passes → fires
```

After, the draft is model state, each edit is an ordinary action, and the UI
subscribes to the state type:

```cpp
handler.subscribe<DraftState>([](DraftState s) { show(s); });
handler.execute(SetMass{.value = m});
handler.execute(SetVolume{.value = v});       // model recomputes, emits DraftState
```

This is more round trips, and that is the honest cost. What it buys: the draft
survives a client restart, two clients editing the same draft see each other,
readiness is decided by the model that owns the rules rather than by a
client-side predicate, and there is one execution path instead of two. It also
removes the in-flight coalescing machinery, whose subtleties exist only because
the draft was remote from its validator.

Forms whose draft genuinely is client-local — a throwaway dialog — should build
the action normally and call `execute` once. That was always the simpler path
and is now the only one.

## Reworking `morph::flows`

`FlowSession<Model, Steps...>` drives each wizard step through
`subscribe<A>` / `unsubscribe<A>` on the step's action type, and is a shipped
feature (E-G8) with its own spec and QML renderer.

Re-expressed on the new primitive, a wizard becomes a **stateful model keyed by
flow instance**: steps are actions against it, the accumulated draft is its
state, and `WizardView.qml` subscribes to that state type instead of to each
step action. This is a better fit than the current design — it gives wizards
resumability and cross-client visibility for free, and removes
`FlowSession`'s per-step subscribe/unsubscribe churn.

It is also a substantial rewrite of a shipped subsystem, and it should be
scoped and specified separately rather than folded into this item. Until it is,
`morph::flows` blocks this removal.

## API reference

| Symbol | Signature | Meaning |
|---|---|---|
| `handler.subscribe<R>(cb)` | `void(std::function<void(R)>)` | Fire `cb` whenever an `R` is produced on the attached instance. Replaces any prior callback for `R`. |
| `handler.unsubscribe<R>()` | `void` | Drop the callback for `R`. |

## Design decisions

- **Keyed on the result type, not the action type.** A subscriber describes what
  it renders, not what someone else must call. Detailed above.
- **The originator is notified too.** No "was this mine" bookkeeping in every
  subscriber.
- **Best-effort, unbuffered, no replay.** Durable streams with cursors and
  checkpoints are a distributed-runtime feature; morph is a UI bridge. A client
  that missed a notification re-reads state, which it already knows how to do.
- **Connection-scoped subscriptions.** They die with the transport, so there is
  no cleanup story beyond the one `closeConnection` already implements.
- **Gated by `authorize` on the model type.** A subscription is a read channel;
  leaving it ungated would let a principal observe results it may not request.
- **The draft mechanism is removed rather than kept alongside.** Two mechanisms
  both named "subscription", with different keying and different scopes, is the
  kind of ambiguity the specs exist to prevent.

## Failure modes

- **A slow or blocked subscriber.** Notifications are posted to the subscriber's
  executor; a subscriber that blocks its executor delays its own callbacks and
  nothing else. It must not be able to stall the producing instance's strand —
  the notification is handed off, never awaited.
- **Notification storms.** A model producing a result per keystroke notifies
  every attached client per keystroke. There is no coalescing (the draft
  mechanism's coalescing is being removed, not carried over). A model that emits
  at high frequency must throttle itself.
- **Uncorrelated frames in older clients.** A client built before this change
  treats an unsolicited `notify` as a protocol error. Servers must only send
  notifications to connections that subscribed, which by construction are new
  clients — but the negotiated protocol version from A6 should gate it
  explicitly rather than relying on that.
- **Subscription outliving its instance.** When an instance is destroyed
  (attach count reaches zero) its subscriptions are dropped. A handler still
  holding a callback for it simply stops hearing anything; re-attaching
  re-establishes the subscription.
- **Result type collision across models.** Two model types producing the same
  result type are distinguished by model id, not by result type alone; the
  server's map is keyed on `(modelId, resultTypeId)`.

## Limitations

- **No replay, no durability, no ordering across instances.** Stated above.
- **No filtering.** A subscriber receives every `R` produced on the instance; it
  cannot ask for a subset.
- **No subscription to a model type in general** — only to a specific attached
  instance. "Tell me about every account" is not expressible.
- **No back-pressure.** A producer never learns that a subscriber is slow.
- **`morph::flows` must be reworked first.** This removal cannot land while
  `FlowSession` depends on the mechanism it deletes.

## Cross-references

- [shared_model_instances.md](shared_model_instances.md) — instances,
  attachment, and re-pointing, which define a subscription's scope.
- [stateful_bank_example.md](stateful_bank_example.md) — the state types a
  subscriber would name.
- [bridge.md](../spec/core/bridge.md) — the draft mechanism being removed.
- [wire.md](../spec/core/wire.md) — the envelope, and the request/reply
  assumption this breaks.
- [backend.md](../spec/core/backend.md) — connection scopes and the transports
  that need an unsolicited-message path.
- [registry.md](../spec/core/registry.md) — `ActionValidator`, which survives
  with a narrowed role.
- [workflows_navigation.md](../spec/forms/workflows_navigation.md) —
  `FlowSession`, which must be reworked first.
- [VERSIONING.md](../spec/VERSIONING.md) — the pre-1.0 latitude this removal
  relies on.

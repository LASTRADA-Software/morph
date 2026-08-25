# RPC and distributed-object systems vs. morph — a survey, not a scorecard

Date: 2026-08-25 · morph read at `master` `e8c8364`

Compares morph's typed action bridge against the systems that have solved (or
claimed to solve) the same problem: **Qt Remote Objects** (same ecosystem,
closest neighbour), **gRPC** and **Cap'n Proto RPC** (IDL-driven), **Meteor
DDP** and **tRPC** (no IDL, types from the implementation), and **CORBA /
ZeroC Ice** (the historical location-transparency attempt).

**This page does not enumerate the local↔remote delta.** That is
[`locality.md`](../../spec/core/locality.md)'s job, and it is the authoritative
list — thirteen rows plus a section on `onBackendChanged()`. This page assumes
it and asks the different question: measured against the systems that tried
this before, is morph's transparency claim the kind that survives, and what
should morph take from each comparator?

Provenance is stated per claim in [§9](#9-provenance).

## 1. Scope

morph's headline claim is a locality claim: the same call site works whether the
model runs in-process or across a socket. Actions are plain C++ aggregates
registered with `BRIDGE_REGISTER_ACTION`; JSON serialisation is reflected by
Glaze with no hand-written codecs and no IDL; results come back as
`Completion<T>`. Backends are `LocalBackend`, `RemoteServer` behind a JSON wire
protocol over Qt WebSockets or a Qt-free raw socket, and `SimulatedRemoteBackend`
for tests.

Only one comparator is trying to do morph's job:

- **Qt Remote Objects** is the closest neighbour — same ecosystem, an actual
  `.rep` IDL, and live replication rather than request/response.
- **gRPC** and **Cap'n Proto RPC** are polyglot RPC transports. Their headline
  features answer questions morph does not ask. Where they *are* comparable is
  schema evolution and deadlines, and there the comparison is sharp.
- **DDP** and **tRPC** are the school morph belongs to philosophically: no IDL,
  the contract is the implementation. DDP is also the closest functional
  analogue to a morph app — a UI synchronised against a server-side model over
  a WebSocket.
- **CORBA / ZeroC Ice** are the precedent. They made morph's claim, much more
  strongly, and Waldo et al.'s 1994 critique is the standard answer to why it
  did not hold. [§3](#3-does-the-transparency-claim-survive) engages with that
  rather than routing around it.

Nothing here argues that morph should acquire streaming, an IDL, or a capability
model. [§7](#7-deliberately-out-of-scope) lists the domain mismatches so they
are not mistaken for gaps.

## 2. Side-by-side

| Dimension | morph | Qt Remote Objects | gRPC | Cap'n Proto RPC | DDP | tRPC | CORBA / Ice |
|---|---|---|---|---|---|---|---|
| **Contract source** | The C++ action struct; Glaze reflects it. `BRIDGE_REGISTER_ACTION` supplies string ids | `.rep` DSL compiled by `repc` | `.proto` → codegen | `.capnp` → codegen | none; EJSON on the wire | the server's TypeScript types | Slice / OMG IDL → codegen |
| **Build step** | none (header-only) | `repc` | `protoc` | `capnp compile` | none | none | IDL compiler |
| **Call shapes** | unary request/response only | property replication, signals, slot invocation | unary, server-stream, client-stream, bidi | unary + promise pipelining | RPC **plus** subscription push | query / mutation / subscription | twoway, oneway, AMI/AMD |
| **Reference semantics** | everything by value | replica is a live proxy object | by value | **interface references pass by reference** as capabilities | by value | by value | object references by reference |
| **Failure surface** | `.onError(std::exception_ptr)`; identical call site local and remote, wider reachable error *set* remotely | replica `State`, incl. `Suspect` and `SignatureMismatch` | status codes; `DEADLINE_EXCEEDED` | exceptions through pipelined promises | `error`/`reason`/`message` | typed errors | local vs. user exceptions |
| **Deadlines** | two, opt-in, both default off; **neither is transmitted** | `waitForFinished(timeout)` | deadline on the wire, server can query remaining time | per call | none in the protocol | AbortSignal | invocation timeouts |
| **Cancellation** | none — *"morph never interrupts a running action"* | none for an in-flight call | either side, propagating | promise cancellation | none | AbortSignal | limited |
| **Backpressure** | admission control, not flow control | Qt socket buffering | transport-level flow control with write-side backpressure | flow-controlled streams | none specified | transport-level | transport-level |
| **Schema evolution** | documented additive-only policy; lenient decode **by field name** | `.rep` signature check | immutable field **numbers** | immutable **ordinals** | none | both ends are one program | by interface id |
| **Skew detection** | **on the journal path only** — see [§4](#4-schema-evolution-names-identities-and-a-fingerprint-that-stops-at-the-journal) | yes, at connect (static replicas) | structural, by number | structural, by ordinal | none | prevented, not detected | by interface id |
| **Ordering** | per model instance, FIFO, on **both** paths — the remote path has an explicit ticket gate | ordered while the connection holds | per stream | per connection | per connection | per connection | per connection |
| **Server→client push** | **no** — `subscribe<R>` is in-process fan-out on one `Bridge` | yes | server streaming | yes | yes, the central feature | subscriptions | callback objects |
| **Transparency posture** | "the same call site works" — and the API is async and fallible in *both* modes | hides that processing is remote | none claimed | none claimed | none claimed | "as if you were calling a function" | explicit location transparency |

## 3. Does the transparency claim survive?

### 3.1 The critique

Waldo, Wyant, Wollrath and Kendall (1994) argued that objects in a distributed
system must be dealt with differently from objects in one address space along
four axes — latency, memory access, partial failure, concurrency — and that of
these, partial failure is the one that cannot be papered over: a local call
fails totally and deterministically, while a remote call can fail partially and
leave the caller unable to determine whether the work happened. The critique
lands on CORBA and DCOM and remains the standard reason location transparency is
treated as an anti-pattern.

Even Ice, which still claims plainly that a client need not know where an
implementation resides, had to work at this. Its manual warns that a synchronous
twoway *collocated* call runs on the calling thread: *"a collocated invocation
behaves like a local, synchronous procedure call. This can cause problems if,
for example, the calling thread acquires a lock that an operation implementation
tries to acquire as well: unless you use recursive mutexes, this will cause
deadlock."* That is the shape of the problem — the API is uniform, the semantics
are not, and the divergence surfaces as a deadlock in the *local* case.

### 3.2 Why morph's narrower claim survives it

Three structural facts do the work, and all three are the *absence* of
something.

1. **The local path already obeys the remote path's rules.** There is no
   synchronous local call whose signature gets silently upgraded to a remote
   one. `BridgeHandler::execute` returns `Completion<T>` in both modes; the
   caller writes `.then(...)`/`.onError(...)` in both; the callback is
   marshalled onto an executor in both; the model runs on a strand, not the
   caller's thread, in both. Waldo et al.'s recommendation is to make the
   interface reflect the harder case rather than hide it, and morph does exactly
   that — at the cost of making the local case more ceremonious than it needs to
   be. `Completion<T>` having no `co_await`, no chaining, and no
   `wait()`/`get()` at all (verified: neither string occurs in
   `morph/core/completion.hpp`) is part of this. There is no synchronous escape
   hatch that would work locally and deadlock remotely — which is precisely the
   Ice collocation hazard above.

2. **There is no distributed object graph.** Actions and results are plain
   aggregates passed by value. No cross-process object references, no
   distributed garbage collection, no object identity preserved across the wire,
   no way for a result to hand back a live handle to server-side state. That is
   exactly the feature set that made the unified-object vision untenable.
   Contrast Cap'n Proto, which *does* pass interface references by reference as
   capabilities — *"They both designate an object to call and confer permission
   to call it"* — a much more ambitious position that needs a whole capability
   protocol to hold up.

3. **The leaks are written down.** [`backend.md`](../../spec/core/backend.md)'s
   Limitations opens with *"Local and remote are not fully interchangeable"*,
   and [`locality.md`](../../spec/core/locality.md) now collects the whole delta
   in one table. That is the difference between a leaky abstraction and a
   dishonest one — and it is the thing Ice's docs also get right, by giving
   collocation its own page.

### 3.3 One place morph did the work rather than documenting the leak

Latency and thread context would ordinarily have destroyed per-instance ordering
on the remote path. `RemoteServer::handle` posts to a multi-worker pool, so two
`execute` envelopes for the same model, sent back to back, could finish their
pre-strand work in either order and reach the strand reversed.

`morph/core/remote.hpp` closes this with a per-model ticket gate: a ticket is
taken **synchronously**, on the transport's own calling thread and therefore in
true send order (`takeExecuteTicket`), the dispatch waits for its turn
immediately before the strand post (`awaitExecuteTurn`), and every early-return
path releases through a `rejectAndRelease` lambda so a rejected call cannot
stall the queue behind it. `releaseExecuteTicket` runs as soon as the strand
post has happened, not when the action finishes, because `StrandExecutor`
serialises everything from there on.

The result is that "per-instance FIFO" means the same thing on both paths, which
is one of the strongest transparency properties morph actually holds — and it is
a property, not a delta row, which is why it belongs here rather than in
`locality.md`. Two caveats: the guarantee is per *transport calling thread*, so
it holds per connection rather than globally; and the wait parks a pool thread,
trading a worker against ordering.

## 4. Schema evolution: names, identities, and a fingerprint that stops at the journal

This is the sharpest comparison in the survey, and the one where morph's
position has moved most.

**What the IDL systems do.** protobuf's field number *"cannot be changed once
your message type is in use because it identifies the field in the message wire
format"*; deleted numbers should be reserved. Cap'n Proto states the same idea
as rules: new members may be added *"as long as each new member's number is
larger than all previous members"*; *"Any symbolic name can be changed, as long
as the type ID / ordinal numbers stay the same"*; *"You cannot change a field,
method, or enumerant's number"*; *"You cannot change a field or method
parameter's type or default value."* Qt Remote Objects has no numbers but has a
**check**: a replica whose `.rep` does not match the source enters
`SignatureMismatch`, documented as occurring *"if a connection to the source is
made, but the source and replica are not derived from the same .rep (only
possible for static Replicas)"* — detected at connect, not on the first
mis-decoded payload.

One honest caveat that sharpens rather than weakens the comparison: protobuf's
rename-safety is a property of the *binary* wire format. The protobuf docs
themselves note that a renamed field breaks JSON and TextFormat encodings unless
the name is reserved too. morph is a JSON protocol, so the comparator that most
resembles it is protobuf-over-JSON, which has morph's problem, not
protobuf-over-binary, which does not.

**What the no-IDL systems do.** tRPC does not solve evolution, it dissolves it:
*"tRPC has no build or compile steps, meaning no code generation, runtime bloat
or build step"*, and the client imports the server's router type, so skew inside
a monorepo is a compile error and skew outside one is undetectable. DDP does
nothing — EJSON is schemaless.

**What morph does.** Three things, verified on `master`:

- A **documented policy** in [`wire.md`](../../spec/core/wire.md) under
  "Action-evolution policy": additive-only within a major version, new fields
  optional or safely-defaulting, *"Never renumber or rename protocol
  vocabulary"*, a one-release deprecation window, and a `kProtocolVersion` bump
  for removals or retypes.
- A **transport-level handshake**: a `"hello"` kind carrying `protocolVersion`,
  a `ProtocolRange` reply, `RemoteServer::setSupportedVersionRange(min, max)`,
  and `wire::interpretHelloReply` classifying a pre-negotiation peer as
  `LegacyPeer` rather than failing. This degrades correctly against an older
  peer in both directions. `kProtocolVersion` is `1`.
- **Lenient decoding by field name**: the generated `fromJson`/`resultFromJson`
  bodies and the outer envelope decode all read with
  `error_on_unknown_keys = false` (four sites in `morph/core/registry.hpp`, one
  in `morph/core/wire.hpp`).

That combination gives the intended behaviour for additions and removals and the
wrong behaviour for the two changes the policy forbids:

| Change | protobuf (binary) | Cap'n Proto | QtRO | morph |
|---|---|---|---|---|
| Add a field | safe | safe | detected | safe |
| Remove a field | safe with `reserved` | safe | detected | safe-ish |
| **Rename a field** | safe (number is the identity) | safe (ordinal is) | detected | **silently decodes to the member's default** |
| **Retype a field** | forbidden, caught structurally | forbidden | detected | decode error, or a silent default, depending on the types |
| Peer built against a different action shape | structural mismatch | structural mismatch | `SignatureMismatch` at connect | **not detected on the wire** |

**The part that has changed: the fingerprint exists now.** The mechanism this
survey would otherwise have proposed — QtRO's `SignatureMismatch` expressed in
morph's vocabulary — has shipped, on one path.
`morph/core/payload_schema.hpp` computes
`morph::model::payloadFingerprint<A>()`, a short stable string derived from the
payload struct's reflected shape, carrying a scheme prefix
(`kPayloadFingerprintScheme`) so a future algorithm change is distinguishable
from a payload change. `BRIDGE_REGISTER_ACTION` exposes it as
`payloadSchema()`; `ActionDispatcher` files it under `(modelId, actionId)` and
hands it back through `schemaFor()`; every journal entry is stamped with it in
`LogEntry::schema`; and `journal::replay()` refuses a mismatch with
`SchemaMismatchError` rather than decoding across it. See
[`journal.md`](../../spec/journal/journal.md), "Payload schema fingerprint".

So three of the conclusions this survey originally drew need restating:

1. **morph's fields have names but no identities — on the wire.** On the journal
   path they now have an identity, and it is exactly the structural fingerprint
   the IDL systems get from ordinals. The gap is no longer "morph lacks the
   concept"; it is that the concept stops at the journal.
2. **`kProtocolVersion` still does not cover this.** It is a build-wide
   *transport* version, not a per-action schema version. Two builds that agree
   on the envelope and disagree about one action's field names negotiate
   successfully and then mis-decode. Verified: neither `payloadSchema` nor
   `payloadFingerprint` appears anywhere in `morph/core/remote.hpp`,
   `morph/core/wire.hpp`, or `morph/core/bridge.hpp`.
3. **The live-`execute` exposure is tracked as #207**, which carries an
   independent repro: a renamed field decodes to zero and the server replies
   `ok`; a payload sharing *zero* keys with the action also decodes and executes
   as a real mutation. Routing (`modelType`/`actionType`) *is* checked, and an
   opt-in `validate()` gate already catches both cases for actions that use it.
   #207's conclusion stands — the fix is mechanical enforcement of the wire's
   own published policy, not a change in decode strictness, since strict
   decoding breaks the additive case the leniency exists to support.

That makes the wire-side fix substantially cheaper than it looked: the
fingerprint function, the per-action registration, and the `(modelId, actionId)`
lookup all already exist and are already exercised by the journal. What is
missing is carrying them in the handshake.

## 5. Streaming, push, and backpressure

**Streaming.** gRPC has four call shapes; Cap'n Proto's answer to the same
latency problem is promise pipelining, where *"The results of an RPC call are
returned to the client instantly, before the server even receives the initial
request!"* morph is request/response only. For *result* streaming that is a
correct scope choice: a form-driven client submitting bounded user actions has
no use for a client-streaming channel, and adding one would drag in flow
control, half-close semantics, and a second lifetime model for `Completion<T>`,
which is deliberately a single-result leaf primitive.

**Server→client push is the part that is not purely scope.** morph's two closest
functional analogues both have it as their central feature: DDP's subscription
flow is the reason DDP exists, and QtRO propagates property changes and signals
source→replica continuously. morph's `subscribe<R>` looks like the same thing
and is not — it fans out to handlers on one `Bridge`, in one process, with no
replay, no cursor, no coalescing. Verified: the string `subscribe` does not
occur in `morph/core/remote.hpp` at all. Because morph *does* ship shared
server-side instances, it creates the exact scenario where the absence is felt,
and the README says so: two separate clients sharing an instance do not see each
other's results until they ask again.

**Backpressure.** morph has admission control, not flow control:
`LimitPolicy::maxInFlightExecutes` → `err "server busy"`,
`LimitPolicy::maxLiveModels` → `err "too many models"`, plus, on the Qt
transport only, `QtWebSocketServerConfig::maxMessageBytes` and a per-connection
`messagesPerSecond` token bucket (`morph/qt/qt_websocket_server.hpp`; the
Qt-free `morph/net/socket_server.hpp` has no rate limiter at all). gRPC by
contrast has real flow control, with the framework waiting *"before returning
from a write call"* to signal backpressure, and a documented deadlock risk if
both ends write without reading.

Rejection instead of flow control is proportionate for a GUI with a bounded
number of in-flight user actions. The one detail this survey previously flagged
here — that a rate-limited frame was dropped silently, leaving the caller's
`Completion` unresolved — **has been fixed** (**#225**). The Qt server now
answers a rate-limited frame with `err "rate limited"`, addressed to the frame's
own `callId` via the same bounded prefix scan the oversized-frame branch uses,
and leaves the connection open.

One residue: `morph/core/backend.hpp`'s doc comment on `ClientTimeoutError`
still describes a frame *"silently dropped by
`QtWebSocketServerConfig::messagesPerSecond`"* as a reason that error fires.
That is no longer reachable. See [§8](#8-candidate-gaps), G-1.

## 6. Deadlines, cancellation, retries

**Deadlines exist but do not propagate.** morph has two, both opt-in and both
defaulting to off: `LimitPolicy::executeTimeout` on the server, replying
`err "timeout"` → `TimeoutError` (the model keeps running on its strand; the
discarded result is dropped via a once-flag), and `Bridge::setExecuteDeadline`
on the client, resolving the `Completion` with `ClientTimeoutError` in a race
with the real reply.

[`completion.md`](../../spec/core/completion.md) draws the distinction better
than most frameworks bother to: `TimeoutError` confirms the action is in flight
server-side, so a blind retry risks a duplicate; `ClientTimeoutError` confirms
nothing, so a retry must be idempotent or reconciled either way. That is exactly
the indeterminacy Waldo et al. said could not be hidden — morph does not hide
it, it types it, and that is the single most direct answer this codebase gives
to the 1994 critique.

What it is not is a deadline in gRPC's sense. gRPC *"allows clients to specify
how long they are willing to wait for an RPC to complete before the RPC is
terminated with a `DEADLINE_EXCEEDED` error"*, and the server can query the
remaining time. In morph the client's deadline is never sent, so a server has no
way to know the caller has already given up, and keeps burning a strand on work
nobody will read.

**Cancellation does not exist, deliberately and consistently.** `completion.md`
says there is no handle to cancel an outstanding operation, and `backend.md`
says morph never interrupts a running action — true of `executeTimeout`, of
shutdown draining, and of graceful close, all of which bound the *caller's wait*
and never the work. gRPC can do better only because its handlers are expected to
poll a context; `Model::execute` is arbitrary user C++ with no cancellation
points. Naming this as a difference is fair; naming it as a gap would not be.

**Retries are the caller's, with one exception.** A live execute interrupted by
a socket drop resolves with `DisconnectedError`; `Bridge` re-registers handlers
on reconnect but does not replay the call. The offline layer does retry —
`SyncWorker` (`morph/offline/sync_worker.hpp`) drains an `IOfflineQueue` with
retry and dead-lettering, and `QueueItem::idempotencyKey`
(`morph/offline/offline_queue.hpp`) exists there as a caller-supplied dedup
token. The asymmetry: morph knows enough to *tell* callers a retry must be
idempotent, but nothing in an action's declaration says whether it is, so every
application re-derives the same judgment by hand.

## 7. Deliberately out of scope

- **gRPC's four streaming shapes and transport flow control.** A different
  traffic model. `Completion<T>` is single-result by design.
- **Cap'n Proto's promise pipelining and capability model.** morph deliberately
  has no distributed object graph — [§3.2](#32-why-morphs-narrower-claim-survives-it)
  argues its *absence* is what makes the transparency claim survivable.
  Acquiring capabilities would forfeit that.
- **Polyglot IDL codegen.** The contract being the C++ struct is the product.
- **QtRO's node registry and live property replication.** Continuous replication
  is a model, not a missing feature; morph's domain object is deliberately not a
  `QObject`.
- **CORBA-style cross-process object identity and distributed GC.** The thing
  the 1994 critique is about.
- **Cancellation.** Consistent, documented, and a consequence of
  `Model::execute` being arbitrary user code on a strand.
- **DDP-style latency compensation.** morph has the offline queue and ordered
  reconnect sequencing but no optimistic-apply-then-reconcile primitive, and
  conflict resolution is explicitly not a framework concern. A defensible
  boundary — it just means a DDP-shaped app builds the compensating half itself.

## 8. Candidate gaps

| | Gap | Shape | Status |
|---|---|---|---|
| **G-1** | `morph/core/backend.hpp`'s `ClientTimeoutError` doc comment still names *"a frame silently dropped by `QtWebSocketServerConfig::messagesPerSecond`"* as a cause. Since **#225** that frame is answered with `err "rate limited"`, so the cited cause is unreachable and the comment sends a reader looking for a hang that no longer happens. | documentation defect in a public header, verified now | **New — worth filing.** One-comment fix; note it touches `include/`, so the header↔spec sync job will want the matching line in [`backend.md`](../../spec/core/backend.md), which already describes the reply correctly. |
| **G-2** | The action-schema fingerprint stops at the journal. `payloadFingerprint<A>()`, `ActionDispatcher::schemaFor()` and per-action registration all exist and are exercised by `journal::replay()`, but nothing carries them on the wire, so a client and server built against different shapes of the same action negotiate `hello` successfully and then mis-decode. | verified; mechanism already exists | Tracked as **#207**. This survey's contribution is the sharper fix direction: exchange the existing per-action fingerprints at `hello` (or lazily on first use of an action) and refuse a mismatch, reusing `SchemaMismatchError`'s shape. That is QtRO's `SignatureMismatch` with parts morph already ships. |
| **G-3** | No server→client push, so two clients on one shared instance cannot converge. `subscribe<R>` is in-process fan-out. Both closest analogues (QtRO, DDP) have push as their central feature, and morph's own shared-instance feature creates the situation where the absence bites. | behaviour (wire change), design question | Unfiled by design — the largest capability gap here and the most expensive (a server-side subscription registry, a push envelope kind, a durability/coalescing story). Identical to the virtual-actor survey's G-D ([`virtual-actors-vs-morph-2026-08-25.md`](virtual-actors-vs-morph-2026-08-25.md)); file **one** issue covering both when the design work is scoped. |
| **G-4** | The client's deadline is not transmitted, so the server keeps burning a strand on work nobody will read. Adding the remaining budget as an envelope field the server compares against its own `executeTimeout` is small and additive, and is the one piece of gRPC's deadline story in scope. | behaviour, parked | Overlaps **#116** (parked), which covers deadline propagation across the executor abstraction; this is the wire half. #116's own re-entry trigger — an action fanning out into a second, separately-dispatched call — is still unmet. Its "resolve #128 first" precondition **has** since been met (#128 is closed). |
| **G-5** | No declared per-action idempotency. `idempotencyKey` lives only at the offline-queue layer and is caller-minted, so the framework can never decide that a `DisconnectedError`ed call is safe to resend, and every application re-derives `completion.md`'s reasoning by hand. An action-level trait would let the framework decide once. | speculative | Keep parked. No current caller is blocked; building it now is API surface with no consumer. Re-entry: a second call site needing to decide programmatically whether a failed action is safe to auto-retry. |

## 9. Provenance

**morph.** Read directly at `master` `e8c8364`, and the basis for every claim
about morph here: `morph/core/bridge.hpp`, `wire.hpp`, `remote.hpp`,
`registry.hpp`, `backend.hpp`, `completion.hpp`, `payload_schema.hpp`,
`morph/journal/action_log.hpp`, `morph/offline/sync_worker.hpp`,
`offline_queue.hpp`, `morph/qt/qt_websocket_server.hpp`,
`morph/net/socket_server.hpp`, plus
[`backend.md`](../../spec/core/backend.md),
[`wire.md`](../../spec/core/wire.md),
[`completion.md`](../../spec/core/completion.md),
[`registry.md`](../../spec/core/registry.md),
[`journal.md`](../../spec/journal/journal.md) and
[`locality.md`](../../spec/core/locality.md). **Nothing was built or run for
this page.** The #207 repro referenced in [§4](#4-schema-evolution-names-identities-and-a-fingerprint-that-stops-at-the-journal)
was executed on that issue, not here.

**Verified against the project's own documentation** (fetched and read while
writing this page): protobuf's field-number rule and its JSON/TextFormat caveat
on renames; Cap'n Proto's four evolution rules and its promise-pipelining and
capability statements; `QRemoteObjectReplica::State`'s `SignatureMismatch` and
`Suspect` definitions, including the "only possible for static Replicas"
restriction; gRPC's deadline/`DEADLINE_EXCEEDED` and either-side-cancellation
statements, and its flow-control page's write-delay and deadlock notes; tRPC's
"no build or compile steps" claim; ZeroC Ice's collocated-invocation deadlock
warning.

**From general knowledge, not re-checked against the source during this pass** —
orientation only, verify before designing against any of it: Waldo et al. (1994)
itself, whose canonical PDF was not retrievable; the four-axis structure and the
primacy of partial failure are corroborated by secondary summaries, and no
argument here depends on its exact wording, so it is paraphrased rather than
quoted. Also from general knowledge: gRPC's flow control being specifically
HTTP/2 window based (its own page describes the mechanism without naming
HTTP/2); QtRO's `repc`/`.rep` toolchain and node-registry details beyond the
`State` enum; Meteor DDP's message set and latency compensation; tRPC's
`createTRPCClient`/`typeof appRouter` mechanics (the concepts page consulted did
not cover them); Ice's Slice versioning and AMI/AMD; CORBA generally.

# Virtual actors and actor sharding vs. morph — a survey, not a scorecard

Date: 2026-08-25 · morph read at `master` `e8c8364`

Compares morph's concurrency and instance model — one strand per model instance,
plus keyed/shared instances (`BridgeHandler<Model, AllowShared>` +
`BRIDGE_MODEL_KEY`) — against Microsoft Orleans, Akka/Pekko Cluster Sharding,
Erlang/OTP with a process registry, Cloudflare Durable Objects, Dapr actors, and
the two C++ neighbours, CAF and Seastar's `sharded<>`.

Provenance is stated per claim in [§7](#7-provenance). Read it before quoting
anything here about a system that is not morph.

## 1. Scope

Every comparison system is a **distributed runtime**. Orleans, Akka/Pekko
Cluster Sharding and Dapr each exist to place a keyed entity somewhere in a
*cluster*, keep at most one live copy there, move it when the cluster changes
shape, and resurrect it after a node dies. Durable Objects does that with a
storage engine attached; Erlang/OTP does the process-per-entity part natively
and leaves naming to a registry. All of them answer *"where does this entity
live, and what happens when that machine goes away?"*

morph does not ask that question. Its server is one `RemoteServer` object in one
process, and its stated purpose is that a model author writes plain,
single-threaded C++ while the framework owns concurrency, marshalling and
transport. Keyed sharing exists to solve one concrete, local problem, stated in
[`shared_instances.md`](../../spec/core/shared_instances.md): five controllers
each constructing a `BridgeHandler<bank::AccountModel>` otherwise produce five
divergent copies of account 42's balance.

So morph reuses the *shape* of a virtual-actor system — keyed identity,
on-demand creation, serialised execution per instance, refcounted lifetime — at
the scale of a single process, and declines everything the shape is usually
attached to. Its own spec says so under Non-goals: *"Not virtual actors. No
perpetual existence, no placement, no clustering, no
activation-on-message-to-a-cold-key."* [§5](#5-deliberately-out-of-scope) lists
what is a domain mismatch rather than a gap; [§6](#6-candidate-gaps) lists the
residue worth acting on.

## 2. Side-by-side

| | Orleans | Akka / Pekko Sharding | Erlang/OTP + registry | Durable Objects | Dapr actors | morph |
|---|---|---|---|---|---|---|
| **Unit** | grain | entity actor | `gen_server` process | Durable Object | actor | model instance (`ModelId` + strand) |
| **Identity** | Guid / Int64 / string / compound | `entityId` string, shard by hash | pid + registered name | name or random id | actor type + id | `(modelTypeId, primary)`; primary is an integral type (not `bool`), `std::string`, or a wrapper over one |
| **Where identity is declared** | interface marker | `EntityTypeKey` | the registry call | at the call site | type registration | `BRIDGE_MODEL_KEY(M, A, &A::f)` beside the registration — the model class body says nothing |
| **Creation** | implicit, virtual | on first message | explicit `start_link` | implicit on first request | on demand | on first keyed action or `attach()` from an `AllowShared` handler — **explicit, not virtual** |
| **Concurrency** | one request at a time, opt-in reentrancy | one mailbox, one message at a time | one process, one mailbox | single-threaded, cooperative | turn-based | one strand per `ModelId`: FIFO, no overlap for one key, different keys parallel |
| **Reentrancy opt-out** | a family of attributes | none | none | none | supported | **none, and none reachable** — see [§4.4](#44-reentrancy) |
| **Deactivation** | idle collection, `CollectionAge` default 15 min, plus memory-pressure shedding | idle passivation on by default, 2 min | never | hibernates when idle | idle deactivation + GC | **immediate destruction at zero attachments** |
| **Lifetime driver** | runtime policy | runtime policy | supervision tree | platform | runtime policy | **reference count** — every attach from any connection is a reference |
| **State across activations** | persistence providers | none built in; use Persistence | whatever the process does | transactional per-object storage | configured state provider | **none** — an explicit non-goal |
| **Distribution** | grain directory over the cluster | `ShardCoordinator` singleton | `global` / gproc | platform-global | placement service | **`std::unordered_map` in one `RemoteServer`** |
| **Failure** | activation dies with the silo, re-activates elsewhere | shard migrates, entity restarts | supervisor restart strategies | platform | migrate to a healthy node | exception → `err` reply; **instance survives, no restart, no supervisor** |
| **Enumeration** | not first-class | not first-class | registry query | not first-class | not first-class | `handler.instances()` → `Completion<vector<PrimaryKey>>`, per model type, unfiltered |

## 3. What morph has

Three mechanisms, read off `master` `e8c8364`.

**One strand per instance.** `morph/core/executor.hpp` defines `IExecutor` as a
single pure-virtual `post(std::function<void()>)`; `ThreadPoolExecutor` and
`MainThreadExecutor` implement it there, and `morph::qt::QtExecutor`
(`morph/qt/qt_executor.hpp`) implements it for the Qt event loop.
`morph::exec::detail::StrandExecutor` (`morph/core/strand.hpp`) wraps any
`IExecutor` with a per-`ModelId` FIFO queue: tasks with the same key *"execute in
FIFO order with no overlap"* even on a thread pool, and different keys may run
concurrently ([`executor.md`](../../spec/core/executor.md)). There are no
coroutines and no fibers — the thread the base executor hands the strand is the
thread that runs `Model::execute`, blocking DB call and all. That is the price of
plain single-threaded C++: a slow model occupies one pool thread and stalls only
its own instance's queue.

**Keyed, shared instances.** `BridgeHandler<Model, AllowShared>` opts a handler
into the directory; `BridgeHandler<Model>` is `NoSharing` and unchanged.
`BRIDGE_MODEL_KEY(M, A, MEMBER)` appears once per model and deduces the key type
from the member pointer; `BRIDGE_KEY_FROM` marks further actions carrying the
key; `BRIDGE_MODEL_KEY_FROM_RESULT` and `BRIDGE_KEY_FROM_RESULT`
(`morph/core/model_key.hpp`) cover a creating action whose key exists only in the
reply. `ModelKey` admits integral types and `std::string`, plus wrappers over
them; `bool` is explicitly excluded because one bit is never an identity. Sharing
eligibility is static (a template parameter); which instance is shared is
dynamic. Re-pointing a handler moves the *handler*, never the instance's
identity — which is what makes "one key, one instance" total.

**The directory.** In `RemoteServer` (`morph/core/remote.hpp`) it is an
`unordered_map<(typeId, primary), ModelId>` plus a reverse map and an
`_attachCount`, all guarded by the same `_regMtx` that guards `_models` and
`_owners`, so directory membership cannot desync from instance existence.
Lifetime is a reference count: attach increments; `deregister`, handler
destruction and `closeConnection` decrement; `releaseInstanceLocked` erases the
instance and its directory entry at zero. `closeConnection` releases exactly as
many references as that connection took (`noteScopeAttachLocked` counts per
`(connection, instance)`), so a client that attached the same instance from two
handlers unwinds both.

Authorization is reused rather than reinvented: `authorize` gates (model type,
action); `authorizeRegister` gates every path that reaches the directory
(`register`, `attach`, `assign`); `authorizeInstance` gates each execute against
the instance's recorded owner; `instances` is gated by `authorize` with an empty
action id. The one deliberate collision is that **a shared instance is
ownerless** — its `ownerPrincipal` is empty, because the documented typical
policy (`owner.empty() || owner == ctx.principal`) would otherwise reject the
second client. Cross-client sharing and per-instance ownership are mutually
exclusive, and the spec says so rather than letting an authorizer silently
defeat the feature.

## 4. The four questions

### 4.1 Distribution

`_directory`, `_models`, `_owners`, `_attachCount`, `_connectionScopes` and the
id counter are all non-static members of one `RemoteServer`. There is no
membership protocol, no shared store, no placement, no rebalancing, no handover.
Two `RemoteServer` processes behind one endpoint therefore have independent,
non-communicating directories: key 42 on server A and key 42 on server B are two
instances — the exact divergence keyed sharing exists to prevent, reintroduced
one layer up, silently.

Two things that could have made this worse do not. `Bridge::switchBackend()`
re-registers every live binding on the new backend, so a reconnect mints fresh
ids rather than reusing stale ones; and `IModelHolder::into<Model>()`
(`morph/core/model.hpp`) compares `std::type_index` and throws `std::bad_cast` on
mismatch, so a stale id landing on a live instance of another type produces an
`err` reply, not undefined behaviour. The failure mode is divergence, not
corruption.

This is **no longer a documentation gap**:
[`shared_instances.md`](../../spec/core/shared_instances.md) now carries a
section headed *"The directory is per-process, and that is load-bearing"*, which
names the two-server case and its consequence outright.

### 4.2 Lifecycle

Every comparison system keeps an entity resident past its last caller and
reloads state on the next activation — Orleans at a 15-minute default
`CollectionAge`, Akka at a 2-minute default idle passivation, Dapr by idle
deactivation with state in a configured provider.

morph destroys immediately at zero attachments, and documents it twice: *"No
idle deactivation. Zero attachments destroys immediately; there is no
keep-alive, no LRU, no eviction policy"*, and an idle grace period is *"**not**
part of this work"*. Non-goals adds that there is no state persistence provider.

So the question "does morph document what happens when the last attachment
goes?" is answered: yes, clearly. What follows is still worth naming, because it
is the one place the inversion has a practical cost. With no persistence
provider *and* no grace period, whatever a shared instance hydrated into memory
is discarded the instant the last handler detaches, and the next attach
re-hydrates from scratch. For a GUI client that reconnects — and reconnect is a
first-class morph feature — a single client's flap tears the instance down and
pays full hydration again. Tracked as **#220**.

### 4.3 Failure

Erlang is the reference point: *"Workers are processes that perform computations
and other actual work. Supervisors are processes that monitor workers."* Orleans
and Akka fold failure into placement instead.

morph has no supervision and, for its scope, mostly needs none: a model instance
lives in the server process, and if that process dies the server, the directory
and every client transport die with it. There is no partial failure to
supervise. What morph has is exception containment at three levels —
`ThreadPoolExecutor`'s loop catches and logs so one failing task neither kills
its thread nor aborts siblings; `RemoteServer`'s strand task catches
`std::exception` and turns it into an `err` reply carrying `what()`; metric and
trace sinks are invoked outside locks under `catch (...)`.

The one failure rule morph had to invent is the half-hydrated instance, and its
answer is deliberately unlike everyone else's. A failed **first** action marks
the instance poisoned (`_poisoned` in `morph/core/remote.hpp`); it is evicted
from the directory the next time *someone else* attaches to that key, not
immediately, and it is not destroyed — the handler that hit the failure keeps its
broken instance until it releases and re-attaches. Orleans or Akka would restart
the entity and hand the caller a fresh one. morph protects future attachers and
leaves the failing caller holding the failure. That keeps live-model accounting
honest and avoids destroying an instance with a call in flight against it, and it
is documented in full — but it is the largest behavioural divergence in the
failure column.

Also present, and already documented rather than newly found: the async attach
path has **no in-flight dedup**. Two calls for the same key issued before the
first reply lands both pass the binding-state guard and both dispatch; the server
answers both with the same `ModelId` but records two attachments, leaking one
attach reference. The leak is bounded — the connection scope releases everything
at close — and the spec names the fix (in-flight tracking on the binding).

### 4.4 Reentrancy

Orleans documents this hazard at length and answers it with a family of
reentrancy attributes. The question for morph is what happens if one model's
`execute` dispatches into another model.

The answer is that **it cannot**, and this is now stated in the spec rather than
left to be inferred. [`concurrency_and_lifetimes.md`](../../spec/concurrency_and_lifetimes.md)
carries the corollary under its invariants: no framework seam hands a model a
handle to its own `Bridge` or `BridgeHandler` — construction is nullary
(`ModelFactory::create<Model>()`), dispatch passes only the action, and the two
optional hooks pass no handle either. The spec is also precise about how far the
enforcement reaches: a **reference** member (`BridgeHandler<Self>& handler;`)
fails to compile through `ModelFactory::create`, while a **pointer** member
compiles and simply stays null, because no framework-owned path ever assigns
into it. Compile-time for the reference case, true-in-practice for the pointer
case.

Two further structural facts back this up. `Completion<T>` is a leaf callback
primitive: no chaining, no `co_await`, and no blocking `wait()`/`get()` — the
string does not appear in `morph/core/completion.hpp` at all. Orleans' cycle
deadlock needs an await inside the turn, and morph has no await to offer. And a
model calling back into its own instance would post behind the currently running
task, which runs after the current one returns.

One blocking path remains, and it is on the *caller's* side rather than a
model's: `BridgeHandler::attach(key)` is synchronous and throwing by design, and
`Bridge::attachHandler` invokes the blocking `attachModel` unconditionally, even
though `attachHandlerAsync` exists beside it. Called on a thread that must not
park — a WASM main thread, or a strand worker in a hand-wired setup — that costs
a full round trip on any backend whose transport call blocks.

The result-keyed **promote** step no longer shares that property.
`Bridge::assignHandlerPrimary` calls `assignPrimaryAsync` first and falls back to
the synchronous `IBackend::assignPrimary` only when the backend returns `false`.
`IBackend::assignPrimaryAsync` exists in `morph/core/backend.hpp` with a
`false`-returning default, and `QtWebSocketBackend`
(`morph/qt/qt_websocket_backend.hpp`) overrides it, documented as returning
*"`true` always"*. **`shared_instances.md` has not caught up**: it still states
that the promote step *"is still synchronous"* and that *"There is no
`assignPrimaryAsync`"*. See [§6](#6-candidate-gaps), G-A.

## 5. Deliberately out of scope

Named here so they are not mistaken for gaps.

- **Placement, rebalancing, cluster membership, activation migration.** All
  answer "which machine hosts this entity, and how does it move". morph has one
  server process; there is no second machine to place onto. Building any of it
  is a different project.
- **Virtual existence.** Orleans' defining property is that a grain always
  exists, virtually. morph's directory tracks what exists rather than conjuring
  it: an instance exists because a handler asked for it, never because a row
  exists in a database. Reversing this needs a persistence provider morph
  deliberately lacks.
- **Supervision trees and restart strategies.** These presume many independent
  processes failing independently within a surviving node. morph's instances are
  objects in the server process. Exception containment ([§4.3](#43-failure))
  covers the real risk.
- **Durable timers and reminders.** They exist so a cold entity can be woken
  later. morph has no cold entities.
- **Cross-instance transactions.** Two shared instances are two strands; an
  operation spanning both is a domain concern, and the spec says so.
- **Seastar's `sharded<T>`.** A near-miss worth naming *because* it is not
  comparable: its key is a shard index and its instance count equals the core
  count, under a shared-nothing per-core design. morph's key is a domain entity
  and its instance count is however many entities are live. A different axis of
  partitioning.
- **CAF.** The closest C++ neighbour, but a general actor runtime — arbitrary
  topologies, dynamic spawn from within actors, links and monitors. morph
  exposes no actor abstraction to the model author at all; a morph model is a
  plain class and the strand is invisible to it. That invisibility is the
  product, not an incomplete actor system.
- **Richer enumeration.** `instances()` being an async, stale-on-arrival,
  unfiltered snapshot looks thin next to a queryable registry, but it must be
  async so the call site is identical local and remote, and it must be read as
  "was live recently" because the directory is concurrent. Paging and predicates
  are a scale problem morph does not have.

## 6. Candidate gaps

Two of this survey's original five gaps have since been fixed and are recorded
here as closed, so a reader does not re-file them. What remains is three items,
one of them new and found while re-verifying this survey against source.

| | Gap | Shape | Status |
|---|---|---|---|
| **G-A** | `shared_instances.md` states the result-keyed promote step *"is still synchronous"* and that *"There is no `assignPrimaryAsync`"*. Both are false on `master`: `IBackend::assignPrimaryAsync` exists, `QtWebSocketBackend` overrides it, and `Bridge::assignHandlerPrimary` prefers it. The stale text also carries a WASM warning ("aborts the page at the promote step") that no longer describes the Qt transport. | documentation defect, verified now | **New — worth filing.** Fix: rewrite that subsection to say the promote step prefers `assignPrimaryAsync` and falls back to the blocking `assignPrimary` only on a backend without an override (e.g. `morph/net/socket_backend.hpp`, which inherits the `false` default). |
| **G-B** | An instance is never told its own key. The model factory is nullary, so the primary reaches the model only inside a keyed action's payload, and hydration is the model's job. Orleans, Akka, Durable Objects and Dapr all inject identity at activation. A keyless action against a freshly attached instance can run before that instance has ever observed its key. | verified structural gap | Tracked as **#219**. |
| **G-C** | No idle grace period: an instance dies the instant its last attachment goes, and with no persistence provider its hydrated state goes with it. Every system surveyed keeps entities resident for minutes. Most visible on the reconnect path. | behaviour, speculative | Tracked as **#220**, parked. Re-entry trigger: re-hydration cost or reconnect churn showing up in a real example. |
| **G-D** | Two clients on one shared instance still cannot see each other's changes. `subscribe<R>` fans out only to handlers on the same `Bridge`; there is no server→client push. Sharing gives a common *write* target but not a common *view*. | behaviour (wire change), design question | Unfiled by design. This is the same question as the RPC survey's gap 1 ([`rpc-and-distributed-objects-vs-morph-2026-08-25.md`](rpc-and-distributed-objects-vs-morph-2026-08-25.md)) and should become **one** issue when that design work is scoped, not two. |

Closed since this survey was first drafted, listed so they are not re-filed:

- *"Server-side directory" never says "one server process"* — fixed (**#217**).
  `shared_instances.md` now has a per-process section naming the two-server
  divergence, and its earlier claim that two attached clients "see each other's
  state" — which contradicted the README — has been corrected to "shared state,
  not shared notifications".
- *No stated rule about a model reaching the bridge* — fixed (**#218**). The
  corollary now lives in `concurrency_and_lifetimes.md`, with the
  reference-vs-pointer distinction spelled out.

## 7. Provenance

**morph.** Everything in §3, §4 and §6 was read directly at `master` `e8c8364`:
`morph/core/executor.hpp`, `strand.hpp`, `remote.hpp`, `registry.hpp`,
`model.hpp`, `model_key.hpp`, `completion.hpp`, `bridge.hpp`, `backend.hpp`,
`morph/qt/qt_websocket_backend.hpp`, `morph/qt/qt_executor.hpp`, plus
[`shared_instances.md`](../../spec/core/shared_instances.md),
[`executor.md`](../../spec/core/executor.md),
[`concurrency_and_lifetimes.md`](../../spec/concurrency_and_lifetimes.md) and
[`locality.md`](../../spec/core/locality.md). **Nothing was built or run.** The
two-`RemoteServer` claims in §4.1 are read off member declarations and lock
scopes, not observed in an experiment.

**Verified against the project's own documentation** (fetched and read while
writing this page): Orleans' `GrainCollectionOptions` defaults — `CollectionAge`
15 minutes, `CollectionQuantum` 1 minute — and memory-pressure activation
shedding; Akka Cluster Sharding's automatic idle passivation *"enabled
automatically with a timeout of 2 minutes"*, its strategy names, and that
*"the state of the entities themselves is not restored unless they have been
made persistent"*; Dapr's turn-based access model, *"An actor's state outlives
the object's lifetime, as state is stored in the configured state provider"*,
its garbage collection of unused actors, and that Dapr *"distributes actor
instances throughout the cluster and automatically migrates them to healthy
nodes"*; Erlang/OTP's *"Workers are processes that perform computations and
other actual work. Supervisors are processes that monitor workers."*

**From general knowledge, not re-checked against the source during this pass** —
treat as orientation, not as citable fact, and verify before designing against
any of it: Orleans' reentrancy attribute family and its documented deadlock
scenario; Orleans' grain-directory internals (consistent-hash ring, virtual
ranges, view-change protocol); Cloudflare Durable Objects' hibernation timing
and in-memory-state reset (the Cloudflare docs host could not be fetched from
this environment); Erlang restart *strategies* specifically (the OTP design
principles page confirms the supervisor/worker split but not the strategy
list); gproc's query facilities; CAF's mailbox, registry and
`monitor()`/`link_to()`; Seastar `sharded<T>`'s per-core semantics and API.

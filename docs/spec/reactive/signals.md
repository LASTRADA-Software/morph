# Signals — design

Design spec for `morph::reactive::Runtime`, `RuntimeOptions`, `Signal<T>`, `Computed<T>`,
`Effect`, `Scope`, `EqualityPolicy` and `kEqualityUsable` (`include/morph/reactive/{runtime,signal,scope}.hpp`,
internals in
`include/morph/reactive/detail/graph.hpp`): the fine-grained reactive graph a morph UI's state,
controllers and view bindings are built from.

Read this before writing a binding, a controller or a frontend's flush hook, and before adding a
node type. [`store.md`](store.md) builds user-intent state on these nodes, and
[`control.md`](control.md) builds server interaction on them.

## Contents

- [Nodes](#nodes)
- [The owner](#the-owner)
- [Scheduling](#scheduling)
- [The algorithm](#the-algorithm)
- [A Computed that fails](#a-computed-that-fails)
- [Misuse: reported, then refused](#misuse-reported-then-refused)
- [Off the owner, operation by operation](#off-the-owner-operation-by-operation)
- [Lifetimes](#lifetimes)
- [`Scope`](#scope)
- [Relation to the bridge](#relation-to-the-bridge)
- [Design decisions](#design-decisions)
- [Cross-references](#cross-references)

## Nodes

| Type | Behaviour |
|---|---|
| `Signal<T>` | A source. `get()` subscribes the innermost tracked run; `peek()` does not. `set(v)` skips a value equal to the current one ([the equality gate](#the-equality-gate)); `mutate(f)` changes the value in place and always notifies. |
| `Computed<T>` | A derived value: lazy (the first read computes it), cached in a `std::optional<T>` so `T` needs no default constructor, recomputed only after a source it read really changed. An equal result stops propagation ([the equality gate](#the-equality-gate)). A failure is state ([below](#a-computed-that-fails)). |
| `Effect` | A side effect: runs once in its constructor, then in a flush after any source it read changed. It carries the depth of the `Scope` that made it, which orders the flush ([below](#flush-order)). |
| `Scope` | Owns Effects, Computeds and adopted objects, destroys them newest first, and nests ([below](#scope)). |

Every node is non-copyable and non-movable: other nodes hold raw pointers to it, and its
destructor unlinks it in both directions. `Runtime` is non-copyable and non-movable too. Every node
takes the `Runtime&` it belongs to; the graph has no global and no `thread_local` state, so two
runtimes on two owners never see each other.

`Computed` and `Effect` accept a move-only callable: it is stored behind a `shared_ptr`, which is
also what keeps it alive for a run that destroys its own node.

<a id="the-equality-gate"></a>**The equality gate.** `Signal::set`, `Computed` and a `Query`'s key skip
an equal value when `kEqualityUsable<T>` holds and the node's `EqualityPolicy` is `Auto` (the default).
`kEqualityUsable<T>` is true when `==` is usable for `T`, checked through element types: a range through
its element type, and `std::optional`, `std::pair`, `std::tuple` and `std::variant` through theirs. The
standard containers declare an unconstrained `operator==`, so `std::equality_comparable<std::vector<X>>`
holds even when `X` has no `==`, and comparing two of them then fails to compile; looking through is what
lets a `Signal<std::vector<X>>`, a `Signal<std::map<K, X>>` or a `Signal<std::variant<int, X>>` of such an
`X` compile. A type for which it is false always notifies. `EqualityPolicy::Always` notifies on every write
or recomputation of a type that does have `==`: a tick, or a value whose `==` is coarser than what a view
shows.

## The owner

A `Runtime` is bound to one **owner** executor, borrowed: it must outlive the runtime and every
node made from it. Every operation on the graph runs on the owner, and flushes are posted to it.

The owner must be serial (`IExecutor::isSerial`), and queue what is posted and run it later, in order,
as `Completion` already requires of its owner: an executor that runs a post inline would run Effects inside
`set()`, and a pool would run two flushes at once. A `Runtime` given an owner that is not serial throws
`std::invalid_argument` from its constructor.

"On the owner" is `exec::detail::OwnerAffinity`'s answer ([`core/executor.md`, "Owner
affinity"](../core/executor.md#owner-affinity)): inside a task the owner runs, or anywhere on the
thread that constructed the runtime, when that thread was running no executor's task at
construction — a Qt slot, a QML handler, `main()`, a test body. A runtime constructed inside a
task records no thread and is on its owner only inside the owner's tasks.

The thread half is a per-thread check, not a per-executor one: on the constructing thread, a task
of a *different* executor is accepted as the owner too. Running another executor's tasks on the
owner's thread and touching the graph from them is therefore not reported. It races nothing,
since it is the same thread, but it is outside the owner model all the same.

## Scheduling

- A write outside `Runtime::batch` is a batch of one; batches nest. Only a write that changes
  something opens one: a `set()` of an equal value marks nothing.
- When the outermost batch ends with Effects queued, and no flush is requested or running, the
  runtime posts **one** flush to its owner. The coalescing flag lives in the core because the owner
  may not coalesce posts itself (`QtExecutor` does not): N writes are still one flush.
- No Effect runs inside `set()`. A write is therefore safe from a Qt handler or a `Completion`
  callback, and a controller can write several signals before anything observes them.
- A batch whose body throws still closes, so what it wrote before the throw is flushed.
- Writes an Effect makes during a flush are queued into that same flush, not a new one.
- The posted flush holds the core weakly and does nothing once the `Runtime` is gone.
- The runtime marks the flush requested before it posts. If posting throws (an allocation failure), the
  mark is cleared and the queue is kept, so the next write posts again; the write that failed to post
  returns normally.
- `RuntimeOptions::afterFlush` is called on the owner after every flush that processed a queued
  Effect, including one whose queued Effects all found their sources unchanged and one stopped by
  a misuse. A frontend schedules its redraw there; a redraw for a flush that changed nothing
  visible is harmless. A posted flush that finds nothing queued, or the runtime gone, or a widget
  event open, does not call it. An exception it throws is reported (`kAfterFlushThrew`) and
  dropped: it runs as a task of the owner executor, which has nothing to do with it, and the flush
  is complete by then.
- `RuntimeOptions::maxEffectRunsPerFlush` bounds how often one Effect may run in a single flush
  before the flush is treated as a write cycle. The default is
  `detail::kDefaultMaxEffectRunsPerFlush`, **100**.
- `RuntimeOptions::maxThrowReposts` bounds how many flushes in a row are re-posted after an Effect
  threw ([below](#misuse-reported-then-refused)). The default is `detail::kDefaultMaxThrowReposts`,
  **3**.
- `Runtime::untracked(f)` runs `f` with tracking off: reads inside it subscribe nothing.
- `Runtime::widgetEvent(f)` runs a widget callback as one batch. A renderer runs every user event
  inside one. A flush that falls due while a widget event is open is **deferred** until the outermost
  widget event returns: no write inside it posts (its batch is open), and a flush already posted that a
  nested event loop inside the callback picks up (a modal dialog) returns at once, keeping the queue,
  without re-posting. When the outermost widget event's batch closes, it posts the flush. So a remount
  never destroys a widget whose native handler is on the stack, and a nested loop neither runs the
  flush under the handler's stack nor spins on a re-post. This is not a misuse and reports nothing.
- `Runtime::isFlushRequested()` says whether a flush is posted and has not run yet.

## The algorithm

Push-dirty, pull-value, with three colours ordered `Clean < Check < Dirty`, as in Reactively and
Preact signals:

1. **Push.** A write marks the signal's direct observers Dirty and their transitive observers
   Check. A node already at least that stale stops the walk, since its observers were marked when
   it turned stale — unless one of them has ended Clean above it since ([below](#clean-above-stale)),
   in which case the walk passes through it once. An Effect leaving Clean is queued; it is queued
   before its colour changes, so an enqueue that fails to allocate leaves it Clean. The next write
   queues it again when it observes the written signal directly, or when every node between them
   was flagged before the failed walk (a flag the walk cleared is restored). An Effect behind a
   Computed that the failed walk raised from Clean is not reached again, because that Computed now
   stops the walk.
2. **Pull.** The flush takes queued Effects in [flush order](#flush-order), owners first, and
   brings each up to date. A Check node brings
   its sources up to date one by one, in the order its last run read them, and stops as soon as one
   of them recomputes to a changed value, which marks the node Dirty. Only a Dirty node recomputes.
   A Computed whose result is unchanged does not mark its observers, so propagation stops there.
3. **Re-tracking.** A run records what it reads in a `TrackingFrame`, deduplicated, in first-read
   order. Afterwards the node's sources are replaced by that set, so a source the run stopped
   reading no longer reaches it. Linking the new sources is the only step that allocates and runs
   first; if it throws, what it linked is undone. A run that throws keeps the union of its old
   sources and those it read before throwing, so a later change to any of them still reaches it.

Because a node is pulled only through its sources and recomputed only when a source really
changed, every node sees one consistent state: an Effect reading `A` and a `Computed` of `A` never
sees one updated and the other stale, and the bottom of a diamond recomputes once per change.

An Effect that writes a signal it is reading does not re-run itself for that write: it is mid-run
and Dirty, so the write cannot raise it further, and it ends Clean. The exemption covers only a
signal the Effect reads directly. One it reads through a Computed is a real feedback loop: the write
leaves that Computed stale, and the Effect re-runs once anything else pulls the Computed: a read
outside a flush, or another Effect reading it, in which case it is a write cycle, cut at the bound
and reported (`kWriteCycle`).

<a id="flush-order"></a>**Flush order: owners first.** Queued Effects run in scope-depth order, shallowest
first, and in creation order within one depth. An Effect a `Scope` makes carries the scope's depth (a root
scope is depth zero, `child()` is one deeper); an Effect no scope made is depth zero. Every node made on the
owner also takes the next number of a counter in the runtime. The queue is a binary heap on the pair, so the
flush always runs the shallowest, then oldest, queued Effect next.

So an Effect owned by a scope runs after the Effects of the scopes that own it. When a single update both
changes what a row's binding reads and makes the row's owner remove the row, the owner runs first and
destroys the row's scope, whose binding never runs against the state its owner is leaving — whichever field
the update wrote first, and whichever Effect was made first (a renderer may bind a row before it binds the
owner's own state). Within one depth, an Effect created while another runs is newer than the Effect that
created it, and a remount gives the child a newer number again. An Effect created during a flush runs, when
queued, after every older one of its depth. Order does not affect what a run reads, which is pulled either
way; it decides only which Effects still exist to run. Queuing and taking an Effect cost O(log n) in the
queue's length; destroying a queued Effect costs O(n), as finding it in the queue already did.

<a id="clean-above-stale"></a>**A Clean node above a stale source.** Three paths end a node Clean
while one of its sources is still stale: an Effect marked Clean without running when a write cycle
is cut; an Effect whose own write staled a Computed it had already read; and a run that threw before
reading a stale source it keeps. Such a source would stop the next write's walk below the node.
So whenever a node ends a pull or is marked Clean without running, its sources are visited: each
stale one is flagged and its own sources are visited in turn. The visit stops at a Clean source,
which a walk passes anyway, and at one already flagged, whose sources were visited when it was
flagged. The next walk that reaches a flagged node clears the flag before passing through to its
observers, so meeting that node again on the same walk stops there. That bounds the walk when
flagged Computeds read each other (a cycle a refused self-read leaves linked). Colours are kept, so
the next pull still recomputes exactly what changed.

## A Computed that fails

A computation that throws leaves the Computed **failed**: the exception is stored, and that is a
change, so its observers re-run. While it is failed, a read behaves as follows:

- The first read after the failing run rethrows that run's exception without computing again.
- Every later read retries the computation, returning its value if it now succeeds and rethrowing
  if it fails again.

A retry that fails again counts as unchanged and notifies nobody, so readers that retry do not wake
each other. A failure after a source really changed always propagates, as does a recovery, even to
the value the Computed had before it failed: its observers last saw a failure. A read of a failed
Computed whose sources changed but compared equal is a retry.

Every read of a Computed on the owner opens a batch, so a retry that recovers outside a flush posts
the flush its newly woken observers need. A read off the owner opens none: it returns the cached
value ([below](#off-the-owner-operation-by-operation)).

## Misuse: reported, then refused

A misuse is reported through `exec::detail::noteOwner(site, owner, false)` and then refused, the
same way in every build:

- With an owner probe installed (`exec::detail::ownerProbe()`; the test suite's
  `OwnerProbeRecorder`), the probe receives the site name and the misuse is an observation.
- Without one, a build without `NDEBUG` asserts; a build with it reports nothing.

The refusal does not depend on `NDEBUG`, so a release build behaves as a test observing the probe
does. The site names are the `detail::site::*` constants.

| Misuse | Site | Refusal |
|---|---|---|
| An operation off the owner | `kOffOwner` | Depends on the operation: [below](#off-the-owner-operation-by-operation) |
| An exception escaping an Effect during a flush | `kEffectThrew` | The flush stops; what is still queued runs in a new posted flush, so a defect does not stall the queue until the next write. At most `maxThrowReposts` such re-posts happen in a row, so an Effect that throws on every run cannot loop for ever; past the bound the queue waits for the next write from outside a flush, which starts a new count. The Effect keeps the sources of the failed run. |
| An exception escaping an Effect's first run, in its constructor | `kEffectThrew` | The constructor returns normally; the Effect is live and re-runs when a source it read changes. |
| One Effect run more than `maxEffectRunsPerFlush` times in one flush (a write cycle) | `kWriteCycle` | The flush stops, and that Effect and everything still queued are marked Clean without running. Since queued Effects run in [flush order](#flush-order), what is still queued includes every Effect after the cut one that the same writes woke: in a view tree, everything deeper or mounted after it stays stale until a source it reads is written again. Nothing is re-posted. The next write to a signal any of them reads, directly or through any chain of Computeds, queues them again; a cycle it resumes is cut again at the bound. |
| `set()` or `mutate()` while a Computed computes | `kSetInComputed` | The write is dropped. |
| Reading a Computed while it is being brought up to date: from its own computation, or from the computation of a node it reads (a cycle, which may form only after the first evaluation) | `kComputedReadsItself` | The read throws `std::logic_error`. |
| `Store::send` from inside that Store's update | `kSendInUpdate` | The message is dropped ([`store.md`](store.md)). |
| `Query::refetch` or `Mutation::run` while a Computed computes | `kIssueInComputed` | Nothing is issued, and no state changes ([`control.md`](control.md)). |
| An exception escaping `RuntimeOptions::afterFlush` | `kAfterFlushThrew` | Dropped; the flush is complete and the runtime unchanged. |
| A `Runtime` destroyed while nodes made from it are alive | `kRuntimeOutlived` | The core is detached: the nodes stay valid, a write still marks observers but posts nothing, and a flush already posted does nothing. |
| A control node built over a handler whose callbacks are not delivered on the owner | `kHandlerExecutor` | The node is built; see [`control.md`](control.md). |

A `Runtime` whose owner is not serial is refused at construction with `std::invalid_argument`, before
any node exists. Two more failures throw `std::logic_error`: a read whose computation destroys the Computed it reads,
which reports nothing, and a read off the owner of a Computed that has never computed successfully,
which is reported as `kOffOwner` first.

## Off the owner, operation by operation

Every operation checks the owner on entry and reports `kOffOwner` when it is not there. What it
does next depends on what refusing can mean for it:

| Operation | Off the owner |
|---|---|
| `Signal::set`, `Signal::mutate` | Dropped. |
| `Signal::get` | Returns the value without subscribing anything and without touching the tracking state. The value is read without synchronisation against the owner's writes. |
| `Signal::peek` | Not checked: it reads the value and does nothing else. |
| `Computed::get`, `Computed::peek` | Recomputes nothing and subscribes nothing; returns the value of the last successful computation, read without synchronisation. Throws `std::logic_error` if there has never been one. |
| `Runtime::batch`, `widgetEvent`, `untracked` | The body still runs, because the caller may need what it returns, but the runtime's state (batch depth, widget-event depth, tracking frame) is left alone. Each graph operation inside the body is checked, reported and refused on its own. |
| Constructing a node | The node is not counted among the runtime's live nodes, whose counter belongs to the owner. An Effect constructed off the owner never runs. |
| Destroying a node | Reported, but destruction cannot be refused: the destructor still edits the owner's graph, queue and counters, and what that does beyond the report is undefined. |
| `Store::send`, `Query::refetch`, `Mutation::run`, a reply or publish delivered to a `Query`, `Mutation` or `Subscription`, a timed-refresh tick | Dropped ([`store.md`](store.md), [`control.md`](control.md)). |

`Scope` itself checks nothing: only the nodes it makes do ([below](#scope)).

## Lifetimes

- Destroying a node unlinks it from its sources and observers and removes every reference the core
  holds to it: its place in the flush queue and its entry in every open tracking frame. A queued
  Effect destroyed during a flush is skipped.
- A node may be destroyed during its own run — an Effect body that remounts the view owning it, a
  computation that destroys its Computed. The run keeps its callable alive until it returns, the
  pull running on the node is told, and neither touches the node again. Reading a Computed whose
  computation destroys it throws `std::logic_error`; during a flush, the graph is left intact.
- Nodes hold the core by `shared_ptr`, so a node that outlives its `Runtime` is a reported misuse
  (`kRuntimeOutlived`), not a use-after-free.
- The `Runtime` and every node must be destroyed on the owner.

## `Scope`

A `Scope` owns Effects, Computeds and arbitrary objects, and destroys them in reverse creation
order. A mounted view, a Switch case and a ForEach row each own one, so tearing one down stops
every binding it made before anything those bindings point at goes away.

- Scopes nest. `Scope(runtime)` is a root, at depth zero; `Scope(runtime, depth)` takes the depth it is
  given; `child()` makes and owns a scope one level deeper. `depth()` reads it. The Effects a scope makes
  carry its depth, which is what runs an owner's Effects before those of the scopes it owns
  ([flush order](#flush-order)).

- `make<T>(args...)` constructs and owns a `T`; `adopt(unique_ptr<T>)` takes ownership of one;
  `effect(fn)` and `computed(fn)` make the two node types, a Computed's type being what `fn`
  returns. Each returns a reference that stays valid until the scope is cleared or destroyed.
- `clear()` destroys everything owned, newest first. Each object leaves the list before it is
  destroyed, so a destructor or a running Effect body that calls back into the scope (`clear()`,
  `make()`) sees a consistent list. A destructor that keeps calling `make()` while the scope is
  clearing never lets `clear()` finish.
- A constructor that throws inside `make()` leaves the scope usable and destroyable.
- An Effect the scope owns may clear or destroy the scope from its own body — a remount. The graph
  keeps the running body alive, and the body must not touch the scope or anything it owned
  afterwards. That holds for the first run too, which happens inside `effect()` before the scope
  stores the Effect:
  - if the first run clears the scope, the new Effect is stored after the clear and stays;
  - if it destroys the scope, the new Effect is destroyed at once, since a dead scope owns
    nothing, and the reference `effect()` returns dangles. That destructor runs after the scope is
    gone, so it must not use the scope. A `make()` nested inside another learns of the destruction
    too, and neither touches the scope afterwards.
- The runtime is borrowed and must outlive the scope.
- Only the nodes check the owner (`kOffOwner`); the scope's own list is unguarded, so using a scope
  off its runtime's owner is a misuse that is not reported.

## Relation to the bridge

This layer is UI-side only. It consumes `Completion` and `BridgeHandler::subscribe` like any other
caller and never enters `BridgeHandler`; nothing in the bridge, the backends or the model layer
knows a reactive runtime exists, and nothing in `morph::reactive` includes `morph::ui`,
`morph::tui` or a toolkit.

## Design decisions

| Decision | Why |
|---|---|
| Fine-grained signals, not re-render plus diff | A bound property updates exactly the widget setter that depends on it; no virtual tree, no diff, and widget identity (focus, selection) survives updates. |
| Flushes are always posted | Running Effects inside `set()` re-enters view code from wherever the write happened — a Qt handler, a `Completion` callback — and makes intermediate states visible. |
| Queued Effects run owners first: by scope depth, then by creation | An owner runs first and unmounts a child before the child can read state the owner has left. In write order, whether a Switch case or a dialog body sees such a state would depend on the order an update happened to write its fields; in creation order alone, on whether the renderer bound the child before its owner. |
| A widget event defers the flush instead of refusing it | A modal dialog spins a nested event loop inside a widget handler. Refusing and re-posting there makes that loop spin on the re-post; deferring to the end of the outermost widget event runs the flush exactly once, after the handler's stack is gone. |
| Re-posts after a throwing Effect are bounded | One re-post keeps a defect from stalling the rest of the queue until the next write; a bound keeps an Effect that throws on every run from turning the owner into a busy loop. |
| One coalescing flag in the core | The owner executor may not coalesce (`QtExecutor` does not); N writes must still be one flush. |
| Report-then-refuse through `noteOwner` | One mechanism, already observable in morph's tests through the owner probe, with the same refusal in release builds. |
| The equality gate looks through element types | `std::equality_comparable` says yes for a container of a type without `==`, and the comparison then fails to compile; a view state of rows of a plain struct must still compile, and notify. |
| A Computed's failure is state | A view bound to a Computed that throws shows the error instead of losing the flush; a retry that keeps failing stays quiet, and a recovery always reaches the observers that saw the failure. |
| Nodes hold the core by `shared_ptr` | A node outliving its `Runtime` is a defect, but not a use-after-free. |
| No global, no `thread_local` | Each graph belongs to the `Runtime` its nodes were given, so two runtimes, two owners or two tests never share state. |

## Cross-references

- [`store.md`](store.md) — `Store<ViewState, Msg>`, user-intent state on these nodes.
- [`control.md`](control.md) — `Query`, `Mutation`, `Subscription`: server interaction as nodes.
- [`../core/executor.md`](../core/executor.md) — owner affinity and the owner probe.
- [`../core/completion.md`](../core/completion.md) — why a `Completion` callback may write a signal.
- [`../core/callback_scope.md`](../core/callback_scope.md) — the gate every reply into this layer
  passes through.

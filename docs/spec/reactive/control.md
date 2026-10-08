# Declarative control — design

Design spec for `morph::reactive::Query`, `QueryOptions`, `InvalidationLink`, `Mutation`, `MutationOptions`,
`Concurrency`, `Subscription`, `Refetchable` and `errorMessage` (`include/morph/reactive/control.hpp`), and for
`Scheduler`, `TimerHandle` (`include/morph/reactive/scheduler.hpp`) and
`testing::ManualScheduler` (`include/morph/reactive/testing/manual_scheduler.hpp`): server
interaction as reactive state, so a controller states what it depends on and what a write
invalidates, and the runtime decides when to talk to the model.

Read this before writing a controller, and before sequencing a call by hand in one.
[`signals.md`](signals.md) specifies the nodes underneath.

## Contents

- [The problem](#the-problem)
- [`Query<A, R>`](#querya-r)
- [`Mutation<A, R>`](#mutationa-r)
- [`Subscription<R>`](#subscriptionr)
- [`errorMessage`](#errormessage)
- [The handler's callback executor](#the-handlers-callback-executor)
- [The controller](#the-controller)
- [Design decisions](#design-decisions)
- [Out of scope](#out-of-scope)

## The problem

A hand-written controller sequences calls: selecting an account calls `reloadHistory`, a deposit's
`.then` calls `refresh`, every call repeats a rethrow/catch to produce an error string, and `busy`
is a counter kept by hand. Two defects follow from that shape:

- **Stale replies win.** Select A, then B; if A's reply lands after B's, the screen shows A's data
  under B.
- **Invalidation is a call graph.** What a write must refresh lives in whichever `.then` remembered
  to call `refresh()`.

## `Query<A, R>`

A derived async resource: the result of fetching whatever its tracked key names.

### Construction

`R` defaults to `model::ActionTraits<A>::Result`. A query is built from:

- a `BridgeHandler<M, S>` — the model is deduced, and the handler must deliver its callbacks on the
  runtime's owner ([checked](#the-handlers-callback-executor)) — or any
  `std::function<Completion<R>(A const&)>` fetcher, whose `Completion` must deliver on the runtime's owner.
  The fetcher is the test seam, the way to query a non-bridge source, and the path the document
  interpreter takes: its fetcher is model-free JSON dispatch through the client's query cache, which shares
  one request and one value among queries with equal keys (spec 5 §5, "The query cache");
- a tracked key, `std::function<std::optional<A>()>`;
- optionally `QueryOptions` ([Timed refresh](#timed-refresh), [Debounce](#debounce)).

The runtime and the handler are borrowed and must outlive the query. The query reads its key in
its constructor and, unless the key is `nullopt`, issues the first call there.

### Latest wins

An internal Effect reads the key. A changed key is fetched under a fresh generation of the query's
`CallbackScope` (`reset()`). The comparison is equality-gated when `==` is usable for `A`
(`kEqualityUsable`, [`signals.md`](signals.md#the-equality-gate)); otherwise every re-run of the key
re-issues. Give an action
`operator== = default` to get the skip. Keys that change and change back within one flush — A,
then B, then A — run the Effect once, and it sees A: with `==` that issues nothing; without it,
A is issued again.

Issuing a call supersedes the one in flight: its reply is dropped, success and failure alike, and
a call that carries a stop source — a bridge call whose handler returns a `Task` — is asked to stop.
`refetch()` supersedes the same way. Destroying the query does the same to the call in flight.

The fetcher runs untracked whoever issues the call — the key's Effect, `refetch()`, an
invalidation, a timer tick. A `refetch()` called from an Effect therefore does not subscribe that
Effect to what the fetcher reads; otherwise a change there would re-run the Effect and issue again.

### Idle

A `nullopt` key is **idle**: the call in flight, if any, is superseded (asked to stop, its reply
dropped), `pending()` is false, `value()` and `error()` are cleared, no timer runs, and `refetch()`
does nothing.

### State

All three accessors are tracked reads:

- `pending()` — true from issue to delivery.
- `value()` — the last successful result, kept while a refetch or a new key is in flight, so a list
  does not blank between reloads; `nullopt` before the first success and while idle. A failure
  clears it when the call was for a different key than the one the value was fetched for, and keeps
  it when the call refetched that same key.
- `error()` — the last failure, cleared by the next success.

After a key change, the previous key's value and failure remain while the new key is in flight.
When the new key settles, a success replaces both, and a failure replaces the error and clears the
value: a view never shows one key's result beside another key's failure — the stale-reply defect
of [the problem](#the-problem), reached through a failure instead of a late reply. A failed
refetch, invalidation or timed refresh of the same key keeps the value beside the error, so a list
does not blank because one reload failed. "A different key" follows the key's own comparison: with
`==` on `A`, a key that compares equal is the same key; without it, every re-run of the key is a
new one.

A delivery writes its fields in one batch.

### Debounce

`QueryOptions{.scheduler, .debounce}` fetches a changed key only once it has stayed the same for `debounce`.

- The first key is fetched at once, and a change to idle applies at once.
- Any other change supersedes the call in flight at once (asked to stop, its reply dropped), sets
  `pending()`, and starts a wait of `debounce` on the scheduler; a further change restarts the wait, so only
  the last key of a burst is fetched. `value()` is kept while the query waits.
- `refetch()`, an invalidation or a timed refresh issues the current key now and ends the wait.
- A `debounce` of zero, the default, fetches at once. A negative one, or a positive one with a null
  `scheduler`, throws `std::invalid_argument` from the constructor before anything is fetched.
- Destroying the query cancels the wait.

### Invalidation links

`link()` returns the query's `InvalidationLink`, which a mutation holds in
`MutationOptions::invalidates`. The query owns the link and clears it in its destructor, so a mutation
never refetches a query that is gone, and the two may be destroyed in either order.

### Refresh on publish

`refreshOn<Pub>(handler)` re-fetches whenever the bridge publishes a `Pub` on the handler's
instance. It uses the handler's one subscription slot for `Pub`: two consumers of the same `Pub`
use two handlers. The subscription is gated by the query's lifetime. The handler is borrowed and
must outlive the query.

### Timed refresh

`QueryOptions{.scheduler, .refreshEvery}` makes a query re-fetch its current key on a timer.

- `refreshEvery` of zero, the default, is no timed refresh. A negative period, or a positive one
  with a null `scheduler`, throws `std::invalid_argument` from the constructor before anything is
  fetched.
- The period restarts whenever a call is issued, whatever issued it: a key change, `refetch()`, an
  invalidation by a `Mutation`, or the timer itself. One period after the last issue, the query
  re-fetches its current key.
- A tick that finds a call in flight, or a debounce wait, issues nothing and waits another period. A timed refresh
  therefore never supersedes a call, and a fetch slower than the period still delivers; ticks
  skipped during a long call leave one timer, which re-fetches one period after the last skip.
- A fetcher that throws still restarts the period: the timer is armed before the fetcher runs, so
  the failed fetch is retried one period later.
- While idle, no timer runs; going idle cancels it.
- A tick off the runtime's owner is reported (`kOffOwner`) and refused, and the timer then stays
  stopped until the next call is issued.
- Destroying the query cancels the timer, and a tick a scheduler delivers afterwards is dropped:
  ticks are gated by the query's lifetime scope.
- The scheduler is borrowed and must outlive the query.

#### The `Scheduler` contract

`Scheduler` runs callbacks on the runtime's owner: `after(delay, fn)` once, `delay` from now (a
non-positive delay means as soon as possible), and `every(period, fn)` every `period`, starting one
period from now (a non-positive period throws `std::invalid_argument`). Each returns a
`TimerHandle`. Every frontend provides one on its own event loop's timers; a test uses
`testing::ManualScheduler`. Every implementation guarantees:

- Once a handle's `cancel()` returns on the owner, its callback never runs, even when its deadline
  has passed and a call is already queued.
- `cancel()` may be called from inside a callback, its own timer's included.
- A `TimerHandle` may outlive its `Scheduler`; cancelling it then does nothing.
- `every()` fires once per period while the owner runs. If the owner is blocked past several
  periods, it fires once and counts the next deadline from that firing: no burst of missed
  firings.

`TimerHandle` is move-only and owns one timer through two functions the scheduler gives it: one that
cancels the timer and one that says whether it is still scheduled. Destroying it, calling `cancel()`,
or move-assigning over it cancels its timer. `active()` is false once a one-shot timer has fired, once
its scheduler is gone, after `cancel()` and after a move from it; a scheduler that gives no
`isScheduled` function leaves it true until `cancel()`. Cancelling a fired timer does nothing. A cancel
function that throws, against its contract, does not escape the handle; its timer is left to the
scheduler.

#### `testing::ManualScheduler`

A deterministic `Scheduler` whose clock moves only in `advance(delta)`, which fires what falls due
on the calling thread. It models time passing while the owner loop runs and is never blocked:

- The clock stops at each deadline on the way, so `every()` fires once per elapsed period, and a
  callback sees the clock at its own deadline. A periodic timer is next due one period after the
  deadline it fired for.
- Timers due at the same instant fire in the order they were created; a periodic timer keeps the
  place its first scheduling gave it.
- A timer a callback creates counts from that callback's deadline and fires in the same advance
  when its deadline falls within it. One created with a non-positive delay is due at the next
  `advance()`, at that advance's starting time, not at the current instant — which keeps a
  self-re-arming zero-delay timer from looping.
- A callback that throws ends the advance: the exception propagates, the clock stays at that
  deadline, the throwing timer is rescheduled (periodic) or gone (one-shot) as if it had returned,
  and the rest fire at the next advance.
- `advance()` with a negative delta throws `std::invalid_argument`; called from a timer callback it
  throws `std::logic_error`.
- `now()` reports the clock — the sum of every `advance()`, or inside a callback that callback's
  deadline — and never decreases. `pendingTimers()` counts periodic timers until cancelled and
  one-shots until they fire or are cancelled.
- It keeps the `Scheduler` contract, and it is not thread-safe.

### Failure and misuse

- A fetcher that throws instead of returning a `Completion` fails the request: the exception becomes
  `error()`, and nothing is pending, since the call it replaced was already superseded. It is a
  failure like any other, so it clears `value()` when it was for a new key.
- A key function that throws is an Effect that threw (`kEffectThrew`); the query keeps its state.
- A reply delivered off the runtime's owner is reported (`kOffOwner`) and dropped. `pending()` then
  stays true until the next call is issued, which supersedes the lost one.
- `refetch()` off the owner is reported (`kOffOwner`) and refused.
- `refetch()` inside a Computed's computation is reported (`kIssueInComputed`) and refused before
  anything is issued. A computation is pure: a fetch issued from one would be issued again on every
  recomputation, while `pending()` and a synchronous failure, which are writes, would be refused and
  lost.

### Lifetime

The key or the fetcher may destroy the query (a remount), and so may another handler of the reply
or an Effect the reply wakes; the query is not touched afterwards. The action a fetcher receives
belongs to the query, so a fetcher that destroys the query must read the action first.

## `Mutation<A, R>`

A command: issues a write and tracks it, then refetches what the write invalidates.

Built from a `BridgeHandler` ([checked](#the-handlers-callback-executor)) or any
`std::function<Completion<R>(A)>` runner, plus
`MutationOptions<A>{.concurrency, .serialKey, .invalidates = {queryA.link(), queryB.link()}}`. The runtime
and the handler are borrowed.

### Concurrency

`run(action)` issues, queues or refuses one call, as `MutationOptions::concurrency` says for a run that
arrives while another is in flight. The modes are the document's (spec 5 §5):

| `Concurrency` | A run while another is in flight | Replies applied |
|---|---|---|
| `Exclusive` (default) | Refused: `run` returns `false`, nothing is sent. A second click on a pending button does nothing. | The one in flight |
| `Serial` | Queued, and sent when the previous run of the same `serialKey` settles, a failure included. An empty `serialKey` puts every run in one queue. | Each, in order |
| `Latest` | Sent | Only the newest run's reply; an older reply only stops counting as pending |
| `Parallel` | Sent | Each, as it arrives |

`run` returns whether the run was sent or queued; `false` means refused (`Exclusive` busy, off the owner,
inside a Computed). The action is built by the caller per gesture, so an idempotency key minted while
building it is fresh per click. The runner runs untracked: a `run()` from inside an Effect does not
subscribe that Effect to what the runner reads, so a change there cannot issue the write a second time.

### State

All four accessors are tracked:

- `pending()` — true while a run is in flight or queued.
- `error()` — the last applied failure, cleared by the next applied success.
- `lastResult()` — the last applied result; a failure keeps it.
- `successCount()` — ticks once per applied success, so an Effect can react to "it succeeded again" when the
  result equals the last one.

An applied success sets `lastResult()`, clears `error()`, ticks `successCount()` and refetches every live
link in `invalidates` — one batch, so one flush and one frame: the owner sees one task for the delivery and
one posted flush for everything it wrote. Invalidating an idle query issues nothing, and a link whose query
is gone refetches nothing. An applied failure sets `error()` and invalidates nothing.

A runner that throws instead of returning a `Completion` fails that run at once: the exception becomes
`error()` and nothing is left pending for it, so an `Exclusive` mutation accepts the next run. In a `Serial`
queue the next queued run is sent.

**Misuse.** `run()` off the runtime's owner is reported (`kOffOwner`) and refused. `run()` inside a
Computed's computation is reported (`kIssueInComputed`) and refused before anything is issued, as a
signal write there is: a computation is pure, a write issued from one would be issued again on every
recomputation, and the pending count it could not write would never be counted back down. A reply delivered
off the owner is reported and dropped, and it is never counted down: no later reply clears it, so `pending()`
stays true for good and an `Exclusive` mutation refuses every later run. It is a wiring error, not a state
to recover from.

**Lifetime.** The runner may destroy the mutation, and so may a query's fetcher during an invalidation,
another handler of the reply, or an Effect the reply wakes; the mutation is not touched afterwards.

## `Subscription<R>`

The latest `R` the bridge published on a handler's instance, as reactive state. `latest()` is a
tracked `std::optional<R>`, `nullopt` before the first publish, fed by `handler.subscribe<R>`.

- `latest()` is **state, not an event stream**: when `R` compares with `==`, a publish equal to the
  current value does not notify. For something to happen on every publish, use
  `Query::refreshOn`.
- It uses the handler's one subscription slot for `R`: two consumers of the same `R` use two
  handlers. The handler must deliver on the runtime's owner ([checked](#the-handlers-callback-executor));
  a publish delivered anywhere else is reported (`kOffOwner`) and dropped.
- The subscription is gated by a scope the Subscription owns, so a publish delivered after it is
  destroyed — one already queued included — is dropped. The handler keeps the dead entry until it
  is destroyed or subscribes to `R` again. An Effect a publish wakes may destroy the Subscription.
- The runtime and the handler are borrowed and must outlive it.

## `errorMessage`

The one place an `exception_ptr` becomes display text: `what()` for a `std::exception`,
**`"unknown error"`** for anything else, and empty for null. Views bind to it through a `Computed`
over a query's or a mutation's `error()`.

## The handler's callback executor

A node built over a handler writes its signals from the handler's callbacks, so they must run where the
runtime runs. `Query`, `Mutation` and `Subscription` check it in their handler constructors
(`detail::checkHandlerExecutor`). The delivery executor is the handler's GUI executor, or the bridge's owner
when it has none. The check is by affinity, not by pointer identity:

- the runtime's own owner is accepted;
- an executor that is not serial (a pool) is reported at once (`kHandlerExecutor`);
- any other serial executor is checked where it runs: the node posts one task to it, which reports
  `kHandlerExecutor` if it finds itself off the runtime's owner. A distinct executor whose tasks run on the
  owner's thread is therefore accepted, and one on another thread is reported when it first runs.

The node is built either way; what it then receives off the owner is reported and dropped, as above.

## The controller

A controller is a plain user struct, not a framework base class. It owns, in this order: handlers,
a `Store` if it has user-intent state ([`store.md`](store.md)), Queries, Mutations, Subscriptions,
and `Computed` projections (row formatting, masking, aggregates). Each Query, Mutation and
Subscription declares its own `CallbackScope` last, so it is destroyed first and gates its replies.
A controller includes nothing from `morph::ui`, `morph::tui` or a toolkit, so it is testable with an
executor and a local bridge and no frontend mounted, and the same controller drives every frontend.

## Design decisions

| Decision | Why |
|---|---|
| Generations through `CallbackScope::reset()` | It is morph's supersede verb: it gates the stale reply and asks a stoppable call to stop, with no counter to keep. |
| Keep `value()` while refetching | A list does not blank between reloads; `pending()` says a reload is happening. |
| Invalidate in the success batch | The result, the cleared error, the pending count and the refetches are one flush and one frame. |
| A fetcher constructor beside the handler one | Any ordering of replies can be forced in a test with `Completion::makeSettleable`. |
| A synchronous throw becomes `error()` | A fetcher or runner that throws before returning a `Completion` would otherwise leave `pending()` set with nothing in flight: a spinner that never clears. |
| The refresh period restarts per issue, and a tick never supersedes | A timed refresh is a floor on freshness, not a deadline: it must not cancel a slow call it would only re-issue, and a user-driven refetch already made the data fresh. |
| `Scheduler` is an interface the frontend implements | Each event loop owns its timers; the query only needs "call me on the owner later", and a test needs a clock it controls. |
| `Subscription` is state | A view binds to the latest value; an event per publish is a refetch, which `refreshOn` provides. |
| `Exclusive` is the default concurrency | A mutation is a user's write: a second click while the first is in flight is almost always a double submit. The other modes are opt-in, and each is what a document's `concurrency` names. |
| Invalidation through links the query clears | A mutation and the queries it refreshes often live in different scopes; a bare pointer would make destroying a query first a use-after-free. |
| Debounce supersedes at once and waits to fetch | A reply for a key the user has already left must not land, and the key a burst ends on is the only one worth a request. |
| The handler check is by affinity | A frontend may deliver on an executor of its own that runs on the GUI thread; identity would refuse that, and a pool would pass an identity-free check that only looked at threads. |

## Out of scope

Paging and optimistic-write policies. The screens that prove this layer do not need them; one that
two examples need becomes an option rather than being hand-rolled twice.

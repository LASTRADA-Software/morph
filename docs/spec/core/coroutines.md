# Coroutines — design

morph's asynchronous surface is `Completion<T>` plus the executors that run
its handlers. This spec describes how coroutines built on core-cpp's
`core::async::Task<T>` sit on top of that surface. There are two sides:

- **Client side.** A coroutine can `co_await` a `Completion<T>`, which is
  usually the result of a `BridgeHandler<Model>::execute`.
- **Model side.** A model's action handler can itself be a coroutine that
  returns `core::async::Task<R>`. The bridge drives it on the model's strand.

It adds no wire message and changes no backend protocol. A Task handler
behind a remote backend is driven on the server exactly as it would be
locally, and its result travels back as the same `ok` or `err` envelope a
synchronous handler's does.

The implementation lives in `include/morph/core/coroutine.hpp`, apart from
three pieces: the awaiter hook in `completion.hpp`, the handler detection in
`model.hpp`, and the driver at the two execution sites (`bridge.hpp`'s
`localOp` and `registry.hpp`'s `ActionDispatcher` runner).

## Contents

- [Where a coroutine resumes](#where-a-coroutine-resumes)
- [Client side](#client-side)
  - [`co_await` on a `Completion<T>`](#co_await-on-a-completiont)
  - [Cancellation](#cancellation)
  - [`spawn` — the entry point from non-coroutine code](#spawn--the-entry-point-from-non-coroutine-code)
  - [`delay`](#delay)
- [Model side](#model-side)
  - [Task handlers](#task-handlers)
  - [The handler's resumer](#the-handlers-resumer)
  - [Not re-entrant: the action gate](#not-re-entrant-the-action-gate)
  - [Execute deadlines](#execute-deadlines)
  - [Journal and observability](#journal-and-observability)
- [Single-threaded WebAssembly](#single-threaded-webassembly)
- [Failure modes](#failure-modes)
- [Design decisions](#design-decisions)
- [Limitations](#limitations)
- [Cross-references](#cross-references)

## Where a coroutine resumes

`core::async::Task` carries a stop token from awaiter to awaitee, but no
executor. Where a coroutine resumes is core-cpp's current-executor context
(`<core/async/ExecutorContext.hpp>`):

- An executor that resumes coroutines states itself as the current executor
  around each resumption, or each batch of them, with
  `core::async::ExecutorScope`. morph's two do: the adapter `spawn` builds over
  a `morph::exec::IExecutor`, and a Task handler's resumer (see
  [The handler's resumer](#the-handlers-resumer)). So do core-cpp's: a strand
  once per batch, `core::async::ThreadPoolExecutor` once per worker thread,
  `core::net::EventLoop` once per turn.
- An awaitable reads it in `await_suspend`, on the thread that is suspending
  the coroutine, as a `core::async::ResumeTarget`, and hands the continuation
  back to it. Where the executor's lifetime is shared -- a model instance's
  strand, a handler's resumer -- the target keeps it alive until then.

The rule:

> **A coroutine resumes on the executor it suspended on.** If an executor was
> current when it suspended, the continuation is submitted to it. If none was,
> it resumes wherever the awaited operation completed: for a `Completion<T>`
> that is the completion's own executor, where its `then()` handlers run; for
> `delay` it is `TimeoutScheduler`'s thread.

So a coroutine started with `spawn(exec, …)` resumes on `exec`, and a Task
handler on its model's strand, after every `co_await` of an awaitable that
follows the rule. This holds even when what it awaited completed on some other
executor, such as another model's completion delivered on the worker pool.

morph's awaiters (`Completion<T>`, `delay`) and core-cpp's
`core::async::AsyncQueue::pop` follow it, a stop included. `core::net`'s socket
and timer awaitables do not: they resume on their `EventLoop`, and a coroutine
that awaits one carries on on the loop's thread, off its strand, until it goes
back. Inside a Task handler `core::async::currentExecutor()` is the handler's
resumer, which lives as long as the handler does, so this is how:

```cpp
auto* const strand = core::async::currentExecutor();     // on the strand
auto bytes = co_await socket.read(buffer);               // on the loop
co_await core::async::ResumeOn{*strand};                 // on the strand again
```

Whatever the handler does between the foreign await and the hop runs beside
its model's other work, not serialised with it. Its end is safe either way: the
driver runs what follows a handler's end -- recording it, settling the call,
leaving the action gate, starting the next action -- on the strand, from
wherever the handler finished.

## Client side

### `co_await` on a `Completion<T>`

```cpp
core::async::Task<void> refresh(BridgeHandler<AccountModel>& accounts)
{
    auto pending = accounts.execute(GetBalance{ .id = 42 });
    Balance const balance = co_await std::move(pending);   // T, or rethrows
    show(balance);
}
```

- **`operator co_await() &&` only.** Awaiting consumes the `Completion`, so it
  is an rvalue operation. `co_await pending` on an lvalue does not compile,
  and `tests/compile_checks/` holds a `requires` check that says so.
- **The result.** `co_await` yields `T` — a copy of the settled value, because
  `CompletionState` never moves out of its stored value (other handlers may
  read it) — or rethrows the stored `exception_ptr`.
- **How it attaches.** The awaiter uses the completion's ordinary `then` and
  `onError` fan-out, so handlers attached before or after it still run. The
  await is one more handler pair, not a replacement for them.
- **Where it resumes.** See the rule above: on the executor the coroutine
  suspended on, or on the completion's executor if there was none.
  With a `MainThreadExecutor` as the completion's executor, the coroutine
  resumes inside `runFor`.
- **An already-settled completion.** The coroutine still suspends and resumes
  through the executor. It never continues inline, because `then()` on a ready
  state posts too, and one rule is simpler to reason about than two.
- **An empty completion.** A default-constructed or moved-from `Completion`
  has no state. Awaiting it throws `std::logic_error` from `await_resume`
  without suspending.

### Cancellation

If the awaiting promise satisfies `core::async::HasStopToken` (every
`core::async::Task` does) and its token can be stopped, the awaiter registers
a stop callback for the duration of the suspension:

- **Stop wins or completion wins, exactly once.** The awaiter and its two
  completion handlers share a small state with one atomic outcome: pending,
  settled or cancelled. Whichever of *the completion's handler* and *the stop
  callback* moves it off pending resumes the coroutine. The other finds it
  already decided and does nothing.
- **A stop detaches the continuation.** The handlers attached to the
  completion hold only that shared state, never the coroutine frame, and are
  guarded by a `CallbackToken` whose scope the stop ends. A completion that
  settles later therefore reaches nothing, and the coroutine's frame, with
  everything it captured, is released as soon as the coroutine unwinds.
- **The coroutine resumes with `core::async::OperationCancelled`,** thrown
  from `await_resume`. It resumes where a normal completion would have: on the
  executor it suspended on, or on the completion's executor.
- **A token already stopped at `co_await`** resumes with
  `OperationCancelled` without attaching anything to the completion.

The completion itself is not cancelled: it is still settled by whatever
produces it, and its other handlers still run. Cancellation withdraws this one
await.

### `spawn` — the entry point from non-coroutine code

```cpp
void morph::async::spawn(morph::exec::IExecutor& exec, core::async::Task<void> task);
```

`spawn` starts @p task detached, and every resumption of it — its first
step included — is posted to `exec`. This is the Qt/QML entry point: with a
`QtExecutor`, a GUI flow written as a coroutine runs every step on the GUI
thread.

- **Detached.** Nothing waits for the task. The frame is freed when it
  finishes.
- **An exception the task lets escape is logged** through `morph::log` and
  swallowed, the same as an exception from a posted task on morph's executors.
- **`exec` must outlive the task.** The adapter that resumes the task holds
  `exec` by reference, as every `Completion` holds its callback executor.
- **No stop source.** The task's stop token is one that is never stopped. A
  flow that needs cancellation owns a `core::async::StopSource` and checks it,
  or awaits under a scope of its own.

### `delay`

```cpp
auto morph::async::delay(morph::async::detail::TimeoutScheduler& scheduler,
                         std::chrono::milliseconds duration);   // an awaiter
```

`co_await delay(scheduler, 50ms)` suspends for at least @p duration.

- **Timing.** The timer is one `TimeoutScheduler` entry, so it fires on the
  scheduler's loop thread natively and on the browser's timer under
  single-threaded WebAssembly.
- **Where it resumes.** On the executor the coroutine suspended on, or on the
  scheduler's thread if there was none.
- **Stop-aware.** With a stoppable token the awaiter registers a stop
  callback. A stop cancels the scheduler entry, which releases the timer's
  capture at once, and resumes the coroutine with `OperationCancelled`: on the
  executor it suspended on, or, if there was none, inline on the thread that
  requested the stop.

## Model side

### Task handlers

```cpp
class LedgerModel
{
  public:
    core::async::Task<PostingResult> execute(PostEntry entry);  // a Task handler
    Balance execute(GetBalance query);                          // an ordinary handler
};
```

A model's `execute(Action)` may return `core::async::Task<R>`.

- **Registration.** `model::HandlerResult<T>` maps `Task<R>` to `R` and
  everything else to itself. `BRIDGE_REGISTER_ACTION` deduces
  `ActionTraits<A>::Result` through it, so the action's result type on the
  wire and in every `Completion` is `R`, not the Task.
  `BRIDGE_REGISTER_ACTION_FOR_CLIENT` names `R` as the result, as for any
  handler. `ActionDispatcher::registerAction` files a Task handler under
  `dispatchAsync`'s runner.
- **One handler shape per action.** A model mixes Task and ordinary handlers
  freely.
- **Where it runs.** Both execution sites detect a Task handler at compile time
  (`model::isTaskHandler`) and drive it instead of calling it synchronously:
  `Bridge::executeVia`'s `localOp` for `LocalBackend`, and
  `ActionDispatcher`'s runner for `RemoteServer`. Validation, computed-field
  recompute and precision reconciliation all run before the handler starts,
  exactly as for an ordinary handler.
- **How it is driven.** The handler is called on the model's strand. That
  constructs the Task, which is lazy: `core::async::Task` suspends at
  `initial_suspend`. It is then started on the strand, inside the handler's
  resumer, and every later resumption goes through that resumer. When
  it finishes, the result — or the exception — settles the call exactly where
  an ordinary handler's return value or throw would.

### The handler's resumer

A Task handler's resumptions go through one `morph::exec::detail::TaskResumer`,
made for it when it starts, and its model instance's strand: one of the
backend's `ModelStrands`, core-cpp's `KeyedStrands` keyed by `ModelId` (see
[`executor.md`](executor.md), "Strands").

- **The current executor wherever the handler runs**: its first step, started
  on the strand, and every resumption after. An awaitable that resumes on the
  current executor therefore hands the handler back to the resumer, whose
  `submit` queues it on the model's strand, serialised with that model's other
  work. A handler that awaits another model's `execute` resumes on its own
  strand, not on the other model's. The `ParkedWork` overload queues the work
  with its claim: the handler's own frames belong to the driver and carry none,
  but a `core::async::DetachedTask` the handler starts, and that parks on an
  awaitable resuming on the current executor, reaches the resumer with its
  claim armed, and keeping it is what stops the chain being freed while its
  handle waits on the strand. Where the strands are closed, the resumer disarms
  the claim and resumes the chain inline, as it does a handler.
- **The action's session, around every resumption.** The strands' keyed
  around-task hook installs the session, with the resumer as the current
  executor, around every task of a model instance whose Task handler has
  started and not finished (`ModelStrands::enroll`). The action gate lets one
  action run on an instance at a time, so an instance has at most one. The
  hook is given the task, not what kind of task it is, so the instance's other
  tasks -- `onBackendChanged`, an action queued behind the handler, a detached
  chain that an earlier, finished handler A left behind and that comes back
  after handler B started -- run under the running handler's session and
  resumer too, B's in the last case (see [`backend.md`](backend.md);
  [core-cpp#53](https://github.com/contour-terminal/core-cpp/issues/53) proposes
  letting the hook tell them apart).
- **Held by the driver.** The driver's frame and every `ResumeTarget` taken
  inside the handler hold the resumer, so it lives until the last of them has
  run.

The resumer shares its backend's strands, so it outlives the backend. Once a
`LocalBackend` has closed them, `trySubmit` refuses, and the resumer resumes the
handler inline instead, with the same session installed and itself the current
executor, on the thread that submitted the resumption. For a `Completion<T>`
that is the completion's callback executor, where its handlers run; for `delay`
it is `TimeoutScheduler`'s thread; for a stop it is the thread that requested
it. Only a handler no stop can reach gets here; see Limitations, "Teardown".

### Not re-entrant: the action gate

A strand serialises the *tasks* posted to it, and a suspended Task handler is
not a task: while it waits, the strand is free. Without more, the next action
for the same model would start while the first was suspended half-way. That
is re-entrancy, which a model author writing sequential code does not expect.

So each model instance has an **action gate** (`model::detail::ActionGate`,
owned by its `IModelHolder`):

- Every action, Task or ordinary, *enters* the gate on the model's strand
  before it runs, and *leaves* when it is finished. For an ordinary handler
  that is when it returns or throws; for a Task handler, when the Task
  completes.
- An action that finds the gate held is queued in the gate, in arrival order,
  and its strand task returns.
- When the holder leaves, the gate starts the oldest queued action, still on
  the strand. That action enters before anything else can.
- Resumptions of the holder's Task do not enter the gate — they belong to the
  action that holds it — so the handler that holds the gate keeps running.
- On `LocalBackend`, an action whose call `cancelPending` failed while it was
  on its way to the gate is skipped when its turn comes: it leaves at once,
  and its handler does not run for a caller that has already been answered.

The next action for a model therefore starts only after the current handler's
Task has completed. The order is still arrival order. `ExecuteOrderGate`,
which orders the *posting* of remote executes to the strand, is unchanged; it
releases its ticket once the post has happened, as before, and does not wait
for the action to run. The gate is touched only on the model's strand, so it
needs no lock.

A Task handler that awaits a second action *on its own model* waits forever:
that action is queued behind the gate the awaiting handler holds. This is the
same deadlock as a synchronous handler blocking on its own strand, and the
spec forbids it the same way rather than detecting it.

### Execute deadlines

`Bridge::setExecuteDeadline` arms a timer per call that rejects the caller's
`Completion` with `ClientTimeoutError`. For a call that reaches a Task
handler through `LocalBackend`, the same timer also requests stop on a
`core::async::StopSource` that the bridge creates per call. It hands that
source to the backend in `ActionCall::stopSource`, and the driver installs its
token as the handler Task's stop token. `LocalBackend` creates one itself for
a call that has no deadline, so that its destructor can stop the handler (see
Limitations, "Teardown"). So:

- the caller's `Completion` rejects with `ClientTimeoutError`, exactly as
  before;
- the suspended handler receives `core::async::OperationCancelled` at its next
  `co_await` of a morph awaiter or a stop-aware core-cpp one, unwinds, and
  leaves the action gate;
- its eventual outcome, now `OperationCancelled`, is discarded by the
  completion's first-result-wins rule, and journalled as a failure like any
  other throw.

A synchronous handler cannot be interrupted, and is not. On a remote backend
the client's deadline cannot reach the server's handler, since nothing on the
wire carries it. The server's own `LimitPolicy::executeTimeout` stops it
instead: `RemoteServer` creates a `StopSource` per dispatch when a timeout is
configured, and hands its token to the handler. The timeout's timer replies
`err "timeout"` to the caller as before, then requests stop, and the handler
unwinds and leaves the action gate as above.

### Journal and observability

A Task handler is journalled when it completes: `Outcome::Succeeded` with the
serialised `R`, or `Outcome::Failed` with the exception's `what()` when the
handler threw. As for an ordinary handler, `Failed` means the model rejected the
action and nothing else: once the Task has completed, the model's mutation has
committed, and a failure to serialise `R` or to append the entry reaches the
caller as `ActionRecordingError` and is not journalled as `Failed`. The
execute span and the latency metric cover the time from when the action
entered the gate to when the Task completed. Suspended time is included,
because it is time the caller waits.

## Single-threaded WebAssembly

The same code runs on the browser's single main thread:

- `spawn` over a `QtExecutor` posts every resumption through Qt's event loop;
- `delay` rides `TimeoutScheduler`'s host-driven loop;
- a strand's base executor is the Qt executor.

Nothing here starts a thread or blocks. One thing differs, and it is core-cpp's
`CORE_CPP_ASYNC_HAS_THREADS`, not `__EMSCRIPTEN__`, that decides it: a backend's
destructor does not wait for its strands to drain, since there is no other
thread to finish the work and a blocking wait is not allowed. It seals them
before it stops the Task handlers, so each stopped handler's resumption is
refused and unwinds inline, and closing them drops only what was queued before
(`ModelStrands::teardown`).

## Failure modes

| Situation | Outcome |
|---|---|
| A Task handler throws, before or after a suspension | The exception settles the call: `onError` on the caller's `Completion`, an `err` reply remotely. The failure is journalled and the gate is left. |
| A Task handler is abandoned (the frame destroyed unfinished) | Never: the driver owns the frame and frees it when the Task completes. A handler whose await never completes is leaked instead. See Limitations. |
| A stop arrives while the handler is not suspended | Seen at its next stop-aware `co_await`. A handler that never awaits again finishes normally. |
| The awaited completion settles after a stop | Its handlers find the await cancelled and do nothing. |
| `spawn`'s task throws | Logged and swallowed. |
| A `co_await` of an empty `Completion` | `std::logic_error`, without suspending. |

## Design decisions

| Decision | Chosen | Why |
|---|---|---|
| The coroutine type | `core::async::Task<T>` | One coroutine type across the Contour Terminal projects, with a stop token that propagates down a chain of awaits. morph adds awaiters and executors, not a second task type. |
| Where resumption happens | core-cpp's current-executor context, stated by every executor that resumes coroutines and read by every awaitable that follows it | `Task`'s promise carries no executor, and a handler that awaits another model's completion must come back to its own strand, not to wherever that completion was delivered. One context shared with core-cpp brings a handler back from `AsyncQueue::pop` too, which morph's own context could not. |
| The session across a suspension | The strands' keyed around-task hook, for the instance's one running Task handler | A context carried by `Task` itself would cost every `co_await` of every consumer; the gate makes one handler per instance the only coroutine the hook has to find. |
| Non-reentrancy | A per-instance action gate on the strand | Holding `ExecuteOrderGate`'s ticket until the Task completes would block a pool thread in `awaitTurn` for the whole suspension. With enough suspended handlers that exhausts the pool that their own awaits need. It would also leave `LocalBackend`, which has no ticket, unordered. |
| Lvalue `co_await` | Refused at compile time | Awaiting consumes the completion's one await slot and moves the handle; an lvalue await would hide that. |

## Limitations

- **No cancellation across the wire.** A client-side deadline or stop does not
  reach a remote handler; only the server's own `executeTimeout` does.
- **Teardown.** `Bridge::switchBackend` and `~Bridge` fail every pending call
  through `cancelPending` and, holding its last reference, destroy the
  outgoing `LocalBackend`. `Bridge::pendingCalls()` reaching zero says nothing
  about a handler still suspended then, because its call was among those
  failed. So `~LocalBackend` ends them itself, in this order:
  1. It requests stop on every Task run it started that is still alive. Every
     Task run has a stop source for this, whether or not the call has a
     deadline. A handler suspended in an awaitable that resumes on the current
     executor -- morph's own, `AsyncQueue::pop` -- resumes with
     `OperationCancelled` through the strand, which is still open, and
     unwinds. One suspended on a `core::net` socket or timer resumes on that
     loop instead and unwinds there; its end is posted to the strand.
  2. It waits for the strands to drain: the resumptions and ends queued on
     them, and every action queued behind them. A handler unwinding on another
     executor whose end arrives meanwhile is queued behind its instance's
     other tasks and runs in turn, on the strand. An action whose call
     `cancelPending` had already failed when it reached the gate is skipped.
     Its handler does not run, and its caller keeps the error it was given.
  3. It seals the strands: they refuse the try-forms, and a resumption or a
     handler's end that arrives from now on runs inline, where it arrives.
     Nothing of its instance runs on a strand beside it: step 2 left the
     strands idle, and what reached them after step 2 returned is the stopped
     handlers' own last steps, one at a time per handler. Sealing before step
     2 would let such an end run inline on a loop thread while step 2 ran a
     task of the same instance on a pool thread, two threads inside that
     instance's action gate.
  4. It waits for the strands to drain again, for whatever reached them
     between steps 2 and 3. It does not wait for a handler still unwinding on
     another executor; that handler's end runs inline.
  5. It closes the strands. A coroutine outside any Task handler that captured
     a key's strand itself, and comes back through a plain submit after this,
     is dropped by the closed strand: its owner has to resume it elsewhere.

  On the single-threaded WebAssembly build the strands are sealed first, before
  step 1, and steps 2 and 4 wait for nothing: every stopped handler's
  resumption is refused and unwinds inline, in the stop, since nothing else
  could run the strand, and nothing else runs beside it.

  A handler that catches `OperationCancelled` and carries on holds up step 2
  while it runs on the strand: its later morph awaits see the stop at once.
  `~LocalBackend` must not run on one of its own strand threads, whose drain
  it would wait for; a debug build asserts it. The only
  handler that survives teardown is one suspended where no stop reaches, inside
  an awaitable that does not observe the promise's stop token, such as a
  coroutine type from outside `core::async`. When that await completes, the
  handler resumes inline (see [The handler's resumer](#the-handlers-resumer)), holding its model
  instance alive; whatever it does from there runs, and its outcome is
  discarded. If that await never completes, the handler is never resumed and
  never freed — a leak. A `RemoteServer` needs none of this, since a suspended
  handler keeps its server alive.
- **A handler awaiting its own model deadlocks**, as described under the
  action gate.
- **Direct `Model::execute` calls** from outside the bridge — tests, replay
  tooling — receive the Task itself and must drive it.
- **Synchronous dispatch.** `ActionDispatcher::dispatch`, which returns the
  JSON result, cannot wait for a Task and throws `std::logic_error` for a Task
  handler. `RemoteServer` asks `dispatchesAsync` which of the two an action
  needs, and uses `dispatchAsync` for a Task handler. `journal::replay`, which
  dispatches synchronously, therefore cannot replay an entry whose handler is
  a Task handler; such an action is not replayable until replay is
  asynchronous.

## Cross-references

- [`completion.md`](completion.md) — `Completion<T>`, its handlers and the
  client-side execute deadline.
- [`bridge.md`](bridge.md) — `executeVia` and `localOp`, where Task handlers
  are driven for `LocalBackend`.
- [`backend.md`](backend.md) — `RemoteServer`'s dispatch and
  `ExecuteOrderGate`.
- [`executor.md`](executor.md) — the strand and the executors a coroutine
  resumes on.
- [`concurrency_and_lifetimes.md`](../concurrency_and_lifetimes.md) — the
  destruction-order rules the driver follows.

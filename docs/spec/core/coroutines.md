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
  - [`StrandCoroExecutor`](#strandcoroexecutor)
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
executor. morph therefore tracks *the resumption context* itself:

- It is a `core::async::IExecutor` recorded in a thread-local,
  `morph::async::detail::currentResumeContext()`.
- The two morph executors that resume coroutines install it for the duration
  of each resumption. Those are the adapter `spawn` builds over a
  `morph::exec::IExecutor`, and `StrandCoroExecutor`.
- morph's awaiters read it in `await_suspend`, on the thread that is suspending
  the coroutine, and hand the continuation back to it.

The rule every morph awaiter follows:

> **A coroutine resumes in the context it suspended in.** If a resumption
> context was installed when it suspended, the continuation is submitted to
> that context. If none was, it resumes wherever the awaited operation
> completed: for a `Completion<T>` that is the completion's own executor, where
> its `then()` handlers run; for `delay` it is `TimeoutScheduler`'s thread.

So a coroutine started with `spawn(exec, …)` resumes on `exec` after every
`co_await` of a morph awaiter, and a Task handler resumes on its model's
strand after every such `co_await`. This holds even when what it awaited
completed on some other executor, such as another model's completion
delivered on the worker pool.

**Only morph's awaiters follow this rule.** The context is morph's, and a
core-cpp awaiter does not know it: `core::async::AsyncQueue::pop`, socket I/O
and every other core-cpp or third-party awaiter resume the coroutine on their
own executor, a stop included. The coroutine then carries on there, off its
strand, until it goes back. `morph::async::resumeContext()` is how: read it
while still on the strand, and hop back after the foreign await. Until then,
even a morph awaiter resumes through the context of whatever thread the
coroutine is now on, which may be none, or another model's strand.

```cpp
auto* const strand = morph::async::resumeContext();   // on the strand
auto item = co_await queue.pop();                      // on the queue's executor
co_await core::async::ResumeOn{*strand};               // on the strand again
```

Whatever the handler does between the foreign await and the hop runs beside
its model's other work, not serialised with it. Its end is safe either way: the
driver posts what follows a handler's end -- recording it, settling the call,
leaving the action gate, starting the next action -- to the strand from
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
- **Where it resumes.** See the rule above: in the resumption context the
  coroutine suspended in, or on the completion's executor if there was none.
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
  from `await_resume`. It resumes in the same context a normal completion
  would have used: the resumption context, or the completion's executor.
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
- **Where it resumes.** In the awaiting context's resumption executor, or on
  the scheduler's thread if there is none.
- **Stop-aware.** With a stoppable token the awaiter registers a stop
  callback. A stop cancels the scheduler entry, which releases the timer's
  capture at once, and resumes the coroutine with `OperationCancelled`: through
  the awaiting context's resumption executor, or, if there is none, inline on
  the thread that requested the stop.

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
  `initial_suspend`. It is then started on the strand, and every later
  resumption is submitted through a `StrandCoroExecutor` for that model. When
  it finishes, the result — or the exception — settles the call exactly where
  an ordinary handler's return value or throw would.

### `StrandCoroExecutor`

```cpp
class morph::exec::StrandCoroExecutor final : public core::async::IExecutor;
```

It posts `h.resume()` onto one model's strand. Both `submit` overloads post
to the strand. The one taking a `ParkedWork` borrows the frame like the plain
handle does, because a handler's frame is always owned by the driver that
started it. Each posted resumption installs the executor as the resumption
context before resuming. Every resumption of a Task handler therefore runs on
its model's strand, serialised with that model's other work, and a handler
that awaits another model's `execute` resumes on its own strand rather than on
the other model's.

The executor is shared by the driver and by every posted resumption, so it
lives until the last of them has run.

It reaches the strand through a link its backend closes on destruction, not
through the strand itself. Once a `LocalBackend` has closed it there is no
strand to post to, and the executor resumes the handler inline instead, with
the same session and resumption context installed, on the thread that
submitted the resumption. For a `Completion<T>` that is the completion's
callback executor, where its handlers run; for `delay` it is
`TimeoutScheduler`'s thread; for a stop it is the thread that requested it.
Only a handler no stop can reach gets here; see Limitations, "Teardown".

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
serialised `R`, or `Outcome::Failed` with the exception's `what()`. The
execute span and the latency metric cover the time from when the action
entered the gate to when the Task completed. Suspended time is included,
because it is time the caller waits.

## Single-threaded WebAssembly

The same code runs on the browser's single main thread:

- `spawn` over a `QtExecutor` posts every resumption through Qt's event loop;
- `delay` rides `TimeoutScheduler`'s host-driven loop;
- a strand's base executor is the Qt executor.

Nothing here starts a thread or blocks, and nothing here depends on
`__EMSCRIPTEN__`.

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
| Where resumption happens | A thread-local resumption context, installed by morph's resuming executors | `Task`'s promise carries no executor, and a handler that awaits another model's completion must come back to its own strand, not to wherever that completion was delivered. |
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
     deadline. A handler suspended in a morph awaiter resumes with
     `OperationCancelled` through the strand, which is still open, and
     unwinds. One suspended in a stop-aware core-cpp awaiter resumes on that
     awaiter's executor instead and unwinds there; its end is posted to the
     strand.
  2. It waits for the strand to drain: the resumptions and ends posted to it,
     and every action queued behind them. It does not wait for a handler still
     unwinding on another executor; that handler's end runs inline once the
     strand is closed, where nothing can race it. An action whose call `cancelPending` had already
     failed when it reached the gate is skipped. Its handler does not run, and
     its caller keeps the error it was given.
  3. It closes the strand.

  A handler that catches `OperationCancelled` and carries on holds up step 2
  while it runs on the strand: its later morph awaits see the stop at once.
  `~LocalBackend` must not run on one of its own strand threads, whose drain
  it would wait for; a debug build asserts it. The only
  handler that survives teardown is one suspended where no stop reaches, inside
  an awaitable that does not observe the promise's stop token, such as a
  coroutine type from outside `core::async`. When that await completes, the
  handler resumes inline (see `StrandCoroExecutor`), holding its model
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

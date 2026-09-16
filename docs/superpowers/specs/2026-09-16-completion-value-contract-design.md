# A value-handling contract for Completion and Bridge — design

`morph::async::Completion<T>` states what it does with `T`: which type
requirements it imposes, how many times it copies, and which of those copies a
caller controls. Today none of that is written down, and the answer is worse
than it needs to be — settling a completion with three handlers attached copies
the value three times, a handler attached afterwards costs two more, and one of
those two is an outright bug.

The change that fixes the copies also removes the type requirement: `T` stops
having to be copyable. The obligation moves off the type and onto the
individual handler that asks for its own value, where the caller can see it and
decide.

## Contents

- [What is true today](#what-is-true-today)
- [The contract](#the-contract)
- [Mechanism](#mechanism)
- [Why the state holds the value, not a shared_ptr](#why-the-state-holds-the-value-not-a-shared_ptr)
- [Bridge](#bridge)
- [Enforcement](#enforcement)
- [What it costs](#what-it-costs)
- [Call-site audit](#call-site-audit)
- [Out of scope](#out-of-scope)

## What is true today

**Baseline: the tree with morph#546 applied**, in which `setValue` no longer
moves out of the stored value. That matters: on `master` as of 2407ffdf,
`setValue` still does `auto savedVal = std::move(*value)`, so a handler
attached *after* settling receives a moved-from husk — the morph#520 defect,
fixed in that pull request and not before it. The numbers below are measured
against the fixed tree, because that is what this design builds on; against
`master` they differ, and one of the values is wrong rather than merely copied
too often.

Measured with an instrumented `T`, for N handlers attached before the
completion settles and M attached after:

| N before | M after | copies | moves |
|---|---|---|---|
| 0 | 0 | 0 | 1 |
| 1 | 0 | 1 | 5 |
| 3 | 0 | 3 | 7 |
| 0 | 1 | 2 | 4 |
| 0 | 2 | 4 | 7 |
| 1 | 1 | 3 | 8 |

The budget is **N + 2M**. Two things are wrong with it.

**The `2M` includes a plain defect.** `attachThen`'s fire-now path copies the
stored value into `savedVal`, and the closure then captures `savedVal` *by
copy* before moving it into the handler. One of those two copies buys nothing.
It is unchanged by morph#546 and present on `master` too.

**The `N` is paid whether or not the handler wants it.** Every handler is
erased as `std::function<void(T)>`, so the value is materialised once per
handler even when the handler only reads it.

`T` must also be **copy-constructible**, and not only incidentally.
`Completion<std::unique_ptr<int>>` fails to compile — three sites, in
`setValue`'s fan-out loop and in `attachThen`'s fire-now path. Fixing those
three would not be enough on its own: `IExecutor::post` takes
`std::function<void()>`, `std::function` requires a CopyConstructible target,
and the dispatch closure captures a `T`.

## The contract

**At the type level, `T` need only be movable.** `std::move_constructible<T>`
is the whole requirement.

**Copyability is a per-handler obligation, not a per-type one.** A handler that
takes `const T&` imposes nothing. A handler that takes `T` by value requires
`T` to be copyable, and that requirement is reported at the place that handler
is written, by ordinary overload resolution — not as a constraint on the whole
instantiation.

**Copy budget: exactly one copy per handler that takes `T` by value. Zero for
every handler that takes `const T&`.** Independent of how many handlers are
attached, and of whether they attached before or after the completion settled.

**The value is observed, never consumed.** No handler can move out of the
stored value. This is not a new restriction dressed up as a feature: morph#520
exists because a handler attached *after* settling must still receive the
value, so `setValue` must not hand it away. Making that structural rather than
conventional is the point.

**A handler that wants to consume takes `T` by value** and moves out of its own
copy. That still works, unchanged, at the documented cost of one copy.

## Mechanism

Two changes.

**`onOk` becomes `std::vector<std::function<void(const T&)>>`.** One erased
type accepts every spelling a caller already writes — verified:

| handler | copies when invoked |
|---|---|
| `[](const T& v)` | 0 |
| `[](T v)` | 1 |
| an existing `std::function<void(T)>` object | 1 |

`std::function<void(const T&)>` is constructible from all three, because each
is invocable with `const T&`. The copy, when a handler wants one, happens at
that handler's own parameter binding. No trait detection, no `if constexpr`, no
signature introspection — and every existing `.then(...)` call site keeps
compiling.

**`CompletionState<T>` gains `enable_shared_from_this`.** The dispatch closure
captures `shared_from_this()` and reads the value in place, instead of carrying
a copy of it. That is what removes the `N` copies; `value` stays a plain
`std::optional<T>`.

Together these also make `Completion<std::unique_ptr<int>>` work, fanned out to
several `const T&` handlers — verified. Nothing in the path copies `T`, and the
closure captures a `shared_ptr` rather than the value, so it remains storable
in `std::function<void()>` and `IExecutor::post` is untouched.

## Why the state holds the value, not a shared_ptr

The obvious alternative is to store `std::shared_ptr<const T>` and let the
closure capture that. Both designs remove the per-handler copies; they differ
only in what they cost when the value is cheap.

Per settle, with handlers taking `const T&`:

| case | today | state (`enable_shared_from_this`) | `shared_ptr<const T>` |
|---|---|---|---|
| small `T`, 0 handlers | 14.6 ns | **17.6** | 23.0 |
| small `T`, 1 handler | 43.0 ns | **55.2** | 54.2 |
| small `T`, 3 handlers | 70.9 ns | **82.5** | 82.9 |
| large `T`, 1 handler | 1140 ns | **370** | 369.7 |
| large `T`, 3 handlers | 2103 ns | **398** | 400.5 |

`large` is a 20-field struct of heap-allocated strings; `small` is an `int`
wrapper. All three variants heap-allocate the state, as morph always does.

With handlers attached the two are indistinguishable — both pay one atomic
refcount pair. They differ when **nobody attaches**: `shared_ptr<const T>`
pays 8.4 ns for a `make_shared` that buys nothing, while the state approach
pays 3 ns for a slightly larger state object. That is the whole basis for
choosing it.

## Bridge

The action path is **already** copy-free, and the contract records that rather
than changing it:

- `BridgeHandler`'s `Executor` is
  `std::function<Completion<std::string>(void*, std::string_view)>` — the
  payload crosses as a `string_view`.
- `executeImpl` stores one `std::shared_ptr<Action>` and passes `*sharedAction`
  to the model.
- Models take `const Action&`.

The one cost is `Bridge::execute(Action action)` taking its argument by value:
one move if the caller moves, one copy if it does not. Documented, not changed
— by-value is what lets a caller pass a temporary without ceremony.

The **result** path is a `Completion<Result>`, so it inherits everything above.
The large-DTO case is exactly `Completion<Result>` on local typed dispatch; the
remote path is `Completion<std::string>` carrying already-serialised JSON,
where a copy is one allocation rather than twenty.

## Enforcement

Both halves, because each catches what the other cannot.

**Compile-time.**

- `static_assert(std::move_constructible<T>)` on `CompletionState<T>`, with a
  message naming the handler-signature rule. A `T` that cannot work produces
  one line instead of pages from inside `std::function`.
- A `tests/compile_checks/` entry asserting that a **by-value** handler on a
  move-only `T` is rejected. Without it, the "copyability is a per-handler
  obligation" half of the contract is a claim no build would ever test.

**Run-time.**

- A copy-counting fixture pinning the budget exactly — not "at most", exactly —
  for: 0/1/3 `const T&` handlers, by-value handlers, late attachers, and mixed
  sets. An exact count is what makes a regression fail; an upper bound quietly
  absorbs one.
- An instantiation-and-fan-out test for `Completion<std::unique_ptr<int>>`,
  pinning move-only support. A type that merely compiles is not evidence that
  handlers fire.
- The same counting fixture applied to `Bridge::execute` end to end, so the
  action path's zero-copy claim is measured rather than asserted from reading
  the code.

Each new test must be observed to fail against the unfixed code before it is
kept.

## What it costs

**Small `T` gets roughly 12 ns slower per settle** when handlers are attached
(43 → 55 ns): the closure pays an atomic refcount pair instead of copying a
cheap value. Against the JSON encode/decode and often a socket write that
surround a settle, that is noise — and a `sizeof(T)` fast path would buy
nanoseconds at the price of two code paths through the most delicate function
in the file. Recorded here rather than hidden, because it is a real regression
on the cheap case.

**The large-`T` win requires `const T&` handlers.** The same design with
by-value handlers measures 678 ns (1 handler) and 1323 ns (3) against 370 and
398 — better than today, because the redundant `savedVal` copy is gone, but far
from the full benefit. So the handler signature is documented as the lever a
caller pulls, not as an implementation detail.

**`then()`'s parameter type changes**, from `std::function<void(T)>` to
`std::function<void(const T&)>`. Source-compatible for lambdas and for existing
`std::function<void(T)>` objects — verified — but it is a public signature
change and belongs in the CHANGELOG. `docs/spec/core/completion.md` changes with
it, including the summary-table row that currently reads "`setValue` moves it
only into the last handler's invocation", which this design makes false.

## Call-site audit

309 `.then(` call sites across `include/`, `src/`, `examples/` and `tests/`.
Every handler that moves its parameter takes it **by value** first, so each
moves from its own copy and none breaks.

Two sites are worth naming because they are a floor rather than an oversight.
`bridge.hpp:2207` and `:2241` chain one completion into another:

```cpp
.then([state](R value) { state->setValue(std::move(value)); })
```

`setValue(T)` must *own* a value, so a chain of this shape costs exactly one
copy whatever the handler signature is. The contract states that rather than
implying it could be optimised away.

## Out of scope

- **Changing `IExecutor::post`.** Move-only `T` works without it, because the
  dispatch closure captures a `shared_ptr` rather than the value. Switching to
  `std::move_only_function<void()>` would be a breaking change to a public
  virtual that every executor in and out of this tree implements, and this
  design does not need it.
- **A move-only *handler* type.** Handlers stay `std::function`-erased and so
  stay copy-constructible. Nothing in the tree needs otherwise.
- **`onErr`.** The error path carries `std::exception_ptr`, which is already a
  refcounted handle; copying it is an atomic increment, not a deep copy.
- **A `sizeof(T)` fast path** for the small-`T` regression. See
  [What it costs](#what-it-costs).
- **Auditing copy counts elsewhere in the tree.** This covers `Completion` and
  the `Bridge` action/result path. Wire encode/decode, the journal and the
  offline queue are not examined here.

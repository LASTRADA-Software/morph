# `runPostCommitTail` — design

Design spec for `morph::model::runPostCommitTail` (`include/morph/core/model.hpp`):
the seam between an action handler's commit and the work that follows it.

Read this before writing a mutating `execute()` that does anything after its
commit — journalling to a model-owned log, a rule cascade, rebuilding cached
state.

## Contents

- [The problem](#the-problem)
- [API surface](#api-surface)
- [Where the tail starts](#where-the-tail-starts)
- [Why a utility, not a dispatch-layer hook](#why-a-utility-not-a-dispatch-layer-hook)
- [Relation to the framework's own recording](#relation-to-the-frameworks-own-recording)
- [Out of scope](#out-of-scope)
- [Cross-references](#cross-references)

## The problem

A mutating handler validates, opens a transaction, mutates, **commits**, and
then often keeps working: it appends to an action log it owns, fires rules the
write triggered, or re-reads state for its return value. Everything after the
commit can still throw — a contended `SQLITE_BUSY` past the busy timeout, a
journal sink whose `append` refuses the entry (which
[`IActionLog::append`](../journal/journal.md)'s contract *requires* it to signal
by throwing).

If that exception leaves `execute()`, the caller is told the action failed while
the write it made stands. A client that believes it will retry, and the retry
applies the mutation twice. That is the defect this helper closes: **a call must
not report failure for a mutation that already happened.**

## API surface

```cpp
namespace morph::model {

template <typename Tail>
    requires std::invocable<Tail> && std::is_void_v<std::invoke_result_t<Tail>>
void runPostCommitTail(Tail&& tail, std::string_view what) noexcept;

template <typename Result, typename Tail>
    requires std::invocable<Tail> && std::convertible_to<std::invoke_result_t<Tail>, Result>
[[nodiscard]] Result runPostCommitTail(Tail&& tail, Result committed, std::string_view what)
    noexcept(std::is_nothrow_move_constructible_v<Result>);

}
```

Both overloads run `tail` once. If it throws, the exception is caught — a
`std::exception` and anything else alike — and reported through
`morph::log::logError`:

```
<what> committed, but its post-commit tail failed: <exception what()>
<what> committed, but its post-commit tail threw a non-std::exception
```

Nothing is rethrown. `what` names the handler; by convention it carries the
model's tag too (`"[kanban::BoardModel] CreateColumn"`), because the log line is
the only place the failure surfaces.

- **The `void` overload** is for a tail whose result the caller does not need —
  the common case, where the tail is journalling alone and the handler's return
  value was computed before the commit. It is `noexcept`: `logError`'s
  formatting overload is itself `noexcept`, so nothing on the path can escape.
- **The value-returning overload** is for a handler whose return value is
  *refreshed* by the tail — a board state rebuilt after a rule cascade the move
  fired. `committed` is the truthful answer the handler already holds from
  before the tail began; if the tail throws, that is what is returned. `Result`
  is deduced from `committed` alone, so the caller receives the handler's own
  result type and the tail may yield anything convertible to it. It is
  `noexcept` whenever returning `committed` cannot throw.

The `void` overload rejects a value-returning tail at compile time rather than
discarding its value, because a tail that computes something is almost always a
tail whose value the caller was meant to get.

## Where the tail starts

Only work *after* the commit goes through the helper. An exception from before
the commit still means the mutation did not happen, and it must still reach the
caller: wrapping it would report success for a write that was rolled back.

Anything the caller's **return value** depends on is best computed *before* the
commit, inside the transaction. A re-read that fails there rolls the write back,
so the caller's "this failed" is true. A re-read that fails after the commit
leaves nothing truthful to return unless the handler already holds an answer —
which is exactly the value-returning overload's precondition. The cost of
reading inside the transaction is that the write lock is held for the length of
the read; for SQLite under contention that is measurable, and the handler that
already needs the state inside the transaction (for an idempotency ledger row,
say) pays nothing extra.

## Why a utility, not a dispatch-layer hook

The seam is a function the handler calls on itself, at the point in its own
body where it knows the commit succeeded. The alternatives — a result type
carrying a deferred continuation, an `ActionTraits` after-commit hook the
dispatcher calls, a journal decorator that swallows `append` failures — all add
an extension point the framework calls into after `execute()` returns. None is
needed for this, and each costs more:

- A hook called after `execute()` returns runs on a different frame, so it
  cannot see what the handler computed inside the transaction unless that
  travels in the result. A rule cascade built from rows inserted in the same
  transaction is an ordinary local variable to a tail closure.
- A continuation-carrying result type would change every `execute()` signature
  and have to be hidden from `resultToJson`, the wire and `journal::replay`.
- A swallowing journal decorator covers only journalling, and makes "the entry
  is recorded" no longer a promise the outbox relay can rely on.

It also keeps a model independent of the dispatch layer: there is no new
model-to-dispatcher contract, only a utility in the same category as
`morph::log::logError`, and a directly constructed model gets the same
guarantee as a registered one.

The one shape this does not express is post-commit work that must run in a
**different execution context** from the one `execute()` returns on — genuinely
asynchronous follow-on work. The tail runs synchronously, before `execute()`
returns. A handler that needs the other shape needs one of the dispatch-level
designs above, alongside this helper rather than instead of it.

## Relation to the framework's own recording

For a registered model, the dispatch layer appends the action's own
`Succeeded` entry after `execute()` returns, and a sink failure there surfaces
as `morph::model::ActionRecordingError` — the caller is told the write happened
and its record did not (journal.md, "A refused recording is not an execution
failure"). That path has a channel back to the caller, because the dispatcher
owns the reply.

A handler's own tail has no such channel without changing its result type, so
the helper's channel is the error log. The two are complementary: the framework
covers the entry it writes, the helper covers the work a handler does itself.

## Out of scope

- **Failures before the commit.** A non-domain exception thrown before the
  commit is the action's failure, and a registered model's dispatch site records
  it `Outcome::Failed` already. The helper deliberately does not touch that
  path.
- **Retrying the tail.** A failed tail is logged, not retried. Whether a missed
  journal entry or cascade should be recovered is the application's decision.

## Cross-references

| Spec | Why |
|---|---|
| [registry.md](registry.md) | `ActionDispatcher`, `IModelHolder` and the dispatch sites whose recording this complements. |
| [../journal/journal.md](../journal/journal.md) | `IActionLog::append`'s throwing contract; `ActionRecordingError`. |
| [logger.md](logger.md) | `morph::log::logError`, the helper's reporting channel. |

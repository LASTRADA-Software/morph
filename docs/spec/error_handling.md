# Error handling — design (cross-cutting)

This is the single authoritative reference for how errors, exceptions, and
failures propagate across **every** layer of morph — from a throwing
`Model::execute` on a worker thread all the way back to a GUI-thread
`.onError` callback, and every typed failure the value codecs, wire protocol,
registry, journal, and auth layers can raise along the way. Per-type specs
link here rather than re-deriving the whole story.

The design has one organising principle: **an error must never vanish
silently, and it must never surface on the wrong thread.** Everything below is
a consequence of holding those two invariants together.

## Contents

- [The async happy-vs-error path](#the-async-happy-vs-error-path)
- [Orphan logging and single-shot callbacks](#orphan-logging-and-single-shot-callbacks)
- [Executor exception handling](#executor-exception-handling)
- [Backend error types](#backend-error-types)
- [Remote wire errors](#remote-wire-errors)
- [Value-codec philosophy: clamp vs. reject vs. propagate-empty](#value-codec-philosophy-clamp-vs-reject-vs-propagate-empty)
- [Other typed failures](#other-typed-failures)
- [How to observe failures](#how-to-observe-failures)
- [Cross-references](#cross-references)

## The async happy-vs-error path

Every action dispatched through a backend flows through the same shape. On the
local path (`morph::backend::LocalBackend`), `Model::execute` runs inside a
strand task on the worker pool:

```
Model::execute(action) throws
  └─ LocalBackend's strand task catches via try/catch (backend.hpp)
       └─ CompletionState::setException(std::current_exception())
            └─ .onError(fn) handler posted to the GUI (callback) executor
                 └─ fn receives exception_ptr; caller rethrows to inspect
```

The strand task (`LocalBackend::execute`) is:

```cpp
_strand.post(mid, [localOp = std::move(localOp), holder = std::move(holder),
                   compState, session = std::move(session)]() mutable {
    try {
        ::morph::session::detail::ScopedContext const scoped{session};
        compState->setValue(localOp(*holder));   // happy path
    } catch (...) {
        compState->setException(std::current_exception());  // error path
    }
});
```

Both `setValue` and `setException` resolve the shared
`morph::async::detail::CompletionState<std::shared_ptr<void>>` exactly once
(first writer wins; a second call while `ready == true` is a no-op). When a
callback is already registered, they build it under the state mutex and then
`post()` it to `cbExec` — the executor supplied at `Completion` construction,
which for GUI code is the GUI executor. **The producing (worker) thread never
invokes the callback directly**, so `.then` / `.onError` bodies always run on
the intended thread.

To inspect the failure, the GUI's `.onError` handler rethrows:

```cpp
handler.execute(action)
    .then([](Result r)          { /* GUI thread */ })
    .onError([](std::exception_ptr e) {
        try { std::rethrow_exception(e); }
        catch (const morph::backend::DisconnectedError&) { /* retry */ }
        catch (const std::exception& ex) { showError(ex.what()); }
    });
```

The remote path (`SimulatedRemoteBackend::execute`, and real WebSocket
backends) is identical from the caller's side: the reply-handling lambda calls
`state->setValue(deser(reply.body))` on an `"ok"` reply, or throws
`std::runtime_error(reply.message)` (caught into `setException`) for **any**
non-`ok` reply — substituting `"malformed reply"` when `reply.message` is empty.
See [Remote wire errors](#remote-wire-errors) for how the server side produces
those `err` messages.

## Orphan logging and single-shot callbacks

`CompletionState<T>` (see `completion.hpp`) protects a value slot, an
`std::exception_ptr error` slot, two callback lists (`onOk` / `onErr`, each a
`std::vector`), and an `onErrAttached` flag, all under one mutex. The `cbExec`
pointer is *not* mutex-guarded: it is set once in the `Completion` constructor,
before any concurrency, and read unlocked thereafter (as
`callback != nullptr && cbExec != nullptr` outside the critical section).

**Single-shot result, composing callbacks.** The *result* (value or error) is
single-shot — `setValue`/`setException` are no-ops once `ready`. But `then()`
and `onError()` each **compose**: every handler attached while the state is not
yet ready is kept and fires, in attachment order, when the result lands. This
was fixed under issue #59 — `onOk`/`onErr` used to be single fields, so a
second `onError()` on the same still-pending `Completion` silently discarded
the first handler (and, because `onErrAttached` was still set from that second
call, suppressed the orphan logger too — losing the error entirely, not just
misdelivering it).

| Situation | Behavior |
|---|---|
| `then` registered before ready | Appended to `onOk`; every stored handler fires, in order, when `setValue` lands |
| `then` on an already-value state | Fired immediately (posted to `cbExec`), for that one handler only |
| `then` on an already-**error** state | Silent no-op — no success value exists |
| `onError` registered before ready | Appended to `onErr`; every stored handler fires, in order, when `setException` lands |
| `onError` on an already-**error** state | Fired immediately (posted to `cbExec`), for that one handler only |
| `onError` on an already-value state | Silent no-op — no error exists |
| Second `then` after a value settled | Re-fires immediately via the fire-now path (posts to `cbExec` again, this handler only). **What it delivers depends on how the value was first consumed:** if any `then` was attached *before* the state settled, `setValue` moved the stored `value` out into the dispatch closure (copying it into every handler but the last, moving only into the final one) — the stored `value` itself is left moved-from by that dispatch, so a later `attachThen` fire-now copies from that now moved-from `*value`. Only when every `then` was attached *after* settling does each receive a copy of the original value |
| Second `onError` after an error settled | Re-fires immediately with the stored `exception_ptr` (same fire-now path, this handler only) |
| Second `then`/`onError` registered **before** ready | Appended alongside the first — both fire when the result lands, in attachment order; neither is discarded |

**Orphan logging.** If a `Completion` is destroyed while its state holds an
unhandled error, `~CompletionState` logs it through `morph::log::logError`:

- `std::exception` subclass → `"[orphan] unhandled exception: " + what()`
- any other type → `"[orphan] unhandled unknown exception"`

The destructor logs only when `ready && error && !onErrAttached`. So attaching
an `.onError` handler suppresses the orphan log — **but only when there is an
executor to actually deliver it on.**

**Recent fix — null callback executor no longer silences the error.** Both
`setException` and `attachOnError` set `onErrAttached = (cbExec != nullptr)`.
Previously, attaching `.onError` unconditionally marked the error handled; with
a null `cbExec` the callback is never posted (`post` runs only when
`cbExec != nullptr`), so the error would be both undelivered *and*
unsuppressed-from — it vanished. Now, with a null executor `onErrAttached`
stays `false`, so even though the handler can't be delivered, the destructor's
orphan logger still fires and the error reaches the log rather than
disappearing silently. An undelivered *value* is still dropped silently (a
value that no one is waiting for denotes nothing); an undelivered *error*
always surfaces.

## Executor exception handling

Executors (`executor.hpp`, `strand.hpp`) are the thread boundaries. A task that
throws must not kill its worker, must not abort sibling tasks, and must not
vanish. **Recently changed:** the pool and strand executors now catch-and-log
rather than swallow.

| Executor | Catch behavior | Log prefix |
|---|---|---|
| `ThreadPoolExecutor` | Catches `std::exception` and `...` per task; worker loops on | `[thread-pool] task threw: <what>` / `[thread-pool] task threw unknown exception` |
| `StrandExecutor` | Catches `std::exception` and `...` per task; next queued task for the same key still runs | `[strand] task threw: <what>` / `[strand] task threw unknown exception` |
| `MainThreadExecutor::runFor` | Catches **only** `std::exception`; continues with the next task | `[main-thread] callback threw: <what>` |

Notes that matter:

- The `StrandExecutor` is where `Model::execute` actually runs (for both
  `LocalBackend` and `RemoteServer`). In normal operation the backend's own
  `try/catch` converts a throwing `execute` into `setException` **before** the
  strand's catch could see it, so `[strand] task threw:` fires only for
  exceptions escaping the backend's own lambda body (e.g. a throw from
  `setValue`/`setException` machinery itself), not for ordinary action
  failures.
- `MainThreadExecutor::runFor` deliberately catches only `std::exception`.
  Any non-`std::exception` type (e.g. a bare `throw 42;` or a foreign SEH-style
  type) **propagates out of `runFor`** to the caller's main loop. The pool and
  strand executors, by contrast, catch `...` too and never propagate.
- The base `IExecutor::post` contract documents that exceptions are "silently
  swallowed unless the implementation documents otherwise" — the two concrete
  executors above document otherwise (they log).

## Backend error types

`backend.hpp` defines three typed errors, all `std::runtime_error` subclasses
with canned messages, thrown *into* in-flight completions (delivered via
`.onError`) rather than out of any call:

| Type | Message | Thrown when |
|---|---|---|
| `BackendChangedError` | `"backend changed before completion resolved"` | `Bridge::switchBackend()` calls `cancelPending()` on the outgoing backend; every still-pending completion resolves with this |
| `BridgeDestroyedError` | `"bridge destroyed before completion resolved"` | `Bridge`'s destructor cancels pending completions |
| `DisconnectedError` | `"transport disconnected before completion resolved"` | A transport (e.g. Qt WebSocket) drops mid-call; framework retries on reconnect if supported, else `.onError` runs |

`cancelPending(exc)` walks the backend's tracked-pending list and calls
`setException(exc)` on each live state. Because `setException` is a no-op once
`ready`, an in-flight server reply arriving *after* the cancel cannot resurrect
a cancelled completion — the cancel wins.

**One failure is deliberately untyped.** When `LocalBackend::execute` is given
a model id it doesn't hold, it resolves the completion with a plain
`std::runtime_error("model not found: id=" + id)` — there is no dedicated
error class for it. The remote path has an analogous plain
`"model not found"` `err` reply (see below).

## Remote wire errors

The wire protocol (`wire.hpp`) uses a single `Envelope` struct with a `kind`
discriminator. Failures come back as `kind == "err"` envelopes carrying a
free-text `message` and an echoed `callId`. `RemoteServer` (`remote.hpp`)
produces them.

**The envelope kinds.** `dispatchMessage` compares `env.kind` against **eight**
values, each with a branch of its own:

    assign  attach  deregister  execute  hello  instances  register  schemas

Anything else is the fall-through, which is what produces `unknown envelope
kind:`. The request and reply shape of each is tabulated in
[`core/wire.md`](core/wire.md), "Discriminator values", and what
`dispatchMessage` does with each in [`core/backend.md`](core/backend.md),
"`RemoteServer` — server-side message handler"; only the refusals are
enumerated here.

**The refusals are grouped by what a client should do about them**, not by the
function they are raised in, because that is the distinction the message text
does not carry. **The three groups below are exhaustive**, and the arithmetic
is checkable: `remote.hpp`'s server half plus the body of `wire::decode` carry
**17** refusal messages (14 fixed, 3 with a runtime suffix). Sixteen of them
appear in the three tables — nine in group 1, two in group 2, five in group 3 —
and the seventeenth is `handleInline`'s, which no transport can reach and which
is called out separately below. The tables add two rows that come from outside
those two headers: `unknown model type: <id>`, thrown by
`ModelRegistryFactory::create` in `registry.hpp`, and the `exc.what()`
passthrough, which carries every application-level refusal.

That count is not a hand tally. `scripts/scenario/scenario_coverage.py`
extracts it from `remote.hpp`'s server half — the part above
`class SimulatedRemoteBackend`, since everything below is the in-process client
stub whose throws no peer ever sees — plus the body of `wire::decode` alone,
since that is the one `wire.hpp` throw `dispatchMessage`'s `catch` turns into a
real `err`. Its exact membership is pinned by that tool's self-test
(`test_real_headers_carry_exactly_these_refusals`, and
`test_real_header_carries_the_kinds_the_spec_records` for the eight kinds). A
refusal added to either header fails those tests, and whoever adds it decides
which group below it belongs in.

### 1. The request is malformed or unsupported

Deterministic for a given request: resending it unchanged produces the same
refusal.

| Error `message` | Raised in | Cause |
|---|---|---|
| `"envelope decode failed: <detail>"` | `wire::decode`, caught by `dispatchMessage` | Malformed request JSON |
| `"unknown envelope kind: <kind>"` | `dispatchMessage`'s final `else` | `kind` is none of the eight listed above |
| `"register requires a typeId"` | `dispatchMessage` (`register`) | `register` envelope with empty `typeId` |
| `"attach requires a typeId"` | `dispatchMessage` (`attach`) | `attach` envelope with empty `typeId` |
| `"assign requires a typeId"` | `dispatchMessage` (`assign`) | `assign` envelope with empty `typeId` |
| `"instances requires a typeId"` | `dispatchMessage` (`instances`) | `instances` envelope with empty `typeId` |
| `"schemas requires a typeId"` | `dispatchMessage` (`schemas`) | `schemas` envelope with empty `typeId` |
| `"protocol version unsupported"` | `dispatchMessage` (`hello`) | `env.protocolVersion` outside the server's negotiated `[min, max]` range |
| `"payload missing required field(s): <names>"` | `dispatchExecute` | The action payload omits a field its schema declares required. **Only when `PayloadCompleteness::RequireDeclaredFields` is installed**; the default is `Lenient`, under which the check never runs. See `remote.hpp`'s `PayloadCompleteness` on why turning it on is a non-additive wire change |

All five `requires a typeId` refusals are checked *before* authorization, so an
unauthenticated caller sending an empty `typeId` learns only that the field is
required. Each is a `throw std::runtime_error`, not a direct `makeErr` — the
outer `catch` in `dispatchMessage` is what turns it into the `err` reply, which
is why the message reaches the wire verbatim.

### 2. The request is well-formed and refused on the merits

The server understood it and said no. Retrying identically helps only if the
server's state or policy changes.

| Error `message` | Raised in | Cause |
|---|---|---|
| `"unauthorized"` | Eight sites: `dispatchExecute` (type-level `authorize`, then instance-level `authorizeInstance`); `dispatchMessage`'s `register`, `attach` and `assign` branches (`authorizeRegister`); its `instances` and `schemas` branches (`authorize` with an empty action id); and its `deregister` branch (`authorizeInstance` against the recorded owner) | An authorizer hook returned `false` — a bad, expired or absent token, a type/action policy denial, a caller who may not create or file an instance of this type, a caller who may not *describe* or *enumerate* it, or a caller whose principal does not own the target instance |
| `"model not found"` | `dispatchExecute` | `execute` for a `modelId` the server does not hold (or no longer holds) |
| `"unknown model type: <id>"` | `ModelRegistryFactory::create`, surfaced by `dispatchMessage`'s outer `catch` | `register` for a type-id with no registered factory |
| any handler exception's `exc.what()` | The outer `try/catch` in `dispatchMessage`, and the strand lambda's `catch` in `dispatchExecute` | A throw from `ActionDispatcher::dispatch` or `Model::execute` — its `what()` becomes the `err` message verbatim. This is the channel every application-level refusal travels on |

### 3. Nothing is wrong with the request; the server's condition produced it

Not protocol errors at all. The same request may succeed later, against the
same server, with nothing changed on the client. A client that treats these as
permanent failures will discard work it could have retried.

| Error `message` | Raised in | Cause | Reachable when |
|---|---|---|---|
| `"server shutting down"` | `dispatchMessage`'s shutdown gate, ahead of every kind | `RemoteServer::shutdown()` has begun draining | Always — no configuration required |
| `"connection closed"` | `attachExistingLocked` and `acquireSharedInstance` (`noteScopeAttachLocked` returning `false`), and the `register` branch's `scopeAlreadyClosed` path | The connection scope the instance was being attached to closed while the attach was in flight | Always; inherently a race |
| `"too many models"` | `dispatchMessage`'s `register` branch (advisory pre-check and post-construction re-test) and `acquireSharedInstance`'s re-test under the insert lock | The live-model cap is reached | `LimitPolicy::maxLiveModels != 0` |
| `"server busy"` | `dispatchExecute` (advisory early shed, then the compare-exchange reservation) | The in-flight execute cap is reached | `LimitPolicy::maxInFlightExecutes != 0` |
| `"timeout"` | The `TimeoutScheduler` callback armed by `dispatchExecute` | The execute did not reply within the configured budget | `LimitPolicy::executeTimeout > 0` **and** a `TimeoutScheduler` installed — with either missing, no timer is armed |

All three limit-gated rows are unreachable against a **default-configured**
server, because every `LimitPolicy` field defaults to `0`, meaning unbounded.
Across the ladder only one of them is live: `bookmarks`, `polls`, `kanban` and
`ledger` each set `maxLiveModels = 256` and leave every other field at `0`, and
`pastebin` installs no policy at all. So `too many models` is asserted by a
real scenario (`scenarios/bookmarks/the-live-model-cap-is-reached.scenario`),
while `server busy` and `timeout` carry written exemptions in
`scripts/scenario/coverage_allowlist.json` — as do `server shutting down` and
`connection closed`, which need no configuration but cannot be driven without
racing a teardown.

Those four, plus group 1's `payload missing required field(s): ` and the
`handleInline` message below, are the whole of that allowlist: six exemptions
against the seventeen extracted refusals, leaving eleven that a scenario
genuinely asserts against a running server. (The two rows sourced from outside
`remote.hpp`/`wire::decode` — `unknown model type: <id>` and the `exc.what()`
passthrough — are outside what that tool measures, so neither count includes
them.)

### Not on the wire

`"handleInline does not support execute (reply is asynchronous)"` is raised by
`RemoteServer::handleInline`, a synchronous in-process API used by morph's own
C++ tests. It is not an envelope kind and no transport reaches it, so no
WebSocket client can ever observe it. It is listed here because it is a
`makeErr` message in the same header, not because it is part of the protocol.

**callId echoing.** The `callId` is copied from the request into the `err`
reply so the client can correlate it. The one exception: when `wire::decode`
itself fails, no envelope could be parsed, so `env.callId` is still its default
`0` and the decode-error reply carries `callId == 0`.

**Encoding/decoding failures.** `wire::encode` and `wire::decode` both throw
`std::runtime_error` on Glaze failure (`"envelope encode failed: ..."` /
`"envelope decode failed: ..."`). `decode` failures on the server become the
canonical decode-error reply above; on the client side (in
`SimulatedRemoteBackend`'s reply lambda) any throw — decode failure or an
`"err"` reply turned into `throw std::runtime_error(reply.message)` — is caught
and routed to `setException`, then to `.onError`.

## Value-codec philosophy: clamp vs. reject vs. propagate-empty

The exact value types take deliberately **different** stances on bad input,
because the kinds of value denote differently:

| Type | Bad-input policy | Rationale |
|---|---|---|
| `math::Rational` (`rational.hpp`) | **Clamps** structure; `std::expected` for arithmetic | A number always denotes *some* quantity; the nearest valid value is a meaningful answer |
| `time::DateTime` (`datetime.hpp`) | **Rejects** (JSON read error) | A malformed instant denotes *nothing*; there is no meaningful "nearest" timestamp |
| `units::Quantity<U, Dec>` (`quantity.hpp`) | **Propagates empty**; clamps precision; throws only on ordering an absent value | A measurement can legitimately be *not entered*; a missing/failed value is `std::nullopt`, which flows through arithmetic rather than raising |

**`Rational` clamps.** Its Glaze read side (`from_json`/`read`) rebuilds
through the canonicalising constructor: `dp` is run through
`detail::clampWireDecimalPlaces` into `[1, kMaxDecimalPlaces]` (18), a zero
denominator is clamped to `1`, and the fraction is gcd-reduced — silently, with
no error. `Rational` itself **never throws**: fallible arithmetic
(`operator/`, `reciprocal`, `dividedBy`, `from`, `fromFloat`) returns
`std::expected<Rational, RationalError>` where `RationalError` is
`{ DivisionByZero, NotFinite, Overflow }`, and mixed expressions
short-circuit the first error left-to-right. There is a two-tier clamp:
`clampWireDecimalPlaces` is silent (untrusted wire), `clampDecimalPlaces`
asserts in debug first (a precision stated in code out of range is a bug).

**`DateTime` rejects.** `fromIso8601` strictly parses `YYYY-MM-DDTHH:MM:SS`
with a hand-rolled routine (no locale, no `std::chrono::parse`); a non-existent
calendar date (`2026-02-30`) or out-of-range clock field returns
`std::nullopt`. Its Glaze read op turns that `nullopt` into
`ctx.error = error_code::syntax_error` — a JSON **read error** that fails the
whole decode, exactly as if the field were the wrong type. There is no clamp
because a mistyped instant has no valid nearby value.

**`Quantity` propagates empty.** A `Quantity<U, Dec>` is an optional
`math::Rational` plus a compile-time unit and declared precision, so its error
surface layers three separate policies:

- *No error channel for arithmetic.* `operator+ - * /`, dimensionless scaling,
  and `fromDouble` (which lifts `Rational::fromFloat`) all take the
  **empty-propagation** stance: empty in → empty out, and a division whose
  divisor is zero (or whose `Rational::dividedBy` otherwise fails) yields
  `std::nullopt` rather than an error — the underlying `expected` from
  `Rational` is folded away to empty. Empty is a first-class "not entered /
  not measured" state, not a failure.
- *One throw, and only one.* `operator<=>` on two same-unit quantities throws
  `std::logic_error("morph::units::Quantity: relational comparison requires
  engaged operands")` when either operand is empty. Ordering an absent value is
  a **programming error**, not a data condition (equality `operator==` does not
  throw — two empties compare equal). This is the only exception `Quantity`
  raises, and it escapes *out of* the call rather than resolving a completion.
- *Silent precision clamp.* The compile-time `DeclaredDecimals` is a
  `static_assert` (a code bug fails the build); the runtime `withDecimalPlaces`
  and `roundedToDecimalPlaces` reuse `Rational`'s `clampWireDecimalPlaces` and
  clamp silently, matching the `Rational` two-tier rule.
- *Silent rounding saturation.* `math::roundToDecimalPlaces` (and so
  `Quantity::roundedToDecimalPlaces` / `atDeclaredPrecision`, and the dispatch
  path's `reconcileDeclaredPrecision`) scales by `10^places` through the
  saturating multiply. A value already representable at the target scale takes an
  exact fast path and cannot saturate; anything else clamps and logs at `error`
  like every other `Rational` overflow, with no error channel. There is no
  `checkedRound`.

## Other typed failures

| Type / message | Where | Trigger |
|---|---|---|
| `journal::SerializationError` (`std::runtime_error`) | `journal::toJson` / `journal::fromJson` (`action_log.hpp`) | `LogEntry` (de)serialisation fails. `fromJson`'s path is real (malformed JSON); `toJson`'s branch is shared but structurally unreachable for `LogEntry` |
| `model::detail::ParseError` (`std::runtime_error`) | `ActionTraits<A>::toJson`/`fromJson`/`resultToJson`/`resultFromJson` (the `BRIDGE_REGISTER_ACTION` codec) | Glaze read/write of an action payload or result fails |
| `std::runtime_error("unknown action: <model>/<action>")` | `ActionDispatcher::dispatch` (`registry.hpp`) | `(modelType, actionType)` pair was never registered |
| `std::runtime_error("unknown model type: <id>")` | `ModelRegistryFactory::create` (`registry.hpp`) | Model type-id was never registered — also surfaces as the `register` wire `err` |
| `std::runtime_error` from `journal::replay` | `journal.hpp` | Replay hits an unregistered model type-id or an entry with an unregistered action type (it delegates to `dispatch`/`create`) |
| `session::AuthError` (`enum { Malformed, BadSignature, Expired, NotYetValid }`) | `TokenVerifier::verify` returns `std::expected<SessionToken, AuthError>` (`session_auth.hpp`) | `Malformed`: not `payload.sig`, bad/non-canonical base64url, or unparseable claims. `BadSignature`: MAC mismatch (forged/tampered). `Expired`: `expiresAtMs` missing/non-positive (`<= 0`) or in the past relative to the clock. `NotYetValid`: `issuedAtMs` is set and more than `kClockSkewMs` (60s) in the future |

Note the auth asymmetry with the wire layer: `AuthError` is a **typed value**
returned to the server's authorizer, never a thrown exception and never sent
to the client verbatim. `SigningAuthorizer::authorize` collapses any `AuthError`
into a `false`, which `RemoteServer` turns into the single opaque
`"unauthorized"` `err` reply — the client is never told *why* authorization
failed. The MAC is checked before the payload is parsed, so untrusted JSON is
never handed to the parser until authenticity is established.

## How to observe failures

Errors reach three different places depending on where they occur; observe them
accordingly.

1. **Attach `.onError`.** This is the primary channel for any failure that
   resolves a `Completion` — a throwing action, a wire `err` reply, or a
   backend cancel error. The handler runs on the GUI executor; rethrow the
   `exception_ptr` to inspect the concrete type.

2. **Set `morph::log::setLogger` early — before any backend or executor runs.**
   Several failure classes surface **only** through the log:
   - executor task exceptions (`[thread-pool]` / `[strand]` / `[main-thread]`
     prefixes) — caught, logged, and otherwise swallowed;
   - orphan errors (`[orphan] ...`) — a failed `Completion` destroyed with no
     `.onError`, or with an `.onError` but a null callback executor.

   The default sink writes `[LEVEL] sanitizeLogLine(msg)` to `stderr`, so these
   are visible out of the box wherever stderr is. Call `setLogger` early to route
   them to spdlog, Qt logging, or a test spy (or to capture them at all in
   environments where stderr is discarded).

3. **The log is the only signal for exceptions on worker threads.** A throw on
   a pool or strand thread never propagates to the main thread (the sole
   exception being a non-`std::exception` type escaping
   `MainThreadExecutor::runFor`, which does propagate to that thread's caller).
   If a worker-side action misbehaves and no `Completion` carried the error
   out, the log entry is the only trace you get.

## Cross-references

- [`completion.md`](core/completion.md) — `Completion<T>` / `CompletionState<T>`, orphan logging, single-shot callbacks.
- [`executor.md`](core/executor.md) — `IExecutor`, `ThreadPoolExecutor`, `MainThreadExecutor`, and the strand.
- [`backend.md`](core/backend.md) — `IBackend`, `LocalBackend`, `cancelPending`, the three backend error types.
- [`wire.md`](core/wire.md) — the `Envelope` protocol and `encode`/`decode`.
- [`registry.md`](core/registry.md) — `ActionDispatcher`, `ModelRegistryFactory`, `ParseError`, the codec macros.
- [`journal.md`](journal/journal.md) — `LogEntry`, `SerializationError`, `replay`, `SessionLog`.
- [`rational.md`](util/rational.md) — `Rational`, `RationalError`, the clamping wire codec.
- [`quantity_type.md`](util/quantity_type.md) — `Quantity<U, Dec>`, empty-propagation arithmetic, the ordering `logic_error`.
- [`datetime.md`](util/datetime.md) — `DateTime` and its strict, rejecting ISO-8601 codec.
- [`session.md`](session/session.md) / [`security.md`](security.md) — `Context`, `IAuthorizer`, `AuthError`, `SigningAuthorizer`.
- [`logger.md`](core/logger.md) — `morph::log`, `setLogger`, log levels.
- [`ARCHITECTURE.md`](../ARCHITECTURE.md#error-propagation) — the "Error propagation" overview this spec expands.

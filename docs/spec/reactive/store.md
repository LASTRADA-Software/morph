# Store — design

Design spec for `morph::reactive::Store<ViewState, Msg>`, `ExhaustiveUpdate` and `request()`
(`include/morph/reactive/store.hpp`): user-intent view state, changed only by messages, on the
nodes [`signals.md`](signals.md) specifies.

Read this before giving a controller state of its own. Server data does not belong in a Store: it
is a `Query` ([`control.md`](control.md)).

## Contents

- [Shape](#shape)
- [Behaviour](#behaviour)
- [`request()`](#request)
- [Lifetime rule](#lifetime-rule)
- [Design decisions](#design-decisions)
- [Out of scope](#out-of-scope)

## Shape

- `ViewState` is a struct of `Signal<T>` fields: what is selected, what is being typed, which tab
  is open. It is not called *model*: in morph a model is the domain object a
  `BridgeHandler<Model>` talks to.
- `Msg` is a `std::variant` of message types.
- `Store(Runtime&, Init, Update)`:
  - `Init` is invocable as `(Runtime&)` and returns exactly `ViewState`, by prvalue. Signals cannot
    move, so the Store's member is initialised from that prvalue directly.
  - `Update` is move-constructible — it may be move-only — and satisfies
    `ExhaustiveUpdate<Update, ViewState, Msg>`: `Update&` is invocable as `(ViewState&, Alt const&)`
    for every alternative `Alt` of `Msg`. An update that misses an alternative fails the
    constructor's constraint, so the Store does not compile. A `Msg` that is not a `std::variant`
    never satisfies it.
  - The runtime is borrowed and must outlive the Store.

## Behaviour

- `send(msg)` applies the update for the message's alternative in one batch, untracked: N field
  writes are one flush, and nothing the update reads subscribes anything.
- `state()` returns `ViewState const&`. The view reads it; only the update writes. A field's
  `get()` tracks exactly that field.
- `action(msg)` returns a `std::function<void()>` that sends a copy of `msg` each time it is called
  — what a button binds to. It refers to the Store, which must outlive it.
- `send` off the runtime's owner is reported (`kOffOwner`) and dropped.
- `send` to a Store from inside that Store's own update is reported (`kSendInUpdate`) and dropped:
  an update is a pure state transition, and a message it sent would run against a half-applied
  state. The guard is per Store, so an update may send to a different Store.
- An exception from the update propagates to the caller of `send`. Writes the update made before it
  threw still flush, and the next `send` runs normally.
- An update may destroy its own Store — a controller that closes its view. The update's callable
  stays alive until it returns, and `send` touches nothing of the Store afterwards. The update
  itself must not use the state or the Store once it has destroyed them.

## `request()`

```cpp
request(store, handler, scope, action, toMsg, toFailMsg);
```

Executes `action` through `handler` and sends `toMsg(result)` or `toFailMsg(exception_ptr)` to
`store` as one message. Each delivery is one `send`, so one batch.

- **Errors travel per call**, inside the message, never as a shared error string.
- **`scope` gates both callbacks**: a reply arriving after it is stopped, reset or destroyed is
  dropped. The Store must outlive every delivery the scope does not gate.
- **Callbacks land on the handler's GUI executor**, which must be the Store's owner; a delivery
  anywhere else is refused by `send` like any other off-owner call.
- **Every call delivers: there is no latest-wins.** Superseding an earlier call means stopping,
  resetting or destroying the scope it was given. A `Query` provides latest-wins
  ([`control.md`](control.md)).
- **A throw is not a failure message.** A throw from `toMsg` or from the update it sends to, on the
  success path, or from `toFailMsg`, is caught and logged by the `Completion` delivering it
  ([`../core/completion.md`](../core/completion.md), per-handler exception isolation).

`Query` and `Mutation` are not built on `request()`: neither owns a Store to send into. They follow
the same delivery rules — errors per call, one batch per delivery, every callback gated by a scope
— on `Completion` and `CallbackScope` directly. A controller reaches for those first, and for
`request()` when a reply should change user-intent state through the update.

## Lifetime rule

The `Store`, the `Runtime` and every mounted view outlive any callback that can still fire. The
`CallbackScope` a controller passes to `request()` is the **last** member of the owning object, so
it is destroyed first and gates anything in flight
([`../core/callback_scope.md`](../core/callback_scope.md#declared-last-destroyed-first)).

## Design decisions

| Decision | Why |
|---|---|
| Exhaustiveness as a constructor constraint | A message the update forgot is a compile error at the Store, not a silent no-op at run time. |
| The update runs untracked, in one batch | An update is a transition, not a binding: reading a field to compute the next value must not subscribe the caller, and a multi-field change must be one flush. |
| `send` inside an update is refused, not queued | A queued message would run against a state its sender had not finished writing, and the ordering would depend on the queue rather than the code. |
| `ViewState`, not `Model` | `Model` already names the domain object behind a `BridgeHandler`. |

## Out of scope

Elm-style `Cmd` values. Server interaction a state change implies is a `Query` keyed on that state,
so an update never needs to issue a request.

# Server-side action validation (planned)

> **Status: planned — not yet implemented.** This spec is the authoritative
> design for closing the "validators do not run server-side" gap documented in
> `../ARCHITECTURE.md` ("Known limitations") and `bridge.md`. It describes the
> intended behavior; the code does not implement it yet. See
> [todo.md](../todo.md).

## The gap

`ActionValidator<A>::ready(action)` (see [registry.md](../spec/registry.md)) is the
framework's readiness/validity predicate. Today it gates only the **client**:

- The reactive `set<...>` path (`BridgeHandler::tryFireImpl`, `bridge.md`) checks
  `ready()` before dispatching.
- The type-erased request/reply path (`ActionExecuteRegistry::execute`,
  `bridge.md`) checks `ready()` and throws `std::invalid_argument` on failure.
- Schema-driven form renderers disable submit until required fields are filled
  (`forms.md`, `allRequiredEngaged`).

But the **server dispatch runner does not**. `ActionDispatcher::registerAction`'s
runner (`registry.hpp`) decodes the payload and calls `Model::execute(action)`
directly:

```cpp
_runners[key] = [](IModelHolder& holder, std::string_view payloadJson) {
    auto action = ActionTraits<Action>::fromJson(payloadJson);   // <-- no ready() check
    auto& model = holder.template into<Model>();
    auto result = model.execute(action);
    ...
};
```

A remote client that builds a `wire::Envelope` by hand — or a buggy/hostile
client that skips the client-side gate — reaches `Model::execute` with an
invalid action. The dispatcher executes whatever arrives. Every model that
dereferences a required field must therefore re-enforce its own precondition
(the `examples/forms` model throws `std::invalid_argument` from the same
`validate()` predicate the GUI uses), which is easy to forget and duplicates the
declaration the framework already has.

This is the last enforcement asymmetry: the same `ActionValidator<A>` that gates
the client is silently ignored on the one path an untrusted client can drive
directly.

## Goal

Run `ActionValidator<A>::ready(action)` **inside the dispatcher runner**, on
every server-side `execute`, immediately after `fromJson` and before
`Model::execute`. A `false` result rejects the call as a defined error rather
than executing an invalid action. This makes validation hold on **all** paths —
local, simulated-remote, and Qt WebSocket — with no per-model code.

## Design

### Injection point

The check goes in `ActionDispatcher::registerAction`'s runner, between decode and
execute:

```cpp
_runners[key] = [](IModelHolder& holder, std::string_view payloadJson) {
    auto action = ActionTraits<Action>::fromJson(payloadJson);
    if (!ActionValidator<Action>::ready(action)) {
        throw ValidationError{ModelTraits<Model>::typeId(),
                              ActionTraits<Action>::typeId()};
    }
    auto& model = holder.template into<Model>();
    auto result = model.execute(action);
    ...
};
```

`ActionValidator<A>::ready` is `constexpr` and auto-detects a `bool validate()
const` member via the `HasValidate` concept, defaulting to `true` when absent
(`registry.hpp`). Therefore:

- Actions with **no** validator (the common case) keep `ready() == true` and
  dispatch exactly as before — **zero behavior change**, backward compatible.
- Actions with a `validate()` member or a `BRIDGE_REGISTER_VALIDATOR`
  specialisation are now enforced server-side too.

### New error type — `ValidationError`

A dedicated exception so the failure is distinguishable from a model throwing:

```cpp
// namespace morph::model
struct ValidationError : std::runtime_error {
    ValidationError(std::string_view modelType, std::string_view actionType);
    // what(): "action failed validation: <modelType>/<actionType>"
};
```

- On the **local** path (`Bridge::executeVia`'s `localOp`) the same check applies
  so an in-process caller that bypasses the reactive gate is also rejected; the
  `Completion` resolves via `onError` with a `ValidationError`.
- On the **remote** path the dispatcher's existing `catch (const std::exception&)`
  on the strand turns it into an `err` reply carrying `exc.what()` and the
  `callId`. The client's `Completion` re-throws it into `onError`.

`ValidationError` derives from `std::runtime_error` so existing `catch`-all error
handling still works; callers that care can `dynamic_cast`/`catch` the specific
type.

### Precision reconciliation moves too

The type-erased client path already runs `forms::reconcileDeclaredPrecision`
before validating (`bridge.md`). To keep the server path consistent with the
schema's advertised `x-decimalPlaces`, the dispatcher runner should reconcile
declared precision on the decoded action **before** the `ready()` check, exactly
as `ActionExecuteRegistry::execute` does. It is a no-op for actions with no
`Quantity` members. This ensures a hand-built envelope with an off-precision
`Quantity` is normalised the same way the reactive path normalises it, so the
validator sees the same value the model will.

## What this does *not* do

- **It is not authorization.** `ready()` answers "is this action well-formed and
  complete?", not "may this caller do it?". Authorization stays in `IAuthorizer`
  (`security.md`) and the planned instance authorization
  ([instance_authorization.md](instance_authorization.md)). A validated action
  can still be rejected by the authorizer, and vice versa.
- **It is not a substitute for model invariants.** A model may still enforce
  deeper business rules `validate()` cannot express (cross-entity constraints,
  balance checks). `validate()` is the field-level readiness contract shared with
  the GUI; the model owns everything beyond it.
- **It does not change the wire format.** No new envelope fields; the rejection
  reuses the existing `err` reply.

## Testing (planned)

- An action with a failing `validate()` dispatched through `RemoteServer` (via
  `SimulatedRemoteBackend` and over the Qt WebSocket transport) produces an
  `err` reply and the client `Completion` resolves through `onError` with a
  `ValidationError` — the model's `execute` is never entered.
- The same action on `LocalBackend` resolves via `onError` with `ValidationError`.
- An action with **no** validator dispatches unchanged on every path (backward
  compatibility).
- A hand-built envelope carrying a `Quantity` at a non-declared precision is
  reconciled before the `ready()` check, so a validator keyed on the value sees
  the declared-precision value.

## Cross-references

- [registry.md](../spec/registry.md) — `ActionValidator<A>::ready`, the `HasValidate`
  concept, `ActionDispatcher::registerAction`'s runner (the injection point),
  `BRIDGE_REGISTER_VALIDATOR`.
- [bridge.md](../spec/bridge.md) — the client-side gates this makes symmetric:
  `tryFireImpl` (reactive) and `ActionExecuteRegistry::execute` (request/reply),
  including the existing precision-reconciliation step.
- [forms.md](../spec/forms.md) — `allRequiredEngaged`, the readiness predicate typically
  used as an action's `validate()` body; `reconcileDeclaredPrecision`.
- [backend.md](../spec/backend.md) — the dispatch call sites (`RemoteServer` remote,
  `LocalBackend` local) and the strand `catch` that turns the throw into an `err`.
- [security.md](../spec/security.md) — why validation is *not* authorization and where
  the two enforcement seams sit relative to each other.

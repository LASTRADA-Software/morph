# Action guards — `morph::model::ActionGuard`

An action guard is a check an application declares per action type and the
framework runs on the decoded action, on every path that reaches
`Model::execute`, after the action's validator and before the model sees it.
It exists for the rule that has to read the payload — "this principal may
approve invoices up to their limit at the invoice's company", "the verifier is
not the analyst who captured the reading" — and must hold whichever topology
the call took. The framework runs the check; what the check decides is the
application's.

Header: `include/morph/core/action_guard.hpp`.

## Contents

- [What a guard is](#what-a-guard-is)
- [Where it runs](#where-it-runs)
- [What a refusal is](#what-a-refusal-is)
- [Where it does not run](#where-it-does-not-run)
- [Direct calls — `executeChecked`](#direct-calls--executechecked)
- [One view of a guard per action](#one-view-of-a-guard-per-action)
- [Enumeration — `actionFacts`](#enumeration--actionfacts)
- [Why not `IAuthorizer`](#why-not-iauthorizer)
- [What a guard does not do](#what-a-guard-does-not-do)
- [Its consumers](#its-consumers)
- [Alternatives considered](#alternatives-considered)
- [Invariants](#invariants)
- [API reference](#api-reference)

## What a guard is

```cpp
namespace morph::model {

template <typename Action>
struct ActionGuard;                       // customisation point, no primary definition

template <typename Action>
concept Guarded = requires(Action const& action) {
    { ActionGuard<Action>::check(action) } -> std::same_as<void>;
};

}  // namespace morph::model
```

An application guards an action by specialising `ActionGuard<Action>` with
`static void check(Action const&)`. `check` returns to allow the action and
throws to refuse it. An action without a specialisation is unguarded, and every
path runs it exactly as it would with no guard header in the program.

The specialisation may be explicit (`template <> struct ActionGuard<Withdraw>`)
or a constrained partial specialisation that covers a family of actions at once:

```cpp
template <typename Action>
    requires app::DeclaresRule<Action>
struct morph::model::ActionGuard<Action> {
    static void check(Action const& action) { app::requireAuthorized(action); }
};
```

The partial form is why the customisation point is a class template rather than
a registration: one declaration in the application covers every action of every
module that declares a rule, and an action joins the family by declaring its
rule, not by being listed anywhere.

`check` receives the action only. It reads the principal from
`session::current()` and anything else it needs from the application's own
state, as `Model::execute` would; it does not receive the model instance (see
[What a guard does not do](#what-a-guard-does-not-do)).

## Where it runs

`Model::execute` is called from exactly four places in `include/morph`, and the
guard runs in each:

| Execution site | Path | Topologies |
|---|---|---|
| `ActionDispatcher`'s `runHandler` | server, ordinary handler | every remote and Qt WebSocket topology, `SimulatedRemoteBackend`, and the by-id raw call on `LocalBackend` (`Bridge::executeRaw`), which runs the server's JSON runner |
| `ActionDispatcher`'s `runTaskHandler` | server, `Task` handler | as above, for a handler returning `core::async::Task` |
| `Bridge::executeVia`'s `localOp` | in-process, ordinary handler | `LocalBackend`, on whichever executor it was given |
| `detail::localTaskOp` | in-process, `Task` handler | as above, for a `Task` handler |

Every other way an action is dispatched — `BridgeHandler::execute`,
`ActionExecuteRegistry`'s by-id executor, `FlowSession::set<>`, the declarative
UI's `RawHandler`, an internal client on the server — ends in one of these four,
so the guard needs no call of its own there.

**Order.** On each site the guard runs after every gate that already precedes
`Model::execute` and immediately before it:

1. decode (server paths only);
2. `forms::reconcileDeclaredPrecision` and `forms::enforceQuantityBounds`
   (server paths only — the in-process path decodes no JSON);
3. `forms::recomputeAll`;
4. `ActionValidator<Action>::ready`, throwing `ValidationError`;
5. **`ActionGuard<Action>::check`**;
6. `Model::execute` (for a `Task` handler, creating the Task; the guard runs
   before the Task exists, synchronously).

The guard therefore sees the authoritative action: precision reconciled,
computed fields recomputed from their inputs, validator satisfied. A guard never
has to defend against a tampered computed field or a malformed payload.

**Session.** `session::current()` during `check` is the execute's session on
every site: `RemoteServer` installs the envelope's session, with the principal
its authorizer authenticated, before it dispatches; `LocalBackend` installs the
call's session around `localOp`. A guard reads the same principal the model
would.

**Thread.** The guard runs where the execute runs — on the model's strand, in
the same task, with nothing between them — so a guard may read the model's
store as `execute` would, and a row it reads is the row `execute` sees unless
`execute` itself changes it.

## What a refusal is

A guard that throws refuses the action. `Model::execute` does not run, and the
exception reaches the caller exactly as a throw from `execute` would:
`LocalBackend` resolves the `Completion` through `onError` with the concrete
type; `RemoteServer` answers `err` with `what()`, and the client's `Completion`
resolves with a `std::runtime_error` carrying that text.

**A refusal is journaled.** When a `journal::IActionLog` is attached to the
holder and the action is loggable, a refusal records a `LogEntry` with
`outcome = Outcome::Failed`, `error = what()` and an empty `result`, as a throw
from `execute` does. A refused attempt is the entry an audit trail exists for —
"who tried to approve an invoice over their limit" — and leaving it out would
make the guard the one refusal the trail cannot show. The guard runs in its own
`try`, just before the execute's, so `Model::execute` stays the only call in the
execute's `try` and an infrastructure fault after the mutation is still
`ActionRecordingError`, never `Failed`.

**A refusal of nobody is not journaled.** When the session at the refusal names
no principal — no context is installed, or its `principal` is empty — the
refusal records nothing. An audit entry that names nobody attributes the attempt
to no one, and an unauthenticated call is the authorizer's to turn away, not a
fact about the action. The consequence is stated plainly: on a server whose
authorizer does not authenticate, `RemoteServer` clears every principal, so no
guard refusal there is journaled. A throw from `execute` is journaled as before,
whatever the principal.

**A model's own journal does not see it.** A model that appends its own entries
from inside `execute` (rather than through the holder's attached log) is never
entered for a refused action, so its journal holds no entry for the refusal. On
the dispatch paths the holder's log does; for a model constructed directly and
run through `executeChecked`, nothing records it.

## Where it does not run

- **`journal::replay()` and `SessionLog::undoLast()`.** Replay re-dispatches
  `Succeeded` entries through `ActionDispatcher::dispatch` to reconstruct state.
  Each entry was guarded when it ran; re-judging it against today's roles, under
  no session, would refuse committed facts and abort the reconstruction. The
  guard is skipped while `journal::isReplaying()` holds, on every site.
- **`ActionDispatcher::evaluate`.** An evaluation answers whether a body is
  ready to dispatch — precision, bounds, computed fields, validator — with no
  model instance and nothing changed. It does not run the guard, so `ready`
  means "well-formed", never "allowed"; an evaluation that ran a guard would
  answer an authorization question to whoever asks it, before execution.
- **A direct `model.execute(action)`.** A plain C++ call to a model runs no gate
  at all — no validator and no guard. That includes a model calling its own
  `execute` (draining a queue it owns, say). `executeChecked` is the call for
  code that must hold the guarantee without dispatching.

## Direct calls — `executeChecked`

```cpp
template <typename Model, typename Action>
auto executeChecked(Model& model, Action const& action);
```

Runs `ActionValidator<Action>::ready` (throwing `ValidationError`), then the
guard, then `model.execute(action)`, and returns what `execute` returned. It is
for a caller that holds a model directly: a unit test of a guard's outcome
against a plainly constructed model, or server-side code that runs a model in
process rather than through a client. It does not recompute computed fields and
journals nothing — there is no holder — so it is a check on the action, not a
second dispatch path.

Server-side work that acts on another model — a scheduled job, a reaction to
another module's event — dispatches through a client of its own (in lims, an
internal client with a service principal) rather than calling `execute`, so the
guard runs on its path as on any client's. The examples' rule that models
re-check their own authorization is met by a guard declared beside the action.

## One view of a guard per action

Whether `ActionGuard<Action>` is specialised is decided where each execution site
is instantiated: `runHandler` and `runTaskHandler` at the action's
`BRIDGE_REGISTER_ACTION`, `localOp` and `localTaskOp` wherever
`BridgeHandler<Model>::execute<Action>` is instantiated, `executeChecked` at its
call. A specialisation visible at some of those points and not at others is an
ODR violation, and in practice an action that is guarded on one path and not on
another.

So a guard is **declared before the action's registration, in a header that
every translation unit naming the action includes** — beside the action, or in
a header the action's registering header includes.

The framework detects a disagreement where it can, rather than leaving it
silent: `localOp`, `localTaskOp` and `executeChecked` compare, once per action
type, their own `Guarded<Action>` with the value the registration recorded, and
throw `std::logic_error` naming the action when they differ. An action whose
registration saw no guard reports `guarded = false` through `actionFacts`,
which is what an application's coverage test reads.

## Enumeration — `actionFacts`

```cpp
struct ActionFact {
    std::string actionType;
    bool guarded = false;
};

std::vector<ActionFact> actionFacts(std::string_view modelType);
```

Lists the actions registered under `modelType`, in `actionsOf` order (sorted),
each with whether its registration saw a guard; empty for an unknown model.
With `effects::effectsOf(actionId)` it lets an application prove "every action
that changes data is guarded or deliberately open" by enumeration, so a new
mutating action that is neither fails the proof without anyone remembering to
list it. Building the list scans the dispatcher's entries for the model; it is
for tests and start-up checks, not for a per-call path.

## Why not `IAuthorizer`

`IAuthorizer::authorize(ctx, modelType, actionType)` and `admit` decide before
the payload is decoded, and one authorizer serves every model. They see the
principal and two ids — never the payload, so never the scope it names, the
amount it carries, or the row it acts on. A guard is per action, sees the
decoded payload, and runs on the model's strand next to `execute`.

They divide the work rather than overlap: the authorizer authenticates and keeps
model-level and per-instance gates (`authorizeRegister`, `authorizeInstance`);
a guard is where a rule that reads the payload lives. The authorizer runs only
on `RemoteServer`; a guard runs on the in-process path too, which is what
[`security.md`](../security.md)'s "security-critical checks must be enforced
inside the model so they hold in both modes" asks for.

## What a guard does not do

- **It does not know the domain.** Roles, scopes, value limits,
  separation-of-duties pairs, tenancy — whatever the rule is made of is the
  application's. The framework knows only that a check is declared, where to
  run it, and what a throw means.
- **It does not compose.** One guard per action type. There is no ordering of
  several guards, no chain, and no check spanning actions or models; an
  application that wants several conditions writes them in one `check`.
- **It does not see the model instance.** A rule whose input is the instance's
  own state — the sample a keyed handler is attached to, for instance — stays in
  `execute`, where that state is loaded. A guard that took the model would be a
  second reader of the instance's state ahead of the one that owns it.
- **It does not decide order inside `execute`.** A rule the model must answer
  only after another check of its own (an idempotent redelivery answered before
  its sender's current role, say) is not a guard's rule: a guard always runs
  first.
- **It is not authentication.** A guard reads the principal it is given.
  Whether that principal is verified is the authorizer's
  ([`security.md`](../security.md)).

## Its consumers

The guard is designed against two instances of the same hand-rolled guarantee
in `examples/lims`, which declares authorization per action
(`lims::Authorization<Action>`: a role, the scope the payload names, an optional
value limit, optional separation-of-duties pairs):

- **lab** — capture and verify: the verifier holds the verifier role at the
  sample's site and is not the analyst who captured the reading.
- **billing** — create, submit and approve an invoice: the approver holds the
  approver role at the invoice's site or, past its deadline, its company, has a
  value limit there covering the invoice's gross, and is neither its creator
  nor its submitter.

Both are covered by one constrained specialisation,
`template <typename Action> requires lims::DeclaresRule<Action> struct ActionGuard<Action>`,
which also covers org's and the ledger's scoped roles. Rules whose scope is the
handler's attached instance (lab's sample lifecycle) keep their scope check in
`execute`, for the reason given under
[What a guard does not do](#what-a-guard-does-not-do).

## Alternatives considered

| Alternative | Why not |
|---|---|
| An `IAuthorizer` overload that receives the payload | It runs in `RemoteServer`'s admission, on the server strand before the call reaches the model's strand, so it cannot read the model's rows in order with the execute; it would not run on the in-process path at all; and one authorizer would have to know every module's actions. |
| A registration macro taking a function pointer (`BRIDGE_REGISTER_GUARD(A, FN)`) | A trait composes with a constrained partial specialisation, which is what lets one application declaration cover every module's actions; a macro is one line per action, and the forgotten line is the hazard the guard exists to remove. |
| Leaving the call in each handler | Every declared action's handler then calls the role check and the rule check itself — in lims, across four modules — so a forgotten call is an unguarded action that compiles and passes every test that does not name it; and the server edge restates every declaration in a hand-kept table, because nothing enumerates a model's actions or says which are checked. |
| Running the guard before the validator | The guard would judge a payload the validator would refuse, and a rule reading a computed field would read the client's value instead of the recomputed one. |
| A guard that receives the model (`check(Model const&, Action const&)`) | See [What a guard does not do](#what-a-guard-does-not-do): the instance's state is `execute`'s to load, and the measured rules that need it are a minority whose check stays beside that load. |

## Invariants

Each is a test in `tests/test_action_guard.cpp`; each would fail if the guard
did nothing.

1. A guarded action that the guard refuses does not reach `Model::execute` on
   the server path, and the caller receives the refusal's text.
2. The same holds on the in-process path (`LocalBackend`).
3. An unguarded action of the same model executes unchanged on both paths.
4. The guard reads the execute's principal: the same action is refused under
   one principal and allowed under another.
5. The guard runs after the validator: an action the validator refuses is
   refused with `ValidationError`, and the guard is not called.
6. A refusal with a principal records one `Outcome::Failed` entry; a refusal
   with an empty principal records none; neither records `Succeeded`.
7. `journal::replay()` of a `Succeeded` entry does not call the guard.
8. `ActionDispatcher::evaluate` does not call the guard.
9. `executeChecked` refuses before `execute` runs and returns `execute`'s result
   when the guard allows.
10. `actionFacts` lists every registered action of a model with `guarded` true
    exactly for the guarded ones, and nothing for an unknown model.
11. A `Task` handler's action is guarded on both the server and the in-process
    path, and its refusal is journaled as an ordinary handler's is.

## API reference

| Symbol | Kind | Description |
|---|---|---|
| `ActionGuard<Action>` | class template | **Customisation point.** Specialise with `static void check(Action const&)`; throw to refuse. No primary definition. |
| `Guarded<Action>` | concept | Whether `ActionGuard<Action>::check(action)` is well-formed and returns `void`. |
| `detail::guardAction(action)` | function template | Runs the guard when `Guarded<Action>`, otherwise nothing. Called by the four execution sites and `executeChecked`. |
| `executeChecked(model, action)` | function template | Validator, guard, `execute`, for a caller that holds a model directly. Throws `ValidationError` or whatever the guard or `execute` throws. |
| `ActionFact` | struct | `actionType` (the registered id) and `guarded` (whether its registration saw a guard). |
| `actionFacts(modelType)` | function | The registered actions of a model with their facts; empty for an unknown model. |

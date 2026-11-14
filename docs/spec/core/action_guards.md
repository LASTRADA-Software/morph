# Action guards — `Action::guard`

An action guard is a check an action declares and the framework runs on the
decoded action, on every path that reaches `Model::execute`, after the action's
validator and before the model sees it. It exists for the rule that has to read
the payload, and sometimes the model instance the action is addressed to —
"this principal may approve invoices up to their limit at the invoice's
company", "the verifier is not the analyst who captured the reading", "the
caller holds a role at the site of the sample this handler is attached to" —
and that must hold whichever topology the call took. The framework runs the
check; what the check decides is the application's.

Headers: `include/morph/core/action_guard.hpp` (public),
`include/morph/core/detail/action_guard_core.hpp` (the declarations the
execution sites need; see [Header layout](#header-layout)).

## Contents

- [What a guard is](#what-a-guard-is)
- [Where it runs](#where-it-runs)
- [What a guard may rely on](#what-a-guard-may-rely-on)
- [What a refusal is](#what-a-refusal-is)
- [Where it does not run](#where-it-does-not-run)
- [Direct calls — `executeChecked`](#direct-calls--executechecked)
- [Why the declaration is inside the action](#why-the-declaration-is-inside-the-action)
- [Enumeration — `isGuarded`](#enumeration--isguarded)
- [Why not `IAuthorizer`](#why-not-iauthorizer)
- [What a guard does not do](#what-a-guard-does-not-do)
- [Its consumers](#its-consumers)
- [Alternatives considered](#alternatives-considered)
- [Header layout](#header-layout)
- [Invariants](#invariants)
- [API reference](#api-reference)

## What a guard is

An action declares its guard with a public member alias named `guard`:

```cpp
struct WithdrawalLimit;                   // a declaration is enough here

struct Withdraw {
    using guard = WithdrawalLimit;
    int amount{};
};

struct WithdrawalLimit {
    static void check(Withdraw const& action);                       // payload only, or
    // static void check(Account const& model, Withdraw const& action);  // the instance too
};
```

```cpp
namespace morph::model {

template <typename Action>
concept DeclaresGuard = requires { typename Action::guard; };

}  // namespace morph::model
```

The guard type provides `check` in one of two forms:

- **`static void check(Action const&)`** — for a rule that reads the payload,
  the session and the application's own state.
- **`static void check(Model const&, Action const&)`** — for a rule whose
  input includes the instance the action is addressed to: the sample a keyed
  handler is attached to, a working copy the model holds. The model is passed
  `const`; the guard reads it and changes nothing.

`check` returns to allow the action and throws to refuse it. For each model the
action is registered under, exactly one of the two forms must be callable with
that model's type; neither, or both, is a compile error naming the action, at
the first execution site instantiated for that pair. An action without a
`guard` alias is unguarded, and every path runs it exactly as it would with no
guard header in the program; an action that is deliberately open says so by
declaring nothing.

`DeclaresGuard` also matches a `guard` alias an action inherits from a base
class, so a family of actions may share one declaration in a common base; that
is intended, and the one-definition argument below holds for it as well.

One guard type may serve many actions through member templates, each form
constrained to the actions it applies to:

```cpp
struct DeclaredRuleGuard {
    template <typename Action>
        requires app::DeclaresRule<Action> && (!app::ScopedByModel<Action>)
    static void check(Action const& action);

    template <typename Model, typename Action>
        requires app::ScopedByModel<Action>
    static void check(Model const& model, Action const& action);
};
```

The alias may name a type that is only declared where the action is defined, so
a DTO header needs no include of the guard's implementation. The type must be
complete wherever an execution site for the action is instantiated, and the
build fails at a site where it is not — see
[Why the declaration is inside the action](#why-the-declaration-is-inside-the-action).

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
5. **the guard's `check`**;
6. `Model::execute` (for a `Task` handler, creating the Task; the guard runs
   before the Task exists, synchronously).

In a `MORPH_CLIENT_ONLY` build `localOp` throws before step 5, as it does
before `execute`: the guard's call sits with the `execute` call, so a guard
that reads the model never needs the model's definitions in a client build.

**Session.** `session::current()` during `check` is the execute's session on
every site: `RemoteServer` installs the envelope's session, with the principal
its authorizer authenticated, before it dispatches; `LocalBackend` installs the
call's session around `localOp`. A guard reads the same principal the model
would.

**Thread.** On the four sites the guard runs where the execute runs — on the
model's strand, in the same task, with nothing between them.

## What a guard may rely on

- **The action `execute` receives.** Precision reconciled and bounds enforced
  (server paths), computed fields recomputed from their inputs, validator
  satisfied. A guard never defends against a tampered computed field or a
  malformed payload; `executeChecked` recomputes and validates too.
- **The execute's session**, as above.
- **State the model's strand alone writes.** Nothing else runs on the strand
  between the guard and the start of `execute`, so the instance's own members,
  and a store only that instance writes, are what `execute` starts from.

Nothing else. State that other strands or processes write — a database several
instances share, another model's rows, the role assignments a rule reads — can
change between the guard's read and `execute`'s. No transaction spans the two,
and for a `Task` handler the code after its first suspension runs later still. A
guard's verdict over such state is a verdict at the moment of the check.

So **a rule that must hold atomically with the write stays in `execute`**, judged
inside the write's transaction: a value limit compared with an amount the row
records, a separation-of-duties performer read together with the row being
approved. The guard may still run the same rule first, to refuse early and on
every path; `execute` then judges it again where the answer and the write
commit together.

## What a refusal is

This section covers any exception a guard throws. A refusal an application throws on purpose and a screen words is
`morph::model::ActionRefusal`, specified in [`../ui/refusals.md`](../ui/refusals.md).

A guard that throws refuses the action. `Model::execute` does not run, and the
exception reaches the caller exactly as a throw from `execute` would:
`LocalBackend` resolves the `Completion` through `onError` with the concrete
type; `RemoteServer` answers `err` with `what()`, and the client's `Completion`
resolves with a `std::runtime_error` carrying that text. A guard that throws a
`morph::model::ActionRefusal` is the exception: the `err` also carries the
refusal's code and arguments, and the client's `Completion` resolves with an
`ActionRefusal` (see [`../ui/refusals.md`](../ui/refusals.md)). Only a throw from the
guard or the model's run carries one; a throw before the model runs (an admission
check) is answered as any other exception is.

**A refusal is journaled as a throw from `execute` is.** When a
`journal::IActionLog` is attached to the holder and the action is loggable, a
refusal that is a `std::exception` records a `LogEntry` with
`outcome = Outcome::Failed`, `error = what()` and an empty `result`, whatever
the session's principal — an empty principal is recorded as empty, exactly as
for a refused `execute`. A refused attempt is the entry an audit trail exists
for, and the trail cannot leave out one kind of refusal. An unauthenticated
call is the authorizer's to turn away before it is dispatched; once dispatched,
its refusal is a fact about the action like any other. A throw that is not a
`std::exception` records nothing and propagates, as from `execute`.

The guard runs in a `try` of its own, just before the execute's, so
`Model::execute` stays the only call in the execute's `try` and an
infrastructure fault after the mutation is still `ActionRecordingError`, never
`Failed`. A throw from recording the refusal propagates in place of the
refusal, as a throw from recording a refused `execute` does.

**A model that keeps its own trail hears the refusal.** `IModelHolder::recordIfAttached`
records nothing for an outbox-managed holder (`setOutboxManaged(true)`), whose
model writes its own entries inside `execute`; and a model that journals into
its own log under its own keys never sees an action its guard refused. Either
kind of model declares

```cpp
void onGuardRefused(Action const& action, std::exception const& refusal);
```

for the actions whose refusals it records. After the holder's own record, every
site — the four dispatch sites and `executeChecked` — calls it on the model, on
the strand, under the execute's session, for a refusal that is a
`std::exception`. It is detected structurally per action, as
`onBackendChanged` is, and a model without it is never called. What it records,
and whether it records a refusal of nobody, is the model's. A throw from it
propagates in place of the refusal: the holder's `Failed` entry carries the
refusal's `what()`, while the caller receives the hook's exception.

The hook may write. It receives the model non-`const` and a model that keeps its
own trail writes that trail in it. "A guard changes nothing" is a rule for
`check`, which receives the model `const`, not for `onGuardRefused`.

## Where it does not run

- **The holder `journal::replay()` reconstructs.** Replay re-dispatches
  `Succeeded` entries through `ActionDispatcher::dispatch` into a holder it has
  just created and has not shared. Each entry was guarded when it ran;
  re-judging it against today's roles, under no session, would refuse
  committed facts and abort the reconstruction. `replay()` marks that holder
  as reconstructing (`IModelHolder::markReconstructing`) for its dispatch loop
  and clears the mark before it returns the holder, and the four sites skip the
  guard for a holder so marked. Only `replay()` may call `markReconstructing`:
  it is a public mutator of a `detail` class, and any other code holding a
  holder could switch its guards off with it. The skip belongs to the holder,
  not the thread: any other dispatch during a replay — a nested dispatch from
  inside a replayed `execute` onto another holder, a live call an executor runs
  inline — runs its guard as always. `SessionLog::undoLast()` replays, so it is
  covered by the same mark. Replay is synchronous, so a `Task` handler is never replayed.
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
auto executeChecked(Model& model, Action action);
```

Recomputes `action`'s computed fields (`forms::recomputeAll`), runs
`ActionValidator<Action>::ready` (throwing `ValidationError`), then the guard in
whichever form the action declares, calling `model.onGuardRefused` on a refusal
when the model declares it, then `model.execute(action)`, and returns what
`execute` returned. It is for a caller that holds a model directly: a unit test
of a guard's outcome against a plainly constructed model, or server-side code
that runs a model in process rather than through a client. There is no holder,
so the framework records nothing and never skips the guard.

It takes the action by value, because recomputing changes it. That copy dies
when `executeChecked` returns, so it accepts ordinary handlers only: for a
handler returning `core::async::Task` it is a compile error, since the Task
would outlive the action it refers to. A `Task` handler's guard is exercised
through a backend.

Server-side work that acts on another model — a scheduled job, a reaction to
another module's event — dispatches through a client of its own rather than
calling `execute`, so the guard runs on its path as on any client's. The
examples' rule that models re-check their own authorization is met by a guard
the action declares.

## Why the declaration is inside the action

Whether an action is guarded is read off its own definition
(`DeclaresGuard<Action>`), which the one-definition rule already requires to be
identical in every translation unit that names the action. Every execution site
and the registration therefore agree on it without any check: there is no
translation unit in which the action is silently unguarded.

What can differ between sites is whether the guard type is complete, and
whether a form is callable. Both fail the build rather than the guarantee: a
site that instantiates the call while the guard type is only declared is a
compile error (the framework requires the type complete before it looks for a
form), and so is a site where no form, or both, is callable for that model. A
guard therefore must be complete, with everything its constrained forms depend
on visible, before the action's registration and wherever
`BridgeHandler<Model>::execute<Action>` or `executeChecked` is instantiated —
in practice, in a header the action's registering header includes. A guard
completed later in the same translation unit than such a site is accepted by
compilers that instantiate function templates at the end of the unit and is
not portable; it is never an unguarded action, because guardedness does not
depend on the site.

## Enumeration — `isGuarded`

```cpp
class ActionDispatcher {
    bool isGuarded(std::string_view modelId, std::string_view actionId) const;
};

namespace morph::model {
bool isGuarded(std::string_view modelType, std::string_view actionType);  // the default dispatcher's answer
}
```

`isGuarded` answers whether the registered pair's action declares a guard;
`false` for an unregistered pair. `registerAction` records the answer in the
pair's `ActionEntry`. The free function asks `detail::defaultDispatcher()`,
which is where `BRIDGE_REGISTER_ACTION` registers. With `actionsOf(modelId)` and
`effects::effectsOf(actionId)`, it lets an application prove by enumeration that
every action that changes data is guarded or deliberately open, so a new
mutating action that is neither fails the proof without anyone remembering to
list it.

`isGuarded` says that a check runs on every execution site for the pair, not
that the check enforces the application's whole rule. An application that wants
the second proves it in its guard type: lims does so at compile time, with one
form per shape of declaration, so a declaration whose shape no form handles
does not compile.

## Why not `IAuthorizer`

`IAuthorizer::authorize(ctx, modelType, actionType)` and `admit` decide before
the payload is decoded, and one authorizer serves every model. They see the
principal and two ids — never the payload, so never the scope it names, the
amount it carries, or the row it acts on — and never the model instance. A guard
is per action, sees the decoded payload and, in its model form, the instance,
and runs on the model's strand next to `execute`. The authorizer runs only on
`RemoteServer`; a guard runs on the in-process path too, which is what
[`security.md`](../security.md)'s "security-critical checks must be enforced
inside the model so they hold in both modes" asks for.

**A guard replaces the authorizer as an execute gate only.** `authorize`'s
per-id answer also admits reads: `RemoteServer` admits a watch on a record when
the principal is authorized for some action holding that record, and a watch on
a model's instances when it is authorized for any of the model's actions, and
re-checks both at fan-out. A watch carries no payload and runs no action, so no
guard runs for it. An application whose actions are role-gated therefore keeps
an id-level role answer in its authorizer, even when every such action is
guarded, or a principal with no role at all can watch what those actions hold.
`isGuarded` says which actions are guarded; it cannot produce the per-id answer
the read gate needs.

So the work divides: the authorizer authenticates, keeps model-level and
per-instance gates (`authorizeRegister`, `authorizeInstance`) and gives the
id-level answer that admits watches; a guard is where a rule that reads the
payload or the instance lives, enforced on every execution.

## What a guard does not do

- **It does not know the domain.** Roles, scopes, value limits,
  separation-of-duties pairs, tenancy — whatever the rule is made of is the
  application's. The framework knows only that a check is declared, where to
  run it, and what a throw means.
- **It does not compose.** One guard per action. There is no ordering of
  several guards, no chain, and no check spanning actions or models; an
  application that wants several conditions writes them in one `check`.
- **It does not change state.** `check` reads; the model form receives the
  instance `const`. A guard does not dispatch, write, or wait on the strand it
  runs on.
- **It is not atomic with the write** (see
  [What a guard may rely on](#what-a-guard-may-rely-on)).
- **It does not decide order inside `execute`.** A rule the model must answer
  only after another check of its own (an idempotent redelivery answered before
  its sender's current role, say) is not a guard's rule: a guard always runs
  first. Such an action declares no guard and keeps its rule in `execute`.
- **It does not gate reads** (see [Why not `IAuthorizer`](#why-not-iauthorizer)).
- **It is not authentication.** A guard reads the principal it is given.
  Whether that principal is verified is the authorizer's
  ([`security.md`](../security.md)).

## Its consumers

The guard is designed against two instances of the same hand-rolled guarantee
in `examples/lims`, which declares authorization per action
(`lims::Authorization<Action>`: a role, the scope the payload or the attached
instance names, an optional value limit, optional separation-of-duties pairs):

- **lab** — capture and verify: the verifier holds the verifier role at the
  sample's site and is not the analyst who captured the reading; every write on
  a sample needs a role at the sample's site, which for the lifecycle
  transitions, capture, save and delete is the site of the sample the handler
  holds — the model form's case.
- **billing** — create, submit and approve an invoice: the approver holds the
  approver role at the invoice's site or, past its deadline, its company, has a
  value limit there covering the invoice's gross, and is neither its creator
  nor its submitter.

What lims needs from the seam: one guard type serving every declared
action of every module through constrained member templates; the model form for
the rules scoped by the attached sample; `onGuardRefused` so the sample's own
audit trail still shows a refused verification or transition; one action
(replaying a queued capture, which answers a redelivery before the role) that
declares no guard; the in-transaction re-judgement for the rules that read rows
the write commits with; and an edge that keeps its per-id role answer for
watches.

## Alternatives considered

| Alternative | Why not |
|---|---|
| A trait the application specialises (`ActionGuard<Action>`), with constrained partial specialisations covering a family of actions | Whether a specialisation exists is decided per translation unit at each point of use. A site compiled without it is silently unguarded: an ODR violation with no diagnostic required; constraint satisfaction that differs between points of use is ill-formed with no diagnostic required, and GCC caches the first answer, so even a same-TU `static_assert` sees it; a specialisation declared after a use that would have instantiated it is ill-formed with no diagnostic required. Only a runtime comparison on some paths could notice, and only a disagreement. The alias inside the action is part of a definition every translation unit sees identically, and an incomplete guard is a compile error. It costs one line per action, and an action type the application does not own cannot declare one (it wraps the type instead). |
| A guard that receives only the action | It cannot judge a rule scoped by the instance — such rules stay as hand calls in `execute`, which the enumeration then reports as guarded — and a rule that reads a store the application injects into the model has to reach it through global state. The model form is read-only, on the strand, immediately before `execute`, which is where `execute` would read the same state. |
| An `IAuthorizer` overload that receives the payload | It runs in `RemoteServer`'s admission, on the server strand before the call reaches the model's strand; it would not run on the in-process path at all; and one authorizer would have to know every module's actions. |
| A registration macro taking a function pointer (`BRIDGE_REGISTER_GUARD(A, FN)`) | Separate from the action's definition, so it has the trait's visibility problem, and a forgotten line is an unguarded action. |
| Leaving the call in each handler | Every declared action's handler then calls the role check and the rule check itself — in lims, across four modules — so a forgotten call is an unguarded action that compiles and passes every test that does not name it, and nothing enumerates which actions are checked. |
| Running the guard before the validator | The guard would judge a payload the validator would refuse, and a rule reading a computed field would read the client's value instead of the recomputed one. |
| Not journaling a refusal that names no principal | A policy keyed on identity inside a mechanism. A refused `execute` is journaled whatever the principal, and a guard's refusal is the same fact; a model that wants no entry naming nobody in its own trail decides that in `onGuardRefused`. |
| Skipping the guard while `journal::isReplaying()` holds | The flag is per thread: every dispatch on the thread during the replay loop would skip, including a nested dispatch onto another holder and a live call an executor pumps inline from inside a replayed `execute`. The mark on the holder replay owns names exactly the dispatches being reconstructed. |

## Header layout

- **`core/detail/action_guard_core.hpp`** — `DeclaresGuard`, the form
  detection, `GuardRefusalObserver`, `detail::guardAction`. Depends on nothing
  in `registry.hpp`; `registry.hpp` includes it, and so `bridge.hpp` and every
  execution site see it.
- **`core/registry.hpp`** — `detail::runGuardJournaled` (the shared refusal
  path: skip for a reconstructing holder, the guard's own `try`, the holder's
  record, `onGuardRefused`), `ActionEntry::guarded`,
  `ActionDispatcher::isGuarded`.
- **`core/model.hpp`** — `IModelHolder::markReconstructing` /
  `isReconstructing`.
- **`journal/journal.hpp`** — `replay()` marks the holder it creates.
- **`core/action_guard.hpp`** — the public header: includes `registry.hpp` and
  the core header, and adds `executeChecked` and the free `isGuarded`.

## Invariants

Each is a test in `tests/test_action_guard.cpp` (or, for 15, a configure-time
compile probe); each would fail if the guard did nothing.

1. A guarded action that the guard refuses does not reach `Model::execute` on
   the server path, and the caller receives the refusal's text.
2. The same holds on the in-process path (`LocalBackend`).
3. An unguarded action of the same model executes unchanged on both paths.
4. The guard reads the execute's principal: the same action is refused under
   one principal and allowed under another.
5. The guard runs after the validator: an action the validator refuses is
   refused with `ValidationError`, and the guard is not called.
6. A refusal records one `Outcome::Failed` entry carrying the principal, and a
   refusal under an empty principal records one carrying an empty principal;
   neither records `Succeeded`.
7. A refusal that is not a `std::exception` records nothing and reaches the
   caller.
8. `journal::replay()` of a `Succeeded` entry does not call the guard, and a
   dispatch onto another holder from inside the replayed `execute` does.
9. `ActionDispatcher::evaluate` does not call the guard.
10. `executeChecked` recomputes, validates, refuses before `execute` runs, and
    returns `execute`'s result when the guard allows.
11. `isGuarded` is true exactly for the guarded actions of a model, and false
    for an unregistered pair.
12. A `Task` handler's action is guarded on both the server and the in-process
    path, and its refusal is journaled before the Task is created.
13. The model form receives the instance the action is addressed to: the same
    action is refused against one instance's state and allowed against
    another's, on both paths and through `executeChecked`.
14. `onGuardRefused` is called once per refusal on every path and through
    `executeChecked`, including for an outbox-managed holder, for which the
    framework records nothing.
15. An action whose guard type is only declared where an execution site is
    instantiated does not compile, nor does one whose guard has no callable
    form; the same action with the guard defined does.

## API reference

| Symbol | Kind | Description |
|---|---|---|
| `Action::guard` | member alias | **The declaration.** Names the guard type; it may be only declared where the action is defined. |
| `DeclaresGuard<Action>` | concept | Whether `Action` has a `guard` alias. |
| guard `check(Action const&)` / `check(Model const&, Action const&)` | static member function of the guard type | The check, in one of two forms; exactly one callable per registered model; throw to refuse. |
| `onGuardRefused(Action const&, std::exception const&)` | optional model member | Called after a refusal on every site and in `executeChecked`; detected per action. |
| `GuardRefusalObserver<Model, Action>` | concept | Whether `Model` declares `onGuardRefused` for `Action`. |
| `detail::guardAction(model, action)` | function template | Requires the guard complete, picks the form, runs it; nothing for an unguarded action. |
| `detail::runGuardJournaled(holder, model, action, payloadOf)` | function template | The four sites' refusal path: the replay skip, the guard's `try`, the `Failed` record, `onGuardRefused`, rethrow. |
| `executeChecked(model, action)` | function template | Recompute, validator, guard, `execute`, for a caller that holds a model directly; ordinary handlers only. Throws `ValidationError` or whatever the guard, `onGuardRefused` or `execute` throws. |
| `ActionDispatcher::isGuarded(modelId, actionId)` | member function | Whether the registered pair's action declares a guard; `false` for an unregistered pair. |
| `isGuarded(modelType, actionType)` | function | `isGuarded` on `detail::defaultDispatcher()`. |
| `IModelHolder::markReconstructing(bool)` / `isReconstructing()` | member functions | The mark `replay()` sets on the holder it reconstructs, for which the sites skip the guard. |

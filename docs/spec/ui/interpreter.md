# The document interpreter — design

Design spec for `morph::ui::Interpreter` and what it builds (`include/morph/ui/interpreter.hpp`): a
loaded UI document ([`document.md`](document.md)) becomes reactive state over the reactive core
([`../reactive/signals.md`](../reactive/signals.md)) and a view tree ([`view_tree.md`](view_tree.md))
mounted through `Mounted`. Queries, mutations, relations and commands run as spec 5 §5–§8 describe
(`docs/superpowers/specs/2026-10-07-ui-document-design.md`); this page says how, and where the
interpreter decided what the design left open.

Read this before changing what a mounted document does. What a document may say is
[`document.md`](document.md); what its expressions mean is [`expressions.md`](expressions.md).

## Contents

- [The seams](#the-seams)
- [Mounting](#mounting)
- [Scopes](#scopes)
- [Queries and the cache](#queries-and-the-cache)
- [Mutations](#mutations)
- [Relations](#relations)
- [Commands](#commands)
- [Lowering](#lowering)
- [Diagnostics](#diagnostics)
- [Lifetimes](#lifetimes)
- [Design decisions](#design-decisions)
- [Not yet lowered](#not-yet-lowered)

## The seams

| Type | Role |
|---|---|
| `Dispatcher` | Binds a model by type id, privately or to a shared instance (`bind`), and returns a `ModelChannel` whose `execute(actionId, bodyJson, CallOptions)` sends one action and resolves with the reply's JSON text on the runtime's owner. |
| `BridgeDispatcher` | The application's `Dispatcher`: `bind` makes a `bridge::RawHandler` (`bindByType`, and `attach` for a key), `execute` is `executeRaw`. It cannot send `evaluate` (the UI service's envelope; refused with a `std::runtime_error`) and has no offline queue (`queueOffline` is ignored). A shared binding without a key is left unattached, which the bridge refuses at the first call. |
| `InterpreterHost` | What only the application can do: `navigate`, `closeSelf` (`return`, `close: self`), `focus`, `hostCommand`, `switchBackend`, `resetPrefs`, `setLocale`, `notify`, `signIn`, `signOut`, `setAppValue` (`set` of `app.name`), `upload`, `download`, `preferenceChanged`, `permissionsStale`. Every default answers "not supported", and the interpreter reports it. |
| `Session` | What screens of one client session share: the query cache, `refreshOn` bookkeeping, the permitted action ids `can` reads (`setPermitted`), the formatting facts (`setFormatting`, `setLocale`), and the `env`, `session` and `host` values. `clear()` is `signOut` and a principal change; `backendChanged()` a backend switch. |
| `Interpreter` | `mount(doc, params, backend, MountOptions)` → `Screen`. |
| `Screen` | A mounted screen: `root()`, `title()`, `identity()`, `dirty()`, `requestClose()` (runs `onClose`; false when it ran `cancelClose`), `runLifecycle(key)`, `setPreference(point, value)`, `lend(alias)`, `read(expression)`, `diagnostics()`. Destroying it runs `onUnmount` and unmounts. |

`SessionOptions` carries the `Scheduler` (`debounce`, `refreshEvery`, cache eviction), the clock
`staleTime` is measured on, how long an unused entry is kept (five minutes), the `Catalog`
(`stateless` decides an alias's default instance; an action's `secretFields` are never cached or kept
in `result`) and the error classifier (the bridge's own failures map to `offline`, `timeout` and
`backendChanged`; everything else is `error`).

## Mounting

`mount` refuses, with a `Refusal`, what only the opener can get wrong: a param missing or of the wrong
type (`/params/<name>`), an alias `{"from": alias}` the opener did not lend (`MountOptions::borrowed`,
filled from the opener's `Screen::lend`), and a document without a view. It then declares the screen
scope, lowers the view, mounts it through `Mounted` at `MountOptions::depth + 1` (the screen scope's
own Effects at `depth`, so they run first), wraps the root in a column carrying the document's `keys`
when it has any, and runs `onMount`.

## Scopes

One `RtScope` per document scope: the screen, each dialog's or drawer's content, each switch case, each
row of a `forEach` or `table`, and a component fallback (a scope with no parent whose only name is
`props`). A scope is declared in three phases, so that every name exists before anything reads it:

1. its names: elements (every node with an `id`, and every input, dialog, drawer and table with or
   without one), the transfer ids of its `upload` commands, model aliases, `let`s (lazy `Computed`s),
   state slots, queries and mutations;
2. state inits, in declaration order; an init that reads state not yet made makes it first;
3. the elements' own reactive parts, checks, the queries' keys and watches.

A name resolves through the names bound at the place first (`event`, a step's `as`, `result`, `body`,
`props`, `boundary`), then the scope chain, innermost first.

- **State** is a `Signal<Value>`. `set` with a path writes into a record (`withPath`), creating records
  along it; `set` of typed state with a value its type does not admit is refused and reported.
- **`let`** is a `Computed<Value>` over `evaluate`.
- **A model alias** has an instance token: `private#<n>` (unique in the session) for `"private"` and for
  the default of a model the catalog does not mark stateless, `shared` for `"shared"` and the default of
  a stateless one, `shared:<key>` for an `instance` expression (`""` while the key is `null`: the alias
  is unbound and its queries idle), the opener's token for `{"from": alias}`. A channel is bound on
  first use and again when the token changes; the old one is released when nothing holds it.
- **Element values** (spec 5 §6) are read through the element's state: `draft` (what the user is
  editing), `committed`, `edited` (`dirty`), `touched`, `open`, `submitted`, a table's `selection` and
  `activeKey`, and `inner`, the dialog content's scope while it is mounted.

## Queries and the cache

A query's key is a `Computed` of what its cache entry is: idle when `when` is not `true`, the alias is
unbound or the body is `null`; otherwise `(model type, instance token, action, canonical body)`. The
canonical body is the body's JSON with record fields in name order.

- **Cache entries** (`Session`) hold the shown value, `hasValue`, the error text and kind, `pending`, a
  generation, `fetchedAt` and `stale`. A fetch takes a new generation; a reply of an older one is
  dropped. One entry serves every query with its key, so two screens on one key send one request.
- **Latest wins.** A key change moves the query to another entry. The previous key's value is shown
  until the new entry has a value; a failure for the new key clears it. A failed refetch of the same key
  keeps the value beside the error.
- **Freshness.** Moving onto an entry fetches unless it is in flight, or has a value younger than
  `staleTime` and is not stale.
- **`debounce`** waits that long after a key change before moving, with `pending` true meanwhile; the
  first key and a change to idle move at once.
- **`refreshEvery`** refetches on the scheduler's period and skips a tick while a request is in flight
  or a debounce waits.
- **`refreshOn`** is the session's bookkeeping: a mutation that succeeds refetches every mounted query
  of the same model type whose `refreshOn` names its action, in every screen of the session.
- **Invalidation** (`invalidates`, `refreshOn`, `refetch`, a backend switch) marks the entry stale and
  refetches it.
- **Eviction.** An entry no mounted query uses is dropped `keepUnused` after its last user left. Without
  a scheduler, entries are kept.
- **Secrets.** An action's `secretFields` are removed from what its entry keeps.

## Mutations

A run evaluates its body (the command's, in the command's scope, or the declaration's, in the
mutation's scope; `{}` without either) and is refused, queued or sent as `concurrency` says:
`exclusive` refuses a run while one is in flight; `serial` queues per `serialKey` (its canonical JSON)
and sends the next when the previous settles; `latest` sends every run and applies only the newest
reply; `parallel` applies each. `pending` counts runs in flight and queued.

- **Optimistic writes** are layers on the entry of the named query: the shown value is the confirmed
  value with every waiting layer over it, in run order. A success folds the run's layer into the
  confirmed value; a failure removes it before `onError` runs. Overlapping runs therefore end at the last
  confirmed value whatever order they settle in.
- **A success** (applied) sets `result`, clears the error, applies `patch` to the confirmed value,
  refetches `invalidates`, tells the session for `refreshOn`, runs `onSuccess` — all in one batch — and
  then removes the action's secret fields from `result`.
- **A failure** (applied) sets the error and its kind and runs `onError`; a `denied` kind calls
  `permissionsStale`.
- **`patch` and `optimistic`** replace the row of the query's list whose `id` field equals `key`; a
  record `with` is merged over a record row (its fields replace the row's), anything else replaces it.

## Relations

- **Inputs.** A text input, date-time input or slider whose `value` is bound (a checkbox's `checked`,
  a select's `selected`) is controlled: its draft follows the bound value while the user has not edited
  it, and keeps the user's edit until the bound value equals it. An input whose value is not bound keeps
  its own `committed` value. A text input's values are text.
- **`commitOn`** (`change` by default, `blur`, `enter`) decides when an edit commits: the committed
  value is written and `onChange` runs. `onCommit` (Enter or focus leaving after an edit) also marks the
  element `touched`. The view tree reports no focus, so `focused` stays false.
- **Errors.** An element's `errors` are, in order: `Required` when `required` is true and the value is
  not engaged (`null`, `""` or an empty list), the node's own `errors`, the field errors of a reply in
  its scope (a query's or mutation's value `errors.<id>`), and each failing check's message. `valid` is
  `errors` empty.
- **`shownErrors`** follows the `showErrors` of the innermost dialog, else the screen: `always`;
  `submit` once submitted; `touched` (the default) once touched or submitted.
- **`ready`** of a dialog or drawer holds when every visible element of its content is valid and no
  blocking check of it fails. A `run` from inside it marks it `submitted`, and is refused while it is
  not ready.
- **Checks** fail when `assert` is `false`; `null` does not fail them.
- **Watches** run their commands when `on` changes, never on the first evaluation, inside the flush
  that saw the change. `source: "user"` runs only when the newest write before it was made by a user
  event; commands a watch runs for a user's change count as the user's, so a cascade carries through.
- **Options.** A select's options are a list of scalars or of records with a key (`key`, `id` or
  `value`) and a `label`. `onStale: "clear"` clears a selection the new options no longer contain;
  `keep`, the default, keeps it.

## Commands

A command list runs each command in order in one batch; a `run` in it does not wait for the one before,
and the list is done when every command in it is. `seq` waits for each step and stops at a failure;
`all` starts every step and waits for all. A step fails when it is refused or its mutation fails, when
its target does not resolve, and when the host does not support it.

- `run` with `each` runs once per element, `as` naming it in the body; an `exclusive` mutation's runs
  go one after another, any other mode's together. Without `each`, `as` names the reply for the later
  steps of the enclosing `seq`.
- `if` with a condition that is not a `bool` (`null` included) runs neither branch.
- `open` and `close` set a dialog's own `open`; a dialog whose `open` is an expression is refused and
  reported. `reset` of a dialog restores its content's state and inputs and clears `submitted`.
- `navigate` runs `onResult` with the returned value as `event`, or `onCancel`.
- `switchBackend` to a listed backend calls `Session::backendChanged`.
- `setLocale` replaces the session's locale, so every `t` re-resolves, and tells the host.
- `signIn` clears the cache and hands the host the mutation's reply, secret fields included (it runs
  inside the mutation's `onSuccess`, before they are removed); `signOut` clears the cache.
- `upload` marks its transfer pending, follows the host's progress, and on success runs its mutation
  with the file reference added to the mutation's body under `as` (`file` by default). A body cannot
  name the reference itself, since a mutation's body is checked where the mutation is declared.
- `download` to `preview` writes the local reference to the state it names.
- `retry` refetches the failed queries of the innermost boundary.

## Lowering

Each node kind lowers to its `view.hpp` kind; every bound property is a numbered slot
(`ui::slot`), a literal a constant. Properties the view tree sets once (`gap`, a grid's `columns`, a
slider's range, a table's columns, `sizing`, `autofocus`) are evaluated once, untracked. `sizing`
applies to the width.

- **Scoped content.** A dialog's, drawer's or switch case's content is lowered behind a one-row
  `forEach` whose row view declares the content's scope, so the scope is made when the content mounts
  and released when it unmounts. Row scopes are made by the row view of a `forEach` or `table`.
- **`switch`** selects the first case whose `when` equals `on` (structurally), else `default`.
- **`table`** from `rows`: the columns' labels and widths are set once; a column without a `cell`
  shows the row's field of the column's `id`; `columns` given as an expression is a list of
  `{id, label}`. `onActivate` runs in the activated row's scope. `rowTone`, `sort`, `filters` and
  `hoveredKey` are not lowered.
- **`boundary`** is a `Switch` over loading, error and children. It owns the queries its children read
  (found statically, through `let`s, not inside a nested boundary): loading while one has neither a
  value nor a failure, error when one failed with no value to keep, children otherwise. Inside `error`,
  `boundary.error` is the first failure's text.
- **`customize`** lowers a `choice` point to a select and a string `value` point to a text input, each
  writing the screen's preference and calling `preferenceChanged`; other points show the point's name.
- **`custom`** lowers its component's fallback with `props` bound, inside `ui::custom`.
- **`form`** is the forms engine's (`docs/spec/forms/engine.md`). Its element is declared like an
  input's, with or without an `id`, and builds a `forms::FormSession` when the scope is declared, so
  its values can be read before it is drawn: the schema comes from `Catalog::schema(action)`; the
  `fields` overlay is evaluated once and applied before the model is read; `submit` (`explicit`,
  `auto`) chooses over `x-submitMode`; every submission and options fetch goes through the `model`
  alias's channel, bound for its instance when the call is made (a Choice whose options action belongs to
  another model therefore fails its fetch); `prefill` is followed, each non-null
  value loading the form as a programmatic change. It lowers to `forms::formView` inside a column that
  carries the node's common properties. `onSubmit`, `onSuccess` and `onError` run once per attempt,
  success and failure, with the body, the reply and the error text as `event`. Its element values
  are `ready`, `body` (the record it would submit, `null` while not ready), `submitted`, `pending`
  and each field by wire name, a record of `value`, `draft`, `valid`, `required`, `visible`,
  `readonly` and `errors`. An action the catalog has no schema for, or a schema that does not build,
  is reported and shown as a muted text naming the action.

## Diagnostics

A runtime expression error, a command the host does not support, a target that does not resolve and a
node kind the interpreter does not lower are reported once per path and reason: kept in
`Screen::diagnostics()` and passed to `InterpreterOptions::diagnostics`.

## Lifetimes

The runtime, the session, the dispatcher and the backend outlive every screen. A scope is held by the
view nodes lowered in it (their bindings hold it by `shared_ptr`; content with no binding is wrapped in
a column whose `visible` binding holds it). A reply, a timer and every callback the host keeps hold it
weakly, so nothing outside the screen keeps a scope past the screen; a sequence waiting for a reply
stops when its scope is gone.

## Design decisions

| Decision | Why |
|---|---|
| Queries and mutations built on `Signal`, `Computed`, `Effect` and `Scheduler`, not on `reactive::Query` and `Mutation` | `patch` and `optimistic` write into a query's value and two screens share one value; `reactive::Query` keeps its value privately. The rules are the same. |
| The cache entry is the single source of a query's value | One request in flight, one value and one error per key, across screens, is what spec 5 §5 asks for, and `patch` then reaches every screen showing the row. |
| Optimistic writes as layers | Rolling back by restoring a saved value is wrong when runs overlap; recomputing from the confirmed value and the remaining layers is right in every order. |
| `source: "user"` from the newest write | A watch cannot see who wrote a value; the session can see whether the write that preceded the flush came from a user event. |
| Scoped content behind a one-row `forEach` | The view tree builds a dialog's content once, as a fixed node; a row view is the one place it builds content each time it mounts. |
| Host commands behind one interface, each default "not supported" | The interpreter runs in local mode, in tests and in a generic client; each supplies what it can. |

## Not yet lowered

Reported when met, and shown as a muted text naming the kind:

- `collection` and `wizard`: the forms engine's composite screens (spec 2 §8).
- A `form` node's `overrides` (spec 2 §7): document subtrees or components replacing a field's
  control. C++ code replaces controls through `forms::Overrides`.
- A `form` node's `showErrors`: a field's error shows on its control as soon as its draft does not
  encode.
- A `table` declared from a model's list action: the table engine (spec 7).
- Component loading: every `custom` node renders its fallback, as `Mounted` does.
- Prefetching on hover: the host starts a screen's `prefetch` queries through `Session::prefetch`.
- The offline queue itself: a dispatcher that has one honours `CallOptions::queueOffline`.

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
- [Development tools](#development-tools)
- [Lifetimes](#lifetimes)
- [Design decisions](#design-decisions)
- [Not yet lowered](#not-yet-lowered)

## The seams

| Type | Role |
|---|---|
| `Dispatcher` | Binds a model by type id, privately or to a shared instance (`bind`), and returns a `ModelChannel` whose `execute(actionId, bodyJson, CallOptions)` sends one action and resolves with the reply's JSON text on the runtime's owner. |
| `BridgeDispatcher` | The application's `Dispatcher`: `bind` makes a `bridge::RawHandler` (`bindByType`, and `attach` for a key), `execute` is `executeRaw`. An `evaluate` query goes through the `Evaluator` it was built with (`ui::evaluatorOver`, the UI service's `ui-evaluate`); without one it is refused with a `std::runtime_error`. It has no offline queue of its own: `queueOffline` is ignored unless a `QueueingDispatcher` wraps it. A shared binding without a key is left unattached, which the bridge refuses at the first call. |
| `QueueingDispatcher` | The client's offline queue (`include/morph/ui/queueing_dispatcher.hpp`), around any `Dispatcher`. A call made with `queueOffline` that fails with `backend::DisconnectedError` is held, and the caller's completion stays pending, so the mutation stays `pending` and its elements `stale`. While anything is held, a later queued call joins the queue unsent, so the server sees calls in the order they were made. `replay()` re-sends the held calls oldest first, one at a time, and settles each caller with its replay: a refusal runs the mutation's `onError`, and a call still unreachable stops the replay and stays held with every call behind it. A call without `queueOffline`, and an evaluation, are never held. `held()` and `setOnHeld` report the count. When to replay is the host's: it calls `replay()` once its transport reconnects. |
| `InterpreterHost` | What only the application can do: `navigate`, `closeSelf` (`return`, `close: self`), `hostCommand`, `switchBackend` (asynchronous: it answers whether the backend is listed and calls `done` once the switch committed or failed), `resetPrefs`, `setLocale`, `notify`, `signIn`, `signOut`, `setAppValue` (`set` of `app.name`), `uploadFor` (an `UploadRequest`: the file, and the model type and action of the mutation that will reference it; its default calls `upload(file, …)`), `download`, `preferenceChanged`, `permissionsStale`. Every default answers "not supported", and the interpreter reports it. A session with host commands (`Session::setHostCommands`, filled by `HostVocabulary`, [`host_vocabulary.md`](host_vocabulary.md)) runs `{"host": …}` itself and answers `{"can": "host:…"}` from them. |
| `Session` | What screens of one client session share: the query cache, `refreshOn` bookkeeping, the permitted action ids `can` reads (`setPermitted`), the formatting facts (`setFormatting`, `setLocale`), and the `env`, `session` and `host` values. `clear()` is `signOut` and a principal change; `backendChanged(backend)` carries out a committed backend switch. `appScope()` is the mounted app document's scope. |
| `Interpreter` | `mount(doc, params, backend, MountOptions)` → `Screen`. |
| `Screen` | A mounted screen: `root()`, `title()`, `identity()`, `dirty()`, `requestClose()` (runs `onClose`; false when it ran `cancelClose`), `beginClose()` (runs `onClose` and returns the `cancelClose` flag it shares with what it starts; null without `onClose`), `pending()` (a tracked read: an open dialog or drawer, a mutation run, or an upload or download in flight), `declares(key)`, `runtime()`, `runLifecycle(key)`, `setPreference(point, value)`, `document()`, `lend(alias)`, `read(expression)`, `diagnostics()`. Destroying it runs `onUnmount` and unmounts. A host mounts and closes screens through [host.md](host.md). |

`SessionOptions` carries the `Scheduler` (`debounce`, `refreshEvery`, cache eviction), the clock
`staleTime` is measured on, how long an unused entry is kept (five minutes), the `Catalog`
(`stateless` decides an alias's default instance; an action's `secretFields` are never cached or kept
in `result`) and the error classifier (the bridge's own failures map to `offline`, `timeout` and
`backendChanged`; everything else is `error`) and the `watcher`, which sends the cache's `watch` and `unwatch` requests
(`ui::watcherOver` builds one over a remote source's exchange; empty for a local source).


## The app root scope

`Interpreter::mountApp(doc)` mounts the app document (id `app`) as an `AppRoot`: its state, lets,
queries and mutations, declared once for the session, and its `onMount`. A second root on the same
session, and a document whose id is not `app`, are refused. While it lives, `Session::appScope()`
names its scope: `{"ref": "app.name"}` in any screen reads the name there, reactively, so a query of
the app document runs once however many screens read it, and `{"set": "app.name"}` writes the root's
state (a name that is not state is reported). Destroying the root runs its `onUnmount` and takes its
scope off the session; the screens that read it are destroyed first, as sign-out does (spec 6 §5).

## Mounting

`mount` refuses, with a `Refusal`, what only the opener can get wrong: a param missing or of the wrong
type (`/params/<name>`), an alias `{"from": alias}` the opener did not lend (`MountOptions::borrowed`,
filled from the opener's `Screen::lend`), and a document without a view. It then declares the screen
scope, lowers the view, mounts it through `Mounted` at `MountOptions::depth + 1` (the screen scope's
own Effects at `depth`, so they run first), wraps the root in a column carrying the document's `keys`
when it has any, and runs `onMount`.

A mount that replaces a screen, such as a development reload's remount, names it in
`MountOptions::keepStateOf`. Each state of the new screen scope then starts from the old screen scope's
value of the state of the same name, when the two declared types have the same name (`int` and `int?`
differ; untyped is `any`) and the new type admits the value; any other state is initialised from its init.
Only the screen scope is matched: nested scopes are made later, when their content mounts, and start
from their inits.

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
  first use and again when the token changes; the old one is released when nothing holds it. A `shared`
  alias binds a private instance: it is a stateless model's (`loadDocument` refuses `"shared"` on any other),
  whose instances are interchangeable, and what it shares with other screens is the query cache.
- **Element values** (spec 5 §6) are read through the element's state: `draft` (what the user is
  editing), `committed`, `edited` (`dirty`), `touched`, `open`, `submitted`, a table's `selection` and
  `activeKey`, and `inner`, the dialog content's scope while it is mounted.

## Queries and the cache

A query's key is a `Computed` of what its cache entry is: idle when `when` is not `true`, the alias is
unbound or the body is `null`; otherwise `(model type, instance token, action, canonical body)`. The
canonical body is the body's JSON with record fields in name order.

- **Cache entries** (`Session`) hold the shown value, `hasValue`, the error text and kind, `pending`, a
  generation, `fetchedAt`, `stale` and `refetchAfter`. A fetch takes a new generation; a reply of an older one is
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
- **`refreshOn`** is the session's bookkeeping: a mutation that succeeds, or a submit of a form, a
  collection or a wizard that succeeds, refetches every mounted query of the same model type whose
  `refreshOn` names its action, in every screen of the session. A collection's own list query and a
  form's choice options are reads, not submits, and refetch nothing.
- **Every reply applies its action's effects** (spec 8 §9). `Session::actionSucceeded`, which every
  success goes through (mutations, and the submits of forms, collections and wizards), takes the reply
  and the instance token of the alias the call went through. Each entry knows the records its action
  returns, with each record's key field and paths, from the catalog's `ActionInfo::records`; an
  `evaluate` entry holds none.
    - `updates R` merges the reply's rows of `R` into every entry holding `R`. Rows are matched by
      the record's key field, at each path the catalog gives (`header.rows` follows members; a list
      on the way is looked through). There is no fetch. An entry whose fetch is in flight is also
      marked, because that fetch may have read before the write and would settle over the patch.
    - `changes R` marks every entry holding `R` stale, and `changesInstance` every entry read through
      the same instance token. A marked entry refetches through a mounted query that uses it.
    - A `latest` run whose reply is discarded applies its effects as marks only: its `updates` act as
      `changes`, because its values are older than the reply already applied. It runs no `refreshOn`.
    - One reply's effects are applied in one reactive batch.
- **Server events** reach the cache through `Session::deliverEvent`, which the source wires to its
  backend's `setEventHandler`. Each event is applied in one reactive batch, and other clients' changes
  only mark, so each receiver refetches through its own authorised read.
    - A `changes` event's record change with keys marks each entry holding the record whose value has
      a row with one of the keys (by the key field, at the record's paths). An entry still waiting
      for its first value is marked too, because it may come to hold the key. A record change without
      keys, or on a keyless record, marks every entry holding the record.
    - An instance change marks every entry read through that instance: for a primary key, the keyed
      shared alias with that key (`shared:<key>`); without one, the connection's private instances of
      that model type.
    - A `resync` marks every entry but an `evaluate` one. Another topic, or a payload that cannot be
      read, is ignored.
    - A marked entry that no query uses is only marked, and fetches when a query mounts on it.
- **Watching what the cache holds** (spec 8 §7). The session counts, per topic, the entries that hold
  it: a record name for every record an entry's action returns, and `(model type, primary key)` for an
  entry read through a keyed shared alias whose action returns no record. A private instance needs no
  watch, because the server knows its owner. A count that leaves zero queues a `watch`, and one that
  returns to zero (the entry evicted, or the cache cleared) queues an `unwatch`.
    - **Watch before fetch.** `fetch` sends the queued changes first, on the same connection, so the
      server holds the watch before the read. A change committed before the read is in its value, and
      one committed after arrives as an event, which a mark during the fetch keeps.
    - The changes queued before one fetch go out as at most one `watch` and one `unwatch`. A screen
      whose queries hold different records sends one small `watch` per fetch.
    - Without a `watcher` the session still counts, so `Session::watchSet()` answers, and sends nothing.
    - **`Session::reconnected()`**, and every `resync`, sends the whole set with `replace: true`, which
      supersedes anything queued, then marks every entry but an `evaluate` one stale. A mounted entry
      refetches, and an entry whose fetch is in flight fetches once more when it settles: that fetch
      may have been filed before the connection came back.
- **Invalidation** (`invalidates`, `refreshOn`, `refetch`, a backend switch) marks the entry stale and
  refetches it.
- **A mark during a fetch is kept.** `Session::markStale` marks an entry stale. While the entry's fetch is
  in flight, it also sets `refetchAfter`. That fetch may have read before the change, so when it settles
  the entry keeps its new value, stays stale, and fetches once more through a mounted query that uses it.
  A success never clears a mark that arrived during the fetch. An entry no query uses when its fetch
  settles stays stale, and fetches when a query mounts on it.
- **Eviction.** An entry no mounted query uses is dropped `keepUnused` after its last user left. Without
  a scheduler, entries are kept.
- **Secrets.** An action's `secretFields` are removed from what its entry keeps.

## Mutations

A run evaluates its body (the command's, in the command's scope, or the declaration's, in the
mutation's scope; `{}` without either) and is refused, queued or sent as `concurrency` says:
`exclusive` refuses a run while one is in flight; `serial` queues per `serialKey` (its canonical JSON;
the key is evaluated with the run's body bound as `body`) and sends the next when the previous settles; `latest` sends every run and applies only the newest
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
  The list is the query's value itself, or, with `at`, the member of that record value `at` names; the
  rest of the record is kept. A key no row has changes nothing and is not a fault: the row is not
  loaded. A patch whose query has a value but finds no list there — the value is not a list and there
  is no `at`, or the value has no list at `at` — changes nothing and is reported once through
  `diagnostics()` at the mutation's `/patch` or `/optimistic` path, since it can never apply. An
  `optimistic` write is checked when the run starts, a `patch` when its reply arrives; before the query
  has a value there is nothing to miss.

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
- `focus` moves keyboard focus to the element of that id in the screen's mounted tree (`Mounted::focus`); an
  element that is not mounted is reported and fails the command.
- `switchBackend` asks the host; a backend it does not list is reported. When the switch commits,
  `Session::backendChanged` runs, in one batch, every mounted screen's and the app root's `onBackendChange`, then
  marks every entry stale and refetches the mounted queries, and sets `{"env": "backend"}`; the command then
  succeeds. A failed switch is reported as `backendSwitchFailed: reason`, fails the command and refetches nothing.
  A switch that commits after its screen is gone touches nothing of it.
- `set` of `app.name` writes the app root's state (below); with no app root it goes to the host's
  `setAppValue`.
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
- **A field's `label`.** A text input, select, slider, date-time input or file picker shows no caption of
  its own, so its `label` is bound as its accessible name (`Common::a11y.name`). An `a11y` part that
  gives a `name` wins over it; one that gives only a `role` leaves the label as the name. A checkbox's
  and a button's label is their text, which already names them.
- **`table`** from `rows`: the columns' labels and widths are set once; a column without a `cell`
  shows the row's field of the column's `id`; `columns` given as an expression is a list of
  `{id, label}`. `onActivate` runs in the activated row's scope. `rowTone`, `sort`, `filters` and
  `hoveredKey` are not lowered.
- **`boundary`** is a `Switch` over loading, error and children. It owns the queries its children read
  (found statically, through `let`s, not inside a nested boundary): loading while one has neither a
  value nor a failure, error when one failed with no value to keep, children otherwise. Inside `error`,
  `boundary.error` is the first failure's text.
- **`customize`** lowers a `choice` point to a select, a string `value` point to a text input, and an `arrange`
  point to one row per child of its container, hidden ones included: a checkbox shows or hides the child and two
  buttons move it a place, disabled at the ends. Each writes the screen's preference and calls
  `preferenceChanged`; an `arrange` edit stores the whole arrangement, every child named. Other points show the
  point's name.
  Storing the choices, and loading them back on mount, is `PreferenceSync`'s
  ([`preferences.md`](preferences.md)).
- **An arranged `column` or `row`**, one an `arrange` point names, lowers to a keyed `ForEach` over its children's
  ids instead of a stack, so a new choice moves the shown widgets, keeping their state and focus. The choice is a
  list of `{"id": child, "hidden": bool}`: the named children come first in its order, a hidden one is not mounted,
  and a child it does not name keeps its declared place after them. An entry naming no child, naming one twice, or
  not of that shape is skipped, and a choice that is not a list leaves the declared order.
- **`custom`** lowers to `ui::custom` with every prop its component declares, a missing one as its type's empty
  value, and the component's host `version` as `minVersion`. `string` crosses as its formatted text, `int` and `exact` as
  plain decimal text (an exact value with its declared places, a quantity with its unit), `json` as its JSON text,
  `bool` as a bool and `number` as a double. An event runs the node's `on.<event>` commands with its payload read as
  the declared type: `i:<int>`, `s:<text>`, or JSON text. The fallback is lowered too, with `props` bound, for a
  backend that loads nothing.
- **`form`** is the forms engine's (`docs/spec/forms/engine.md`). Its element is declared like an
  input's, with or without an `id`, and builds a `forms::FormSession` when the scope is declared, so
  its values can be read before it is drawn: the schema comes from `Catalog::schema(action)`; the
  `fields` overlay is evaluated once and applied before the model is read; `submit` (`explicit`,
  `auto`) chooses over `x-submitMode`; every submission and options fetch goes through the `model`
  alias's channel, bound for its instance when the call is made (a Choice whose options action belongs to
  another model therefore fails its fetch); `prefill` is followed, each non-null
  value loading the form as a programmatic change: the first value, and a value whose `key` equals the
  previous one's, `refill` the form, so a field the user changed keeps what they entered; a new `key`,
  or any later value of a form without a `key`, `prefill`s every field. It lowers to `forms::formView` inside a column that
  carries the node's common properties. `onSubmit`, `onSuccess` and `onError` run once per attempt,
  success and failure, with the body, the reply and the error text as `event`. Its element values
  are `ready`, `body` (the record it would submit, `null` while not ready), `submitted`, `pending`
  and each field by wire name, a record of `value`, `draft`, `valid`, `required`, `visible`,
  `readonly` and `errors`. An action the catalog has no schema for, or a schema that does not build,
  is reported and shown as a muted text naming the action.
- **`collection`** and **`wizard`** are the forms engine's composite screens. Each element is
  declared like a form's and builds its session when the scope is declared: a
  `forms::CollectionSession` from `Catalog::view(action)`, whose `v-query` must be the node's
  `action`, or a `forms::WizardSession` from `Catalog::wizard(action)`, whose first step must be it
  (the action is the wizard's id, the stem of its translation keys). The row editor's and the steps'
  schemas come from `Catalog::schema`. Every call (the list query, the row and collection actions,
  the editor's and every step's submissions) goes through the `model` alias's channel, as a form's
  does. They lower to `forms::collectionView` and `forms::wizardView`, with the forms' own titles
  left out, inside a column that carries the node's common properties. They have no element values
  and no events. A document the catalog lacks, one for another action, one that does not build, or
  a wizard step without a schema is reported and shown as a muted text naming the action.

## Development tools

In development mode (`SessionOptions::developmentTools` is `On`), a screen also records what the development inspector reads
([`inspector.md`](inspector.md)): each node as it is lowered, with its scope and bindings held weakly; each live
scope; and each command list `runList` runs, with the batch it ran in. `Screen` befriends `Inspector`, which reads
them and the scopes' queries and mutations, and returns copies. A screen whose session is `Off` keeps these members
empty and records nothing into them.

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

- The chrome of a `collection` or `wizard` (its header, rows, confirm, editor and navigation drawn
  by the host's own kit): C++ code replaces them through `forms::CollectionChrome` and
  `forms::WizardChrome`.
- A `form` node's `overrides` (spec 2 §7): document subtrees or components replacing a field's
  control. C++ code replaces controls through `forms::Overrides`.
- A `form` node's `showErrors`: a field's error shows on its control as soon as its draft does not
  encode.
- A `table` declared from a model's list action: the table engine (spec 7).
- Prefetching on hover: the host starts a screen's `prefetch` queries through `Session::prefetch`.

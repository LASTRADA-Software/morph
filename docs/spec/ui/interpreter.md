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
| `InterpreterHost` | What only the application can do: `navigate`, `closeSelf` (`return`, `close: self`), `hostCommand`, `switchBackend` (asynchronous: it answers whether the backend is listed and calls `done` once the switch committed or failed), `resetPrefs`, `setLocale`, `notify`, `signIn`, `signOut`, `setAppValue` (`set` of `app.name`), `uploadFor` (an `UploadRequest`: the file, and the model type and action of the mutation that will reference it; its default calls `upload(file, …)`), `download`, `readFile(file, maxBytes, done)` and `saveFile(InlineFile{name, mediaType, bytes}, option, done)` (a file's bytes moved inline; a host reads a file's size before its bytes and refuses one over `maxBytes`), `preferenceChanged`, `permissionsStale`. Every default answers "not supported", and the interpreter reports it — `notify`'s own default is still this safe no-op. `client::ApplicationHost` (the generic client's host, [`client/client.md`](../client/client.md)) has a working `notify`, wired through its `ShellFrame`'s toast; an application building its own host without `ApplicationHost` still gets the safe default unless it wires its own sink the same way. A session with host commands (`Session::setHostCommands`, filled by `HostVocabulary`, [`host_vocabulary.md`](host_vocabulary.md)) runs `{"host": …}` itself and answers `{"can": "host:…"}` from them. |
| `Session` | What screens of one client session share: the query cache, the mounted queries, the permitted action ids `can` reads (`setPermitted`), the formatting facts (`setFormatting`, `setLocale`), and the `env`, `session` and `host` values: `setEnvironment` replaces the `env` record, and `setEnvironmentValue(name, value)` sets one value and keeps the others, as a committed backend switch keeps the width class. `clear()` is `signOut` and a principal change; `backendChanged(backend)` carries out a committed backend switch. `appScope()` is the mounted app document's scope. |
| `Interpreter` | `mount(doc, params, backend, MountOptions)` → `Screen`. |
| `Screen` | A mounted screen: `root()`, `title()`, `identity()`, `dirty()`, `requestClose()` (runs `onClose`; false when it ran `cancelClose`), `beginClose()` (runs `onClose` and returns the `cancelClose` flag it shares with what it starts; null without `onClose`), `pending()` (a tracked read: an open dialog or drawer, a mutation run, or an upload or download in flight), `declares(key)`, `runtime()`, `runLifecycle(key)`, `setPreference(point, value)`, `document()`, `lend(alias)`, `read(expression)`, `diagnostics()`. Destroying it runs `onUnmount` and unmounts. A host mounts and closes screens through [host.md](host.md). |

`SessionOptions` carries the `Scheduler` (`debounce`, `refreshEvery`, cache eviction), the clock
`staleTime` is measured on, how long an unused entry is kept (five minutes), the `Catalog`
(`stateless` decides an alias's default instance; an action's `secretFields` are never cached or kept
in `result`) and the error classifier (the bridge's own failures map to `offline`, `timeout` and
`backendChanged`; everything else is `error`) and the `watcher`, which sends the cache's `watch` and `unwatch` requests
(`ui::watcherOver` builds one over a remote source's exchange; empty for a local source), and `maxInlineFileBytes`,
the most bytes a document moves inline in one file (default `kDefaultInlineFileBytes`, 1 MiB). `Session` construction
throws `std::invalid_argument` for 0 and for a cap above `kMaxInlineFileBytes`, ⌊(`wire::kMaxEnvelopeBytes` −
64 KiB) · 3 / 4⌋ = 6 242 304 bytes: base64 grows bytes by 4/3, and 64 KiB are left for the rest of the envelope.


## The app root scope

`Interpreter::mountApp(doc)` mounts the app document (id `app`) as an `AppRoot`: its state, lets,
queries and mutations, declared once for the session, and its `onMount`. Its `formatting` patterns become the
session's (`Session::setDatePatterns`), so every screen of the bundle writes and reads dates in them; a C++ host may
set the same `FormatContext` fields directly. A `dateTimeInput` reads the pattern of its mode once, untracked, when
it is lowered, as it reads the zone: a pattern changed later applies to inputs mounted later. A second root on the same
session, and a document whose id is not `app`, are refused. While it lives, `Session::appScope()`
names its scope: `{"ref": "app.name"}` in any screen reads the name there, reactively, so a query of
the app document runs once however many screens read it, and `{"set": "app.name"}` writes the root's
state (a name that is not state is reported). Destroying the root runs its `onUnmount` and takes its
scope off the session; the screens that read it are destroyed first, as sign-out does (spec 6 §5).

`AppRoot::navigationBadges()` is a `reactive::Computed` of the document's `navigation` badges that are visible, in
order, each a `NavigationBadge{screen, text, tone}`: the parts are lowered once in the app scope when the root
mounts, so a count follows the state it reads. It is empty when the document declares none, and goes with the root,
before its scope. A shell copies it onto its navigator ([client.md](../client/client.md#the-shell)).

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
- **Every reply applies its action's effects** (spec 8 §9). `Session::actionSucceeded`, which every
  success goes through (mutations, and the submits of forms, collections and wizards; a collection's own list
  query and a form's choice options are reads, not submits), takes the reply
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
      `changes`, because its values are older than the reply already applied.
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
- **Invalidation** (`refetch`, a backend switch) marks the entry stale and
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
- **A success** (applied) sets `result`, clears the error, applies the action's effects through the
  session, runs `onSuccess` — all in one batch — and then removes the action's secret fields from `result`.
- **A failure** (applied) sets the error and its kind and runs `onError`; a `denied` kind calls
  `permissionsStale`.
- **`optimistic`** replaces the row of the query's list whose `id` field equals `key`; a
  record `with` is merged over a record row (its fields replace the row's), anything else replaces it.
  The list is the query's value itself, or, with `at`, the member of that record value `at` names; the
  rest of the record is kept. A key no row has changes nothing and is not a fault: the row is not
  loaded. A write whose query has a value but finds no list there — the value is not a list and there
  is no `at`, or the value has no list at `at` — changes nothing and is reported once through
  `diagnostics()` at the mutation's `/optimistic` path, since it can never apply. It is checked when the
  run starts; before the query has a value there is nothing to miss.

## Relations

- **Tabs.** The tab ids map to indices once. A bound `selected` is the controlled index: a pick runs `onChoose`
  with the tab's id as a user event and leaves the highlight on the tab `selected` names until it changes. A value
  that names no tab shows the first and is reported once at the node's `selected` path. Without `selected` the node
  keeps the pick itself, which is its element value `selected`.
- **Inputs.** A text input, date-time input or slider whose `value` is bound (a checkbox's `checked`,
  a select's `selected`) is controlled: its draft follows the bound value while the user has not edited
  it, and keeps the user's edit until the bound value equals it. An input whose value is not bound keeps
  its own `committed` value. A text input's values are text.
- **Tables.** A table's `selection` is controlled as a select's `selected` is: any expression but a literal
  `null` binds it, and the table then shows the bound keys and nothing else. A user's change runs
  `onSelectionChange` with `event` the new keys, and shows only once the document writes them back.
- **Date inputs.** A date input holds its value as a `timestamp` or as a string. The loader records the
  static type of `value` when it is a `timestamp` or a `string`; a value whose type is `any` or `key`
  takes the type it holds at the edit, and an uncontrolled input holds a timestamp. A string field
  shows a `yyyy-MM-dd` date as midnight in the display zone and an ISO-8601 date-time as its instant
  (it reads the value with `readDateValue`, as the date functions do),
  and writes `yyyy-MM-dd` in `date` mode or the ISO-8601 instant in `date-time` mode (`""` on
  clearing); a timestamp field writes the timestamp (`null` on clearing). A value that names no date
  shows empty and is reported once at the node's `value` path. A date input follows its bound value by
  instant: the draft and the bound value show the same when both read to the same instant, and two
  values that read to nothing are the same.
- **`commitOn`** (`change` by default, `blur`, `enter`) decides when an edit commits: the committed
  value is written and `onChange` runs. A text input is given a submit handler only when it has `onSubmit` or
  commits on `enter`; any other one lets Enter go on to its ancestors' chords once it has committed. `onCommit` (Enter or focus leaving after an edit) also marks the
  element `touched`. A date input's change is its commit: `onChange` runs, then `onCommit` with the same
  value. A slider reports `onChange` on each step and `onCommit` on a finished move (a release after a
  drag, a key step, a wheel step). Both commits mark the element `touched`. A check box's toggle and a select's
  choice are their commits, and mark them `touched` too. A `group` has no value to touch: it counts as touched once
  any element inside it is. The view tree reports no focus, so `focused` stays false.
- **Errors.** An element's `errors` are, in order: `Required` when `required` is true and the value is
  not engaged (`null`, `""` or an empty list; never for a `group`, whose `required` is its legend's mark only),
  the node's own `errors`, the field errors of a reply in
  its scope (a query's or mutation's value `errors.<id>`), and each failing check's message. `valid` is
  `errors` empty. A field's `tone` is not an error: it changes the field's look and nothing of `valid` or `ready`.
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
- `close: self` is resolved by scope. The command's scope and its parents are walked to the first one a dialog or
  drawer owns (its content, `default`, `cancel` and actions all run there), and that overlay is closed as
  `close: <id>` closes it. A walk that reaches the screen's root scope asks the host's `closeSelf`, so a screen
  opened as a dialog still ends itself. A component's fallback scope has no parent, so a walk that ends there, with
  no dialog or drawer of the component's own around the command, is reported ("`close: self` inside a component
  names no dialog or drawer") and fails the command: it never closes the screen the component sits in. A
  mutation's `onSuccess` runs in the scope that declares the mutation, so the same lexical rule applies to it.
- `navigate` runs `onResult` with the returned value as `event`, or `onCancel`. A `navigate` with neither asks
  the host with no handlers, so it files no call ([host.md](host.md#documentscreenhost)): the screen it opens
  answers nobody.
- `focus` moves keyboard focus to the declaration its target resolved to at load (`Document::focusTargets`), in the
  screen's mounted tree (`Mounted::focus`). The lowering gives every element the focus key of its declaration: the
  node's JSON Pointer, after the key of the `custom` node whose fallback lowered it, followed by `@<canonical row
  key>` for each row scope it sits in, outermost first. A key is unique per declaration, row and component use, and
  the same on every re-render. The command keys each candidate with its own enclosing row keys, as many as enclose
  the candidate, the innermost replaced by `row`'s value when it gives one, and focuses the candidate that is
  mounted. None mounted, or a `row` of `null`, is reported as `` `x` is not mounted `` and fails the command.
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
- **Inline bytes.** `upload` with `"into": "bytes"` asks the host's `readFile` with the session's cap, refuses a
  reply over the cap even when the host did not, and runs the mutation with `as` (default `file`) set to the bytes
  in base64, and `nameAs` and `typeAs`, when given, to the file's name and media type; its transfer's `pending`,
  `progress` (1 once the bytes are read) and `error` behave as for a reference. `download` with `"from": "bytes"`
  evaluates its file to base64 text (with or without padding) or a list of integers 0 to 255, decodes it, and calls
  `saveFile` with its `name` and `type`; content over the cap, or that does not decode, fails the command with a
  diagnostic (`download: 2.4 MiB is over the 1 MiB inline cap; serve it through the file store`) and the host is
  not called. A field component writing a member whose schema says `contentEncoding: base64`
  (`FieldSpec::bytes`) is held to the cap too (`field <name>: <n> bytes is over the <cap> inline cap`); any other
  field is not.
- `retry` refetches the failed queries of the innermost boundary.

## Lowering

Each node kind lowers to its `view.hpp` kind; every bound property is a numbered slot
(`ui::slot`), a literal a constant. Properties the view tree sets once (`gap`, a grid's `columns`, a
slider's range, a table's columns, `sizing`, `autofocus`) are evaluated once, untracked. `sizing`
sizes the width, or both dimensions from a `{width, height}` record (one left out is content); a computed
sizing that evaluates to none of its forms is reported once for its path and lowers to content. The size bounds,
literal integers the loader checked, become `LayoutHints::minWidth`, `maxWidth`, `minHeight` and `maxHeight`. `align`,
`justify` and `alignSelf`, names the loader checked, become the stack's or grid's `align` and `justify` and
`LayoutHints::alignSelf`.

**Holders.** The interpreter mounts some content through a holder: a `switch` case, an overlay's body (a one-row
`forEach`, so that the content's scope lives only while it is mounted), several nodes written where one is expected
(a `default`, a panel's, a scroll's or a collapsible's content, a table cell, an `empty` state, a group heading, an
error boundary's fallback) and a `forEach` row's content (a column). A holder's sizing on each axis is a stretch, by
the largest weight, when one of its children's is, and content otherwise; a node's bounds and `alignSelf` stay with
the node. The holder a slot sees is the one-row `forEach`, because a slot reads only its direct child's sizing, so
that `forEach` carries the hints too; they are worked out from the document's children before the content mounts. A
literal sizing is read as written and a computed one is evaluated once in the enclosing scope. A computed sizing that
reads a name the case or overlay itself declares cannot be evaluated before the content mounts: it is reported at its
path and the holder stays content-sized on that axis. A tab page is mounted into a slot that fills the tabs node's page area, is scrollable and pads its own content (`Mounter::mountPage`): a screen needs no `scroll` of its own, and a stretching child fills the page but is never shorter than its `minHeight`. A single node of a list is lowered unwrapped. The column that wraps several nodes is a holder (`Column::holder`): a child of it that bleeds reaches past the padding of the container the document names (a panel's, a tab page's), not past the column's own, which has none; a column the document wrote is not a holder and gives its children nothing to bleed past. A document panel or collapsible puts `gap` (default 2) between several
children and a dialog or drawer (default 3), the holder's column carrying it; a panel built in the view tree holds one child and
its author picks that child's gap.

A `group` lowers to a `Column` (gap `gap`) of existing kinds, so the view tree and the contract do not change: a
`Text` of its `label` whose `field.required` is its `required` (so the mark follows the legend's last line), its
children, a muted `Text` of its `description` shown while it is not empty, and an `Error` `Text` of its
`shownErrors`, one per line, shown while there are any. The column's `Common` is the node's own, with `a11y.role`
`group` and `a11y.name` the label unless the node's `a11y` names it. The description is read in order after the
children, not as an accessible description: `Common` carries a name and a role only.

Wherever the lowering turns a decimal into a number for the view tree, it reads the exact value: a `progress`'s
`value` and a custom component's `number` prop take the quotient unrounded, and an integer property (a grid's
`columns`, `gap` and `span`, `sizing`, a slider's value and range, the steps' `current`, a table's virtualization
bounds) truncates it toward zero. A decimal's places are a display fact, so only display rounds to them: `29 / 100`
keeps its operands' 0 places and still fills 0.29 of a bar.

- **Scoped content.** A dialog's, drawer's or switch case's content is lowered behind a one-row
  `forEach` whose row view declares the content's scope, so the scope is made when the content mounts
  and released when it unmounts. Row scopes are made by the row view of a `forEach` or `table`.
- **`switch`** selects the first case whose `when` equals `on` (structurally), else `default`.
- **`splitter`** binds `sizes`, each item read as an integer property is. A value that is not a list of numbers is
  reported once for its path and lowers to an empty list, which shares the space equally. The document has no
  `onResize`, so a user's drag stays the splitter's own.
- **Action parts.** `ScreenCore::lowerActionParts(parts, ctx)` lowers a list of `actionButton` parts into
  `ActionModels`: the label, icon, `enabled` and `visible` bound in `ctx`'s scope, the emphasis, and a run function
  that runs the part's `onClick` there as a user event, in one batch.
- **Overlay actions.** A dialog's or drawer's `actions` are lowered with its content, in its scope, and end the
  content as a `ui::ActionBar` of buttons (test id `action_<id>`, shown and enabled as the action is), so their
  bindings hold the overlay's scope as the content does; the mount puts the bar in the overlay's footer. `default`
  and `cancel` keep their Enter and Escape bindings, chords of the content column. So the `default` runs for Enter
  that no focused widget inside took: a button, a text input with `onSubmit` or that commits on Enter, and a table
  take it first.
- **The shown root's `pageEdge`.** `Screen::pageEdge()` is a tracked read that starts at the view's root and, while the
  node is a `switch`, moves to the case shown now (the default when none matches) when that case holds exactly one node
  and declares no scope of its own; it answers that node's `pageEdge`, and `false` for any other node or a case of
  several nodes. A `byWidth` switch and a nested one are seen through, and the answer follows a width or state change.
- **The shown root.** `pageEdge()` and `headJoined()` share one walk, `shownRoot()`: the node the screen shows as its
  root, seen through `switch`es as above. `Screen::headJoined()` answers that node's `headJoined`.
- **The head's placement.** `Screen::leadPlacement()` is a tracked read of the document's `leadPlacement` as a
  `LeadPlacement` (`Below`, `Inline`, `End`); absent is `Below`, and a value that names none of the places is reported
  once at its path and taken as `Below`. `Screen::headSurface()` is a tracked read of `headSurface` as a name, empty for
  none, and `Screen::pageSurface()` of `pageSurface` alike (empty is the built-in `page`). `AppRoot::principalPlacement()` answers the app document's `principalPlacement` as a `PrincipalPlacement`
  (`Sidebar`, `Head`), `Sidebar` when the document gives none.
- **A footer action.** `actionBarOf` draws each `actions` part of a dialog or drawer as a button with the part's `icon`;
  with `iconOnly` the button's label is empty while the condition holds and the part's label is its accessible name and
  tooltip. A head action carries `iconOnly` in `HeadAction`.
- **The head.** A screen's `status`, `tools`, `actions` and `menu` are lowered once, at mount, in the screen scope;
  `Screen::status()`, `tools()`, `actions()` and `menu()` read the visible ones (a tracked read), `Screen::subtitle()`,
  `eyebrow()`, `titleSpans()`, `eyebrowSpans()`, `subtitleSpans()` and `menuLabel()` evaluate `subtitle`, `eyebrow`, `titleSpans`, `eyebrowSpans` and `subtitleSpans` (into runs, with a run it cannot read
  dropped and a tone of no known name reported once) and `menuLabel`, and `Screen::runAction(id)`, `chooseMenuEntry(id)`,
  `editTool(id, value)` and `submitTool(id)` run an enabled, visible part's commands as a user event, reporting an
  unknown, hidden or disabled id. A head tool is controlled: the interpreter holds no draft for it, so its `value` or
  `selected` is the document's binding alone, and an edit reaches the screen only through its commands.
- **A field's `label`.** A text input, select, slider, date-time input or file picker shows no caption of
  its own unless the document asks, so its `label` is bound as its accessible name (`Common::a11y.name`). An
  `a11y` part that gives a `name` wins over it; one that gives only a `role` leaves the label as the name. A
  text input, select and date-time input also take the label as their view tree `label`, and `showLabel`
  (`true`) makes them draw it as a visible caption above the control; the label stays the accessible name
  either way. A checkbox's and a button's label is their text, which already names them.
- **`table`** from `rows`: the columns' labels and widths are set once; a column without a `cell`
  shows the row's field of the column's `id`; `columns` given as an expression is a list of
  `{id, label}`. `onActivate` runs in the activated row's scope. `rowTone` is evaluated in each row's
  scope and gives the row its tone: `"neutral"`, `"info"`, `"ok"`, `"warn"`, `"err"`, or `null` for
  none; another value shows no tone and is reported once for the table (`rowTone is <v>; a tone is
  neutral, info, ok, warn or err`). The selection the table shows is a slot over the bound `selection`
  (each item a key), or over the table's own selection when no key binds it; a bound value that is `null`
  selects nothing, and one that is neither a list nor `null` selects nothing and is reported once at the
  table's `selection` path. A user's change sets the table's own selection when no key binds one, then
  runs `onSelectionChange` in the table's scope with `event` the new keys, as a user event, in one batch.
  The mount re-asserts the slot after the handler, so a bound selection the document does not write back
  snaps back, and a literal one never changes. The element values `selection` and `selectedCount` read the
  bound value when there is one and the table's own otherwise.
  **The rows' pipeline.** The table's element holds a computed made when the element is wired, beside its
  `rows`, so every reader of its element values (a pager drawn before the table included) and the table's own
  lowering see one order, and `rows` is evaluated once per change. It evaluates `rows`, then sorts the list by
  the table's sort: the bound `sort`, or the table's own (starting at `[]`) when no key binds one. Each key
  names a sortable column; the column's `sortBy`, or the row's field of the column's `id`, is read once per row
  with the row bound, in the table's scope, and the rows are ordered by those values as `sort` orders
  (`expressions.md`, Sorting), key by key. A sort of `null` or `[]` keeps the source order; one that is not a
  list of `{column, direction}` records naming sortable columns, or whose values cannot be ordered, keeps the
  source order and is reported once at the table's `sort` path, so a data error does not blank the screen.
  `sortBy` runs before any row's scope exists, as `key` does, so a row-scope name in it reads `null`. Then it
  pages the sorted list, so page 2 holds the second page of the whole sorted list: `pageSize` and `page` are read
  in the table's scope; a `pageSize` of `null` does not page, and one that is no positive integer is reported once
  at its path and does not page either; a `page` of `null` is the first, one that is negative or no integer is
  reported once and shows the first, and one past the last shows the last, unreported, since a filter that shrinks
  the rows under the user is normal. The backend holds the page's rows alone, as it would a short table. On a
  user's change of the selection, an `Extend` report keeps the selected keys of the rows on other pages, in their
  order, before the reported keys; a `Replace` report is the whole new selection, so a plain click or a range
  clears the other pages' keys as it clears the rest of its own, and every `Single` change is one. A key whose row
  `rows` no longer has drops on any user change. `empty` shows while `rows` is empty, not while a page is.
  **Groups.** With `group`, the pipeline splits the source rows into runs by `by` (read per row as `sortBy` is,
  adjacent rows with equal values, `null` equal to `null`), gives each row its run's index, and sorts by that index
  first and the sort's keys second, so every group keeps its place and its rows are ordered inside it. It pages the
  data rows, and puts a heading entry before the first row of each run the page holds: the run record of the whole
  group, computed before paging, so a group split across pages reads the same `count`, `first` and `last` on both,
  with its key in a reserved field. The row view lowers `heading` for that entry as one cell, in a row scope of its
  own whose row is the record, and the table makes it a heading row (`TableRowView::heading`). A heading's key is
  `"\u001Fgroup:"`, the canonical JSON of the run's key, `":"` and its ordinal among runs with an equal key, so
  groups A, B, A get three headings; a data key equal to one is reported as a duplicate by the mount. Heading keys
  never reach `onSelectionChange`: the lowering drops them from a report, since a virtualized backend's Ctrl+A or
  range can name a row it has not realized. `viewCount`, `selectedCount` and the page positions count data rows. A table
  lowered where names are bound (a component's `props`) sorts its own copy with those names, which its element
  values cannot see. The mount's keyed reconciliation moves the sorted rows, so each keeps its widget, its
  native state and its selection. The header marks the sort's first key (`Table::sort`); a header click on a
  sortable column (`Table::onSort`) makes that column the single key, `desc` when it already is the first key
  and `asc`, `asc` otherwise, sets the table's own sort when no key binds one, and runs `onSortChange` with
  `event` the new sort, as a user event, in one batch. The element value `sort` reads the bound sort or the
  table's own, `viewCount` the rows the table orders (every page), and `page`, `pageCount`, `pageFirst` and
  `pageLast` the page. `filters` and `hoveredKey` are not lowered: the
  loader refuses `filters` and `highlightKey` on a table over `rows`. With `empty`,
  the table lowers to a `Column` holding the table and the lowered `empty` content, in the table's own scope:
  while `rows` evaluates to an empty list the table is hidden and the content shown; while it is a non-empty
  list, or `null` (a query not answered yet), the reverse. The column takes the node's `visible` and its layout,
  so it is placed as the table would be, and has no test id; every other common property stays on the table
  (`testId`, the element id `focus` finds, `enabled`, `tooltip`, `a11y`, `keys`, drag and drop). A hidden table
  keeps its element values. No view-tree kind is added, so both backends draw it with what they have. A
  model-backed table's `empty` waits for the table engine, as the table does.
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
- **A form field's component.** A top-level field whose overlay names a `component` is drawn by it: the
  form's view is given a `forms::Overrides::byField` per such field, whose render builds the `custom` node
  `lowerCustom` would, with the field's built-in control as the fallback. The form fills the props it knows:
  `value` (the field's wire value, its JSON text for a `json` prop, `null` while blank or invalid), `errors`
  (the messages, as a JSON list), `readonly`, `required` and `label`; the overlay's `props` give the rest,
  evaluated in the form's scope. A `change` payload is read as the `value` prop's type and written with
  `FieldState::edit`, a user's edit; one that does not decode, or does not fit the field (an object field takes
  an object, a list field an array), changes nothing and is reported (`field <name>: the component's change does
  not decode: <reason>`).
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
  carries the node's common properties. `title` (a heading over the fields) and `submitLabel` (the explicit-mode
  button, and the status line's `ready: press <label>`) are bound in the node's scope, so a `t` re-resolves on
  `setLocale`; `showStatus`, `columns` and `fieldLayout` are read at load, `columns` setting every group's grid and
  the implicit group's, `fieldLayout` the engine's `FormViewOptions::fieldLayout`. `onSubmit`, `onSuccess` and `onError` run once per attempt,
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
  left out, inside a column that carries the node's common properties. `submitLabel`, `showStatus`, `columns` and
  `fieldLayout` reach every form the view draws (`options.form`), as on a `form`; `title`, bound in the node's scope, replaces the
  view's heading (`collection_title`, `wizard_title`). They have no element values
  and no events. A document the catalog lacks, one for another action, one that does not build, or
  a wizard step without a schema is reported and shown as a muted text naming the action.

### Charts

`lowerChart` binds one `ChartData` (`view_tree.md`, Chart data) in the chart's scope: one tracked read evaluates
`summary`, `yLabel`, each series' `points`, each point's keys with `row` the point's element (as a table's `key` is
read, with no row scope made), and each line, so a change to the list or to anything a point reads recomputes the
chart once, and a recomputation that changes nothing sets nothing. The range, the ticks and the labels are computed
in `math::Rational` (`chart_scale.hpp`) and converted to `double` only at the end; no label is derived from a
position.

- **Values.** `y` and a line's `value` are numbers or quantities, and convert to the unit of the chart's first
  value read (the series in order, then the lines), as `sum` converts. A value that is not a number, or does not
  convert, is left out and reported for its path, once (`a chart value is a number or a quantity; this one is left
  out`; `… does not convert to the unit of the chart's first value …`). A `null` `y` is a gap: the point is kept,
  and no line joins across it. A line whose `value` is `null` is not drawn.
- **Points.** `points` that is `null` draws as empty without a report (a query still loading); any other value
  that is not a list draws as empty and is reported. A series draws its last 1000 points, reported once past that:
  a chart is a picture of a few hundred values, and a larger set is a table's. `xLabel` and `note` show as `textOf`
  shows a value, so an `int` needs no `format` call; `null` marks nothing, and a `tone` outside the five names marks
  nothing and is reported once for its path.
- **The horizontal axis.** Without `x`, point *i* of every series sits at *i*, its element's index, so series of
  equal length line up run by run. With `x`, a number is its value and a date or an instant its milliseconds since
  the epoch (a `yyyy-MM-dd` date is midnight of it in the display zone, as a date input reads it); a point whose `x`
  is `null` is left out, and one that is neither is reported and left out. Numbers mixed with dates and instants
  are reported once, and every point then sits at its index. The axis spans every position, widened by 5 % of the
  span at each end; a span of zero is widened by one either side.
- **The value axis.** It spans every drawn value and every line, widened by 5 % of the span at each end, so the
  outermost point and line lie inside the plot; a span of zero is widened by one unit in the last of the places
  either side, and a chart with nothing to draw spans 0 to 1. The places are `yPlaces`, or else the widest places of
  the values and lines. `yMin` and `yMax`, evaluated and converted as values are, replace the computed ends; ends
  that leave the axis empty are reported (``yMin` is not below `yMax``) and the computed range is kept. A value
  beyond a fixed end is drawn at that end as clipped (`ChartClip`), not as the end's value, and is reported nowhere:
  a document that fixed the axis chose that, and the summary carries the value.
- **Ticks.** With three or more lines, the lines label the axis (they are the scale a control chart is read
  against) and `yTicks` is empty. With fewer, the ticks are the multiples, inside the range, of the largest step of
  1, 2 or 5 × 10ᵏ that puts three or more there (three to six, or seven or eight where a step of 5 × 10ᵏ would put
  two), each shown at the wider of the places and the step's own places, in the active locale. Two specification
  lines alone would leave the values between them without a scale.
- **Labels.** A line without a `label` is labelled with its value at the places. Every text is formatted through
  the session's `FormatContext`, so a decimal takes the locale's separator.
- **Overflow.** A range the checked arithmetic cannot hold is reported once, and the chart draws its summary alone.

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
| Queries and mutations built on `Signal`, `Computed`, `Effect` and `Scheduler`, not on `reactive::Query` and `Mutation` | `optimistic` and an `updates` reply write into a query's value and two screens share one value; `reactive::Query` keeps its value privately. The rules are the same. |
| The cache entry is the single source of a query's value | One request in flight, one value and one error per key, across screens, is what spec 5 §5 asks for, and an `updates` reply then reaches every screen showing the row. |
| Optimistic writes as layers | Rolling back by restoring a saved value is wrong when runs overlap; recomputing from the confirmed value and the remaining layers is right in every order. |
| `source: "user"` from the newest write | A watch cannot see who wrote a value; the session can see whether the write that preceded the flush came from a user event. |
| Scoped content behind a one-row `forEach` | The view tree builds a dialog's content once, as a fixed node; a row view is the one place it builds content each time it mounts. |
| Host commands behind one interface, each default "not supported" | The interpreter runs in local mode, in tests and in a generic client; each supplies what it can. |

## Not yet lowered

Reported when met, and shown as a muted text naming the kind:

- The chrome of a `collection` or `wizard` (its header, rows, confirm, editor and navigation drawn
  by the host's own kit): C++ code replaces them through `forms::CollectionChrome` and
  `forms::WizardChrome`.
- A `form` node's subtree overrides (spec 2 §7): document subtrees replacing a field's control. A host
  component replaces one through the field overlay's `component`; C++ code replaces controls through
  `forms::Overrides`.
- A `form` node's `showErrors`: a field's error shows on its control as soon as its draft does not
  encode.
- A `table` declared from a model's list action: the table engine (spec 7).
- Prefetching on hover: the host starts a screen's `prefetch` queries through `Session::prefetch`.

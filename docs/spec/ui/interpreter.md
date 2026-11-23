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
| `Session` | What screens of one client session share: the query cache, the mounted queries, the permitted action ids `can` reads (`setPermitted`), the formatting facts (`setFormatting`, `setLocale`), and the `env`, `session` and `host` values: `setEnvironment` replaces the `env` record but for `locale`, which is always the formatting facts' locale, and `setEnvironmentValue(name, value)` sets one value and keeps the others, as a committed backend switch keeps the width class. `clear()` is `signOut` and a principal change; `backendChanged(backend)` carries out a committed backend switch. `appScope()` is the mounted app document's scope. |
| `Interpreter` | `mount(doc, params, backend, MountOptions)` → `Screen`. |
| `Screen` | A mounted screen: `root()`, `title()`, `identity()`, `dirty()`, `requestClose()` (runs `onClose`; false when it ran `cancelClose`), `beginClose()` (runs `onClose` and returns the `cancelClose` flag it shares with what it starts; null without `onClose`), `pending()` (a tracked read: an open dialog or drawer, a mutation run, or an upload or download in flight), `declares(key)`, `runtime()`, `runLifecycle(key)`, `setPreference(point, value)`, `document()`, `lend(alias)`, `read(expression)`, `diagnostics()`. Destroying it runs `onUnmount` and unmounts. A host mounts and closes screens through [host.md](host.md). |

`SessionOptions` carries the `Scheduler` (`debounce`, `refreshEvery`, cache eviction), the clock
`staleTime` is measured on, how long an unused entry is kept (five minutes), the `Catalog`
(`stateless` decides an alias's default instance; an action's `secretFields` are never cached or kept
in `result`) and the error classifier (the bridge's own failures map to `offline`, `timeout` and
`backendChanged`; a server's refusal, the `err` message `unauthorized` of an execute or a refused register or attach,
to `denied`; an `ActionRefusal` and everything else is `error`) and the `watcher`, which sends the cache's `watch` and
`unwatch` requests
(`ui::watcherOver` builds one over a remote source's exchange; empty for a local source), and `maxInlineFileBytes`,
the most bytes a document moves inline in one file (default `kDefaultInlineFileBytes`, 1 MiB). `Session` construction
throws `std::invalid_argument` for 0 and for a cap above `kMaxInlineFileBytes`, ⌊(`wire::kMaxEnvelopeBytes` −
64 KiB) · 3 / 4⌋ = 6 242 304 bytes: base64 grows bytes by 4/3, and 64 KiB are left for the rest of the envelope.


## The app root scope

`Interpreter::mountApp(doc)` mounts the app document (id `app`) as an `AppRoot`: its state, lets,
queries and mutations, declared once for the session, and its `onMount`. Its `formatting` patterns become the
session's (`Session::setDatePatterns`) and win over the locale table's patterns in every locale
([`locale_table.md`](locale_table.md)), so every screen of the bundle writes and reads dates in them; a C++ host sets
its own patterns through `setDatePatterns` too, since a `setLocale` replaces a pattern set only through
`setFormatting` with the locale's. A `dateTimeInput` reads the pattern of its mode reactively, so a pattern a `setLocale` installs reaches the inputs
already mounted; the zone is read once, when the input is lowered. A second root on the same
session, and a document whose id is not `app`, are refused. While it lives, `Session::appScope()`
names its scope: `{"ref": "app.name"}` in any screen reads the name there, reactively, so a query of
the app document runs once however many screens read it, and `{"set": "app.name"}` writes the root's
state (a name that is not state is reported). Destroying the root runs its `onUnmount` and takes its
scope off the session; the screens that read it are destroyed first, as sign-out does (spec 6 §5).

`AppRoot::navigationBadges()` is a `reactive::Computed` of the document's `navigation` badges that are visible, in
order, each a `NavigationBadge{screen, text, tone}`: the parts are lowered once in the app scope when the root
mounts, so a count follows the state it reads. It is empty when the document declares none, and goes with the root,
before its scope. A shell copies it onto its navigator ([client.md](../client/client.md#the-shell)).

`AppRoot::tours()` is a `reactive::Computed` of the document's tours, in declaration order, each a `Tour{id, screen,
title, summary, steps}` whose steps are `TourStep{title, body, screen, params, target, advanceOn}`. The texts are
evaluated in the app scope inside the computed value, so a change of locale, or of the app state a text reads,
re-evaluates them; it is one value for every tour, since the texts are few and a locale change changes all of them.
It goes with the root, before its scope. `setRuntimeTours(tours)` gives it the runtime tours a client listed; they follow
the app document's tours, a runtime tour with one of their ids is left out, their texts are evaluated in the same
scope, and each `Tour` carries the `revision` of its runtime tour (0 for an app-document tour).

## Mounting

`mount` refuses, with a `Refusal`, what only the opener can get wrong: a param missing or of the wrong
type (`/params/<name>`), an alias `{"from": alias}` the opener did not lend (`MountOptions::borrowed`,
filled from the opener's `Screen::lend`), and a document without a view. It then declares the screen
scope, lowers the view, mounts it through `Mounted` at `MountOptions::depth + 1` (the screen scope's
own Effects at `depth`, so they run first), wraps the root in a column carrying the document's `keys`
when it has any, and runs `onMount`.

A mount that replaces a screen, a development reload's remount or a draft's (host.md, "Drafts"), names it in
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

- **Cache entries** (`Session`) hold the shown value, `hasValue`, the error text and kind, the action
  refusal (`null`, or `{code, args}` when the failure was an `ActionRefusal`), `pending`, a
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
    - **Extra watches.** `Session::addExtraWatch(record)` watches a record for something other than
      the cache, such as a list a client keeps beside it (the custom screens list). The `watch` goes out
      at once, `watchSet()` names the record, so `reconnected()` and every `resync` carry it in their
      `replace: true` set whatever order the owner and the cache acted in, and the cache dropping its
      last entry on the record does not unwatch it. `removeExtraWatch(record)` sends the `unwatch`
      unless an entry still holds the record. Events arrive through `deliverEvent` as any other.
- **Open, partial and opaque entries.** The catalog's `ActionRecordInfo` carries, beside a record's key
  field and paths, `open` and `partial` (paths of members a holder of the record's rows declares through
  `openMembers()` and `partialMember()`, as a table's `Page` declares `awaiting`, `distinct` and
  `unloadedRows`) and `opaque` (the result holds the record at no path, as `Table.<id>` holds its table's
  record). A member is set when its value is a non-empty list, a record, `true`, or a number other than zero.
    - **An open entry is marked by every change of its record**, whatever the keys: while an `open` member is
      set, a row the entry does not hold may come to belong to it.
    - **A partial entry is not merged into.** While an `open` or `partial` member is set, an `updates` effect
      on the record acts as `changes` for that entry: a merge replaces rows by key and cannot update what such
      a member says about rows by position.
    - **An opaque entry is both,** always: its rows cannot be found by key. Its reply is read with
      `detail::tableValueFromJson`, which keeps a JSON number with a fraction or an exponent as the decimal its
      shortest round-trip text in fixed notation spells (`table::parseDecimal`; `0.0001` stays a decimal), or as
      its shortest text where no decimal holds it; a page's
      `Number` cells are such numbers, and the strict `valueFromJson` would refuse the whole page. Every other
      entry keeps the strict rule.
- **Reading and watching without an entry.** A page window's fetches are not queries.
    - `Session::read(channel, action, body, stop)` sends the action and settles with the reply's JSON text,
      unparsed: a `Value` holds no number with a fraction or an exponent, and a page's `Number` cells are such
      numbers, so the caller decodes the text against the table's description (`table::readGenericPage`). It
      makes no entry, reads none and applies no effects. `stop` goes to the channel as `CallOptions::stop`,
      which sends the wire's `cancel` where the server takes one; a stop settles the read with
      `core::async::OperationCancelled` and drops the reply.
    - `Session::watchRecord(record, handler)` counts the record into the watch set as an entry does and
      resolves with a `WatchHolder` once the server has the watch (at once when it already had it, or there is
      no watcher). The holder is copyable: the watch lasts while any copy lives, and the last copy's death
      removes the handler and counts the record out. A `Completion` hands its value as `const&`, so a caller
      keeps the watch by copying the holder in its `then` handler. The handler hears each `changes` event that
      names the record, after the entries are marked, and a whole-type change on reconnect and on `resync`. A
      holder that outlives its session releases nothing.
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
- `setLocale` replaces the session's locale and installs the locale table's number, money and date facts
  ([`locale_table.md`](locale_table.md)), so every `t` and display function re-resolves, and tells the host.
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
path and the holder stays content-sized on that axis. A tab page is mounted into a slot that fills the tabs node's page area, is scrollable and pads its own content (`Mounter::mountPage`): a screen needs no `scroll` of its own, and a stretching child fills the page but is never shorter than its `minHeight`. A single node of a list is lowered unwrapped. A holder's width is never a share, except that the holder of a lone node (`listColumn`) takes that node's `percent` and the node is lowered with `stretch(1)` so that the share is taken once; a `percent` on a node inside a `switch` case or an overlay's content is reported once ("size the `switch` or the overlay itself") and the node is lowered as `stretch(1)` (the author sizes the `switch`, whose slot is the row's child), and a `percent` among several nodes of a holder is reported and lowered as content. The column that wraps several nodes is a holder (`Column::holder`): a child of it that bleeds reaches past the padding of the container the document names (a panel's, a tab page's), not past the column's own, which has none; a column the document wrote is not a holder and gives its children nothing to bleed past. A `switch` case's holder and a `default`'s take the gap of the vertical list the switch sits in (`LowerCtx::listGap`): a column's, a panel's, a collapsible's, a scroll's or a tab's content, or a dialog's or drawer's body. `ScreenCore::lower` zeroes the gap for every kind but `switch`, and only the holders of a vertical list set it for the nodes they hold, so no kind's children see their grandparent's gap; a switch in a row or a grid cell is a column with gap 0, and a switch inside a case keeps the gap of the case it sits in. A document panel or collapsible puts `gap` (default 2) between several
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
  reported once for its path and lowers to an empty list, which leaves each pane to its own sizing. The document has no
  `onResize`, so a user's drag stays the splitter's own. A literal `sizes` that matches the panes places them, and a
  pane of a horizontal splitter whose width is fixed or a percent is reported once ("this splitter's `sizes` places its
  panes; pane N's sizing is not used").
- **Action parts.** `ScreenCore::lowerActionParts(parts, ctx)` lowers a list of `actionButton` parts into
  `ActionModels`: the label, icon, `enabled` and `visible` bound in `ctx`'s scope, the emphasis, and a run function
  that runs the part's `onClick` there as a user event, in one batch.
- **Overlay actions.** A dialog's or drawer's `actions` are lowered with its content, in its scope, and end the
  content as a `ui::ActionBar` of buttons (test id `action_<id>`, shown and enabled as the action is), so their
  bindings hold the overlay's scope as the content does; the mount puts the bar in the overlay's footer. A document's own `actionBar` lowers to the same `ui::ActionBar`, with its `align` and `justify`. `default`
  and `cancel` keep their Enter and Escape bindings, chords of the content column. So the `default` runs for Enter
  that no focused widget inside took: a button, a text input with `onSubmit` or that commits on Enter, and a table
  take it first.
- **The shown root's `pageEdge`.** `Screen::pageEdge()` is a tracked read that starts at the view's root and, while the
  node is a `switch`, moves to the case shown now (the default when none matches) when that case holds exactly one node
  and declares no scope of its own; it answers that node's `pageEdge`, and `false` for any other node or a case of
  several nodes. A switch on the width class and a nested one are seen through, and the answer follows a width or state change.
- **The shown root.** `pageEdge()` and `headJoined()` share one walk, `shownRoot()`: the node the screen shows as its
  root, seen through `switch`es as above. `Screen::headJoined()` answers that node's `headJoined`.
- **The head's placement.** `Screen::leadPlacement()` is a tracked read of the document's `leadPlacement` as a
  `LeadPlacement` (`Below`, `Inline`, `End`); absent is `Below`, and a value that names none of the places is reported
  once at its path and taken as `Below`. `Screen::titleRole()` is a tracked read of `titleRole` as a `TextRole`
  (`Heading` or `Headline`); absent is `Heading`, and a value that names neither is reported once at its path and
  taken as `Heading`. `Screen::headSurface()` is a tracked read of `headSurface` as a name, empty for
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
  neutral, info, ok, warn or err`). `rowSpans` is evaluated in each row's scope as a slot, like `rowTone`, and gives the
  row its `TableRowView::spans`: a list of `{column, rows, columns}` records, `column` a column's id (a derived column's
  too) turned into its index, `rows` and `columns` integers from 1, each 1 when absent. A record that is not of that
  shape, names an id the table has not, or has a `rows` or `columns` that is no integer of at least 1 is dropped and
  reported once for the table (`rowSpans is <v>; a span is {column: a column's id, rows: an integer from 1, columns: an
  integer from 1}`); `null` and `[]` give none. A span's `columns` is cut by the table's layout (`cutColumns`): it covers
  its column, then each next visible column while that is the next declared one, so a span across a column the user
  moved away or hid stops there, and a span whose own column is hidden is dropped. A table over `rows` with
  `summaries`, or with a `group` part that has them, is mounted over a `SummaryRows` it keeps over its page's entries:
  the table's summaries before or after the page's rows and each run's after the run. A summary reads `rows` as every
  row of the view in view order (a paged table's summary is the same on every page), a run's summary its run's rows,
  and `column` the column's id; its cell shows the value by the column's display parts, as a default cell does, and
  its part's `label` names its row. The table's `bands` parts become
  `TableOptions::bands`. What follows from the rows'
  order (a span past the last row, over a group heading, onto a covered column) is the mount's to refuse. The
  pipeline pages before the table sees its rows, so a span reaching past the page's last row is refused and
  reported. The table's sort is the one `options.sort` carries: while it names a column the table draws no spans. The selection the table shows is a slot over the bound `selection`
  (each item a key), or over the table's own selection when no key binds it; a bound value that is `null`
  selects nothing, and one that is neither a list nor `null` selects nothing and is reported once at the
  table's `selection` path. A user's change sets the table's own selection when no key binds one, then
  runs `onSelectionChange` in the table's scope with `event` the new keys, as a user event, in one batch.
  The mount re-asserts the slot after the handler, so a bound selection the document does not write back
  snaps back, and a literal one never changes. The element values `selection` and `selectedCount` read the
  bound value when there is one and the table's own otherwise.
  **The rows' pipeline.** The interpreter does not sort or page a table: its element holds, from when it is
  wired, the table's rows on the engine, so every reader of its element values (a pager drawn before the table
  included) and the table's own lowering see one order, and `rows` is evaluated once per change. The evaluated list
  goes to a `ValueRowsSource`, matched by `key`, under a `table::Engine` whose owner is the runtime's, with no worker,
  owner steps of 4 ms and `ValueRowsCollator` for text, read through an `EngineRows`; a changed row is repaired in
  place, not sorted again, and a row whose fields changed is drawn again even when no sort reads them. The table's
  sort, the bound `sort` or the table's own (starting at `[]`), is the engine's sort chain. A key names a sortable
  column, whose cells the source holds while a key reads it: the column's `sortBy`, read per row in a tracked
  computation with the row bound in the table's scope, or the row's field of the column's `id`, converted to the
  column's kind, its literal `kind` or else the kind its cells infer. A quantity reads its unit from a registry of
  the screen's unit table, each unit in its canonical unit's dimension; a unit the table does not list is its own
  canonical unit, as an expression reads it: when a row's field brings one, the source and the engine are made
  again over a registry that holds it (a unit only a `sortBy` produces is not seen, and is an invalid cell). Values of one kind order as `sort` orders
  them (`expressions.md`, Sorting). A cell not of its column's kind is invalid, sorts last in either direction,
  and is reported once for the table at its `sort` path. A sort that is not a list of `{column, direction}` records
  naming sortable columns keeps the source order and is reported at the table's `sort` path, so a data error does
  not blank the screen. `sortBy` runs before any row's scope exists, as `key` does, so a row-scope name in it reads
  `null`. A list of more than 20,000 rows, or of more than 1,000,000 cells (rows times the column parts), is still
  ordered, and reported once for the table: `table <id> holds <n> rows; a table over rows is ordered on the client
  up to 20,000 rows or 1,000,000 cells — serve it as a model's table`. The page is a slice of the engine's view, so
  page 2 holds the second page of the whole sorted list: `pageSize` and `page` are read in the table's scope; a
  `pageSize` of `null` does not page, and one that is no positive integer is reported once at its path and does not
  page either; a `page` of `null` is the first, one that is negative or no integer is reported once and shows the
  first, and one past the last shows the last, unreported, since a filter that shrinks the rows under the user is
  normal. The mount reads the page through a `TableSource` whose `alwaysWindowed` is `false`, the path a
  model-backed table's mount takes: below the virtualization threshold it realizes every row of the page, as a
  short table does, and above it the rows in view. A column with its own `cell` content is lowered in its row's
  one scope, found by its key, and is marked in `rowScopedCells`, so a realized row the mount gives to another key
  has those cells built again rather than rebound; a plain column's text reads the record alone and is rebound. A
  change of the engine's view reaches the mount as the engine's
  own operations when the page is the whole view and the table is not grouped, and as a reset otherwise; the
  mount rebinds only the rows whose record changed. Each change still costs the lowering a pass over every row: the
  view's records are read again for `TableView`, the page's entries are rebuilt, each evaluation of `rows` reads
  every field of every row for a unit the screen does not list, and above the virtualization threshold the mount
  gives the backend every key (`setExistingKeys`). A `multiple` table's `Ctrl+A` (`Table::onSelectAll`) selects
  the engine's whole view, every page, through `table::Selection::selectAll`, or clears the selection when every row
  of the view is selected already; a backend that does not take the handler selects the rows it holds. On a user's change of the selection, an `Extend` report keeps
  the selected keys of the rows on other pages, in their order, before the reported keys; a `Replace` report is the
  whole new selection, so a plain click or a range clears the other pages' keys as it clears the rest of its own,
  and every `Single` change is one. A key whose row `rows` no longer has drops on any user change. `empty` shows
  while `rows` is empty, not while a page is.
  **Its state and its status line.** A table over `rows` with an `id`, while the session has a table-state store and
  its `persist` is not `false`, keeps its sort under the key `{application, model "", table <id>, qualifier <the
  persist name or the screen's id>, part "state"}`: its sort and its column layout. Its rows are held back (`EngineRows::setRestoring`) until the
  stored state has been applied and the engine has ordered by it, or until `tableStateTimeout` passes (without a
  scheduler, once the work the stored sort posted has run), so the first rows drawn are in the stored order, the
  mount drawing placeholder rows meanwhile; a state that arrives later is applied unless the user changed the table
  meanwhile. The stored state is fitted to the node's column parts, of the kinds their cells infer, and its layout
  is in place in the turn the rows are let through. A sort or layout gesture is saved as a model-backed table's is,
  debounced, and an unmount saves at once. A table with no `id` keeps no
  state. Its status line shows its counts and the latest message (`ScreenCore::tableMessage`), and takes space only
  while it has a message; `status: true` shows it always, and `status: false` never, its messages going to the
  host's notifications.
  **Saved views.** A table that keeps a state, model-backed or over `rows`, also holds a `TableViews` over the
  same key: its `views` part, and its `shared` part when the store shares, loaded in the turn its `state` loads, so
  a store that batches reads every part of a screen in one call, and a second mount reads them from the session's
  kept copies. A change applies a views operation to the document loaded and saves it with the revision loaded, or
  `kNoDocument` when nothing was; a `Conflict` reads the document again from the store, applies the operation to it
  and saves once more, with its revision, or with an empty `expected` when nothing is stored; a second `Conflict`
  is "Views could not be saved: another window changed them at the same time", and an operation that does not
  apply to the document read again ("A view named … exists", "No view named …") saves nothing. A mounted table does
  not follow saves made elsewhere. Views that cannot be read are left unsaid at mount. The table's stored state is
  applied once both its state and its views have answered: with no stored `state`, the `default` view is applied,
  its name the state's `view`; with `defaultOnMount`, the default view's sort and filters replace the stored ones
  and the stored layout still applies (`stateAtMount`). Either is fitted as a stored state is. A default view applied
  with nothing stored is the table's state from then on (`TableStates::start`), unsaved until the next change, which
  saves on top of it, so its layout, sort, filters and name outlast that change. Under `defaultOnMount` the stored
  `view` stays, so the menu marks it modified while its sort or filters differ from the default's. A stored `view`
  names one of the user's own views: a shared view applied is marked as shared only until the table mounts again.
  **The `Views` submenu.** The table menu of a table that keeps a state (model-backed, over `rows` with an `id`, or a
  collection) has `Views` (`viewsMenu`): each of the user's own views, checked while it is the state's `view` and
  labelled `{name} (modified)` while the arrangement (the layout laid over the table's columns, the sort and the
  filters) differs from that view as fitted to the table; then "Save view…", "Rename view…", "Remove view", "Set as
  default" (the last three enabled while the state's `view` is one of the user's own) and "Start from the default
  view", checked with `defaultOnMount`, which it toggles. Applying a view fits it as a stored state is fitted and sets
  its layout, sort and filters through the gestures' own paths (held and saved unless the document binds them,
  `onLayoutChange`, `onSortChange` and `onFiltersChange` run), then writes its name to the state's `view`; a filter or
  sort key on a column the table lacks is not applied. A table over `rows` knows a column's kind only while the column
  is in use, and its engine takes the kind in when it next flushes its changes: the columns a stored state or a view
  sorts and filters by are put in use before it is fitted, and its filters (a view whole) go to the engine a task
  later. "Save view…", "Rename view…" and "Publish view…" open the view
  name dialog (`viewNameDialog`, test id `table_view_dialog`: the input `table_view_name`, the buttons
  `table_view_save`, enabled while the name is not blank, and `table_view_cancel`), whose state lives in the
  element. Saving under a name the user already has is a `ReplaceView`, any other an `AddView`, decided against the
  views document the save is applied to (`TableViews::saveView`), so a name another window took after the table
  loaded, met as a `Conflict` and read again, is replaced too; done, the state's
  `view` is that name and the status line says "Saved view {name}". A rename onto a taken name is refused in the
  status line, "A view named {name} exists", and changes nothing. "Remove view" clears the state's `view`. Every
  refusal or failure of a views change goes to the status line, and a change answering after the table is gone
  reaches nothing. Every menu choice clears the status line's message first.
  **Shared views.** While the store's `shared` part holds views, `Views` adds the heading "Shared views" and each
  shared view, applied as an own view is; while the state's `view` is a shared view so applied, "Save as my view"
  adds it to the user's views (an `AddView`) and says "Saved view {name}". While the store shares, "Publish view…"
  names the arrangement and publishes it (`TableViews::publishView`, the model's `publish` action, never `save`): an
  `AddView`, or a `ReplaceView` over a shared view of that name in the shared document it is applied to, as a save
  decides; done, "Published view {name}", refused or failed,
  "This view could not be published: {reason}". Over a store that does not share, neither entry is offered and no
  shared part is read.
  **Groups.** With `group`, the source splits its rows into runs by `by` (read per row, untracked: what `by` reads
  besides the row regroups the rows when it changes; adjacent rows are one run while their `by` is the same element
  as the run's first row's, by the rule of `runs`, quantities compared through the screen's unit table), holds each
  row's run index as a hidden leading sort key, and the engine sorts by it first and the sort's keys second, so
  every group keeps its place and its rows are ordered inside it. The lowering pages the data rows, and puts a
  heading entry before the first row of each run the page holds: the run record of the whole group, so a group
  split across pages reads the same `count`, `first` and `last` on both, with its key in a reserved field. The row
  view lowers `heading` for that entry as one cell, in a row scope of its own whose row is the record, and the table
  makes it a heading row (`TableRowLook::heading`). A heading's key is `"\u001Fgroup:"`, the canonical JSON of the
  run's key, `":"` and its ordinal among runs with an equal key, so groups A, B, A get three headings; a data key
  equal to one is reported as a duplicate. Heading keys never reach `onSelectionChange`: the lowering drops them
  from a report, since a virtualized backend's Ctrl+A or range can name a row it has not realized. `viewCount`,
  `selectedCount` and the page positions count data rows. A table lowered where names are bound (a component's
  `props`) has a source and an engine of its own, read with those names, which its element values cannot see, and
  keeps no state. A row keeps its widget, its native state and its selection while its key stays on the page. Its
  header, menus and arrangement are configured as below ("Configuring a table"), its sort bound the node's literal
  `maxSortKeys`, else 8. The element value `sort` reads the bound sort or the
  table's own, `viewCount` the rows the table orders (every page), and `page`, `pageCount`, `pageFirst` and
  `pageLast` the page, and `filters` the filters its user set from the header, which the table holds (and keeps in
  its persisted state) and does not take from the document: the loader refuses `filters` and `highlightKey` on a
  table over `rows`. `hoveredKey` is not lowered. Its `groups` parts are the groups its source declares to the
  engine, by which a group filter and a pooled values list name them. With `empty`,
  the table lowers to a `Column` holding the table and the lowered `empty` content, in the table's own scope:
  while `rows` evaluates to an empty list the table is hidden and the content shown; while it is a non-empty
  list, or `null` (a query not answered yet), the reverse. The column takes the node's `visible` and its layout,
  so it is placed as the table would be, and has no test id; every other common property stays on the table
  (`testId`, the element id `focus` finds, `enabled`, `tooltip`, `a11y`, `keys`, drag and drop). A hidden table
  keeps its element values. No view-tree kind is added, so both backends draw it with what they have.
- **`table`** of a model's table (`model` and `table`): its rows come by view index from a `WindowRows`
  ([`../table/table.md`](../table/table.md), "The client table model"), which the element makes when it is wired,
  together with a query of `Table.<table>.describe` on the model's channel (one per mounted table, an entry of the
  query cache like any query). The rows send no page before the description, and none before the stored state
  below. The node lowers to a list of one row keyed by the held description's revision, whose row is the `Table`
  built from that description (`Table::source`, a `TableSource` over the rows); before a description, and after
  one that does not read, the row is a `Table` with no columns and no source, so its status line keeps its place
  and `empty` is not shown. A new revision is a new key, so the table is mounted again from it: the rows, their
  sort and filters, the selection and the status-line message are the element's and stay; the realized rows, the
  cells and the scroll offset are the mount's and are built again. A reply that names another revision than the
  one held asks for the description again, once per revision; the fitting of the new one drops a sort key, a
  filter and a fetched column that name a column it lacks, and reports a dropped filter once at the table.
  Columns are the description's columns but its companions, those it does not show hidden by the layout until the
  user shows them, or the node's `columns` parts, each naming a described column by `id` (one it lacks is reported
  and left out) and setting its label, width, role, places, `wrap` and `locked`; a column is sortable as the
  description says. Its header, menus and arrangement are configured as below ("Configuring a table"), its sort
  bound the description's `capabilities.maxSortKeys`, which its server's `limits` set. A column's `cell` is lowered per cell, in a row scope over the cell's own row
  signal; without one the cell is a text of the record's field. `rowTone` and `rowLabel` are lowered once per
  realized row, in a row scope over the row's signal, which `onActivate` runs in. The rows fetch, with every
  page, the description's key and each field `{"row": "<id>"}` names in `rowTone`, `rowLabel` and `onActivate`; a
  column's cell's fields (`TableSource::cellReads`) while the column is in the window; and every column when one of
  them passes `{"row": ""}` on whole (reported once). A column without a `cell` is the default cell, one text that
  follows the column the mount gives it (`defaultCellView`), so a cell of one role serves any column of it. A
  path read out of the whole row with `get` is the row's own read: a row scope of a model-backed table has
  `RtScope::onMissingRowField`, which a read of a field the record lacks calls from inside the binding; it
  writes nothing there, and one task posted for the turn fetches every missed field that is a described column,
  once, for the rows in view (a field that is not one is never sent). The read is `null` meanwhile, and the
  binding runs again when the column arrives. `body` is evaluated once and is the query's fixed part; its
  `query` member's `sort`, `filters`, `page`, `columns`, `keys` and `retry` are the table's own and replace
  what the body gives; each the body gives is reported once, at its pointer under `body/query`. A literal `sort` is the default a stored one replaces, any other expression binds it; a
  literal `filters` object (`columns` and `groups` only) is likewise the default, an expression binds them. The
  headers of the columns the rows' filter names are marked filtered (`Table::filtered`). **Persisted state.** With a session store and `persist` not `false`,
  the state's key is `{application, model type, table, persist's name or the screen's id, "state"}`; its load
  starts when the element is wired, and the rows wait for it at most `tableStateTimeout`. A state that comes in
  time is read (`readState`), fitted to the description held, and its sort, filters and layout applied where the
  document binds none of them, before the first query; one that comes later is applied the same way, one more query,
  unless the user scrolled, selected or sorted the table meanwhile. A state that does not read is reported once
  (`the stored state of table <id> is not used: …`) and a failed load once (`… could not be loaded: …`); the
  table starts from its defaults and changes still save. A sort or layout gesture saves its part
  (`TableStates::changed`, debounced), and unmounting saves a pending change at once. **The status line** (`Table::status`) always takes
  space, unless `status` is `false`: its counts are `countsText` of the rows' counts and the selection's size; a
  failed level fills its state with "<Level> data could not be loaded." and a failed fetch with "The rows could
  not be loaded: <reason>", both with Retry (`WindowRows::retry`). `ScreenCore::tableMessage(table, text)` shows
  a refusal or notice there for `kStatusMessageHold` (6 s) on the session's scheduler, until the table's next
  gesture (a header click, a menu choice, a column moved or resized, a selection change, a scroll, Retry) clears
  it; under `status: false` it goes to
  `InterpreterHost::notify` as a warning. A table state's save that does not end `Saved` is said there by every
  mounted table with its key: "The table's arrangement could not be saved: <reason>". `empty` shows once a reply
  said the view has, exactly, no row and no row is undecided. Its element values are a table's, with
  `viewCount` the reply's total (`null` before one), `viewCountKind` (`exact` or `atLeast`), `undecided`,
  `awaiting`, `failed`, `pending` (the description or a fetch in flight) and `error`.
  **Range selection.** A range the backend reports as two view rows (`Table::onSelectRange`) is resolved
  through `TableRows::keysOf`: the rows held give their keys, and every page of the range with a row not held is
  fetched naming no columns (`"columns": []`), so its reply carries keys alone and the window holds none of it.
  The keys select as the gesture says (`Replace`, or `Extend` with the keys selected), and `onSelectionChange`
  runs. A range of more than `SessionOptions::maxRangeRows` (10,000) rows is refused in the status line, "A range
  selects at most 10,000 rows" (the number grouped by the locale), and selects nothing. **`highlightKey`.** Each
  non-null key, once a description is held, is located by one keyed fetch (`Table.<table>`, or a typed list
  action itself, `keys` the key, `columns` none, the table's sort and filters; `WindowRows::locate`), which stops
  the one an earlier key still has in flight; the reply's `indices[0]` is the row's view index, and `Table::highlight` becomes
  `TableHighlight{key, index}`, which the mount scrolls to and marks. A reply with no row, and a `null` key, set it
  to nothing. A reset of the rows (a new sort or filter) clears it and locates the key again. It never changes
  the selection or the keyboard focus. An element in a column's `cell` is focused by the row the cell shows now
  (`RtScope::rowKeyNow`), since the mount rebinds a pooled cell to other rows.
- **`table`** over a typed list action (`model` and `action`): lowered as a model's table is, through the same
  element state, `WindowRows` (`WindowRowsOptions::action`) and `Table`, with no `describe` query: a catalog carries
  an action's request schema and never its reply's, so the description is built when the element is wired, from
  the node's `columns` parts (each part's id, its label from the action's view document when that names the
  column) or else the view document's `v-columns` (its `field`, `label`, and `v-hidden` as not `shown`). The key
  is the node's `key` when it reads one row field (`{"row": "<id>"}`), else the view document's `v-rowKey`, else
  `id`. A column's kind is the part's literal `kind`, else its `v-columns` entry's `kind`, else inferred from the
  first reply that has rows (`readTypedPage`'s vote over the JSON members of its id) and then fixed for the
  window's life: a later member not of it is an invalid cell, and a fractional number makes the column `Number`
  where the same number in a document's `rows` is a `Decimal`. The description's revision is empty and never
  changes, so the table is mounted once. Every fetch is the action itself, with `body` evaluated once as the
  request and the query written into the request's one member the schema marks `x-table`
  (`tableQueryMembers`); the reply is a `Page<Row>` read with `readTypedPage`, which turns each row object into
  cells for every described column, whatever the query named. The rows watch each record the action's catalog
  entry names; a reconnect or `resync` of the session drops their pages whether they watch one or none
  (`Session::onReconnected` for a window that watches none). A keyed refetch sends `keys` in the query member; a
  reply with no `indices` for it drops the window's pages and costs a page refetch. **Units.** A `v-columns`
  entry's `ExtUnits` is a unit (`unitAscii` its id, `unitUnicode` its symbol, `x-decimalPlaces` its places) and
  each `x-unitAlternatives` entry another of its dimension (`num`/`den` its factor to the first, 1 when absent; a
  factor of zero or less is reported and the unit left out), which the description lists; such a `quantity` column
  is read as `Measure`. Columns that share a unit share its dimension whatever their order: a column one of whose
  units is listed already joins that unit's dimension, its factors scaled through it, and a unit two columns
  declare differently is reported and kept as first listed. A unit a reply's member names that the description
  does not list is its own canonical unit, in a dimension of its own, as an expression reads a unit missing from
  the screen's table, so the cell is a measure in that unit and not an invalid one; a window lists at most
  `kMaxUnlistedUnits` (64) such units, the rest staying unknown and their cells invalid. **Filters on
  inferred columns.** Until the first reply has fixed the inferred kinds, a filter (stored, literal or bound) is
  held, not compiled against the placeholder: the first query goes without it, and once the kinds are fixed the
  stored filter is fitted to them (each filter it drops is reported once, "the stored filter on `<id>` is not
  applied: it does not apply to the column's kind") and the document's is set, or reported as refused; either is a
  second query, so the first page drawn is unfiltered until the second reply arrives. A filter the user set through
  the table meanwhile wins over a held literal or stored one; any other gesture (a sort, a scroll) does not, since
  a stored state held here arrived in time; a bound filter is the document's and is applied. A held literal or
  bound filter is fitted as a stored one is, so a column filter the inferred kind refuses is reported and dropped
  and the rest applied, where on a table of known kinds the same filter is refused whole. A filter the rows hold that was compiled against the
  placeholders, the user's among them, is fitted again to the inferred kinds, each column filter they refuse
  reported ("the filter on `<id>` is not applied: …") and dropped. A stored state set after the kinds are fixed
  replaces a filter still held. Persisted state is kept as a model's table's, its `table` the action's id. A node the loader would
  refuse (no member marked `x-table`, or neither `columns` nor a view document) is reported at the table when a
  catalog that lacks the lookup let it load, and a `key` that is not one row field is reported and the view
  document's `v-rowKey`, or `id`, is used.
- **Configuring a table**, of either kind (`ScreenCore::configureTable`, over the `ui/table_config.hpp` gestures;
  [`../table/table.md`](../table/table.md), "Configuring a table"). The element keeps the table's columns as its
  layout knows them (id, label, `locked`, kind), its declared hidden columns, the layout it holds and what
  `configurable` allows, across every mount. A table over `rows` takes its columns when it is made: a derived
  `columns` expression is read once there, and a column with no `id` is named `#<n>` by its place, derived columns
  first, and locked. `Table::sort` is the whole chain, each key marking its column.
  `Table::onSort` maps a plain click to `SortGesture::Click` and `Shift` to `Extend` and applies
  `applySortGesture` with the table's bound; a refusal ("A table sorts by at most {n} columns") goes to the status
  line and keeps the chain; a change sets the table's own sort when no key binds one, saves it, and runs
  `onSortChange` with `event` the new sort, in one batch. A column move (`Table::onColumnMove`, `MoveColumnTo`)
  and a resize (`Table::onColumnResize`, `ResizeColumn`, offered only while `resize` is allowed) go through
  `applyLayoutGesture` with the columns and what the table allows; a refusal ("{label} is locked", "This table's
  columns cannot be moved", …) goes to the status line; a change sets the table's own layout when `layout` binds
  none, saves it, and runs `onLayoutChange` with `event` the new layout. `Table::arrangement` follows the layout,
  so the mount shows the visible columns in order, pinned and at their widths, and fetches the columns it shows;
  a layout that is the declared one is the default arrangement. Unless `configurable` is `false`, `Table::menu`
  builds a header's menu (`headerMenu`) or the table's (`tableMenu`) from the chain and the layout of the moment,
  and `Table::onMenu` builds it again and carries out the chosen entry: the sort entries as sort gestures, "Move
  left" and "Move right" as `MoveColumn` by one, "Pin column", "Unpin", "Hide column" and "Reset columns" as their
  layout gestures, and "Columns…" and "Sort…" open the column chooser and the sort dialog (`ui/table_dialogs.hpp`),
  which the lowering mounts beside the table in a column that takes the table's layout and visibility; their open
  state and the group being formed live in the element, so a new mount keeps them. The chooser lists the table's
  columns in the layout's order (a companion is not one of them), and applies `ShowColumn`, `HideColumn`,
  `MoveColumn` by one, `ResetColumns`, `FormGroup` and `RemoveGroup`, which also drops the group filter that names
  the removed group's columns, as a user's filter change; the sort dialog applies `applySortEdit`, whose
  add, remove and direction are the header's sort gestures, so a key past the bound is refused as a Shift-click's
  is. Each change goes the way the same gesture from the header goes: refused in the status line, or set, saved and
  run through `onLayoutChange` or `onSortChange`. "Display unit" sets or, for the checked unit, clears
  `SetDisplayUnit` in the layout. A filterable column (a column part with an id whose `filterable` is not `false`,
  on a model's table also one its description marks filterable) draws a filter button (`Table::onFilter`) that,
  like its menu's "Filter…", opens its filter menu (`ui/table_filter_menu.hpp`), a `FilterMenu` the element keeps
  and a dialog (`table_filter`) mounted beside the chooser; a group, declared or user-formed, is named in its
  members' header menus and in the table's menu ("Filter {group}, {k} columns…"), and while its columns stand side
  by side in the layout as a header over them (`Table::columnGroups`, `Table::onGroupFilter`), each opening the
  group's menu. The menu asks the rows for values (`TableRows::distinctValues`; a table over `rows` keeps the
  listed columns in use while it is open); a search asks again, and a condition is sent,
  `SessionOptions::tableFilterDebounce` (250 ms) after the last edit, while ticks apply on Apply. It sets the filters through one
  path: the rows take them, the table holds them unless the document binds `filters`, they are saved with the
  table's state, and `onFiltersChange` runs with `event` the new filters; a refusal goes to the status line.
  "Clear all filters" sets empty filters the same way. `Table::filtered` names every column with a filter, a hidden
  one included, so the header says so again once it is shown; the filter keeps applying while its column is hidden.
  A table with `configurable: false` has no menus, and keeps its filter buttons and their menu. A table that keeps
  a state has `Views` in its table menu (see Saved views above). A menu entry the lowering does not carry out is not
  offered, and a separator left beside another or at an end goes with it.
  `Ctrl+C` (`Table::onCopy`) and the table menu's "Copy" put the selected rows on the clipboard through
  `IViewBackend::copyText`: in view order, by the visible columns in layout order, each cell's text as its column
  shows it (`formatCell` with the session's formatting and the layout's display units on a model's table; a column
  with its own `cell` copies its value's text, not the template's), written by `ui::copyText`, so a tab or line
  break in a cell is a space. "Copy with headers" adds a first line of the columns' headers, each with its unit
  (`headerText`). With no row selected the focused cell (`Table::onFocusCell`) is copied alone. More than
  `SessionOptions::maxCopyRows` (10,000) selected keys are refused in the status line, "A copy holds at most
  10,000 rows", and nothing is copied. A model's table copies the rows its window holds and fetches the others
  with `TableRows::fetchRows`: keyed queries naming the copied columns, `maxKeys` keys each, through the read that
  makes no cache entry; a key no reply holds is left out. A cell of a level its row has not loaded is copied empty
  and counted, and no copy waits for a level. The status line then says "Copied {n} rows", with ", {m} cells were
  not loaded" when there were any; a failed fetch says "The rows could not be copied: {reason}", and a backend
  whose `copyText` wrote nothing "The rows could not be copied: there is no clipboard". A copy that finds none of
  its rows leaves the clipboard and the status line alone, and a copy's rows that arrive after a later copy started
  are dropped. The slots a mount sets on the table's configuring (its filter targets, display units and copy
  gathering) are cleared when that mount's handlers go, so a slot may hold the mount's scope without keeping the
  screen alive. Each
  gesture the lowering receives clears the status line's message before it acts, so the next gesture on the
  table clears a refusal. The element values `layout`, `visibleColumns` and `hiddenCount` read the layout the
  table holds or the document binds, fitted to its columns.
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
- **A grid of a form.** A `table` with `form` and `field` lowers to `ui::makeFormGrid` over the form element's
  session and the named collection, made when the table is first lowered and kept in the table's element, so every
  mount of the table shows the same grid, its focus, its open editor, its layout, its sort and its filters. Its
  columns are the column parts, each editing the member its `field` names, or a display column showing its `value`,
  read with `row` bound to the row's record (each member's encoded value, `null` when blank), which the walk skips;
  each is labelled by its `label` read once; with no parts, every member of the row type in schema order. A `field`
  expression becomes the column's `GridColumn::fieldOf`, read in the row's scope as `value` is, and a name it gives
  that is no member is reported once (the cell has no field, so it is read-only); such a column sorts and filters
  its drafts as text. The column's `decimals`, `unit`, `readonly`, `options` (each value's JSON literal kept) and
  `placeholder` become its per-row overlays, read in the same scope; `FormGridSource` applies `decimals`, `unit`,
  `options` and `placeholder` to each row's member (`FieldState::places`, `switchUnit`, `narrowOptions`,
  `overridePlaceholder`) in an effect that follows them, a row's `unit` only when it first gives one or gives another
  (converting the draft), so a unit the user picks in the editor stays, and `readonly` is the cell's `CellFacts::overlayReadOnly`.
  `enterMoves`, `walk`, `appendRows` and `rowsFixed` give its `GridOptions`, with the session's `maxPasteCells`, and
  `onActivate` runs with `event` the row's key when `Enter` or a double click lands on a read-only cell. The grid
  selects cells (`Table::cellSelection`, bound to `GridController::selection`); a document's bound `selection` is
  shown through `GridController::select`, and `onSelectionChange` runs with `event` the range after each change of it
  (`GridController::onSelectionChange`); the element value `selection` is the range. `Ctrl+C` on the table copies the
  cell selection (`GridController::copy`) through the backend's clipboard, "Copied 2 rows" in the status line (over
  `maxCopyRows` rows refused, "A copy holds at most 10,000 rows", by `GridController::copyRefusal`), and
  with no cell selection the table's copy runs; `Ctrl+V` pastes the backend's `pasteText` (`GridController::useClipboard`).
  The table menu offers the focused row's `GridController::rowEntries` after its own entries and a separator, and
  `appendRows` draws "Add row" under the table. What a display column and `onActivate` keep holds the scope weakly.
  The grid's `bands` parts become `TableOptions::bands`. A `group` part groups the grid's rows by its `by`, read with
  `row` the row's record, over the array's order (`GridRows::setGroup`, `FormRowsSource::setGroup`): a sort the grid
  is given is led by `kGroupRunColumn`, so each run keeps its rows, and an empty sort stays empty, since the array's
  order already keeps a run together. A grid with a `group` or with `summaries` is mounted over a `SummaryRows` built
  over the grid's rows, which the element keeps: the table's `summaries` before (`position: "top"`) or after its rows,
  each run's after the run, each run's heading (the group's `heading`, read in the run record's scope, as a `rows`
  table's) before it. The grid's walk, paste and cell rectangle stay over the grid's own rows: a click, a move or a
  rectangle's corner that lands on a summary or heading row goes to the nearest data row (`SummaryRows::data`), and
  the grid's focus is shown at its row's mounted index (`SummaryRows::mounted`). A copy of a rectangle copies the rows
  between its corners as they are drawn, a summary row's cells as their text (`GridController::copySummaries`), a
  heading row not at all. A summary cell's text is its part's `cells`
  expression for the column, read with `rows` the records of the data rows in its scope (`summaryScopeRows` over the
  grid's view, so a summary follows the filter) and `column` the column's id, shown at its member's places and empty
  for `null`; a column the part does not name shows an empty cell. A summary cell's test id is `summary_<key>_<column>`,
  a group's `summary_<run ordinal>_<key>_<column>`. Every column's cells are built per row
  (`TableSource::rowScopedCells`), so a cell built for a data row is never rebound to a summary row. Its
  header, menus, cell menu and arrangement are a table's (`configureTable` over a `TableConfig` the grid keeps): an
  edit column whose part says `sortable` sorts by its header, and the sort orders the grid's rows; every edit column
  filters unless its part says `filterable: false`, with its header's filter button, its values list, `onFiltersChange`
  and the table menu's "Clear all filters"; a display column neither sorts nor filters; the layout (the document's
  `layout` while it binds one, else the grid's own, which the header and the column chooser change) arranges the
  columns and is fed to `FormGridSource::setLayout`, so the walk follows the visible columns in layout order. The rows'
  columns are the edit columns by their own ids, so a sort and a filter name a column as the table does. The cell menu
  shows "Copy", "Copy with headers", "Filter by this value" and "Exclude this value", then the grid's "Revert cell" and
  "Revert row", then, unless `rowsFixed`, a separator and "Insert row above", "Insert row below" and "Delete row" (or
  "Delete rows"), then a separator and the document's `cellMenu` entries, read in a row scope with `row`, `column` and
  `value` bound as a table over `rows` reads them. A copy with no row selected copies the focused cell. The scope's
  formatting is followed (`FormGridSource::setFormat`). The grid's notices are posted through `tableMessage`, held as
  a table's messages are, a repeated one posted again, and what stands while its cause lasts is the status line's
  `standing`. A grid the form cannot make (a column naming no member) is reported and drawn as muted text. The form's
  `form_submit` submits through `FormSession::submit`, whose before-submit hooks commit the grid's open editor first.
  Element values `modifiedCount`, `modifiedRows`, `errorCount` and `editing` read the grid. A form field overlay's
  `widget: grid` (or the schema's `x-widget: grid`) draws the field as a grid of every member inside the form's own
  view; with a `grid` part, through a `forms::Overrides::byField` for the field, the same table a `table` node with
  those keys makes, kept in an element of its own on the form's element (`ElementState::fieldGrids`, by the field's
  name, its node the part) so every mount of the form shows the same grid.
- **A fixed grid.** A `table` with `layoutRows` lowers to a `ui::FixedGridSource` over the session of the form
  element `form` names (none without `form`) and a `GridController`, made when the table is first lowered and kept in
  the table's element as a grid of a form is (`GridTable::fixed`, its controller `GridTable::grid`). Each row part
  becomes a `FixedRow`, its `label` read once, and each block part a `FixedBlock` of them; a cell's `field` and `text`
  are kept as given, its `rows` and `columns` as counts, and its `value`, `readonly`, `decimals`, `unit`, `options`
  (each value's JSON literal kept) and `placeholder` become functions read in the table's scope, with `row` bound to
  the block's collection row's record (each member's encoded value) in a block. `FixedGridSource` applies
  `decimals`, `unit`, `options` and `placeholder` to each cell's field in an effect that follows them, as
  `FormGridSource` applies a grid column's (`detail::applyUnitOverlay` for the unit, given as loaded when first
  applied, and again after each load of the form, `FormSession::loadGeneration`, to a field the user has not
  changed), and `readonly` is the cell's `CellFacts::overlayReadOnly`. The source keeps one row per row part and per block row and collection row, in order: a block's rows
  are expanded again on each change of its collection's rows, keyed `"<row key>/<key>"`, so a row's focus, draft and
  marks stay with it while collection rows are added above it. Its view is an `EngineRows` over a `table::VectorSource`
  of the rows' keys, never sorted or filtered. The columns are the column parts, labelled by their `label` read once,
  each of `TableColumn` and `LayoutColumn` kind text and not sortable; `layout` and the column chooser arrange them and
  feed `FixedGridSource::setLayout`, so the walk follows the visible columns and a span is cut where they part. Its
  walk options, `onActivate`, its cell selection, its copy and paste, its status line and its table menu are a grid of
  a form's (`gridOptions`, `gridTableNode`); its copy writes each cell's text as shown, and each row's accessible name
  is its label (`TableRowLook::label`). A grid the source cannot make (a field the form lacks at run time) is
  reported and drawn as muted text. Element values `modifiedCount`, `modifiedRows`, `errorCount` and `editing` read
  it as they read a grid of a form.
- **An editable model column.** A model's table whose description gives a shown column an `EditInfo` mounts through
  `GridController::node(columns, options, base)` over a `ModelGridSource`, `base` the `TableSource` the lowering
  builds for any model table, so every column keeps its `cell`, tone, tooltip and accent, and each cell draws its
  marks from its own row (`markedCell`). No default cell is shared across columns, and a plain cell shows the cell's
  draft while one is held. Each editable column's field is `forms::FormModel::fromSchema` over
  `Catalog::schema(action)`, `value` of a `CellEdit` and `values.<member>` of a `RowEdit`; a column whose action the
  catalog has no schema for has no field, so it is read-only, and one whose schema does not read is reported. A
  column is read-only where `Session::can(action)` does not hold, so a principal the server refuses the action sees
  it read-only. The rows the mount reads are the source's decorator over the table's rows, which ends a row's
  modified marks when the window fetches it. The drafts and commits are the element's `table::CellEdits`, made with
  the table: its commit sends through the model alias's calls, never as a run of the session, so the table's own
  edits make no row busy, one call per `CellEdit` cell and one per `RowEdit` action with every cell of the request
  it edits; a reply's `errors` member, a `FailedRow` of the row or a failure's sentence refuses the cells it names,
  and a success whose `ChangedRows` does not name the row counts the row as fetched at once. A changed row's `from`
  moves the rows' and the drafts' keys, as a mutation's does, and a keyed refetch that finds a row gone drops its
  drafts (`WindowRows::onRowsAbsent`). `editMode` (`cell`, the default, or `row`) chooses when a row's edits are sent;
  `enterMoves` and `walk` give the walk, which follows the layout's visible columns. The grid's refusals reach the
  status line through `tableMessage` ("This value is not loaded yet", "This row is being saved", a locked row's lock
  text, "This cell is read-only"), and the rows' busy and failed states, which are not reactive, reach the cells
  through the table's marks revision. The table keeps its row selection: the grid takes no
  rectangle of cells, no paste and no fill, and `Delete` blanks the focused cell. Focus follows its row's key through
  a key change. Where a focused row the window no longer holds is, and which row takes focus when the focused row
  leaves the view, are the mount's, as for any table: the mount locates the row, and hands the grid the key that
  took focus. Row mode sends no held cell that became read-only meanwhile. A table with no editable column is lowered as a plain model table.
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
  or any later value of a form without a `key`, `prefill`s every field. A form whose model has a computed member
  (at any depth), or that names `derive`, makes rounds of derived values (`FormSession::deriveWith`): `evaluate` is
  the alias's channel `execute(action, body, {.evaluate = true})` while a member is computed, and `derive` is
  `execute(derive, {"body": body})` when the form names one, on the session's timers, `deriveDebounce` (150 ms when
  absent) after a grid's last commit; each path a derive reply names that is no derived member is reported once. It
  lowers to `forms::formView` inside a column that
  carries the node's common properties. `title` (a heading over the fields) and `submitLabel` (the explicit-mode
  button, and the status line's `ready: press <label>`) are bound in the node's scope, so a `t` re-resolves on
  `setLocale`; `showStatus`, `columns`, `fieldLayout` and `captionWidth` are read at load, `columns` setting every group's grid and
  the implicit group's, `fieldLayout` the engine's `FormViewOptions::fieldLayout`, `captionWidth` its `inlineLabelWidth`. `onSubmit`, `onSuccess` and `onError` run once per attempt,
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
  view's heading (`collection_title`, `wizard_title`). A collection's rows are a table on the
  engine, over the session's `source()` with the collator every other table uses, kept in the
  element across mounts: every `v-columns` entry is a column of it (`v-hidden` ones hidden), each
  sortable and filterable, with the header and table menus, the column chooser, the sort dialog and
  copy of any table; then a last column of the row's buttons, which no layout arranges. Each cell
  shows the session's formatted text, with the test id `cell_<field>_<rowKey>`. The engine keys a
  row by its integer key's value and the session by its text; the master-detail selection and a
  row's activation are translated between the two, so the open row stays selected through a sort.
  The arrangement, sort and filters persist under
  `{application, the model alias's type, the collection's action, the screen's id, "state"}`, and
  the rows are held back while that state loads, as a `rows` table's are; its saved views load
  beside that state, and its table menu has `Views`, as every persisted table's do. They have no
  element values and no events. A document the catalog lacks, one for another action, one that does not build, or
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
- **Bars.** A series whose `style` is `bars` lowers to `ChartStyle::Bars` with `joined` false. `barSlot` is the
  smallest positive distance between two distinct positions among the kept points of `Bars` series, gaps included,
  or 1 when there is one such position; it is 0 with no `Bars` series or none with a kept point. The horizontal axis spans every
  position widened by half `barSlot` at each end before its 5 % padding is added. The value axis includes the
  baseline, 0, in the values it spans when a `Bars` series has a value, and pads the baseline's side only when a value
  or a reference line lies beyond 0 on it: with every value and line at or above 0 the axis starts at 0 with no
  padding below, so the baseline is the plot's bottom edge, and with every one at or below 0 it ends at 0, the top
  edge; values of both signs keep the padding at both ends. A fixed `yMin` above 0 or `yMax` below 0 leaves the
  baseline outside the axis, and the frontend clamps it to the nearer end. Dates and instants give `barSlot` in
  milliseconds.
- **Overflow.** A range the checked arithmetic cannot hold is reported once, and the chart draws its summary alone.

### Document pages and locale scopes

`page` and `pageRegion` lower to `ui::Page` and `ui::PageRegion`. `localeScope` lowers to a holder column whose children
mount in a child `RtScope` carrying a formatting context: the session's with the scope's tag, its row of the locale
table and the date patterns the application declared, else the row's, as a reactive value of the session's facts and the
`locale` expression evaluated in the enclosing scope. `RtScope::formatting()` walks to the innermost scope with a context
and answers the session's otherwise; `ScopeEnv::formatting()` and every node-level conversion use it, while entry (the
drafts and places of inputs, a date input's pattern, `parseDecimal` and every parse) stays on the session's facts. A tag
the locale table does not list is reported once per tag through the screen's `report` at the `locale` path, the once-set
held by the `Session`. See [`document_page.md`](document_page.md#the-locale-scope).

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

- The chrome of a `collection` or `wizard` (its header, row, confirm, editor and navigation drawn
  by the host's own kit): C++ code replaces them through `forms::CollectionChrome` and
  `forms::WizardChrome`.
- A `form` node's subtree overrides (spec 2 §7): document subtrees replacing a field's control. A host
  component replaces one through the field overlay's `component`; C++ code replaces controls through
  `forms::Overrides`.
- A `form` node's `showErrors`: a field's error shows on its control as soon as its draft does not
  encode.
- A `table` declared from a model's list action (`model` with `action`) rather than one of its tables.
- Prefetching on hover: the host starts a screen's `prefetch` queries through `Session::prefetch`.

## Where a screen says what it holds

A chat turn says which instances the open screen holds, so an assistant can call the same ones
(see [chat.md](chat.md)). `ModelChannel::facts()` reports the instance a channel is bound to, as `ChannelFacts`, and
`Screen::boundAliases()` lists the aliases of the screen's own scope that hold one. An alias still binding is left out,
and so is one of a nested scope: it is mounted per row or per opening, so it names no one instance.
`DocumentScreenHost::chatContext(handle)` builds a `chat::TurnRequest` from them, with the screen's id, its params and
its identity's canonical text.

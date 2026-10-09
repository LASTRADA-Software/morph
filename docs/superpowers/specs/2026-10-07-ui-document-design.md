# The UI document: server-defined screens for a generic client — design (spec 5)

A morph server defines its screens as data. Each screen is a **UI document**: JSON that declares the
screen's state, the models it talks to, the queries and mutations it runs, the relations between its
elements, and its view tree. A generic client fetches the document at runtime, evaluates it with the
reactive core of spec 1, and renders it: as generated QML on the desktop and in the browser (spec 3),
and on a best-effort basis in a terminal. A screen changes when the server changes, without a new
client build, and a client and a server of different versions agree on what both understand.

This spec defines the document — its structure, its expression language, its control, relation and
command vocabulary, custom components, server-side authoring, and delivery. It is the view
representation of the program: spec 1 §5 refers here.

## Contents

[1 Intent](#1-intent) · [2 Architecture](#2-architecture) · [3 Structure](#3-document-structure) ·
[4 Expressions](#4-expressions) · [5 Models, queries, mutations](#5-models-queries-and-mutations) ·
[6 Relations](#6-relations-between-elements) · [7 Server-calculated values](#7-server-calculated-values) ·
[8 Commands](#8-commands) · [9 View nodes](#9-view-nodes) · [10 Custom components](#10-custom-components) ·
[11 Authoring](#11-authoring-on-the-server) · [12 Delivery](#12-delivery-versioning-and-discovery) ·
[12b Permissions and customization](#12b-permissions-and-per-user-customization) ·
[13 Security](#13-security) · [14 Tests](#14-tests) · [15 Out of scope](#15-out-of-scope)

## 1. Intent

**Goals:**

- **The server owns the UI.** Screens are emitted by the server at runtime, so the server and the
  client can be released independently; the handshake (§12) settles what both understand.
- **Desktop and web first.** The primary renderer is Qt Quick, native and WebAssembly. A terminal
  renderer covers what it can.
- **Live server calculation is the primary interaction.** A user edits one value of a row; the
  client sends the row to a server action; the reply updates the values the server calculates (§7).
- **Complex relations between elements** — visibility, options, validation, derived values and
  cascades that depend on other elements — are declared, not coded (§6).
- **Several models in one screen or dialog**, each addressed by name (§5).
- **Custom components** give screens a designed look and may carry their own logic, such as a chart
  (§10).

**Decisions:**

- **JSON travels, QML is generated.** The wire format is the document; the client generates QML from
  it. A document can be validated and versioned before anything runs, a non-Qt renderer (a terminal,
  a future HTML renderer) reads the same document, and no number passes through QML's JavaScript
  doubles. Delivered code is limited to custom components.
- **Every expression is evaluated in C++ on the client.** QML binds only to finished values. Exact
  integers, decimals and quantities stay exact.
- **UI state lives on the client.** The server is stateless with respect to the UI: no per-session
  view process, no round trip per keystroke, and the offline queue keeps working. Server-side view
  state (the LiveView model) is rejected for those reasons.
- **What the expression library cannot express is computed by the server** and returned as a field
  of an action's reply (§7). The library stays small, total and versioned.
- **The client is generic.** One client binary serves every application. An application that runs
  without a server runs its server parts in-process (local mode, §2) and is rendered the same way.

## 2. Architecture

```text
server (or in-process, local mode)               client (one generic binary)
  models + actions ─────────────┐                ┌─ document interpreter (morph::reactive)
  screen definitions (C++, §11) ├─ UI registry ──┤     values, scopes, queries, mutations
  custom components (QML, §10) ─┘    │           ├─ forms engine (spec 2): form nodes
                                     │           ├─ dispatch: Bridge::executeJson, model-free
             handshake, catalog, ui envelopes    └─ renderers: QML generator (spec 3),
                                                               terminal (best effort)
```

- **Model-free dispatch.** The client sends `(model type, instance, action type, JSON body)` through
  `Bridge` and receives the JSON reply, with the session, timeout, cancellation and in-flight
  accounting `Bridge` applies to typed calls. The client compiles in no model or action type. This
  needs seams `Bridge` does not have today (below).
- **Local mode.** A desktop application without a server links its models and screen definitions
  into the client process: the UI registry is read in-process and dispatch goes to `LocalBackend`.
  The document and its rendering are identical to remote mode.
- **The interpreter** turns a document into reactive nodes (spec 1 §3–4b): state into `Signal`s,
  `let` and every bound property into `Computed`s, queries into `Query`s, mutations into `Mutation`s,
  relations into `Computed`s and `Effect`s. Renderers observe those values; they evaluate nothing.

### Bridge seams this design adds

`BridgeHandler<Model>::executeJson` needs a handler built for a concrete `Model`, and a handler is
created only through templates over `Model`. A client that compiles in no model needs three additions:

- **A binding by type id.** `Bridge::bindByType(typeId, sharing, instanceKey)` returns a
  `HandlerBinding` keyed by the model's registered type id string, with the same registration,
  deregistration, owner principal and `switchBackend` re-binding as a typed handler. Local mode
  resolves the type id in the model registry; a remote backend already carries it as a string.
- **A raw execute.** `Bridge::executeRaw(binding, actionId, bodyJson)` returns the JSON reply with the
  session, timeout, cancellation and in-flight accounting of a typed call. It does not run the
  client-side `recomputeAll` or `ActionValidator`, which need the action type: the forms engine
  validates in the client (spec 2), and the server validates every call.
- **A string-keyed attach.** `attach(binding, instanceKey)` for a shared instance, as
  `attachHandler<Model>` does with a typed key.

`RawHandler` wraps the three as `BridgeHandler` wraps the typed calls: it binds on construction and
deregisters on destruction.

The catalog tells the client, per model type, whether it is **stateless** (a shared singleton is
equivalent to a private instance) and whether it is **keyed** (it can be shared by an instance key). A
model that is not keyed cannot be given an `instance` expression; the document is refused at load.
These seams, the `ui` envelope kinds of §12 and their authorisation rules are specified in the wire
and bridge documents when the interpreter lands, and come before it in the delivery order.

## 3. Document structure

A **bundle** is everything an application serves: its screens, its app shell (menu and routes, the
`app-*` vocabulary of `forms/engine/app_shell.hpp`), and its custom components. A **screen** document:

```json
{ "vocab": "ui/1", "id": "lims.results", "title": { "t": "lims.results.title", "fallback": "Results" },
  "params":    { "sampleId": "int" },
  "models":    { ... },  "state":   { ... },  "let":    { ... },
  "queries":   { ... },  "mutations": { ... }, "watch": [ ... ], "checks": [ ... ],
  "view":      { ... } }
```

- **Names.** Every declaration has a name matching `[A-Za-z_][A-Za-z0-9_]*`, unique within its
  scope. A reference resolves in the innermost scope first, then outward.
- **Scopes.** The screen is a scope. So is every dialog, every `switch` case, and every row of a
  `forEach` or `table`. Any scope may declare `models`, `state`, `let`, `queries`, `mutations`,
  `watch` and `checks`. A scope is created when its content mounts and destroyed when it unmounts. A
  row scope is keyed by its row key, so its state survives reordering and is kept while the row
  exists.
- **Params** are the screen's typed inputs, set by navigation (§8).
- **Lifecycle keys.** A screen may also declare `title` (an expression), `dirty` (a bool expression),
  `identity` (an expression; equal identities are one screen), `onMount`, `onUnmount`, `onClose`
  (command lists; `{"cancelClose": true}` is valid only in `onClose`), `onSave` and `onBackendChange`. Their
  semantics, and the host's side of them, are spec 6 §3–4. These keys, the commands `cancelClose`,
  `signIn` and `signOut`, and the node and expression additions of spec 6 are part of `ui/1`.
- **Types.** `bool`, `int` (int64, exact), `string`, `decimal` (`{num, den, dp}`, exact),
  `quantity` (`{num, den, dp, unit}`), `timestamp` (UTC), `key` (`int` or `string`), `list<T>`,
  records, and `T?` for an optional value (`null`). Query and mutation results take their record
  types from the action schemas the server already emits; a document refers to them by action id
  and never restates them.
- **Load-time validation.** A client refuses a document before mounting it when a reference does not
  resolve, a type does not check, the `let` graph or the `watch` graph has a cycle (§6), an
  expression exceeds the size bound (§4), or a node kind or function is not in the document's
  `vocab`. The refusal names the document, the path inside it and the reason. The server runs the
  same validation when a screen is registered (§11), so a refused document is a server defect found
  in the server's own tests.

## 4. Expressions

An expression is a JSON tree. Every bound property, query body, condition and command argument is
one.

| Form | Meaning |
|---|---|
| a JSON scalar, or `{"lit": v}` | A constant. `lit` wraps an object or array. |
| `{"ref": "name.path"}` | A param, state, `let`, element value (§6) or record field, by name then path. |
| `{"row": "path"}` | A field of the innermost row. |
| `{"query": "q"}`, `{"pending": "q"}`, `{"error": "q"}` | A query's value, whether it is in flight, its error text. |
| `{"result": "m"}`, `{"pending": "m"}`, `{"error": "m"}` | A mutation's last result, in flight, error. |
| `{"can": "m"}` | Whether the signed-in principal may run mutation or query `m` (§12b). |
| `{"pref": "point.path"}` | The user's choice at a customization point (§12b). |
| `{"fn": "name", "args": [e, ...]}` | A library function (below). |
| `{"t": "key", "fallback": "text", "args": {"name": e}}` | The text of `t(key, fallback, args)`: shorthand for the library function. |
| `{"ref": "event"}` | The payload of the event whose command list is running: the new text, the chosen key, the dropped payload. Valid only inside a command list. |
| `{"if": [cond, then, else]}` | Choice. |
| `{"obj": {"k": e}}`, `{"list": [e, ...]}` | Constructors, used for action bodies. |
| `{"map": e, "as": "x", "to": e}`, `{"filter": e, "as": "x", "where": e}` | Comprehensions over a list. |
| `{"env": "widthClass"}` | Client environment: `widthClass` (`compact`, `medium`, `expanded`), `locale`, `platform`, `online`, `backend` (spec 6 §5, §9). |
| `{"errorKind": "m"}` | The kind of query or mutation `m`'s error: `notFound`, `validation`, `connectionLost`, `denied`, `conflict`, `backendChanged`, `other` (spec 6 §10). |
| `{"session": "displayName"}` | A session attribute: `principal`, `userName`, `displayName`, `userKey`, `tenant`, `authenticated`. |
| `{"ref": "app.name"}` | A name of the app document (spec 6 §5). |
| `{"host": "name"}` | The value an application registered with the client (`host/1`, spec 6 §14); `null` when the client lacks it. |

**Semantics.**

- Expressions are pure and total. There is no recursion, no user-defined function and no loop other
  than a comprehension over a list the data already holds, so evaluation always ends.
- **Absence propagates.** A function whose required argument is `null` yields `null`. Logic is
  three-valued, as `x-rules` is: `and`, `or` and `not` follow Kleene logic over `true`, `false` and
  `null`. A condition that is `null` presents as its declared default (§6).
- **Errors are values.** Overflow, division by zero or a type mismatch at runtime yields `null` and
  is reported once per expression path on the client's diagnostic channel; it never throws into a
  renderer.
- **Size bound.** A document whose single expression exceeds 256 nodes is refused at load.

**Function library `expr/1`.** Every function is exact where its inputs are:

- logic and comparison: `and`, `or`, `not`, `eq`, `ne`, `lt`, `le`, `gt`, `ge` — comparing `int`,
  `decimal` and `quantity` exactly, converting quantity units by their declared exact factors;
- arithmetic: `add`, `sub`, `mul`, `div`, `neg`, `round(x, dp)` over `int`, `decimal`, `quantity`;
- optional: `engaged`, `coalesce`;
- string: `concat`, `format(template, args...)`, `trim`, `length`, `isEmpty`, `contains`;
- list: `count`, `sum`, `mean`, `min`, `max`, `any`, `all`, `first`, `includes`, `indexOf`
  (`mean` is the exact sum over the count, at the operands' declared places, rounded half to even);
- display: `number`, `decimal(x, dp)`, `quantity(x, unit?)`, `money(x, currency)`,
  `date(x, style)`, `dateTime(x, style)` — using the client's locale and display zone through
  `render/locale_format.hpp`;
- text: `t(key, fallback, args)`, the i18n lookup of `render/i18n.hpp` (a `TranslationProvider`
  over `(key, locale)`). A message and its `fallback` are Unicode MessageFormat 2 patterns, and `args`
  is an object of named arguments. `ui/1` accepts the subset of MessageFormat 2 that a UI needs:
  placeholders (`{$count}`), `:number` and `:string` formatting, and `.match` on a `:number` (CLDR
  plural categories for the active locale, and exact values such as `=0`) or on a `:string` (a
  select). So `".input {$n :number} .match $n one {{{$n} sample}} * {{{$n} samples}}"` reads
  "1 sample" and "3 samples" in English and takes Polish's or Arabic's forms from their catalogues.
  A pattern outside the subset is refused when the catalogue loads, and the fallback is shown.

The forms engine's `x-rules` (spec 2 §3) translate into this language, so a client has one
evaluator.

## 5. Models, queries and mutations

### Models

```json
"models": {
  "sample":  { "type": "Lims_SampleModel", "instance": { "ref": "sampleId" } },
  "catalog": { "type": "Lims_AnalysisCatalogModel" },
  "clients": { "type": "Lims_ClientModel" }
}
```

- A scope names every model it uses, under an alias. A screen or a dialog may use any number of
  models; each query and mutation names the alias it goes through.
- `instance` is `"private"`, `"shared"` or an expression:
  - an expression selects a shared model instance (the bridge's shared-instance key); when it
    changes, the alias rebinds, and the queries through it refetch. The model must be keyed;
  - `"private"` binds an instance of its own to the scope, so the model's state, such as an open
    record, is that scope's alone; it is released when the scope unmounts (spec 6 §3);
  - `"shared"` is the model's singleton and needs no key.
  An alias that names none is `"shared"` when the catalog marks the model stateless and `"private"`
  otherwise.
- A private instance costs a server-side model instance and, for a database-backed model, its own
  connection. A row scope's alias must therefore name an `instance` (a document that leaves it out
  there is refused at load), and the server's `maxLiveModels` limit is the brake on the total.
- A shared instance runs on one strand, so calls through it serialise, whichever scopes make them.
- A model handle belongs to the scope that declares it. A dialog that declares its own handle
  releases it when it closes.

### Queries

```json
"results": { "model": "sample", "action": "Lims_ListResults",
             "body": { "obj": { "sampleId": { "ref": "sampleId" } } },
             "when": true, "debounce": 0, "refreshOn": ["Lims_CaptureConcentration"], "refreshEvery": 0 }
```

A query is spec 1 §4b's `Query`, declared as data:

- `body` is the key. When it changes, the query fetches under a new generation; **the latest
  request wins** and a reply from an older generation is dropped.
- `when` false, or a `body` that evaluates to `null`, makes the query idle.
- `debounce` delays a fetch until the key has been stable for that many milliseconds.
- The last value is kept while a refetch is in flight, so lists do not blank.
- `refreshOn` names action ids of the query's model type: the query refetches when a call to one of
  them, made by this client through any alias of that model type in any mounted screen, completes
  successfully. It matches by model type, not by instance, because two private instances of a
  database-backed model read the same rows: a capture in one tab must refresh the list in another. It
  is the interpreter's own bookkeeping over its dispatch (through the query cache, below), not a
  bridge subscription, so it works without the result type and two actions with one reply type do not
  trigger each other. A change made by another client is not pushed; `refreshEvery` is the polling
  answer, and it refetches on a timer and **skips a tick while a request is in flight**.
- `staleTime` (milliseconds, default 0) is how long a cached value counts as fresh (below).

### The query cache

Queries are declared per scope but stored per client session, so screens that show the same data
share it.

- **Key.** A query's cache key is `(model type, instance, action, canonical body)`. The instance is
  the shared key, or the private binding the alias holds; the canonical body is the evaluated body's
  JSON with sorted object keys. Two queries with equal keys — in one screen, in two tabs, in the app
  root scope — share one entry: one request in flight, one value, one error.
- **Freshness.** A query that mounts on an entry younger than its `staleTime` uses the cached value and
  sends nothing; an older entry is shown at once and refetched (the value is kept while it refetches,
  as for any query). An entry no mounted query uses is kept for 5 minutes, then dropped.
- **Invalidation.** `invalidates`, `refreshOn` and `{"refetch": "q"}` mark entries stale and refetch
  those a mounted query uses, in one batch. `invalidates` names queries of the mutation's own document;
  `refreshOn` reaches every mounted screen (above).
- **Isolation.** The cache belongs to the session: `signOut` and a principal change clear it, and a
  backend switch (spec 6 §9) marks every entry stale. A `secret` field is never cached (§13).
- **Prefetch.** A screen may declare `"prefetch": ["q", ...]`: queries whose bodies depend only on the
  screen's `params`. A navigator entry, a menu item or a row whose activation would open the screen
  starts them on hover or keyboard focus, after 150 ms, so the screen mounts on a warm cache. A
  prefetch is a query like any other, so it is authorised as one and counts against the server's
  limits.

The reactive core's `Query` (spec 1 §4b) is unchanged: the interpreter builds each one with a
fetcher that goes through the cache.

### Mutations

```json
"capture": { "model": "sample", "action": "Lims_CaptureConcentration", "body": { ... },
             "concurrency": "exclusive", "invalidates": ["results"],
             "patch": { "query": "results", "key": { "ref": "result.id" }, "with": { "ref": "result" } },
             "onSuccess": [ ... ], "onError": [ ... ] }
```

- `concurrency` decides what a run does while another is in flight:
  - `exclusive` (default): the run is refused. Elements bound to `{"pending": "capture"}` disable.
  - `serial`: runs are queued and sent in order; `serialKey` (an expression) makes one queue per key.
  - `latest`: every run is sent; only the newest reply is applied.
  - `parallel`: every run is sent; replies are applied as they arrive.
- `invalidates` refetches the named queries after a success, in one batch with the result, so the
  screen updates in one frame.
- `patch` replaces one row of a query's cached list (the row whose key equals `key`) with `with`,
  without refetching. `key` and `with` are evaluated against the reply, bound as `result`.
- `optimistic` has `patch`'s shape — `{"query": "q", "key": e, "with": e}` — but is applied when the run
  starts, evaluated against the run's body bound as `body`. The reply replaces it (and `patch`, when
  declared, applies to the reply); a failure rolls the row back to its value before the run, and
  `onError` runs after the rollback. Only client-owned fields may be written optimistically: a field
  the server calculates (§7) is never predicted, and a document whose `with` names one is refused at
  load. Overlapping optimistic runs on one row roll back in reverse order, so the row ends at its
  last confirmed value.
- `onSuccess` and `onError` are command lists (§8). The reply is readable as `{"result": "capture"}`
  until the next run.
- `"offline": "queue"` sends a run that cannot reach the server through the client's offline queue,
  which replays it on reconnect; the mutation stays `pending` until the replay settles, and its
  elements show `stale`. Without it, a run while offline fails at once. Offline queuing is part of
  `ui/1`; a replay the server rejects runs the mutation's `onError`, as an online failure would. A
  mutation that sends a credential or returns a token is never queued: it is refused at load with
  `"offline": "queue"`.
- A reply field the server marks `secret` (a session token) is never readable as `{"result": "m"}`
  after the mutation's own `onSuccess` has run, and is left out of diagnostics, the offline queue and
  journals.

### Several models in one dialog

A dialog that registers a client, then a sample for that client, then schedules its analyses, uses
three models and one sequence:

```json
"onClick": { "seq": [
  { "run": "createClient", "as": "c" },
  { "run": "registerSample", "body": { "obj": { "clientId": { "ref": "c.id" }, "matrix": { "ref": "matrix.value" } } }, "as": "s" },
  { "all": [ { "run": "scheduleAnalysis", "each": { "ref": "analyses.selection" }, "as": "a",
               "body": { "obj": { "sampleId": { "ref": "s.id" }, "analysisId": { "ref": "a" } } } } ] },
  { "close": "registerDialog" } ] }
```

- Each step's reply is bound under its `as` name for later steps.
- A failed step stops the sequence and runs the failing mutation's `onError`. Steps already applied
  stay applied: a sequence across models **is not atomic**.
- When the steps must commit together, the server exposes one action that performs them, and the
  dialog runs that action. Atomicity is a server decision, never a client sequence. Composite
  actions that span several models in one transaction are not part of this design.

## 6. Relations between elements

Every element may carry an `id`. An element's live values are addressable as `{"ref": "id.value"}`:

| Element | Values |
|---|---|
| any | `visible`, `enabled`, `valid`, `errors`, `shownErrors` |
| text input, date, slider | `value`, `draft`, `dirty`, `focused`, `touched` |
| checkbox | `checked` |
| select | `selected`, `selectedOption` |
| table | `selection`, `activeKey`, `sort`, `filters`, `viewCount`, `sourceCount`, `selectedCount`, `pending`, `error`, `hoveredKey` (spec 7 §12) |
| dialog, drawer | `open` |
| a transfer (`upload`, `download` with an `id`) | `progress` (0 to 1, or `null`), `pending`, `error` (spec 6 §18) |
| form (spec 2) | each field's `value`, `draft`, `valid`, `required`, `touched`, and the form's `ready`, `body`, `submitted` |

Relations are expressions over those values and the scope's declarations:

| Relation | Declared as |
|---|---|
| show, enable, require, make read-only | the element's `visible`, `enabled`, `required`, `readonly` |
| options that depend on another element | `options` from a query whose `body` references it; `onStale: "clear" \| "keep"` decides what happens to a selection the new options no longer contain |
| a value derived on the client | a `let` entry; `let` entries may reference each other |
| a value derived by the server | a query or mutation (§7) |
| validation across elements | a `checks` entry (below) |
| a cascade (A changes, so B resets) | a `watch` entry (below) |
| an aggregate over rows | a `let` with `sum`, `count`, `map`, `filter` over a query's rows |

**Checks.**

```json
"checks": [ { "id": "dilutionRange", "assert": { "fn": "le", "args": [ { "ref": "dilution.value" }, { "ref": "limits.maxDilution" } ] },
              "message": { "t": "lims.dilution.tooHigh", "fallback": "Dilution above the method limit" },
              "targets": ["dilution"], "blocking": true } ]
```

A check fails when `assert` is `false`; `null` does not fail it, because an incomplete input blocks
through `required` instead. A failing check adds its message to each target's `errors` and clears
its `valid`. A dialog's or form's `ready` holds when every required element is engaged, every element
is valid and no blocking check fails. The server re-validates every action it receives: client checks
are guidance, never authority.

**Showing errors.** `errors` is what is wrong now; `shownErrors` is what the renderer displays, and
is what an element's error text binds to by default. A dialog, form or screen declares when the two
meet with `showErrors`:

- `"touched"` (the default): an element's errors show once it is `touched` — it has lost focus at
  least once — or once the enclosing form or dialog is `submitted`;
- `"submit"`: errors show only once it is `submitted`;
- `"always"`: errors show at once.

`submitted` becomes true when a run of the dialog's or form's submitting mutation is attempted,
whether or not `ready` refused it, and is reset by `{"reset": ...}` of the dialog's state. A required
field the user has not reached therefore does not show "required" while they fill the fields above it.

**Watches.**

```json
"watch": [ { "on": { "ref": "analysis.selected" }, "source": "user",
             "do": [ { "set": "unit", "value": null }, { "set": "dilution", "value": null } ] } ]
```

A watch runs its commands when the value of `on` changes; never on the first evaluation.
`source: "user"` restricts it to changes caused by the user's own edits, so a server patch or a
prefill does not trigger the cascade; `source: "any"` (the default) runs for every change. Watches
run after the flush that changed the value, in declaration order, as one batch.

**Ordering and cycles.**

- The `let` graph must be acyclic.
- A `watch` that writes state another `watch` observes forms an edge. A cycle among watches is
  refused at load. At runtime, the reactive core's per-flush effect limit stops a cycle that only
  data can create.

## 7. Server-calculated values

**The use case.** A LIMS result row holds ten values. The user edits one. The client sends the row's
values to a declared server action. The server calculates the derived quantities, such as a
concentration from mass, volume and dilution, and replies. The client updates the calculated cells of
that row. Calculation stays on the server, where it is authoritative and has access to model state;
the client shows the result as soon as it arrives.

Two declarations cover it.

**Preview: calculate without saving.** The row scope keeps a draft and runs a query keyed on it:

```json
{ "kind": "table", "rows": { "query": "results" }, "key": { "row": "id" },
  "scope": {
    "state":   { "draft": { "init": { "row": "inputs" } } },
    "queries": { "calc": { "model": "sample", "action": "Lims_EvaluateResult", "debounce": 300,
                           "body": { "obj": { "resultId": { "row": "id" }, "inputs": { "ref": "draft" } } } } } },
  "columns": [
    { "label": "Mass",   "cell": { "kind": "textInput", "id": "mass", "commitOn": "change",
                                   "value": { "ref": "draft.mass" }, "onChange": { "set": "draft.mass", "value": { "ref": "event" } } } },
    { "label": "Result", "cell": { "kind": "text",
                                   "text": { "fn": "quantity", "args": [ { "fn": "coalesce", "args": [ { "ref": "calc.concentration" }, { "row": "concentration" } ] } ] },
                                   "stale": { "pending": "calc" } } } ] }
```

**Write-through: save each edit.** Committing a value runs a mutation that persists it and replies
with the recalculated row. `concurrency: "serial"` with `serialKey` set to the row id sends one row's
edits in order. The mutation's `patch` replaces the row in the list, so the calculated cells update
without refetching the list.

**Rules for both:**

1. **Ownership.** Inputs are client-owned drafts. Calculated values are server-owned. A reply never
   overwrites an input the user changed after the request was sent: each request carries the
   draft's generation, and the client applies a reply's input values only to inputs whose draft is
   unchanged since that generation. The server may normalise an untouched input, such as rounding it
   to its declared decimals.
2. **Triggering.** An input's `commitOn` is `change`, `blur` or `enter`. `debounce` on the query
   batches fast typing into one request.
3. **Latest wins, per row.** A reply from a superseded request is dropped. Rows are independent:
   editing row B while row A is in flight affects neither.
4. **Staleness.** While a calculation is in flight, its cells keep the last value and carry `stale`,
   which the renderer shows, for example by dimming. They never blank.
5. **Errors.** A reply's field errors (`errors: {field: message}` in the action's result) attach to
   the row's elements of the same name. A transport failure becomes the row's error and keeps the
   stale value.
6. **Exactness.** Inputs and results travel as `int`, `decimal` and `quantity`, and are formatted in
   C++.
7. **Offline.** A preview that cannot reach the server leaves the cells stale and shows the row's
   error. A write-through edit goes through the client's offline queue when its mutation declares
   `"offline": "queue"` (§5).

**Preview through `evaluate`, with no dedicated action.** When the calculation depends only on the
action's own fields — its `computedFields` — a query asks the server to evaluate a body of the action
the row will eventually run:

```json
"calc": { "model": "sample", "evaluate": "Lims_CaptureConcentration", "debounce": 300,
          "body": { "obj": { "resultId": { "row": "id" }, "mass": { "ref": "draft.mass" },
                             "volume": { "ref": "draft.volume" }, "dilution": { "ref": "draft.dilution" } } } }
```

The server answers an `evaluate` envelope by decoding the body, running `recomputeAll` and the
action's validation, and replying with the body, its computed fields filled, and its field errors.
It never calls the model's `execute`, so an evaluation has no effect and can be repeated freely. It
is authorised as the action itself: a principal who may not run the action may not evaluate it. An
evaluation writes no journal or action-log entry, counts against the server's in-flight and rate
limits per principal, and is not protected by the client's `debounce`, which only spares an honest
client. A calculation that reads model state — a calibration, a method limit — is a dedicated action
whose model computes it.

**On the server,** every calculation is authoritative: `computedFields` are recomputed on every
execute and every evaluation, whatever the client sent.

## 8. Commands

| Command | Effect |
|---|---|
| `{"set": "name.path", "value": e}` | Writes state, or an element's draft. |
| `{"reset": "name"}` | Restores state to its `init`. |
| `{"run": "m", "body": e?, "as": "x"?, "each": e?}` | Runs a mutation, optionally with a body override, once per element of `each`. |
| `{"refetch": "q"}` | Refetches a query. |
| `{"open": "dialog"}`, `{"close": "dialog"}` | Opens or closes a dialog. |
| `{"navigate": "screen", "params": e, "as": "tab"\|"dialog"\|"drawer"\|"window", "onResult": [...], "onCancel": [...]}` | Opens another screen: as a tab the shell decides (spec 6 §7), as a dialog, drawer or window it can answer (spec 6 §16). |
| `{"return": e}`, `{"close": "self"}` | Ends a screen opened as a dialog, drawer or window: the opener's `onResult` gets `e`, or its `onCancel` runs. |
| `{"focus": "id"}` | Moves keyboard focus to a node (spec 6 §17). |
| `{"host": "name", "args": e?}` | Runs a command an application registered with the client (`host/1`, spec 6 §14). |
| `{"switchBackend": "name"}` | Asks the application's `AppSource` to switch to a backend it lists (spec 6 §9). |
| `{"resetPrefs": "point"\|"all"}` | Restores the document's defaults for a customization point, or for all of them (§12b). |
| `{"setLocale": "de"}` | Changes the active locale and re-resolves every `t`; stored as a preference (spec 6 §19). |
| `{"notify": e, "role": "success"}` | Shows a transient message. |
| `{"seq": [...]}`, `{"all": [...]}` | Runs commands in order, stopping at a failure; or together, waiting for all. |
| `{"if": [cond, [...], [...]]}` | Runs one branch. |
| `{"cancelClose": true}` | Keeps the screen open; valid only in `onClose` (spec 6 §3). |
| `{"retry": true}` | Refetches the failed queries of the innermost `boundary`; valid only inside a boundary's `error` (§9). |
| `{"signIn": {"from": "m"}}`, `{"signOut": true}` | Installs the session token mutation `m` replied with and reloads the catalog; or ends the session and unmounts every screen (spec 6 §5). |
| `{"upload": e, "to": "m", "as": "x"?, "id": "t"?}` | Sends a file the user picked or dropped through the server's file side channel, then runs mutation `m` with its reference (vocabulary `files/1`); `id` names the transfer for `progress`. |
| `{"download": e, "save": "dialog", "name": e?}` or `{"download": e, "to": "preview", "as": "url"}` | Fetches a file by reference through the side channel and lets the user save it, or binds a local temporary reference to state `url` for a viewer (`files/1`, spec 6 §18). |

A command list triggered by one event runs in one batch: the screen updates once.

Every screen instance has a **route**, the string form of its id and params (spec 6 §20). `navigate`
accepts one in place of a screen id, `{"navigate": {"route": e}}`, which is how a link, a bookmark
and the browser's address bar open a screen.

## 9. View nodes

This section is the one normative palette of the program (spec 1 §5 refers here). The kinds are text,
button, text input, checkbox, select, menu, column, row, grid, spacer, panel, scroll, switch, tabs,
dialog, busy, forEach, table, date-time input, slider and file picker, written as
`{"kind": "...", ...}` with every property an expression and every event a command list.

**Common properties.** `id`, `visible`, `enabled`, `a11y`, `testId`, `tooltip`, `keys` and `autofocus`
(spec 6 §17), `surface` (a named token set for the subtree, spec 6 §6), `sizing` (`"content"`,
`{"fixed": n}` or `{"stretch": w}`; units are spec 3 §8's), `gap` on containers, and `columns` and
`span` on grids.

**Events** (command lists; the payload is `{"ref": "event"}`): `onClick` and `onActivate` (button,
row, menu item), `onChange` (every edit), `onCommit` (Enter or loss of focus), `onToggle`,
`onChoose`, `onDismiss` (dialog and drawer), `onSubmit` (a single-line text input), `onPick` (file
picker), `onDrop` (a node with `drop`, or a `dropZone`), and `on: {name: [...]}` for a custom component's own events.

**Drag and drop.** A node may declare `"drag": {"payload": e}` and `"drop": {"accepts": e,
"onDrop": [...]}`. A drag carries the payload, never a node; a drop target runs `onDrop` with the
payload as `{"ref": "event"}` when the dragged payload matches `accepts`. A file picker or a `dropZone`
panel also accepts files the operating system drops on it, as `{"ref": "event"}` references usable by
`upload` (`files/1`). A renderer that cannot drag ignores `drag` and `drop`.

**Further kinds.** Each has a built-in fallback in the terminal renderer (a text line or a plain
list) and is restylable (§10).

| Kind | Properties and events |
|---|---|
| `banner` | `tone`, `text`, `action` (`{label, onClick}`), `dismissible`, `onDismiss` |
| `badge` | `tone`, `text`, `icon` |
| `progress` | `value` (0 to 1; `null` is indeterminate), `label` |
| `steps` | `items` (`[{label, state}]`), `current` |
| `keyValue` | `items` (`[{label, value}]`) |
| `emptyState` | `title`, `text`, `icon`, `action` |
| `drawer` | As `dialog`, plus `side` (`start` or `end`) |
| `splitter` | `orientation`, `sizes` (the user's sizes are a `layout` customization point), children |
| `collapsible` | `title`, `open`, `onToggle`, `header` (nodes shown beside the title), children |
| `dropZone` | `accept`, `multiple`, `onDrop` (spec 6 §18) |
| `customize` | `point`: the editor of a customization point (spec 6 §19) |
| `boundary` | `loading`, `error` (node lists), children. Shows `loading` until every query read inside its children has a first value, `error` when one of them has failed with no value to keep, and the children otherwise. Inside `error`, `{"ref": "boundary.error"}` is the first failure's text and `{"retry": true}` refetches every failed query of the boundary. A refetch of a query that has a value does not leave the children: per-element `stale` covers it. Boundaries nest; the innermost one owns a query. |

A `menu` item is `{label, icon, keys, checked, enabled, onClick, items}`; `items` nests a submenu and
`checked` makes the item checkable. A status bar, a timeline, a diff table, a readout tile and a
signature strip are compositions of the kinds above or host components, not kinds. `icon` names an
entry of the theme's icon map (spec 6 §6).

The document adds:

- `id` on any node (§6); `readonly`, `required`, `errors` and `stale` on inputs and text;
- `a11y` (`{"name": e, "role": "..."}`) and `testId` on any node: the accessible name and role, and a
  stable identifier for automated tests (spec 6 §6);
- `form`: a schema form of the forms engine (spec 2), `{"kind": "form", "model": "sample",
  "action": "Lims_CaptureConcentration", "prefill": e, "submit": "explicit"}`, whose fields are
  addressable as elements, which may carry the `overrides` of spec 2 §7 and a `fields` overlay
  (`omit`, `hidden`, `label`, `unit`, `decimals`, `widget`, `blankAs`) applied to the parsed form
  model before the session is built (spec 6 §10);
- `collection` and `wizard`, the forms engine's list and wizard screens;
- `custom` (§10);
- editable cells: a table cell is any node, and its row scope is the row's. A `table` may also be
  declared from a model's list action, with its columns derived from the row type and its sorting,
  filtering, selection and paging provided by the table engine (spec 7);
- **adaptive layout:** `grid.columns`, `visible` and sizing accept expressions over
  `{"env": "widthClass"}`, so one document lays out for a phone-width browser, a laptop and a large
  screen.

## 10. Custom components

```json
"components": {
  "ConcentrationChart": {
    "qml": "sha256:4b1e...",
    "props":  { "series": "json", "title": "string", "limit": "exact" },
    "events": { "pointSelected": "key" },
    "fallback": { "kind": "table", "rows": { "ref": "props.series" }, "...": "..." } } }
```

```json
{ "kind": "custom", "name": "ConcentrationChart",
  "props": { "series": { "ref": "trend" }, "title": "Trend", "limit": { "ref": "limits.max" } },
  "on":    { "pointSelected": [ { "set": "selectedResult", "value": { "ref": "event" } } ] } }
```

- A component is a QML file delivered in the bundle and identified by its content hash. It receives
  its declared props and emits its declared events. It may contain JavaScript for its own behaviour,
  such as scaling, hit-testing or animation in a chart.
- **Prop types** say how a value crosses into QML: `string`, `bool`, `int` (as text, so no id loses
  precision), `exact` (an exact decimal or quantity, as text), `number` (a double, for charts, where
  rounding is acceptable), and `json` (structured data, exact values inside it as text).
- **Containment.** A component sees its props and nothing else: no session, no bridge, no other
  element. Its only way out is its declared events, which run the document's commands.
- **Restyling built-in kinds.** The bundle's `restyle` map names a component for a built-in kind,
  such as `"restyle": {"button": "BrandButton"}`; the component must have the same props and events,
  which is checked when the server registers the bundle and again when the client loads it.
- **Host components.** A component may instead name one the client carries, `"host": "ResultGrid"`
  with a `version`, in place of `"qml"`. It needs no hash and no verified TLS connection, because it
  is part of the client binary; the handshake (§12) says which ones a client has. Theme tokens
  (`ui-theme/1`) come first, host components second, delivered components third (spec 6 §6). Because
  a document is data that loads on any connection the client accepts, a host component's props and
  event payloads are untrusted input, and a component with a side effect is usable only from a
  verified connection (§13).
- **Hover link.** A custom or host component may declare the event `pointHovered` (a key, or
  `null`) and the prop `highlightKey`, so a chart and a table that share keys highlight each other:
  the table's `hoveredKey` element value feeds the chart's `highlightKey`, and the chart's event
  sets the table's `highlightKey`.
- **Fallback.** Every component names a fallback subtree of built-in nodes. A renderer that cannot
  load the component — a terminal, a client without the right Qt version, a failed hash check —
  renders the fallback.

## 11. Authoring on the server

Screens are written in C++ with typed builders that produce the document:

```cpp
auto screen = ui::screen<"lims.results">([](ui::Scope& s) {
    auto sampleId = s.param<int64_t>("sampleId");
    auto sample   = s.model<SampleModel>("sample", {.instance = sampleId});
    auto results  = s.query<ListResults>(sample, {.sampleId = sampleId});
    return ui::table(results, ui::key(&ResultView::id), [&](ui::RowScope& r) {
        auto draft = r.state("draft", r.field(&ResultView::inputs));
        auto calc  = r.query<EvaluateResult>(sample, {.resultId = r.field(&ResultView::id), .inputs = draft},
                                             {.debounce = 300ms});
        return ui::cells(
            ui::textInput({.value = draft.at(&ResultInputs::mass), .commitOn = ui::Commit::Change}),
            ui::text({.text  = ui::fn::quantity(calc.value().at(&Evaluated::concentration)
                                                    .orElse(r.field(&ResultView::concentration))),
                      .stale = calc.pending()}));
    });
});
MORPH_REGISTER_SCREEN(screen);
```

- Operators on expression handles build trees: `a + b`, `a < b`, `a && b`, `!a`, `x.orElse(y)`.
- Fields are member pointers and actions are types, so a renamed field or a wrong body type is a
  compile error on the server. Query and mutation bodies are the action's own struct with expression
  handles in place of values.
- Registration emits the JSON once and runs the load-time validation of §3; a screen that fails is a
  failing server test, never a client surprise.
- Registration also runs the **accessibility rules**: every text input, date-time input, slider, select,
  checkbox and file picker has a label or `a11y.name` (a form field takes its field's label); a button
  without text, every `custom` node, every `table` and every `dialog` or `drawer` has `a11y.name` or a
  title; an `icon` alone never names a node. A screen that breaks a rule fails registration, with the
  path and the rule. The client does not repeat these rules: they guard authoring, and a client must
  not refuse a document an older server registered.
- The builders cover every construct of the document: the lifecycle keys (`identity`, `title`,
  `dirty`, `onMount`, `onUnmount`, `onClose`, `onBackendChange`), `refreshOn` over action types,
  `form` with its `fields` overlay, `dataTable`, `custom` and host components, and the command
  forms. The reference screen of spec 6 §11 is the first full use and fixes the API.
- Inline lambdas cannot appear in a screen. A value the library cannot express is a server-computed
  field.

## 12. Delivery, versioning and discovery

- **Handshake.** The wire `hello` is unchanged: it is unauthorised, carries no session and names no
  model, so it cannot carry an application's inventory. The UI service has its own envelope kinds,
  each added to the wire kind table:

  | Kind | Carries | Authorisation |
  |---|---|---|
  | `ui-hello` | Request: the vocabularies the client speaks (`ui/1`, `expr/1`, `forms/1`, `ui-theme/1`, `files/1`), its Qt version, whether it loads custom components, its host components, host commands and host values with versions. Reply: the application's id and version, the vocabularies it accepts, the manifest digest | An authoriser call for the pseudo-action `ui/hello`, so an application that serves a login screen before sign-in allows it for an empty principal |
  | `ui-catalog` | The catalog and app shell for the principal | The principal's session |
  | `ui-fetch` | A document, component or theme by hash | The session, except for entries the catalog marks `public` (a login screen) |
  | `ui-evaluate` | An `evaluate` request (§7) | As the action it evaluates |
  | `ui-file` | The file side channel (`files/1`) | As the mutation that references the file |

  A server without the UI service answers `unknown envelope kind`; the client then reports that the
  peer serves no UI. An old client against a new server is unchanged, since it sends none of these
  kinds. A client that is missing a vocabulary the bundle needs is refused after `ui-hello` with a
  message naming it. New kinds are additive and do not bump `kProtocolVersion`: a peer without the UI
  service answers `unknown envelope kind: ui-hello`, which a client reads as "no UI service", the way
  it reads the same reply to `hello` as a peer that predates the handshake. The kinds are added to
  the wire document's kind table, with their authorisation rules, as part of the change.
- **Catalog.** A `ui-catalog` request returns the application's screens and its app shell, filtered
  for the signed-in principal (§12b). A screen document is fetched by id. Each entry carries `id`,
  `title`, `module`, `group`, `icon`, `singleton`, `available`, `reason` and `whenDenied` (`hide` or
  `disable`), `public`, `host` and `reuseEmpty`, so a navigator can list what an application has not built, disabled with its reason
  (spec 6 §8). The bundle may also carry an **app document** and a **theme** (spec 6 §5–6).
- **Content addressing.** The bundle's manifest lists every document, component, theme and message catalogue by SHA-256. The
  client verifies each file against the manifest, and the manifest against the digest from the
  `ui-hello` reply, before using it. The digest is only as trustworthy as the channel that carried it. Verified files are cached by hash; an unchanged manifest fetches
  nothing, and a client without a connection starts from its last verified bundle.
- **Versions.** Within a vocabulary version, changes are additive: a client ignores an optional key it
  does not know, unless the key's name begins with `!` (`"!requires"`), which marks it
  must-understand: a client that does not know it refuses the node and renders its fallback, or
  refuses the document. A new node kind, function or command is a new vocabulary version
  (`ui/2`, `expr/2`). A server may keep documents for more than one version and sends the newest the
  client speaks.
- **Development reload.** A client started in development mode re-sends `ui-hello` every second. When
  the manifest digest changes, it fetches the changed entries and remounts the mounted screens whose
  documents changed, keeping each scope's state where the new document still declares it under the
  same name and type, and the screen's identity. A remount that fails validation keeps the old
  document and reports the refusal. Production clients do not poll: the digest is read once per
  connection.

## 12b. Permissions and per-user customization

A screen's document is the same for every user of a given vocabulary version, so it is cached and
verified by one hash, and validated and tested once. What differs between users comes from three
places, none of which changes the document:

- **Permissions.** By default (`whenDenied: "hide"`) the catalog lists a screen only when the
  principal may run every query the screen declares at its top level, and the app shell's menu omits
  what the catalog omits; an entry with `whenDenied: "disable"` is listed, disabled, with the reason
  `denied`. Within a screen, `{"can": "m"}` is true when the principal may run `m`: the catalog reply
  carries the principal's permitted action ids for the application, answered by the same
  `IAuthorizer` the server applies to each call. The set is a snapshot: the client asks again after
  `signIn` and after any reply of error kind `denied`. A screen binds `visible` or `enabled` to it, so a verifier sees a
  Verify button and a technician does not. The server still authorises every call; `can` decides
  only what is shown.
- **Data.** What a user sees in a list or a form is what the queries return for that user.
- **Customization points.** A document declares where a user may adjust it:

  ```json
  "customize": {
    "resultColumns": { "kind": "columns", "of": "resultsTable" },
    "density":       { "kind": "choice", "options": ["comfortable", "compact"], "default": "comfortable" },
    "resultFilter":  { "kind": "value", "type": "string?", "default": null } }
  ```

  - `columns` lets the user reorder, hide and resize a table's columns; `choice` and `value` hold one
    typed setting, readable in expressions as `{"pref": "density"}`; `layout` lets the user collapse
    panels and choose the default tab of a `tabs` node; `saved` keeps named sets of `value` points,
    such as saved filters.
  - The app document may declare a `customize` block as well, with two more kinds: `pins` (an ordered,
    optionally grouped list of catalog ids) and `arrange` (an ordered list of the declared children
    of a container node, each enabled or hidden; it orders and hides, and never positions or sizes).
  - The client stores each choice through an `IPreferencesStore` (spec 6 §8), by default a framework
    preferences model on the server, keyed by the person the server derives from the verified
    principal, the application, the screen (or `app`) and the point id, so a user's choices follow
    them to every device. A key the client supplies is never trusted; an application with its own
    storage supplies its own store, which maps the principal to the person on the server. A stored
    value has a size bound. Choices are fetched with the screen and written as the user makes them;
    offline, they are kept locally and written on reconnect.
  - A choice that no longer fits its point — a column the document no longer has, an option it no
    longer offers, a value of the wrong type — is ignored, so a new document version never breaks on
    an old preference.
  - A user can reset a screen's customization to the document's defaults.

## 13. Security

- The server is trusted with code execution: custom components are code. Loading a component
  therefore requires a verified TLS connection to the server, or loopback with an explicit
  development flag. Documents are data and load on any connection the client accepts.
- Documents cannot execute code: expressions are total and bounded, commands are a closed set, and
  every action they run is authorised by the server like any other call.
- Components run in a dedicated QML engine whose context holds only their props and event sinks.
- An `evaluate` request is authorised as the action it evaluates, and is limited per principal (§7).
  A preference is readable and writable only by its own principal, which the preferences model checks
  against the verified session, since the authoriser sees no request body.
- A host component runs client code with props a document chose. Props and event payloads are
  untrusted input and a host component validates them. A component that opens files, reaches the
  network or touches the host marks itself `safeForUntrusted: false`, and the client mounts it only
  when the connection is verified; on any other connection its fallback renders.
- A session token never appears in reactive state after its use, in diagnostics, in the offline
  queue or in a journal (§5).

## 14. Tests

- **Document validation:** a corpus of valid and invalid documents, each invalid one with the path
  and reason the refusal must name.
- **Expressions:** a shared JSON corpus of expressions, inputs and results, covering exact arithmetic
  across units, null propagation, three-valued logic, every function, and the size bound.
- **Control:** query latest-wins under forced reply orders; `debounce`; `refreshEvery` skipping while
  pending; each mutation concurrency mode; `patch`; one-batch invalidation.
- **Relations:** cascading `watch` with `source: "user"` against a server patch; check targets and
  `ready`; dependent options with `onStale`; cycle refusal.
- **Server-calculated values (§7):** replies out of order; typing while a calculation is in flight;
  an input edited after its request was sent; field errors; transport failure; serial write-through
  per row; edits in two rows at once.
- **Several models:** a sequence across three models; failure in the middle; rebinding when an
  `instance` changes.
- **Evaluate:** computed fields and field errors come back, the model's `execute` is never called,
  and a principal who may not run the action is refused.
- **Offline:** a queued mutation stays pending and stale until its replay, and a rejected replay runs
  `onError`.
- **Permissions:** the catalog omits a screen whose queries the principal may not run, and `can`
  follows the principal's permitted actions.
- **Customization:** each point kind round-trips through the preferences model; an outdated choice is
  ignored; reset restores the defaults.
- **Hosting (spec 6 §12):** lifecycle keys, the host contract, the app document and session, the
  design package, the workspace shell, application-scope preferences, the backend switch, the form
  overlay and error kinds.
- **Palette:** each further kind renders, falls back in the terminal, and is restylable; menus nest
  and check; `tooltip`, `surface`, `keys` and `autofocus` apply; `mean` is exact.
- **Commands:** `navigate` as a dialog returns through `onResult` or `onCancel`; `focus`; `host`
  with a missing command; `switchBackend` for a listed and an unlisted name; `resetPrefs`;
  `setLocale`.
- **Authoring:** every registered screen of every example validates; builder output matches golden
  JSON.
- **Compatibility:** an old client against a new server and the reverse, through the `ui-hello`
  handshake; a server without the UI service; a `!`-prefixed key an old client does not know.
- **Bridge seams:** a binding by type id, a raw execute and a string-keyed attach behave as their
  typed counterparts for session, timeout, cancellation, accounting and `switchBackend`.
- **Dispatch refresh:** `refreshOn` fires on the named action through the alias and not on another
  action with the same reply type.
- **Query cache:** two screens with an equal key send one request; `staleTime` spares a remount's
  fetch; a mutation in one tab refreshes, through `refreshOn`, a query of the same model type in
  another; `signOut` clears the cache; a prefetch warms the entry the screen then mounts on.
- **Optimistic:** the row shows `with` before the reply, the reply replaces it, a failure rolls it back
  before `onError` runs, two overlapping runs end at the last confirmed value, and an optimistic write
  to a calculated field is refused at load.
- **Showing errors:** each `showErrors` policy against `touched` and `submitted`.
- **Boundary:** first load, failure without a value, `retry`, a refetch that keeps the children, and
  nesting.
- **Messages:** plural categories for English, Polish and Arabic from the CLDR rules; a `.match` on a
  string; a pattern outside the subset falls back.
- **Accessibility rules:** each rule refuses a screen at registration with its path.
- **Routes and reload:** a route round-trips through `navigate`; a changed document remounts in
  development mode with its state kept by name, and a refused one leaves the old one mounted.
- **Security:** a credential-bearing mutation is refused with `"offline": "queue"`; a `secret` field
  is absent from `result`, diagnostics and the queue; a side-effecting host component renders its
  fallback on an unverified connection.

## 15. Out of scope

- **Free layout editing.** A user adjusts a screen only through its declared customization points
  (§12b); positioning or sizing panels freely is not part of this design. Ordering and hiding the
  children a document declares is the `arrange` point.
- **Server push.** A document learns of a change by its own calls, by `refreshOn` over its own
  dispatch and by polling with `refreshEvery`. A server-initiated message (pushed progress, another
  client's change) needs a wire kind the protocol does not have; it is a candidate for `ui/2`, as a
  `ui-event` kind negotiated by `ui-hello`.
- **Server-side composite actions.** Committing several models in one transaction is not part of this
  design; a dialog that must commit atomically runs one server action (§5).

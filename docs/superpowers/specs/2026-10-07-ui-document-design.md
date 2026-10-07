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
  accounting `Bridge` already applies to typed calls. The client compiles in no model or action type.
- **Local mode.** A desktop application without a server links its models and screen definitions
  into the client process: the UI registry is read in-process and dispatch goes to `LocalBackend`.
  The document and its rendering are identical to remote mode.
- **The interpreter** turns a document into reactive nodes (spec 1 §3–4b): state into `Signal`s,
  `let` and every bound property into `Computed`s, queries into `Query`s, mutations into `Mutation`s,
  relations into `Computed`s and `Effect`s. Renderers observe those values; they evaluate nothing.

## 3. Document structure

A **bundle** is everything an application serves: its screens, its app shell (menu and routes, the
`app-*` vocabulary of `forms/app.hpp`), and its custom components. A **screen** document:

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
| `{"if": [cond, then, else]}` | Choice. |
| `{"obj": {"k": e}}`, `{"list": [e, ...]}` | Constructors, used for action bodies. |
| `{"map": e, "as": "x", "to": e}`, `{"filter": e, "as": "x", "where": e}` | Comprehensions over a list. |
| `{"env": "widthClass"}` | Client environment: `widthClass` (`compact`, `medium`, `expanded`), `locale`, `platform`. |

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
- list: `count`, `sum`, `min`, `max`, `any`, `all`, `first`, `includes`, `indexOf`;
- display: `number`, `decimal(x, dp)`, `quantity(x, unit?)`, `money(x, currency)`,
  `date(x, style)`, `dateTime(x, style)` — using the client's locale and display zone through
  `render/locale_format.hpp`;
- text: `t(key, fallback, args...)`, the i18n lookup of `render/i18n.hpp`.

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
- `instance` selects a shared model instance (the bridge's shared-instance key) and is an expression:
  when it changes, the alias rebinds, and the queries through it refetch.
- A model handle belongs to the scope that declares it. A dialog that declares its own handle
  releases it when it closes.

### Queries

```json
"results": { "model": "sample", "action": "Lims_ListResults",
             "body": { "obj": { "sampleId": { "ref": "sampleId" } } },
             "when": true, "debounce": 0, "refreshOn": ["Lims_ResultChanged"], "refreshEvery": 0 }
```

A query is spec 1 §4b's `Query`, declared as data:

- `body` is the key. When it changes, the query fetches under a new generation; **the latest
  request wins** and a reply from an older generation is dropped.
- `when` false, or a `body` that evaluates to `null`, makes the query idle.
- `debounce` delays a fetch until the key has been stable for that many milliseconds.
- The last value is kept while a refetch is in flight, so lists do not blank.
- `refreshOn` refetches when the model publishes one of the named events; `refreshEvery` refetches
  on a timer and **skips a tick while a request is in flight**.

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
- `onSuccess` and `onError` are command lists (§8). The reply is readable as `{"result": "capture"}`
  until the next run.
- `"offline": "queue"` sends a run that cannot reach the server through the client's offline queue,
  which replays it on reconnect; the mutation stays `pending` until the replay settles, and its
  elements show `stale`. Without it, a run while offline fails at once. Offline queuing is part of
  `ui/1`; a replay the server rejects runs the mutation's `onError`, as an online failure would.

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
| any | `visible`, `enabled`, `valid`, `errors` |
| text input, date, slider | `value`, `draft`, `dirty`, `focused` |
| checkbox | `checked` |
| select | `selected`, `selectedOption` |
| table | `selection`, `activeKey` |
| dialog | `open` |
| form (spec 2) | each field's `value`, `draft`, `valid`, `required`, and the form's `ready`, `body` |

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
is authorised as the action itself: a principal who may not run the action may not evaluate it. A
calculation that reads model state — a calibration, a method limit — is a dedicated action whose
model computes it.

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
| `{"navigate": "screen", "params": e}` | Moves to another screen. |
| `{"notify": e, "role": "success"}` | Shows a transient message. |
| `{"seq": [...]}`, `{"all": [...]}` | Runs commands in order, stopping at a failure; or together, waiting for all. |
| `{"if": [cond, [...], [...]]}` | Runs one branch. |
| `{"upload": e, "to": "m", "as": "x"?}` | Sends a file the user picked through the server's file side channel, then runs mutation `m` with its reference (vocabulary `files/1`). |
| `{"download": e, "save": "dialog"}` | Fetches a file by reference through the side channel and lets the user save it (`files/1`). |

A command list triggered by one event runs in one batch: the screen updates once.

## 9. View nodes

The node palette is spec 1 §5's — text, button, text input, checkbox, select, menu, column, row,
grid, spacer, panel, scroll, switch, tabs, dialog, busy, forEach, table, date-time input, slider,
file picker, with drag-and-drop on every node — written as `{"kind": "...", ...}` with every
property an expression and every event a command list. The document adds:

- `id` on any node (§6); `readonly`, `required`, `errors` and `stale` on inputs and text;
- `form`: a schema form of the forms engine (spec 2), `{"kind": "form", "model": "sample",
  "action": "Lims_CaptureConcentration", "prefill": e, "submit": "explicit"}`, whose fields are
  addressable as elements;
- `collection` and `wizard`, the forms engine's list and wizard screens;
- `custom` (§10);
- editable cells: a table cell is any node, and its row scope is the row's;
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
- **Restyling built-in kinds.** The bundle may map a built-in kind to a component with the same
  props and events, such as `"button": "BrandButton"`, which restyles every button of the
  application.
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
- Inline lambdas cannot appear in a screen. A value the library cannot express is a server-computed
  field.

## 12. Delivery, versioning and discovery

- **Handshake.** The client's `hello` lists the vocabularies it speaks (`ui/1`, `expr/1`,
  `forms/1`), its Qt version and whether it loads custom components. The reply carries the
  application's id and version and the digest of the UI bundle's manifest. A client that is missing a
  vocabulary the bundle needs is refused at connect time with a message naming it.
- **Catalog.** A `catalog` request returns the application's screens and its app shell, filtered
  for the signed-in principal (§12b). A screen document is fetched by id.
- **Content addressing.** The bundle's manifest lists every document and component by SHA-256. The
  client verifies each file against the manifest, and the manifest against the digest from the
  handshake, before using it. Verified files are cached by hash; an unchanged manifest fetches
  nothing, and a client without a connection starts from its last verified bundle.
- **Versions.** Within a vocabulary version, changes are additive: a client ignores an optional key it
  does not know. A new node kind, function or command is a new vocabulary version. A server may keep
  documents for more than one version and sends the newest the client speaks.

## 12b. Permissions and per-user customization

A screen's document is the same for every user of a given vocabulary version, so it is cached and
verified by one hash, and validated and tested once. What differs between users comes from three
places, none of which changes the document:

- **Permissions.** The catalog lists a screen only when the principal may run every query the
  screen declares at its top level, and the app shell's menu omits what the catalog omits. Within a
  screen, `{"can": "m"}` is true when the principal may run `m`: the catalog reply carries the
  principal's permitted action ids for the application, answered by the same `IAuthorizer` the
  server applies to each call. A screen binds `visible` or `enabled` to it, so a verifier sees a
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
  - The client stores each choice through a framework preferences model on the server, keyed by
    principal, application, screen and point id, so a user's choices follow them to every device.
    Choices are fetched with the screen and written as the user makes them; offline, they are kept
    locally and written on reconnect.
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
- An `evaluate` request is authorised as the action it evaluates. A preference is readable and
  writable only by its own principal.

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
- **Authoring:** every registered screen of every example validates; builder output matches golden
  JSON.
- **Compatibility:** an old client against a new server and the reverse, through the handshake.

## 15. Out of scope

- **Free layout editing.** A user adjusts a screen only through its declared customization points
  (§12b); rearranging a screen freely, such as moving dashboard panels, is not part of this design.
- **Server-side composite actions.** Committing several models in one transaction is not part of this
  design; a dialog that must commit atomically runs one server action (§5).

# The UI document — design

Design spec for the UI document's C++ model and its load-time validation
(`include/morph/ui/document.hpp`), and for the typed values every document expression evaluates to
(`include/morph/ui/value.hpp`). A server defines a screen as a JSON document (spec 5,
`docs/superpowers/specs/2026-10-07-ui-document-design.md`); `ui::loadDocument` parses it into a
`ui::Document` and refuses it, with the JSON Pointer and the reason, when it breaks a rule of
`ui/1`. The interpreter mounts only a document that loaded; the server runs the same function when
a screen is registered, so a refused document is a server defect found in the server's own tests.

Read this before changing what a document may say. The expression language inside it is
[`expressions.md`](expressions.md); the view tree a document becomes is
[`view_tree.md`](view_tree.md).

## Contents

- [Values and types](#values-and-types)
- [The document](#the-document)
- [Declarations](#declarations)
- [Commands](#commands)
- [View nodes](#view-nodes)
- [Scopes and names](#scopes-and-names)
- [Load-time rules](#load-time-rules)
- [The catalog](#the-catalog)
- [Refusals](#refusals)
- [Warnings](#warnings)
- [Design decisions](#design-decisions)
- [Out of scope](#out-of-scope)

## Values and types

`ui::Value` is null, `bool`, `int` (int64), `string` (UTF-8), `Decimal`, `Quantity`, `timestamp`
(`time::DateTime`), a list (`ValueList`) or a record (`ValueRecord`, ordered by field name).

- **Exact numbers.** A `Decimal` is a `math::Rational` whose `DecimalPlaces` are the value's declared
  places; a `Quantity` adds a unit id. Places are part of equality (`1.5` at one place and `1.50` at
  two differ), so a reactive equality skip never hides a change of display; `expr/1`'s `eq` compares
  magnitudes.
- **Sharing.** A value is immutable; lists and records share their storage between copies, so the
  copy every reactive read makes costs a reference count.
- **JSON form** (`valueFromJson`, `valueToJson`). Structural: an object of exactly `num`, `den`,
  `dp` is a decimal, with `unit` a quantity — the wire form of `Rational`. A number with a fraction
  or an exponent is refused (an exact value is never a double), as is an integer beyond int64 or a
  zero denominator. A timestamp has no structural form; `valueFromJson(json, type)` reads it from
  ISO-8601 text when the type says `timestamp`, widens an `int` to a decimal when the type says
  `decimal`, and checks the result against the type.
- **Types** (`ui::Type`, `parseType`): `bool`, `int`, `string`, `decimal`, `quantity`, `timestamp`,
  `key` (an `int` or a `string`), `record`, `list<T>`, each optionally `T?`, and `any` for what a
  document cannot know — a query's result takes its type from the action schema, which the document
  never restates. `Type::admits` decides whether a value is of a type: `null` only for `T?` and
  `any`, an `int` for `decimal`.

## The document

`ui::Document` is a screen, or the app document (id `app`, spec 6 §5), which has no view and may
declare the app-only customization kinds.

| Member | JSON key | Notes |
|---|---|---|
| `vocab` | `vocab` | Must be `ui/1` |
| `id` | `id` | Non-empty |
| `title`, `identity`, `dirty` | same | Expressions; `dirty` is a condition |
| `params` | `params` | Name to type, in declaration order |
| `scope` | `models`, `state`, `let`, `queries`, `mutations`, `watch`, `checks` | The screen scope's declarations |
| `lifecycle` | `onMount`, `onUnmount`, `onClose`, `onSave`, `onBackendChange` | Command lists |
| `keys` | `keys` | Chord to command list |
| `showErrors` | `showErrors` | `touched`, `submit` or `always` |
| `prefetch` | `prefetch` | Screen queries started before the screen mounts |
| `components` | `components` | Name to `Component` |
| `customize` | `customize` | Name to `CustomizePoint` |
| `view` | `view` | The root `DocNode`; required on a screen |
| `warnings` | — | What validation found that does not refuse the document ([Warnings](#warnings)) |

A key a client does not know is ignored, so additions within `ui/1` are compatible; a key whose
name begins with `!` is must-understand, and an unknown one refuses the document wherever it
appears (spec 5 §12).

## Declarations

A scope's declarations are a `ScopeDecl`, each kind in declaration order.

| Declaration | JSON | Model |
|---|---|---|
| Model alias | `"alias": {"type": "T", "instance": "private" \| "shared" \| e \| {"from": alias}}` | `ModelDecl`, `InstanceMode` `Default`, `Private`, `Shared`, `Keyed` (a key expression) or `Borrowed`; `"shared"` on a model the catalog does not mark stateless is refused |
| State | `"name": {"type": "T"?, "init": e?}` | `StateDecl`; without a type, the type is the init's |
| Derived | `"name": e` under `let` | `LetDecl` |
| Query | `{"model": alias, "action" \| "evaluate": id, "body", "when", "debounce", "refreshOn", "refreshEvery", "staleTime"}` | `QueryDecl`; exactly one of `action` and `evaluate` |
| Mutation | `{"model", "action", "body", "concurrency", "serialKey", "invalidates", "patch", "optimistic", "onSuccess", "onError", "offline"}` | `MutationDecl`, `Concurrency`, `PatchDecl` |
| Patch | `{"query": q, "at": member, "key": e, "with": e}` under `patch` and `optimistic`; `at` is optional and not empty | `PatchDecl` |
| Watch | `{"on": e, "source": "any" \| "user", "do": commands}` under `watch` | `WatchDecl`, `WatchSource` |
| Check | `{"id", "assert": e, "message": e, "targets": [ids], "blocking": bool}` under `checks` | `CheckDecl` |

An `instance` string other than `private` and `shared` is a constant key. Durations are
non-negative integers of milliseconds. `serialKey` is valid only with `"concurrency": "serial"`, and reads the run's body as `body`
(`event` is not in scope there: a run started by a button and one started by a drop key the same).

## Commands

A command list is an array of commands, or one command object. `Command` carries every command of
`ui/1` (spec 5 §8 and spec 6) as a `CommandKind` and a few generic members:

| Member | Holds |
|---|---|
| `target` | The name acted on: `set`'s `name.path`; the state of `reset`; the mutation of `run`, `signIn`, `upload`; the query of `refetch`; the element of `open`, `close`, `focus`; the screen of `navigate`; the name of `host`, `switchBackend`, `setLocale`; the point of `resetPrefs`; the state a `download` to preview binds |
| `value` | `set`'s value; `run`'s body; `navigate`'s params; `return`'s value; `host`'s args; `notify`'s message; `if`'s condition; the file of `upload` and `download` |
| `extra` | `run`'s `each`; `navigate`'s `route`; `download`'s suggested `name` |
| `binding` | `run`'s and `upload`'s `as` |
| `option` | `navigate`'s `as`; `notify`'s `role`; `download`'s `dialog` or `preview` |
| `transfer` | `upload`'s `id`, whose `progress`, `pending` and `error` are element values |
| `commands`, `alternative` | `seq` and `all` steps; `if`'s branches; `navigate`'s `onResult` and `onCancel` |

`run`'s `as` names the element of `each` inside its body when `each` is given, and the reply for
the later steps of the enclosing `seq` otherwise. `if` takes two or three parts: a missing else
runs nothing.

## View nodes

A `DocNode` is a node or a structured part of one. Its properties stay keyed by their document
keys — `props` (expressions), `events` (command lists, a custom node's events as `on.<event>`),
`children` (node lists), `parts` (records such as columns and tabs), `names` (plain strings) and
`keys` — so the interpreter reads a kind's properties by name. One table in `document.hpp`
(`detail::kKinds`) says, per kind, which keys it has and what shape each takes; it is the only
place a kind is described.

**Every node** takes `id`, `visible`, `enabled` (conditions), `tooltip`, `sizing`, `span`,
`autofocus`, `testId`, `surface` (strings), `a11y` (`{name, role}`), `drag` (`{payload}`), `drop`
(`{accepts, onDrop}`) and `keys`. **Text and the inputs** also take `readonly`, `required`, `stale`
(conditions) and `errors`.

| Kind | Keys (a ⓘ key is in the node's own scope) | Element values beyond `visible`, `enabled`, `valid`, `errors`, `shownErrors` |
|---|---|---|
| `text` | `text`, `role` | |
| `button` | `label`, `icon`, `onClick`, `onActivate` | |
| `textInput` | `value`, `placeholder`, `mode`, `commitOn` (`change`, `blur`, `enter`), `label`, `onChange`, `onCommit`, `onSubmit` | `value`, `draft`, `dirty`, `focused`, `touched` |
| `checkbox` | `label`, `checked`, `onToggle`, `onChange` | `checked` |
| `select` | `options`, `selected`, `style`, `label`, `onStale` (`clear`, `keep`), `onChoose`, `onChange` | `selected`, `selectedOption` |
| `menu` | `items`: parts `{label, icon, keys, checked, enabled, onClick, items}` | |
| `column`, `row` | `children`, `gap` | |
| `grid` | `children`, `gap`, `columns` | |
| `spacer` | | |
| `panel` | `title`, `padding`, `collapsible`, `collapsed`, `children` | |
| `scroll` | `axis`, `children` | |
| `switch` | `on` (required), `cases`: parts `{when, scope, children ⓘ}`, `default` | |
| `tabs` | `tabs`: parts `{label, children}`; the node keeps the user's pick, so picking a tab mounts and shows its page | |
| `dialog`, `drawer` | `open`, `title`, `showErrors`, `side`, `scope`, `children` ⓘ, `default` ⓘ, `cancel` ⓘ, `onDismiss` | `open`, `ready`, `submitted` |
| `busy` | `active`, `label` | |
| `forEach` | `rows` (required), `key` ⓘ, `axis`, `gap`, `scope`, `children` ⓘ | |
| `table` | `rows`, or `model` and `action` with `body` and `mode`; `key` ⓘ, `columns` (parts `{id, label, width, cell ⓘ}`, or an expression), `selectionMode`, `selection`, `sort`, `filters`, `rowTone` ⓘ, `highlightKey`, `onActivate` ⓘ, `scope` | `selection`, `activeKey`, `sort`, `filters`, `viewCount`, `sourceCount`, `selectedCount`, `pending`, `error`, `hoveredKey` |
| `dateTimeInput` | `value`, `mode`, `label`, `onChange`, `onCommit` | as `textInput` |
| `slider` | `value`, `min`, `max`, `step`, `label`, `onChange`, `onCommit` | as `textInput` |
| `filePicker` | `accept`, `multiple`, `mode`, `label`, `onPick` | |
| `banner` | `tone`, `text`, `dismissible`, `action` (`{label, onClick}`), `onDismiss` | |
| `badge` | `tone`, `text`, `icon` | |
| `progress` | `value`, `label` | |
| `steps` | `items`, `current` | |
| `keyValue` | `items` | |
| `emptyState` | `title`, `text`, `icon`, `action` | |
| `splitter` | `orientation`, `sizes`, `children` | |
| `collapsible` | `title`, `open`, `header`, `children`, `onToggle` | |
| `dropZone` | `accept`, `multiple`, `children`, `onDrop` | |
| `customize` | `point` (required) | |
| `boundary` | `loading`, `error`, `children` | |
| `form` | `model`, `action` (required), `prefill`, `key` (the identity of the record `prefill` holds), `submit` (`explicit` or `auto`; absent keeps the schema's mode), `showErrors`, `fields` (field name to overlay of expressions), `onSubmit`, `onSuccess`, `onError` | `ready`, `body`, `submitted`, `pending`, and each field as a record of `value`, `draft`, `valid`, `required`, `visible`, `readonly` and `errors` (`form.field.value`) |
| `collection`, `wizard` | `model`, `action` (required) | |
| `custom` | `name` (required), `props` (record of expressions), `on` (record of command lists) | |

`gap` and `padding` are spacing steps, 0 to 5 ([`view_tree.md`](view_tree.md)); the mount clamps any other value.

A node list is an array of nodes or one node. `nodeKinds()` lists the kinds.

## Scopes and names

- **Scopes.** The screen is a scope; so is every `dialog` and `drawer`, every `switch` case, and
  every row of a `forEach` or `table`. A scope-creating node writes its declarations under
  `"scope"`. A component's fallback is a scope of its own with no parent: it sees its `props` and
  nothing of the screen.
- **What a scope declares.** Its params (the screen), model aliases, state, `let`, queries,
  mutations, the ids of the elements inside it that belong to no nested scope, and the transfer ids
  of the `upload` commands in it. All share one namespace per scope; a name declared twice is
  refused, and `event`, `app`, `boundary`, `props`, `result` and `body` are reserved.
- **Resolution.** A name resolves in the innermost scope first, then outward. Bound names come
  before declarations: a comprehension's variable, a `seq` step's reply, an `each` element, `result`
  in a `patch`, `body` in an `optimistic`, `props` in a fallback. `event` is valid inside a command
  list, `boundary.error` and `retry` inside a boundary's `error`, `row` inside a row scope and any
  scope nested in one, `cancelClose` inside `onClose`. `app.name` reads the app document and is not
  checked against it.
- **What a reference reads.** A param or state reads its type; a `let` the type of its expression;
  a query its value (`any`); an element one of its values, which the reference must name; a transfer
  `progress`, `pending` or `error`. A mutation is read through `result`, `pending` and `error`, a
  model alias not at all.

## Load-time rules

`loadDocument` returns the first refusal; each rule has a case in
`tests/ui/corpus/documents.json`.

- **Vocabulary.** `vocab` is `ui/1`; node kinds, commands, expression forms and functions are those
  of `ui/1`; no unknown `!` key.
- **Shape.** Each key has the shape its kind gives it; required keys are present; names match
  `[A-Za-z_][A-Za-z0-9_]*`; closed choices (`showErrors`, `concurrency`, `source`, `offline`,
  `commitOn`, `onStale`, `navigate`'s `as`, `notify`'s `role`) take one of their values; chords are
  modifiers (`Ctrl`, `Shift`, `Alt`, `Meta`, once each) and a key name, and are not the workspace
  shell's (`Ctrl+Tab`, `Ctrl+Shift+Tab`, `Ctrl+W`, `Ctrl+K`).
- **Expressions.** Each parses and has at most 256 nodes; each name resolves as above; a function's
  statically known argument kinds are ones it takes; `visible`, `enabled`, `readonly`, `required`,
  `stale`, `open`, `when`, `dirty`, a check's `assert` and every `if` condition are `bool` where
  their type is known; a literal `t` fallback is in the message subset.
- **Types.** A state's `init` has its declared type; a `set` of typed state has a value of that type.
- **Cycles.** The `let` graph, the state inits and the watch graph are acyclic. A watch's edges run
  from what it writes (`set`, `reset`) to the watches that read it in `on`, directly or through
  `let`; a watch that writes what it reads is a cycle of one. The refusal names the cycle in order.
- **Targets.** `set` names state, an input (`textInput`, `checkbox`, `select`, `dateTimeInput`,
  `slider`) or `app.name`; `reset` state or a dialog; `run`, `signIn` and `upload` a mutation;
  `refetch` a query; `open` and `close` a dialog or drawer (`close` also `self`); `focus` an element;
  `resetPrefs` a point or `all`; a download to preview a state. A check's targets are elements;
  `invalidates`, `patch` and `optimistic` name queries of the document.
- **Models.** Queries, mutations and model-backed nodes name a model alias in scope. An alias in a
  row scope names its `instance`; only a screen's own aliases borrow (`{"from": alias}`).
- **Components and points.** A custom node names a declared component and only its props and
  events; a component names exactly one of `qml` (`sha256:` and 64 lower-case hex digits) and `host`
  with an integer `version`, prop types from `string`, `bool`, `int`, `exact`, `number`, `json`, and
  a fallback. A `customize` node names a declared point; a `columns` point names a table; a `choice`
  lists its options and defaults to one of them; a `value` point declares its type and its default
  has it; an `arrange` point's `of` names a `column` or `row` of the same screen whose children
  all have ids, no other `arrange` point names the same container, and the app document, which has no view,
  may not declare one; `pins` belong to the app document.
- **Prefetch.** Each name is a screen query whose body reads nothing but the screen's params.

## The catalog

`ui::Catalog` holds two lookups the caller supplies: `model(typeId)` → `ModelInfo{stateless,
keyed}` and `action(actionId)` → `ActionInfo{modelType, calculatedFields, secretFields,
credential, listFields, effects, records}`. In local mode the caller answers them in-process from the model registry; on the
server, registration answers them from the same registry; a remote client answers them from the UI
service's `ui-catalog` reply. With them, a document is also refused when a model type or action is
unknown, an `instance` expression names a model that is not keyed, an action belongs to another
model type, `refreshOn` names an action of another model type, an `optimistic` `with` writes a field
the server calculates, or a mutation that sends a credential or returns a secret is queued offline.
`listFields`, when present, says the query's action answers a record and names the record's members
that hold a list; a `patch` or `optimistic` of such a query is then refused without an `at` (it could
never find a list) or with an `at` that is not one of them. When it is absent, the catalog does not
know the result's shape, and a patch that finds no list is reported by the interpreter when it runs.
`effects` and `records` are what the core registries hold for the action: `effects` is the list of
`{kind, record?}` the action declared, `kind` one of `updates`, `changes`, `changesInstance` and `none`, `record` the
record's wire name for the first two; it is absent when the action declared nothing, which makes the action a read.
A record derived (`derivedFrom`) from one the action `updates` or `changes`, directly or through others, is listed as
`changes` too, once. The derivation itself is never sent: a client knows only that the derived record changes, which
lets the caller's own reply mark entries holding it, in process as well as remotely.
`records` lists the records the action's result holds as `{record, keyField?, paths}`, empty when it holds none. They
are read from the registries where `CatalogFacts::action` records the action, so an invalid effects and records
configuration (`EffectsRegistrationError`) fails at that call, at startup. They are what a client's watch is derived
from, and the loader reads them for one refusal and three warnings:

- **A query must read.** A query whose action declares effects, `none` included, is refused: a fetch then never
  produces a change, so two clients cannot refetch each other through their queries. An `evaluate` query is exempt,
  because an evaluation runs no handler.
- **The warnings** are in [Warnings](#warnings).

An action the catalog does not answer is refused before either is asked, so effects registered for an action id that
no action registers are never seen by a document: the catalog describes registered actions only.
A lookup left empty skips its rules; the document is otherwise checked in full. `stateless` decides
an alias's default instance, which is the interpreter's. A third lookup, `schema(actionId)`, answers
an action's schema JSON; it is not a load-time rule but what the interpreter builds a `form` node's
form from. Two more do the same for the forms engine's screens: `view(actionId)` answers the `v-*`
view document whose `v-query` is the action (a `collection` node's), and `wizard(actionId)` the
`w-*` wizard document whose first step is the action (a `wizard` node's).

## Refusals

`ui::Refusal` is the document id, a JSON Pointer (RFC 6901) into the document, and the reason;
`message()` prints `document <id> at <path>: <reason>`. A refusal from inside an expression points
at the expression node. Text that is not JSON (`loadDocumentText`) is refused at the root, and so is text nested deeper than 64
objects and arrays (`kMaxJsonDepth`), which is checked before the JSON parser runs.

## Warnings

`Document::warnings` holds the `LoadWarning{kind, path, text}` findings that do not refuse the document, in the order
found; `message(documentId)` prints them as a refusal is printed. They need the catalog's `action` lookup, and a
document loaded without it has none. A warning is given only where the declared effects certainly cover, or
certainly miss, what the document declares.

| Kind | When |
|---|---|
| `Redundant` | An `invalidates` entry, a `patch` or a `refreshOn` entry that the effects already cover (below). The path names the entry. |
| `Uncovered` | A mutation whose action declares nothing, not even `none`, that has no `invalidates` or `patch`, no `refetch` anywhere in its `onSuccess`, and that no query of the document names in `refreshOn`. After it succeeds nothing on any screen updates. `effects::none` silences it for an action that changes nothing, such as one that opens or attaches. |
| `WatchFeedback` | A `watch` declaration whose `source` is `any` (the default), whose `on` reads a query's value (`{"query": q}` or a `ref` to it, directly or through a `let`), and whose `do` runs (`run`, `upload`, `signIn`, at any depth) a mutation whose action declares an effect other than `none`. Another client's change refetches the query, the watch runs the mutation, and its change reaches the other client. `"source": "user"` silences it. |

**What covers a query.** A query holds exactly the records of its action (`records`); an `evaluate` query holds none.

- `updates R` or `changes R` covers a query that holds `R`.
- `changesInstance` covers a query that reads through the same model alias declaration as the mutation, since that
  is the instance the action ran on. Two aliases of one model type are two instances, so it covers nothing through
  another alias.
- A `patch` is covered only by `updates R` of a record the patched query holds: the reply then patches the rows
  itself. `changes R` refetches instead, which a patch exists to avoid.
- A `refreshOn` entry names an action run through any instance, so it is covered by the action's `updates` and
  `changes` alone, never by `changesInstance`.

`UiRegistry::addScreen`, `UiRegistry::addFile` for the app document, and `author::ScreenRegistry::add` log each warning at `warn`
(`morph::log`) when they register a document; the document registers as it would without them.

## Design decisions

- **One generic node, one kind table.** A typed struct per kind would double the description of
  the palette: once in the view tree, once here. Keeping properties keyed by name, with the shapes in
  one table, makes the document model a faithful image of the JSON and leaves lowering to the
  interpreter, which knows the view tree.
- **The loader throws inside, returns outside.** Every rule is a throw of an internal
  `DocumentRefused`; `loadDocument` turns the first into its `std::expected`. Threading an error
  return through every parser and checker step would double the branches without telling anyone
  more.
- **First refusal only.** A document is a server artefact checked in the server's tests; the first
  refusal, with its path, is what a developer fixes next.
- **Types are coarse.** The document does not restate action schemas, so most types it sees are
  `any`. The checks catch what a document can get wrong on its own: a string where a condition is
  expected, a field of an `int` param, an init of the wrong type.

## Out of scope

- Mounting a document, control, relations and commands at run time: the interpreter,
  [`interpreter.md`](interpreter.md).
- The accessibility rules of spec 5 §11: they guard authoring, run at registration with the
  server-side builders ([`author.md`](author.md)), and a client does not repeat them.
- Delivery: the `ui-*` envelope kinds, the catalog and bundles are the UI service's, [`service.md`](service.md).

# The app explorer — design

A person looking at a morph application can see what it is made of: its screens, the models the screens use, every
action of those models, and, for one action, its schema and catalog facts, the screens and nodes that use it, and the
widget the framework builds for it, live and runnable. It works in every build, on Qt Quick and on the terminal, in
local mode (`--app`) and remote mode (`--server`).

It builds on the app-shell frame (`2026-10-17-app-shell-style-design.md`): the explorer is a tab of the workspace,
so the client part lands after the client is on the frame (that design's §7). The library parts land first.

## What a person sees

```
┌ Explorer ───────────────────────────────────────────────────────────────────┐
│ Search…                  │ bank::TransactionModel / Transfer                │
│ SCREENS (6)              │ ┌ Facts ───────────────────────────────────────┐ │
│  Accounts                │ │ model     bank::TransactionModel             │ │
│  Transfers               │ │ sends a credential   no                       │ │
│ MODELS (5)               │ │ server-calculated    id, postedAt             │ │
│ ▾ bank::TransactionModel │ │ secret               —                        │ │
│    Deposit               │ └──────────────────────────────────────────────┘ │
│   ▌Transfer              │ ┌ Fields ──────────────────────────────────────┐ │
│    Withdraw              │ │ from     integer  required                    │ │
│ ▸ bank::AccountModel     │ │ to       integer  required                    │ │
│                          │ │ amount   money    required  min 0.01          │ │
│                          │ └──────────────────────────────────────────────┘ │
│                          │ ┌ Used by ─────────────────────────────────────┐ │
│                          │ │ transfers  /mutations/send          [open]   │ │
│                          │ └──────────────────────────────────────────────┘ │
│                          │ ┌ Try it ──────────────────────────────────────┐ │
│                          │ │ (the form the framework builds for Transfer)  │ │
│                          │ └──────────────────────────────────────────────┘ │
│                          │ ▸ Schema JSON                                   │
└─────────────────────────────────────────────────────────────────────────────┘
```

## What it lists

- **Screens**: the catalog's entries, in the catalog's order, each with its title, module and group; choosing one opens
  it as a tab.
- **Models**: every model the catalog names (`CatalogReply::models`, and every `ActionInfo::modelType`), in name
  order. A model lists **every** action of it, from its `schemas` document, including actions no screen calls. The
  client already fetches every catalog model's `schemas` document when it opens (`ClientSource::loadSchemas`); it
  keeps each one by model instead of only flattening them to action → schema, so the explorer needs no second
  request. Models no served document uses are not listed: the catalog does not name them and no request lists every
  registered model.
- A model whose `schemas` request is refused (remotely, the authorizer may refuse describing a type) is listed with
  "not described: <reason>" and no actions. In local mode the source catches what `schemasJson` throws (an
  unsatisfiable form) and reports it the same way.

## One action

Choosing an action shows four sections and a disclosure:

- **Facts**: the model, `ActionInfo::credential`, `calculatedFields`, `secretFields`, `listFields` (when the catalog has
  `ActionInfo` for it), the payload fingerprint and shape the schema carries (`x-payloadFingerprint`,
  `x-payloadShape`), and whether the principal may run it. `CatalogReply::permitted` covers only actions a served
  document names, so an action the catalog has facts for and `permitted` lacks is "not permitted", and any other is
  "decided when it runs".
- **Fields**: one row per field of the action, in `x-order`, built from the forms engine's own reading of the schema
  (`forms::FormModel::fromSchema`): name, type (a `oneOf` of consts is "one of: …", a quantity its unit and
  `x-unitAlternatives`), required, and the bounds and rules (`minimum`, `maximum`, `minLength`, `maxLength`,
  `pattern`, the `x-rules` by name).
- **Used by**: every place a loaded document names the action — a query, a mutation, a `form`, `collection` or
  `wizard` node, at any scope, in the app document or a screen — as the screen id and the JSON Pointer, with a button
  that opens the screen. A `run` command names a mutation, not an action, so it is not a use of its own. The client
  loads the app document as well as the screens for this.
- **Try it**: the widget the framework builds for the action, mounted live: a `collection` when the catalog has a view
  document for it (`CatalogReply::views`), a `wizard` when it has a wizard document, and a `form` otherwise. Its
  calls go to the application's real backend through the client's dispatcher, under the principal's session, so
  submitting runs the action and a collection queries. For a form, the reply or the failure shows under it: the
  generated document's `onSuccess` and `onError` set states the column shows. A collection and a wizard show their
  own results and errors, as they do on a screen.
- **Schema JSON** (collapsed): the raw schema, and the view or wizard document when there is one.

## How it is built

### In the library

- `ui::actionUses(Document const&)` (`include/morph/ui/explorer.hpp`) walks a loaded document and returns every
  `ActionUse{actionId, modelType, kind, pointer}` — `kind` one of `query`, `mutation`, `form`, `collection`, `wizard`.
  A model type is the alias's, found in the nearest enclosing scope that declares it (each node keeps its scope's
  `models`); an alias that borrows another (`{"from": …}`) has no type in the document, and takes the catalog's
  `ActionInfo::modelType`.
- `ui::actionFields(std::string_view schemaJson)` gives `ActionField{name, type, required, rules}` rows, built from
  `forms::FormModel::fromSchema` and reading the raw schema only for what the form model does not keep.
- `ui::tryItDocument(actionId, modelType, TryItKind)` writes the one-node document "Try it" mounts: a `models` entry
  of an alias for the model, a private instance, states `result` and `error`, and a `form` (with `onSuccess` and
  `onError` setting them), `collection` or `wizard` node over the action, in a column that shows the two states.
  Load-time validation refuses an action the catalog does not know, and the catalog knows only actions a served
  document names, so it is loaded with `tryItCatalog(catalog, schemas)`: the client's catalog whose `action` lookup
  also answers `ActionInfo{.modelType = …}` for every action a model's `schemas` document lists.
- `ui::Explorer` is a host screen built from backend widgets, as `ui::Navigator` is, and implements `ScreenHandle`
  (its title is "Explorer"; it is never dirty; it closes when asked): a splitter with the list on the left (a search,
  the Screens and Models groups, a collapsible per model) and the selected action's sections on the right. "Try it"
  is a slot into which it mounts the generated document through the `DocumentScreenHost` it is given, under the
  screen id `explorer:try:<action>`; the host's `documents` lookup answers that id from the explorer.
  `ExplorerOptions` gives it the catalog reply, the schemas documents by model, the loaded documents by screen id,
  the host and its documents hook, and `onOpen(screenId)`.

### In the client

The generic client registers `ui::Explorer` as the host screen `explorer` and adds a catalog entry for it in the module
"Tools"; `--screen explorer` opens it at start. `ClientSource` exposes what it needs: `catalogReply()`, the schemas
documents by model, and the app document. The client also hands the catalog's `permitted` to
`Session::setPermitted`, which it never did, so `{"can": …}` reads true where it should.

It is available in every build and every mode; nothing about it is development-only. Two gates bound what it shows:
the catalog lists only what the principal may see, and the server answers `schemas` for a model only when
`authorize(session, modelType, "")` allows describing it. `docs/spec/security.md` gets that paragraph, since the
`schemas` gate is cited from the server code but described nowhere.

## Errors

- A schema that does not parse is shown as "unreadable schema" with its first 200 bytes; the action is still
  listed and "Try it" is not offered.
- "Try it" for an action the principal may not run is offered disabled with the reason "not permitted".
- A failed call shows the error kind and message under the widget, as the screens do.

## Tests

- `actionUses`: a document with a query, a mutation, a `form` and an `onClick` `run` names each, with its pointer and
  model type; a nested scope's alias resolves.
- `actionFields`: a schema with an enum, a required field, an optional one, bounds and an `x-order` gives rows in
  order with the right flags; an empty object gives none.
- `tryItDocument`: loads without a refusal for each kind against a catalog that has the action, and mounts the node
  kind it names on `RecordingBackend`.
- `Explorer` on `RecordingBackend` with a fake catalog of two models: the list shows both, expanding one lists its
  actions from the fake `fetchSchemas`, selecting one shows the four sections, and "open" calls `onOpen`.
- `tryItCatalog` answers an action only its model's `schemas` document lists, and a `tryItDocument` over such an action
  loads and mounts.
- On bank in local mode (the ladder's medium rung): the explorer lists every bank model the catalog names, `Transfer`
  shows its fields, and submitting its "Try it" form with two accounts moves money (the balances read back changed).
- Terminal: the explorer renders and navigates by keys.

## Tasks

1. `actionUses` and `actionFields`.
2. `tryItDocument` and `tryItCatalog`, mounted through a `DocumentScreenHost` with a documents lookup.
3. The client source: `catalogReply()`, schemas kept by model, the app document loaded, `setPermitted`, the local
   throw caught.
4. `ui::Explorer` as a `ScreenHandle`; `RecordingBackend` and terminal tests.
5. The client's registration (after the app-shell §7), the bank test, and the `security.md` paragraph.

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Where it lives | A library host screen, registered by the client | Every client gets it; built from backend widgets, it runs on both frontends. |
| What "Try it" mounts | A generated one-node document through the document host | It is exactly what a screen author would get, so it shows the framework's real rendering, not an imitation. |
| Which models | Those the catalog names, with all their actions | No protocol change; the authorizer's `schemas` gate stays the disclosure control. |
| Every build | Not development-only | It reads only what the principal may see and runs only what the principal may run. |

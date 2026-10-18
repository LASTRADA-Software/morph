# The app explorer (`ui::Explorer`)

`include/morph/ui/explorer.hpp`. What a morph application is made of, for a person looking at it: its screens, every
model the catalog names with every action of it, and for one action its facts, its fields, the places that use it and
the widget the framework builds for it, mounted live. A host screen built from backend widgets, so it runs on every
frontend, in every build.

## Contents

- [The queries](#the-queries)
- [Try it](#try-it)
- [The screen](#the-screen)
- [What it may show](#what-it-may-show)
- [Design decisions](#design-decisions)

## The queries

| Function | What it answers |
|---|---|
| `actionUses(document, catalog)` | Every `ActionUse{actionId, modelType, kind, pointer}` of a loaded document, in document order: its queries and mutations at every scope, and its `form`, `collection` and `wizard` nodes. |
| `actionFields(schemaJson)` | One `ActionField{name, type, required, rules}` per top-level field, in `x-order`, or why the schema cannot be read. |
| `schemasOf(document)` | The schema of each action a model's `schemas` document lists. |

- A use's model type is its alias's, from the nearest enclosing scope that declares it. A borrowed alias
  (`{"from": …}`) has no type in the document and takes the catalog's `ActionInfo::modelType`.
- A `run` command names a mutation, not an action, so it is no use of its own: the mutation is.
- A field's type is what the forms engine reads (`forms::FormModel::fromSchema`): the kind's name, an enum as
  "one of: …", a quantity as "quantity in <unit>" with its alternatives, a decimal with its places, a choice with the
  action its options come from.
- A field's rules are its `minimum`, `maximum` and `multipleOf` (an integer bound's exact digits when the schema has
  them), `read-only`, `hidden`, and the name of each `x-rules` rule that names it.

## Try it

`tryItDocument(actionId, modelType, kind)` writes the document "Try it" mounts, with the screen id
`tryItScreenId(actionId)`, `explorer:try:<action>`:

- a private instance of the model under the alias `target`;
- the states `reply` and `failure`;
- a column holding the node over the action (test id `tryit`): a `form` (`TryItKind::Form`), a `collection` or a
  `wizard`.

A form submits from its button only, so a person fills it before anything runs. Its `onSuccess` sets `reply` to the
reply and its `onError` sets `failure` to the failure's text; two texts under it show them (`tryit_result`, in the
reply's JSON, and `tryit_error`). A collection and a wizard show their own results and errors, as they do on a screen.

Load-time validation refuses an action the catalog does not know, and a catalog knows only the actions a served
document names. `tryItCatalog(catalog, schemas)` is the catalog the document loads with: its `action` lookup also
answers `ActionInfo{.modelType = …}` for every action a model's `schemas` document lists, and its `schema` lookup
answers from those documents too.

## The screen

`Explorer(runtime, backend, page, options)` appends a splitter to `page`. `ExplorerOptions` gives it:

| Option | What it is |
|---|---|
| `catalog` | The `CatalogReply` for the current principal: its entries, `permitted`, `models`, `actions`, `views` and `wizards`. |
| `facts` | The client's catalog facts, which a "Try it" document loads with; empty uses `catalog.facts()`. |
| `schemas` | Every model's `schemas` document by model type, or why it did not arrive (`SchemasByModel`). |
| `documents` | The loaded documents by screen id, the app document under `app`. |
| `host`, `provide` | The `DocumentScreenHost` "Try it" mounts through, and what hands it the generated document first, for the host's `documents` lookup to answer `explorer:try:<action>` with. Either null offers no "Try it". |
| `onOpen` | Called with a screen id when the person asks to open it. |

The list, on the left:

- a search (`explorer_search`) that shows the screens whose title, and the actions whose id, contain its text,
  ignoring ASCII case; a model shows while its type or one of its actions matches, and opens when an action does;
- "Screens (n)": a button per catalog entry (`explorer_screen_<id>`), calling `onOpen`;
- "Models (n)": a collapsible per model type (`explorer_model_<type>`), in name order, holding a button per action of
  its `schemas` document (`explorer_action_<id>`), including actions no screen calls. A model is one the catalog names:
  its `models`, every `ActionInfo::modelType`, and every key of `schemas`. A model without a document shows
  "not described: <reason>" and no actions.

Choosing an action (or `select(actionId)`) replaces the right side with, in order:

| Section | Test id | What it shows |
|---|---|---|
| The heading | `explorer_title` | `<model> / <action>` |
| Facts | `explorer_facts` | the model; with catalog facts, whether it sends a credential, the server-calculated, secret and list fields; the schema's `x-payloadFingerprint` and `x-payloadShape`; and whether the principal may run it |
| Fields | `explorer_fields` | a grid of name, type, required and rules; an unreadable schema as "unreadable schema: <why>" with its first 200 bytes |
| Used by | `explorer_uses` | per use, the screen id, the pointer and the kind, with an "Open" button (`explorer_open_<n>`) unless it is the app document |
| Try it | `explorer_tryit` | the generated document, mounted into a slot; or why it is not offered (`explorer_tryit_status`) |
| Schema JSON | `explorer_schema` | collapsed: the schema, and the view or wizard document when there is one |

Whether the principal may run an action: "permitted" when `catalog.permitted` lists it; "not permitted" when the
catalog has facts for it and `permitted` does not list it, since `permitted` covers exactly the actions a served
document names; "decided when it runs" otherwise. "Try it" is withheld from an action that is not permitted, and from
one whose schema cannot be read.

As a `ScreenHandle`, its title is "Explorer", it is never dirty, it has no identity, and it closes when asked; closing
unmounts the "Try it" screen.

## What it may show

The explorer reads only what the principal may see and runs only what the principal may run. The catalog lists only
the entries and facts the principal sees, the server answers `schemas` only for a type its authorizer lets the
principal describe (`docs/spec/security.md`), and "Try it" calls go through the client's dispatcher under the
principal's session, so the server authorizes each one as it would a screen's.

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Where it lives | A library host screen | Every client gets it; built from backend widgets, it runs on both frontends. |
| What "Try it" mounts | A generated one-node document through the document host | It is exactly what a screen author gets, so it shows the framework's real rendering and calls, not an imitation. |
| Which models | Those the catalog names, with every action of their `schemas` document | No protocol change: the authorizer's `schemas` gate stays the disclosure control. |
| A form's submit | Explicit | An automatic form would run the action at the first ready keystroke, which an explorer must not do. |
| Every build | Not development-only | It discloses nothing the principal could not already read, and runs nothing they could not already run. |

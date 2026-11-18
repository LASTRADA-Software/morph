# Custom screens — design

A person's own version of an application screen, stored on the server through the application's store, checked
whenever it is saved or loaded, and served by the framework's `morph.customScreens` model. The design is in
`docs/superpowers/specs/2026-11-18-custom-screens-design.md`; this page is the server side of it: the store seam, the
model, the checks and the base rule.

| Header | What it holds |
|---|---|
| `include/morph/ui/custom_screens.hpp` | `ICustomScreenStore` and `MemoryCustomScreenStore`, `checkBaseRule`, `checkCustomScreen`, `withScreenId`, `VerdictCache`, `verdictKey`, the wire types, `CustomScreensModel`, `registerCustomScreens` |
| `include/morph/ui/service.hpp` | `permittedActions`, `screenAdmitted`, `kCustomScreenIdPrefix`, `ServedBundle::catalog` |
| `src/ui/custom_screen_checks.cpp` | The checks and the base rule's walk |
| `src/ui/custom_screens.cpp` | The model, its registration, the record and its effects |
| `src/ui/document_actions.hpp` | `DocumentActions`: the actions a document names, and the kind of position of each |

## Contents

- [The store](#the-store)
- [The model](#the-model)
- [The checks](#the-checks)
- [The base rule](#the-base-rule)
- [Records and effects](#records-and-effects)
- [Design decisions](#design-decisions)

## The store

`ICustomScreenStore` has `list(person, app)`, `get(key)`, `put(key, screen)` and `remove(key)`, all asynchronous, with
`key` a `CustomScreenKey{person, app, id}`. `get`, `put` and `remove` answer a `CustomScreenRefusal` the person is
shown as it is. A `CustomScreen` carries `id`, `title`, `basedOn`, the `document` text, `savedAt` and `savedBy`.

The application's store is where every policy over custom screens lives: what a person may open (which may include
screens others saved), what they may overwrite or delete, and how many they may keep. The model calls it only with a
person the server derived and a document that passed every check. A store owns the executor its completions are
delivered on, and any atomicity its own rules need, such as a count.

`MemoryCustomScreenStore(base)` is the framework's store: a map on an `OwnerStrand` that runs on `base`, each person
seeing only their own screens under one application. `list` answers by id. A missing screen is `no such custom
screen`, from `get` and `remove` alike; `put` refuses nothing. Its entries last as long as it does.

## The model

`registerCustomScreens(store, bundle, authorizer, dispatcher, registry, options)` registers `morph.customScreens` and
its five actions, with a factory that gives every instance one shared context (store, bundle getter, authoriser,
bounds and one `VerdictCache`). A server that offers custom screens calls it once, before it serves.

- `bundle` is a getter, called at each action, so a development reload is seen.
- `authorizer` is the server's, for `permittedActions` and the base screen's admission. The server's authoriser must
  admit `morph.customScreens` and its actions for every signed-in principal; the model keeps each person to what the
  store answers them.
- It throws `std::invalid_argument` for a null store, bundle getter or authoriser, and for a `maxDocumentBytes` above
  `kMaxFileBytes`: every document the model accepts is one `UiRegistry` could serve as a file.

| Action id | Kind | Body | `ok` reply |
|---|---|---|---|
| `morph.customScreens.list` | query | `{app}` | `{screens: [CustomScreenSummary]}` |
| `morph.customScreens.get` | query | `{app, id}` | `{summary, document}` |
| `morph.customScreens.check` | query | `{app, basedOn, document}` | `{refusal?: {pointer, reason}}` |
| `morph.customScreens.save` | mutation | `{app, id?, title, basedOn, document}` | `{id}` |
| `morph.customScreens.remove` | mutation | `{app, id}` | `{removed}` |

What every action does, in order:

1. **`app` is the served application.** `app` is 1 to `kMaxNameBytes` bytes and equals the current bundle's
   `application.id`, or the action is refused, before anything else, a `list` without a principal included. A screen
   checked against one application is never stored under another.
2. **The person** is the verified principal of the session, copied at the start of the action, before anything is
   awaited. A body naming a person has the key ignored: no body type has one. With no verified principal, `list`
   answers no screens and the other four are refused with `custom screens are stored for a signed-in person`.
3. The action's own work, below.

- **`list`** asks the store for the caller's screens and answers a `CustomScreenSummary` for each, in the store's
  order. The permitted set is computed once for the whole list and not kept past the call.
- **`get`** answers `{summary, document}`: the `list` entry of the screen and its stored text. The store's refusal is
  the `err` reply's message, as the store worded it.
- **`check`** answers the first refusal of `checkCustomScreen` structured, or none when the document would be saved.
- **`save`** with an `id` first asks the store's `get` for that screen under the caller; a refusal, such as `no such
  custom screen`, is the error as the store gave it, so a client cannot choose an id. With no `id`, the id is
  `custom:` and 32 hexadecimal digits from `newFileRef`. The document then passes `checkCustomScreen`; a refusal is the
  `err` reply's message, pointer included. `withScreenId` writes the custom screen's own id into the document, and the
  rewritten text, whose id is longer, must still be within `maxDocumentBytes`. Only then is `put` called, with
  `savedAt` now and `savedBy` the caller. `title` and `basedOn` are each 1 to `kMaxNameBytes` bytes.
- **`remove`** calls the store's `remove` under the caller.
- **A store's refusal is passed through unchanged**, from `get`, `put` and `remove`: the model adds no wording over
  the reason, so the person is told what the store said.

A `CustomScreenSummary` carries `id`, `title`, `basedOn`, `savedAt`, `savedBy` and:

- `mine`: whether `savedBy` is the caller. A screen another person saved is shown as theirs.
- `opensWithoutParams`: whether the stored document declares no params, so it opens from the navigator.
- `broken`: the refusal the checks give the stored document today, with its pointer; absent when it may be mounted. A
  broken screen is listed, so the person can open its source and fix it, and is never mounted.

The actions are `Loggable::No`: a screen is not application data, and the store keeps who saved it and when.

## The checks

`checkCustomScreen(bundle, session, authorizer, basedOn, document, options, cache)` answers the first refusal, with its
JSON Pointer, or none. An overload takes the person's permitted set already computed, in front of `basedOn`, so a list
of many screens computes `permittedActions` once. `save` and `check` run it on the request's document; `list` and `get`
run it on every stored document they answer.

1. **Bounds.** At most `maxDocumentBytes` (256 KiB by default, at most `kMaxFileBytes`), nested within
   `kMaxJsonDepth`. These cost nothing to evaluate and come first, so an oversized document is refused before it is
   parsed.
2. **The base.** `basedOn` names a screen of the bundle that has a document (a host screen has none), and
   `screenAdmitted` admits the person to it: it is available and the person may run every top-level query it declares.
3. **The application screen checks.** `loadDocumentText` with `ServedBundle::catalog`, the facts every served document
   was loaded with, and the result is a screen, not the app document. The base is loaded the same way; a base that
   does not load is itself the refusal.
4. **The accessibility rules**, `author::checkAccessibility`.
5. **The base rule**, `checkBaseRule`.
6. **Permission.** Every action the document names, found by `DocumentActions`, is in the permitted set.

**The verdict cache.** Check 2 runs on every call, before the cache: the bundle's digest hashes its manifest, which
does not say whether an entry is available, and the person's admission is not part of it either, so a cached verdict
cannot vouch for the base. The cache covers checks 3 to 6. `VerdictCache` is keyed by `verdictKey`: the document's
content hash, `basedOn`, the bundle's digest and a hash of the permitted set. A reload, a permission change or an edit
is a new key and checks again. It holds at most `verdictCacheSize` verdicts (1024 by default, at least one), dropping
the least recently used, and is safe to share between threads. A passing document's verdict is cached as well, as
`nullopt` under the key. Passing no cache checks every time.

The checks bound what a document contains. What it does is still bounded by the server's checks on each call it
makes, and by the base rule for what the client does without one.

## The base rule

`checkBaseRule(base, baseText, custom, customText)` refuses the first use in the custom document that the base does
not have. A custom document's capabilities are a subset of its base's.

| Compared | How |
|---|---|
| Params | Exactly the base's, names and types |
| Actions: queries, mutations, forms, collections, wizards, tables, wherever declared, component fallbacks included | Each `(model type, action)` the base names, in a position of the same kind: a query, a mutation, a form with no `submit`, an `explicit` form, an `auto` form, a model-backed table, a collection, a wizard |
| Commands run without the person pressing anything | The same trigger in the base, below |
| Client-side commands `upload`, `download`, `switchBackend`, `signIn`, `signOut`, `resetPrefs` | Only a kind the base uses anywhere; an `upload` only as below |
| `host` commands | Identical to one of the base's commands, name and arguments, and its arguments are literals |
| Host names | Every string member named `host`: host commands, host values and host components share one set |
| Delivered components | Name and content hash |

**Positions.** The same action in another kind of position runs differently, so the kind is compared along with the
action. A form with no `submit` keeps its action schema's mode, which the check cannot see and which is automatic
unless the schema says otherwise; it is a position of its own, `form`, and matches only a `form` of the base.

**Unattended triggers.** Commands that run without a press are compared by trigger: a custom document may run
something from a trigger only when the base runs it from the same trigger, so an unattended run cannot chain to an
action the base only runs on a press. The triggers are:

- the lifecycle keys `onMount`, `onUnmount`, `onBackendChange`, `onClose` and `onSave`;
- watches;
- a form whose `submit` is not `"explicit"`;
- a delivered or host component's `on` events, whose own code decides when they fire;
- an `upload` or a `signIn` command, whose `onSuccess` and `onError` chains run when the transfer or sign-in settles.

A mutation's `onSuccess` and `onError` reached from a trigger inherit that trigger, followed through every chain, with
each name resolved in the scope that declares the mutation.

**Uploads.** Every `upload` of the custom document sends exactly `{"ref": "event"}` and sits directly in the `onPick`
of a `filePicker` that opens files (`mode` absent, or the literal `"open"`) or in a `dropZone`'s `onDrop`, so the file
is always one the person just chose.

**Host commands.** A host command must equal one of the base's, name and arguments. Its arguments are literals: an
argument that names a state, a let or an element would resolve in the custom document, whose author chose its value.
A host command's handler still treats its arguments as untrusted input.

**Not compared:** text, layout, styles, states, lets, expressions other than host values, `navigate` targets (opening a
screen goes through the catalog's admission), and ids.

Why each row is there:

- A custom screen someone else saved is code the viewer runs. Anything that acts without a press, or acts on the
  viewer's machine rather than through the server, must already be something the viewer accepts by opening the base.
- A component or host name the base does not use could register under a name another screen relies on, or reach a
  host feature no shipped screen exposes.
- A button the person presses runs an action through the server under their own session, so the server's checks
  bound it. Restricting it to the base's actions still keeps a shared screen from putting an action, say a delete,
  where its base never offers one.

`checkBaseRule` throws `std::logic_error` for a document that names a model alias it does not declare, which only a
document that skipped load-time validation can.

## Records and effects

`CustomScreenSummary` is the record `morph.customScreens`, keyed by `id`, registered `privateKeys`: its keys must not
reach other principals, because a screen's id is a person's own. `save` and `remove` declare
`changes<CustomScreenSummary>`, the whole type: a save can add a screen to lists no key names yet. `list` and `get`
hold it, so a client's list refetches after any save or remove, from anyone. Saves are a person's deliberate act, so
the refetch is rare, and the verdict cache keeps it cheap.

The record, the result slots that let a list be found holding it, and the effects are registered at static
initialisation in `src/ui/custom_screens.cpp`, beside `registerCustomScreens`, because the process-level registries
resolve once, at their first read. A static build links them only into a server that calls `registerCustomScreens`; a
shared build carries them for every application, where they are inert.

## Design decisions

| Decision | Chosen | Why |
|---|---|---|
| Where policy lives | The application's store | Sharing, roles and limits differ per application; what a document may contain does not. |
| What a shared screen may do | No more than its base | A viewer opening someone else's screen can then do only what opening the base would do, with no policy and no trust in who saved it. |
| Action ids | `morph.customScreens.<action>` | Action ids are process-global; a bare `list` would clash with an application's. |
| Which application | `app` must equal the served one, checked first | A screen checked against one application is never stored under another. |
| Store refusals | Passed through unchanged | The person is told what the store decided, in the store's words. |
| Effects | `changes`, record-wide | A list refetches after any save; the verdict cache keeps the refetch cheap. |
| Base admission | Every call, outside the verdict cache | Availability is not part of the bundle digest. |
| The session | Copied before the first await | The session is the dispatching thread's; an action resumes elsewhere. |
| Servability | A bound on `maxDocumentBytes`, rechecked on the rewritten text | Any stored document fits a file and an envelope, so no per-document check is needed at serve time. |

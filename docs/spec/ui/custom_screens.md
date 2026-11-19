# Custom screens — design

A person's own version of an application screen, stored on the server through the application's store, checked
whenever it is saved or loaded, and served by the framework's `morph.customScreens` model. The design is in
`docs/superpowers/specs/2026-11-18-custom-screens-design.md`; this page is the server side of it: the store seam, the
model, the checks and the base rule.

| Header | What it holds |
|---|---|
| `include/morph/ui/custom_screens.hpp` | `ICustomScreenStore` and `MemoryCustomScreenStore`, `checkBaseRule`, `describeAllowance`, `checkCustomScreen`, `withScreenId`, `VerdictCache`, `verdictKey`, the wire types, `CustomScreens`, `CustomScreensModel`, `registerCustomScreens` |
| `include/morph/ui/service.hpp` | `permittedActions`, `screenAdmitted`, `kCustomScreenIdPrefix`, `ServedBundle::catalog` |
| `src/ui/custom_screen_checks.cpp` | The checks, the base rule's walk and the table that classifies every command-list key |
| `src/ui/custom_screens.cpp` | `CustomScreens`, the model, its registration, the record and its effects |
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

`put`'s key may name a screen this person can open but not change, such as one another person shared with them. The
model checks the document, not the right to replace what is stored, so the store must refuse that write.
`CustomScreen::savedBy` is the caller, not the screen's first author.

`MemoryCustomScreenStore(base)` is the framework's store: a map on an `OwnerStrand` that runs on `base`, each person
seeing only their own screens under one application. `list` answers by id. A missing screen is `no such custom
screen`, from `get` and `remove` alike; `put` refuses nothing. Its entries last as long as it does, and it bounds
neither their number nor their size: it serves tests and examples, not a deployment.

## The model

`registerCustomScreens(store, bundle, authorizer, dispatcher, registry, options)` registers `morph.customScreens` and
its five actions, with a factory that gives every instance one shared `CustomScreens` (store, bundle getter,
authoriser, bounds and one `VerdictCache`), which it returns. A server that offers custom screens calls it once,
before it serves.

- `bundle` is a getter, called at each action, so a development reload is seen. Actions run on strands, on any thread,
  so it must be thread-safe.
- `authorizer` is the server's, for `permittedActions` and the base screen's admission. The server's authoriser must
  admit `morph.customScreens` and its actions for every signed-in principal; the model keeps each person to what the
  store answers them.
- It throws `std::invalid_argument` for a null store, bundle getter or authoriser, and for a `maxDocumentBytes` that
  could not be served: above `kMaxFileBytes`, or with `4 * maxDocumentBytes + kCustomScreenReplyOverhead` above
  `wire::kMaxEnvelopeBytes`. A `get` reply carries the document escaped once into the reply's JSON and the reply
  escaped again as the envelope's body, so a document of quotes and backslashes grows four-fold; the overhead (64 KiB)
  is the reply's other fields, with a wide margin, which every summary's caps make a bound (below). At the 8 MiB
  envelope bound the largest accepted value is
  2,080,768 bytes, well above the 256 KiB default.

| Action id | Kind | Body | `ok` reply |
|---|---|---|---|
| `morph.customScreens.list` | query | `{app}` | `{screens: [CustomScreenSummary], truncated}` |
| `morph.customScreens.get` | query | `{app, id}` | `{summary, document}` |
| `morph.customScreens.check` | query | `{app, basedOn, document}` | `{refusal?: {pointer, reason}}` |
| `morph.customScreens.save` | mutation | `{app, id?, title, basedOn, document}` | `{id}` |
| `morph.customScreens.remove` | mutation | `{app, id}` | `{removed}` |

What every action does, in order:

1. **`app` is the served application.** `app` is 1 to `kMaxNameBytes` bytes and equals the current bundle's
   `application.id`, or the action is refused, before anything else, a `list` without a principal included. A screen
   checked against one application is never stored under another.
2. **The names** the action takes (`id`, `title`, `basedOn`) are each 1 to `kMaxNameBytes` bytes, or it is refused;
   they are checked before the person is looked at, except `save`'s optional `id`, which is checked after it. An `id`
   of `get`, `save` or `remove` also starts with `custom:` (`kCustomScreenIdPrefix`), so a request never reaches the
   store with an application screen's id.
3. **The person** is the verified principal of the session, copied at the start of the action, before anything is
   awaited. A body naming a person has the key ignored: no body type has one. With no verified principal, `list`
   answers no screens and the other four are refused with `custom screens are stored for a signed-in person`.
4. The action's own work, below.

- **`list`** asks the store for the caller's screens and answers a `CustomScreenSummary` for each, in the store's
  order. The permitted set is computed once for the whole list and not kept past the call.
- **`get`** answers `{summary, document}`: the `list` entry of the screen and its stored text. The store's refusal is
  the `err` reply's message, as the store worded it. A stored document longer than `maxDocumentBytes` is refused as
  the store's document breaking the bound, so every `get` reply fits an envelope whatever the store holds.
- **`check`** answers the first refusal of `checkCustomScreen` structured, or none when the document would be saved.
- **`save`** with an `id` first asks the store's `get` for that screen under the caller; a refusal, such as `no such
  custom screen`, is the error as the store gave it, so a client cannot choose an id. With no `id`, the id is
  `custom:` and 32 hexadecimal digits from `newFileRef`. `withScreenId` then writes that id into the document,
  escaping control bytes inside the writer as the wire does (the writer's default leaves them raw, which is invalid
  JSON and can corrupt a byte after an earlier escaped character), and `checkCustomScreen` checks the rewritten text,
  the text that is stored, so the size bound holds for the longer id too; a refusal is the `err` reply's message,
  pointer included. Only then is `put` called, with `savedAt` the current time and `savedBy` the caller. `check` runs
  the same rewrite under a placeholder id as long as a real one, so its verdict is the one `save` would give.
- **`list` answers at most `maxListed` summaries** (200 by default) and says `truncated: true` when the store held
  more. A capped summary is at most `kMaxSummaryBytes` (3 KiB) and grows at most seven-fold in an envelope: the reply
  is written with control bytes escaped, so one becomes a six-byte sequence, and the envelope doubles each backslash of
  the reply. `registerCustomScreens` refuses a `maxListed` for which
  `maxListed * 7 * kMaxSummaryBytes + kCustomScreenReplyOverhead` exceeds `wire::kMaxEnvelopeBytes`, which is above 387;
  the default takes at most 4.3 MB.
- **`remove`** calls the store's `remove` under the caller.
- **A store's refusal is passed through unchanged**, from `get`, `put` and `remove`: the model adds no wording over
  the reason, so the person is told what the store said.

**Bounded replies.** Every field of a summary is capped when the summary is built, so the part of a `get` reply that is
not the document is bounded whatever the store holds: `id`, `title`, `basedOn`, `savedBy` and `broken.pointer` at
`kMaxNameBytes`, and `broken.reason` at `kMaxBrokenReasonBytes` (1 KiB). A longer value is cut on a UTF-8 boundary and
ends with an ellipsis (U+2026). A reason can quote a whole host command, and a pointer or a principal can be of any
length, so without the caps the reply would have no bound. `mine` is computed from the stored `savedBy`, before the cap.
One summary is then at most `kMaxSummaryBytes` (3 KiB) before escaping and at most seven times that after it (see
`list`).
A store keeps its ids within `kMaxNameBytes`, since an id shown cut could not be given back to `get` or `remove`; the
model's own are 39 bytes.

**Discovery.** A client learns that a server offers custom screens by calling `list`: a server that did not call
`registerCustomScreens` has no `morph.customScreens` model, and answers the client's registration of it with the
dispatcher's refusal, `unknown model type: morph.customScreens`. The client offers no custom screens then.

**The handle.** `CustomScreens` holds what the model uses, so anything else on the server shares its checks and its
cache. Its members are tasks to await from a coroutine, and refuse by value rather than by throwing:

- `check(session, app, basedOn, document)` answers what the `check` action answers, a refusal or none. A session with
  no verified principal, an application the server does not serve and a name outside its bounds are refusals.
- `get(session, app, id)` answers what the `get` action answers, `{summary, document}` or a `CustomScreenRefusal`.
- `allowance(basedOn)` answers `describeAllowance` of the base screen, for a prompt: the params; each action with the
  kind of position it may be named in; each command the base runs without a press, with its trigger; the client-side
  commands; the host names and host commands; the delivered components. It is built from the same walk as
  `checkBaseRule`, so it lists what the rule compares. A `basedOn` that names no screen document of the bundle, or a
  base that does not load, is a refusal.

A `CustomScreenSummary` carries `id`, `title`, `basedOn`, `savedAt`, `savedBy` and:

- `mine`: whether `savedBy` is the caller. A screen another person saved is shown as theirs.
- `hash`: the content hash of the stored document. It changes whenever the document does, so a client holding a copy
  tells from a list alone whether its copy is current.
- `opensWithoutParams`: whether it opens from the navigator: it is not broken, and its base declares no params, since a
  custom screen declares exactly its base's. The base is read once per list, not once per screen.
- `broken`: the refusal the checks give the stored document today, with its pointer; absent when it may be mounted. A
  broken screen is listed, so the person can open its source and fix it, and is never mounted.

The actions are `Loggable::No`: a screen is not application data, and the store keeps who saved it and when.

## The checks

`checkCustomScreen(bundle, session, authorizer, basedOn, document, screenId, options, cache)` answers the first
refusal, with its JSON Pointer, or none. `screenId` is the id the document is stored under. An overload takes the
person's permitted set already computed, in front of `basedOn`, so a list of many screens computes `permittedActions`
once; the set is never kept across calls. `save` and `check` run it on the request's document after the id rewrite;
`list` and `get` run it on every stored document they answer, under the entry's id.

1. **Bounds.** At most `maxDocumentBytes` (256 KiB by default, at most `kMaxFileBytes`), nested within
   `kMaxJsonDepth`. They are cheap and need no parse of the document, so they come first and an oversized document is
   refused before
   it is parsed.
2. **The base.** `basedOn` names a screen of the bundle that has a document (a host screen has none), and
   `screenAdmitted` admits the person to it: it is available and the person may run every top-level query it declares.
3. **The application screen checks.** `screenId` starts with `custom:`, and `loadDocumentText` with
   `ServedBundle::catalog`, the facts every served document was loaded with, gives a document whose `id` is
   `screenId`. A stored entry whose id lacks the prefix, or whose document names another id, is therefore broken (at
   `/id`), and a document that is the app document (its id is `app`) is never a custom screen. The base is loaded the
   same way; a base that does not load is itself the refusal.
4. **The accessibility rules**, `author::checkAccessibility`.
5. **The base rule**, `checkBaseRule`.
6. **Permission.** Every action the document names, found by `DocumentActions`, is in the permitted set.

**The verdict cache.** Check 2 runs on every call, before the cache: the bundle's digest hashes its manifest, which
does not say whether an entry is available, and the person's admission is not part of it either, so a cached verdict
cannot vouch for the base. The cache covers checks 3 to 6. `VerdictCache` is keyed by `verdictKey`: the document's
content hash, `basedOn`, `screenId`, the bundle's digest and a hash of the permitted set. A reload, a permission change
or an edit
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
| Actions: queries, mutations, forms, collections, wizards, tables, wherever declared, component fallbacks included | Each `(model type, action)` the base names, in a position of the same kind: a query, an evaluate (a query that names `evaluate`, a dry run of the action's checks), a mutation, a form with no `submit`, an `explicit` form, an `auto` form, a model-backed table, a collection, a wizard |
| Commands run without the person pressing anything | The same trigger in the base, below |
| Client-side commands `upload`, `download`, `switchBackend`, `signIn`, `signOut`, `resetPrefs` | Only a kind the base uses anywhere, compared by kind; `switchBackend` by kind and backend name; an `upload` only as below |
| `host` commands | The whole command's canonical JSON matches one of the base's, and its arguments are literals |
| Host names | Every string member named `host`: host commands, host values and host components share one set |
| Delivered components | Name and content hash |

`describeAllowance(base, baseText)` writes this table's contents for one base as plain text, from the same walk.

**Positions.** The same action in another kind of position runs differently, so the kind is compared along with the
action. A form with no `submit` keeps its action schema's mode, which the check cannot see and which is automatic
unless the schema says otherwise; it is a position of its own, `form`, and matches only a `form` of the base.

**Unattended triggers.** Commands that run without a press are compared by trigger: a custom document may run
something from a trigger only when the base runs it from the same trigger, so an unattended run cannot chain to an
action the base only runs on a press. The triggers are:

- the lifecycle keys `onMount`, `onUnmount`, `onBackendChange`, `onClose` and `onSave`;
- watches;
- a form whose `submit` is not `"explicit"`;
- a delivered or host component's `on` events, whose own code decides when they fire.

What runs a command-list key is a table in `custom_screen_checks.cpp` (`detail::eventKeys`) that lists every such key
of every node and part kind of the element table: a press, a chosen file (`filePicker`'s `onPick`, `dropZone`'s
`onDrop`), a form's own submit and outcomes, or a component's event. A test walks the element table and fails for a key
the table does not list, so a key added to the vocabulary is classified before it can be used; a key missing from the
table anyway is run under a trigger of its own, so only a base with the same key and commands allows it.

Under a trigger, a `run`, an `upload` or a `signIn` is followed: the `onSuccess` and `onError` of the mutation it
reaches inherit that trigger, through every chain, with each name resolved in the scope that declares the mutation.

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

**What the rule does not bound.** The base rule bounds an action and the trigger that runs it, not its arguments. A
shared screen can run a mutation the base runs unattended with a body its author chose, and can multiply queries
(`refreshEvery`, a query inside a `forEach`) under the viewer's session. The boundary for argument values is the
server's checks on every call and the application's own validation of each action.

`checkBaseRule` throws `std::logic_error` for a document that names a model alias it does not declare, which only a
document that skipped load-time validation can.

## Records and effects

`CustomScreenSummary` is the record `morph.customScreens`, keyed by `id`, registered `privateKeys`: its keys must not
reach other principals: a person learns of a screen's id only through their own list, whatever the store shares. `save`
and `remove` declare
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
| Servability | A bound on `maxDocumentBytes` that fits a file and the escaped `get` reply, rechecked on the rewritten text | Any stored document is then served within the envelope, so no per-document check is needed at serve time. |

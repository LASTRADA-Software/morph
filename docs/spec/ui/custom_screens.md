# Custom screens — design

A person's own version of an application screen, stored on the server through the application's store, checked whenever
it is saved or loaded, and served by the framework's `morph.customScreens` model. The design is in
`docs/superpowers/specs/2026-11-18-custom-screens-design.md`; this page is the store seam, the model, the checks and the
base rule on the server, and the client that offers them.

| Header | What it holds |
|---|---|
| `include/morph/ui/custom_screens.hpp` | `ICustomScreenStore` and `MemoryCustomScreenStore`, `checkBaseRule`, `describeAllowance`, `checkCustomScreen`, `withScreenId`, `VerdictCache`, `verdictKey`, the wire types, `CustomScreens`, `CustomScreensModel`, `registerCustomScreens` |
| `include/morph/ui/service.hpp` | `permittedActions`, `screenAdmitted`, `kCustomScreenIdPrefix`, `ServedBundle::catalog` |
| `include/morph/ui/custom_screen_sync.hpp` | `isCustomScreenId`, `routeScreen`, `CustomDocuments`, `CustomScreensOffer`, `CustomScreenSync` |
| `include/morph/ui/custom_screen_controller.hpp` | `CustomScreenControllerOptions`, `CustomScreenController`, its outcomes and results as data |
| `include/morph/ui/custom_screen_chrome.hpp` | `CustomScreenChromeOptions`, `CustomScreenChrome` |
| `include/morph/ui/custom_screen_versions.hpp` | `ActionSchemaLookup`, `baseVersion`, `layoutChanges`, `LayoutChanges`, `LayoutChoices`, `applyLayoutChoices`, `withKnownVersion` |
| `include/morph/ui/json_source.hpp` | `prettyJson`, `pointerOffset`, `lineAt` |
| `include/morph/ui/chat/propose_screen.hpp` | `customScreensChatTool` (`propose_screen`), `customScreensDocumentSource`, `enableCustomScreensChat` |
| `src/ui/chat_propose_screen.cpp` | The tool: its offer, its handler and what the conversation keeps of a call |
| `src/ui/custom_screen_checks.cpp` | The checks, the base rule's walk and the table that classifies every command-list key |
| `src/ui/custom_screens.cpp` | `CustomScreens`, the model, its registration, the record and its effects |
| `src/ui/custom_screen_versions.cpp` | The versions: the walk that finds elements and forms, the comparison, the update |
| `src/ui/document_actions.hpp` | `DocumentActions`: the actions a document names, and the kind of position of each |

## Contents

- [The store](#the-store)
- [The model](#the-model)
- [The checks](#the-checks)
- [The base rule](#the-base-rule)
- [Records and effects](#records-and-effects)
- [Versions](#versions)
- [The assistant](#the-assistant)
- [The client](#the-client)
- [Not yet](#not-yet)
- [Design decisions](#design-decisions)

## The store

`ICustomScreenStore` has `list(scope)`, `get(key)`, `put(key, screen)` and `remove(key)`, all asynchronous, with
`key` a `CustomScreenKey{person, app, id, session}` and `scope` a `CustomScreenScope{person, app, session}`. `session`
is the action's verified `session::Context`, copied as the model copies it (below): its `principal` is `person`, and
its `token` is the credential the server verified, whose claims (roles among them) a store may read. So a policy can
rest on more than the person, such as "an administrator may remove anyone's screen"; the authoriser cannot carry it,
since it sees only the model and the action, not whose screen a write replaces. `get`, `put` and `remove` answer a
`CustomScreenRefusal` the person is shown as it is. A `CustomScreen` carries `id`, `title`, `basedOn`, the `document`
text, `savedAt`, `savedBy` and `attributes`.

**Attributes** are the store's own record about a screen: string keys, JSON values (`Json::object_t`), such as whether
it is shared, private or someone's default. The model carries them through `save` (the request's, to `put`), `list`
and `get` (the stored ones, on the summary) and never reads them: the framework adds no sharing or default semantics,
it only stops dropping what a store decides. The store validates what a `save` asks for and refuses what it does not
accept, in its own words. A record is at most `kMaxAttributesBytes` (1 KiB) written as JSON: a `save` with a longer
one is refused before the person is looked at, and a stored record longer than that is not sent; its screen is listed
broken with the reason, so a store that writes one hears about it rather than its clients silently losing it.

The application's store is where every policy over custom screens lives: what a person may open (which may include
screens others saved), what they may overwrite or delete, and how many they may keep. The model calls it only with a
person the server derived, with that person's verified session, and a document that passed every check. A store owns
the executor its completions are delivered on, and any atomicity its own rules need, such as a count.

`put`'s key may name a screen this person can open but not change, such as one another person shared with them. The
model checks the document, not the right to replace what is stored, so the store must refuse that write.
`CustomScreen::savedBy` is the caller, not the screen's first author.

`MemoryCustomScreenStore(base)` is the framework's store: a map on an `OwnerStrand` that runs on `base`, each person
seeing only their own screens under one application. It keys every entry by the person and reads nothing else of the
session. `list` answers by id. A missing screen is `no such custom screen`, from `get` and `remove` alike; `put`
refuses nothing. Its entries last as long as it does, and it bounds neither their number nor their size: it serves
tests and examples, not a deployment.

## The model

`registerCustomScreens(store, bundle, authorizer, dispatcher, registry, options)` registers `morph.customScreens` and
its five actions, with a factory that gives every instance one shared `CustomScreens` (store, bundle getter,
authoriser, bounds and one `VerdictCache`), which it returns. A server that offers custom screens calls it once,
before it serves.

- `bundle` is a getter, called at each action, so a development reload is seen. Actions run on strands, on any thread,
  so it must be thread-safe.
- `dispatcher` is borrowed: the schemas of the actions a base's forms bind, which a base's version reads
  ([Versions](#versions)), come from it at every `list`, `get` and `save`, so it outlives every call, as it does for
  the server.
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
| `morph.customScreens.save` | mutation | `{app, id?, title, basedOn, document, attributes?}` | `{id}` |
| `morph.customScreens.remove` | mutation | `{app, id}` | `{removed}` |

What every action does, in order:

1. **`app` is the served application.** `app` is 1 to `kMaxNameBytes` bytes and equals the current bundle's
   `application.id`, or the action is refused, before anything else, a `list` without a principal included. A screen
   checked against one application is never stored under another.
2. **The names** the action takes (`id`, `title`, `basedOn`) are each 1 to `kMaxNameBytes` bytes, or it is refused;
   they are checked before the person is looked at, except `save`'s optional `id`, which is checked after it. An `id`
   of `get`, `save` or `remove` also starts with `custom:` (`kCustomScreenIdPrefix`), so a request never reaches the
   store with an application screen's id. `save`'s `attributes`, when given, are at most `kMaxAttributesBytes` as
   JSON, checked with the names.
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
  JSON and can corrupt a byte after an earlier escaped character), a new screen records its base's version
  ([Versions](#versions)), and `checkCustomScreen` checks the rewritten text,
  the text that is stored, so the size bound holds for the longer id too; a refusal is the `err` reply's message,
  pointer included. Only then is `put` called, with `savedAt` the current time and `savedBy` the caller. `check` runs
  the same rewrite under a placeholder id as long as a real one, so its verdict is the one `save` would give.
- **`list` answers at most `maxListed` summaries** (200 by default) and says `truncated: true` when the store held
  more. A capped summary is at most `kMaxSummaryBytes` (4 KiB) and grows at most seven-fold in an envelope: the reply
  is written with control bytes escaped, so one becomes a six-byte sequence, and the envelope doubles each backslash of
  the reply. `registerCustomScreens` refuses a `maxListed` for which
  `maxListed * 7 * kMaxSummaryBytes + kCustomScreenReplyOverhead` exceeds `wire::kMaxEnvelopeBytes`, which is above 290;
  the default takes at most 5.8 MB.
- **`remove`** calls the store's `remove` under the caller.
- **A store's refusal is passed through unchanged**, from `get`, `put` and `remove`: the model adds no wording over
  the reason, so the person is told what the store said.

**Bounded replies.** Every field of a summary is capped when the summary is built, so the part of a `get` reply that is
not the document is bounded whatever the store holds: `id`, `title`, `basedOn`, `savedBy` and `broken.pointer` at
`kMaxNameBytes`, `broken.reason` at `kMaxBrokenReasonBytes` (1 KiB), and `attributes` at `kMaxAttributesBytes` (1 KiB,
as a whole: a record cannot be cut, so one too long is not sent and the screen is listed broken). A longer value is cut on a UTF-8 boundary and
ends with an ellipsis (U+2026). A reason can quote a whole host command, and a pointer or a principal can be of any
length, so without the caps the reply would have no bound. `mine` is computed from the stored `savedBy`, before the cap.
The four flags (`mine`, `opensWithoutParams`, `needsUpdate`, `needsAttention`) take under 80 bytes with their keys, so
with the keys, the punctuation and `savedAt` the fixed part stays under 350 bytes beside the 3399 of capped text.
One summary is then at most `kMaxSummaryBytes` (4 KiB) before escaping and at most seven times that after it (see
`list`).
A store keeps its ids within `kMaxNameBytes`, since an id shown cut could not be given back to `get` or `remove`; the
model's own are 39 bytes. An entry whose id lacks `custom:` is listed broken, and `get` and `remove` refuse its id
before the store is called, so the model cannot reach it: cleaning it up is the store's.

**Discovery.** A client learns that a server offers custom screens by calling `list`: a server that did not call
`registerCustomScreens` has no `morph.customScreens` model, and answers the client's registration of it with the
dispatcher's refusal, `unknown model type: morph.customScreens`. The client offers no custom screens then.

**The handle.** `CustomScreens` holds what the model uses, so anything else on the server shares its checks and its
cache. `check` and `get` are tasks to await from a coroutine, `allowance` is synchronous, and all refuse by value rather than
by throwing:

- `check(session, app, basedOn, document)` answers what the `check` action answers, a refusal or none. A session with
  no verified principal, an application the server does not serve and a name outside its bounds are refusals.
- `get(session, app, id)` answers what the `get` action answers, `{summary, document}` or a `CustomScreenRefusal`.
- `allowance(session, app, basedOn)` answers `describeAllowance` of the base screen, for a prompt: the params; each action with the
  kind of position it may be named in; each command the base runs without a press, with its trigger; the client-side
  commands; the host names and host commands; the delivered components. It is built from the same walk as
  `checkBaseRule`, so it lists what the rule compares. It is refused as `check` is: for an application the server does not serve, a name outside its bounds, a session
  with no verified principal, a `basedOn` that names no screen document of the bundle, a base the person may not open
  (`screenAdmitted`, with the base check's wording), and a base that does not load.

A `CustomScreenSummary` carries `id`, `title`, `basedOn`, `savedAt`, `savedBy` and:

- `mine`: whether `savedBy` is the caller. A screen another person saved is shown as theirs.
- `hash`: the content hash of the stored document. It changes whenever the document does, so a client holding a copy
  tells from a list alone whether its copy is current.
- `opensWithoutParams`: whether it opens from the navigator: it is not broken, and its base declares no params, since a
  custom screen declares exactly its base's. The base is read once per list, not once per screen.
- `broken`: the refusal the checks give the stored document today, with its pointer, or that its stored `attributes`
  are too long to send; absent when it may be mounted. A broken screen is listed, so the person can open its source
  and fix it, and is never mounted.
- `attributes`: the store's record, as the store answered it; absent when it records nothing.
- `needsUpdate`: the version the document knows (`knownVersion`) is below its base's, so the base changed since
  ([Versions](#versions)).
- `needsAttention`: the document refers to elements or fields its base no longer has.

The actions are `Loggable::No`: a screen is not application data, and the store keeps who saved it and when.

## The checks

`checkCustomScreen(bundle, session, authorizer, basedOn, document, screenId, options, cache)` answers the first
refusal, with its JSON Pointer, or none. `screenId` is the id the document is stored under. An overload takes the
person's permitted set already computed, in front of `basedOn`, so a list of many screens computes `permittedActions`
once; the set is never kept across calls. `save` and `check` run it on the request's document after the id rewrite;
`list` and `get` run it on every stored document they answer, under the entry's id.

1. **Bounds.** At most `maxDocumentBytes` (256 KiB by default, at most `kMaxFileBytes`), nested within
   `kMaxJsonDepth`. They are cheap and need no parse of the document, so they come first. The id rewrite parses the
   document, so `save` and `check` first refuse any document longer than `maxDocumentBytes` plus the most the rewrite
   can add (129 bytes: the `,"id":""` member and the 39-byte custom id, and the `,"knownVersion":""` member with the
   longest version a new screen records), with this check's wording, before the document is parsed. A document within that margin is rewritten, and the bound is checked again on the rewritten
   text, which is the text that is stored.
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
or an edit is a new key and checks again. It holds at most `verdictCacheSize` verdicts (1024 by default, at least one), dropping
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
reach other principals: a person learns of a screen's id only through their own list, whatever the store shares. `save` and `remove` declare `changes<CustomScreenSummary>`, the whole type: a save can add a screen to lists no key names yet. `list` and `get`
hold it, so a client's list refetches after any save or remove, from anyone. Saves are a person's deliberate act, so
the refetch is rare, and the verdict cache keeps it cheap.

The record, the result slots that let a list be found holding it, and the effects are registered at static
initialisation in `src/ui/custom_screens.cpp`, beside `registerCustomScreens`, because the process-level registries
resolve once, at their first read. A static build links them only into a server that calls `registerCustomScreens`; a
shared build carries them for every application, where they are inert.

## Versions

A custom screen is a full copy of its base's document, so a base that changes in a new release does not change the copy.
Versions let the person see that their copy is older than its base, and take over what they want once, after which the
copy counts as current. The functions are pure, over JSON texts, so the server and a client compute the same answer.

**The base's version.** An element of a document may carry `since` next to its `id`
([`document.md`](document.md#element-versions)), and a field of an action carries `x-since`
([`../forms/forms.md`](../forms/forms.md#field-versions--since)); both are `forms::FieldVersion`s. `baseVersion(base,
schemas)` is the highest of the base's elements' and of the fields of the actions its `form` nodes bind, the first
version (`0`) when there is none. `schemas` is an `ActionSchemaLookup`, an action's schema JSON by its id; an action it
does not know adds no fields.

**The copy's version.** A custom screen's document carries `knownVersion`, the base version it was last saved or
updated against; one without counts as the first version. The loader refuses one that is not a version.

- **A new screen records its base's version.** `save` without an `id`, and `check`, which checks a document as a new
  one, write the base's version as `knownVersion` into a document that carries none, after the id rewrite and before
  the checks, so the stored text is the checked text. A base at the first version has nothing to record, since a
  document without `knownVersion` already knows it. A document that carries one keeps it: **Save as…** of an older
  copy is as old as the copy. An overwrite (`save` with an `id`) keeps the document's as sent. What the person changes
  is saved through the same actions, so the controller's **Save**, **Save as…** and **Check and save** need nothing of
  their own.
- **The assistant does not change it.** `propose_screen` refuses a patch whose result has another `knownVersion` than
  the turn's current document, or none where it had one: `The patch changes /knownVersion, ...`. Only the person's
  update step moves it.

**What changed.** `layoutChanges(base, baseSchemas, layout)` loads both texts (with no catalog: the comparison reads
only their structure) and answers a `LayoutChanges`: both versions, `added`, `removed`, `needsUpdate()` (the copy's
version is below the base's) and `needsAttention()` (something is removed).

- **Elements.** An element is a node or a part with an `id`, found by walking the head's parts (`status`, `actions`,
  `menu`, `tools`) and the view; a form's field overlay is not one (its key is a field name), and a component's
  fallback is a scope nothing outside it names, so it is not walked. An id is matched by its first occurrence in
  document order.
- **Added elements:** each base element whose `since` is above the copy's `knownVersion` and whose id the copy does not
  have, in the base's document order, with its place: `parent`, the id of the nearest element holding it (empty for the
  document itself); `path`, the pointer from that element to the member holding it (`/children`, `/tabs`,
  `/view/children`, `/actions`); and `index`, its position when that member is an array. An element held by another
  added element comes with it and is not listed.
- **Added fields:** each field of a base form's action whose `x-since` is above `knownVersion`, for a form the copy
  has (the same id, or for a form without one the copy's first form of the same action) and gives no overlay for that
  field. A field with an overlay was decided already; a form the copy dropped shows nothing to decide.
- **Removed elements:** each element of the copy that carries a `since` and whose id the base no longer has. An element
  a person adds carries none, so it is never taken for one the base removed; an element of the base that may be
  removed in a later release is given a `since`.
- **Removed fields:** each field overlay of a copy's form that names a field its action's schema no longer has. A
  form whose action the lookup does not know is not compared.
- **Keys.** Each item has a `key` a choice names it by: an element's id, or `<form>/<field>`, where `<form>` is the
  form's id, or its action for a form without one.

A schema whose `x-since` is not a version is a refusal at the form's pointer; a document that does not load is the
loader's refusal.

**The update.** `applyLayoutChoices(base, baseSchemas, layout, choices)` takes the person's choice per item
(`LayoutChoices`, by key) and answers the updated copy:

1. Each removal chosen `Drop` is removed: an element from wherever it sits, a field's overlay from its form's `fields`.
   A removal with no choice is kept.
2. Each field addition not chosen `TakeOver` gets `"omit": true` in its form's `fields` (created when the form has
   none). An addition with no choice is left out, so the copy changes only where the person said. A field taken over
   needs nothing: the form shows every field its schema has.
3. Each element addition chosen `TakeOver` is inserted at its place: into the copy's element with the base's `parent`
   id (the document itself for none), in the same member, at the base's index or at the end when the member is
   shorter; a member the copy lacks is created. A parent the copy does not have, a member that is not a list where the
   base's was, and an occupied member where the base's held the element itself are refusals.
4. `knownVersion` is set to the base's version.

The result is loaded as a save loads it, so what comes back is a document; the server's checks still decide when it
is saved. A choice whose key names no item is refused, rather than ignored. `withKnownVersion(document, version)` sets
the member alone.

**Status.** The model computes it, since it holds the base: `list` and `get` fill each summary's `needsUpdate` and
`needsAttention` from `layoutChanges` of the stored document against the base as the bundle serves it, both `false`
for a screen whose base is gone or whose document does not load (such a screen is `broken`). The schemas come from
the server's dispatcher (`ActionDispatcher::schemasJson` of each model type that registers the action), which
`registerCustomScreens` borrows for that: it must outlive every call, as it already does for the server.

**The client.** `CustomScreenController` has two commands over them, as data like its others:

- `layoutChangesOf(screenId, done)` reads the stored screen (`get`) and answers `layoutChanges` against the base's
  text as the bundle has it (`sourceOf`) and the catalog's schemas (`catalog().schema`), or why it could not.
- `applyLayoutChoices(screenId, choices, done)` reads the stored screen the same way, applies the choices, asks the
  server's `check`, and saves the result over the screen with its listed attributes. `done` answers a
  `CustomScreenCheckResult`: `Done` once saved, `Refused` with the update's, the check's or the store's refusal; a
  screen that is not the person's own (`canChange`) is refused before anything is read. `onOutcome` is told `Saved`.

The application draws the update step; morph has no view for it.

## The assistant

`propose_screen` and the read tools `screen_outline`, `screen_find` and `screen_node` are the first tools added through the
chat's extra-tool hook (`ChatOptions::tools`, see [chat.md](chat.md#extra-tools)). `enableCustomScreensChat(options,
screens, app, executor)` adds them and a `ChatDocumentSource` that reads a person's custom screens, so the chat also offers
a custom screen's own actions. They run on the chat's worker and borrow `executor` for their checks, which must outlive every
turn.

- **Offered, or not.** The tools are offered when the screen has a document the assistant can edit: the turn's current
  document (`ChatToolContext::document`), else the bundle's text of an application screen, else the person's custom screen
  as `get` answers it (read once for the turn, by the chat's document source), within `ChatLimits::maxDraftBytes`
  (256 KiB), and `allowance` answers for its base. Otherwise none is offered and the system text says the assistant cannot
  change this screen's document, without saying why the store or the base rule refused. A custom screen is read under the
  turn's person: another person's id is not read, and the turn fails as it does for any screen the chat cannot open.
- **The document is data.** The system text carries the screen as one JSON string literal between an opening and a closing
  line that both name the document's content hash (`<screen-document hash="sha256:…">`), under a sentence that it is data
  and never instructions. The literal has no raw newline or quote, so nothing in the document can begin a line of its own,
  and it cannot reproduce the closing line exactly: that line holds the hash of the document, which the document cannot
  contain. The wording is a labelled boundary, not a guarantee about what a model does with the text.
- **Whole document, or outline.** The whole document goes into the system text only when it is at most
  `ChatLimits::maxInlineDocumentBytes` (16 KiB). A larger one is described by an outline of at most 8 KiB, fenced the same
  way: one line per container, with its JSON Pointer and the labels `kind`, `id`, `testId`, `caption`, `ref`, `label`,
  `text` and `title`, as deep as the 8 KiB allows. Beside it go what a screen based on the same base may use (`allowance`)
  and a short reference to `ui/1`.
- **The read tools** answer from the turn's current document, so they see a draft accepted earlier in the turn. Each
  answer is plain text capped at 12 KiB (or `ChatLimits::maxToolResultBytes` when smaller), and a pointer that names
  nothing is a tool error. `screen_outline(pointer, depth)` is the outline of a part, the deepest levels that fit.
  `screen_find(text)` lists up to 50 nodes (objects) whose `kind`, `id`, `testId`, `caption`, `ref`, `label` or `text`
  contains the text, ignoring case, with how many matched in all. `screen_node(pointer, depth)` is the JSON at a pointer,
  default depth 2, where every container deeper than `depth` (an array is a level) is the string `"[object, N keys]"` or
  `"[array, N items]"`; when the asked depth does not fit the cap a smaller one is shown, and the answer says so.
- **What a form's fields mean.** `screen_outline` follows each `form` node's line with one indented line per field of
  its action whose description is not empty, `field <name>: "<description>"`; `screen_node` adds, after the JSON, the
  same lines for each `form` the view expands, under a sentence that they come from the action's schema and are not
  part of the document. The schema is the turn's (`ChatToolContext::schemas`), and a description is resolved as the
  form shows it: the field's `x-i18nKey` key, then `<action>.<field>.help`, from the application's texts in the
  conversation's locale (`fieldTextsFor`, [service.md](service.md#texts)), else the schema's `description`. Each is cut
  at 200 bytes, and both stay within the answer's cap: the outline counts the lines with their node's, and the node
  lists the descriptions that fit and says when it left some out.
- **A call to `propose_screen`** carries `patch`, an RFC 6902 array (`add`, `remove`, `replace`, `move`, `copy`, `test`)
  of at most `ChatLimits::maxProposalBytes` (64 KiB) as JSON; the bound is on the patch alone. A whole document is one
  `replace` of the path `""`, with the document as `value`. There is no base to name: the patch applies to the turn's
  current document, which only the server changes within a turn. The operations apply one at a time to a copy, so a refusal
  names the failing one as `op <index> (<kind> <path>): <reason>`, counting from 0, and nothing is applied unless every
  operation applies. A call is a tool error, with the reason, when the arguments are malformed, an operation does not
  apply, the result is not one JSON object whose `id` is the id of the open screen (the client mounts a draft only under the
  screen's own id), or the result is longer than `maxDraftBytes`. Then `check` runs on the result under the turn's session
  against the screen's base (the screen's own id for an application screen, the stored `basedOn` for a custom one), which
  are the checks of a save. A refusal comes back as `The patch was refused at <pointer> of the patched document: <reason>`.
  Only an accepted result is set as `TurnState::draft` and sent as the `draft` event, so a second patch in the same turn
  applies on top of the first.
- **Its limit.** Every refused call, one the shape check refused included, counts toward `maxProposalsPerTurn`. The call that
  reaches it ends the turn as failed, with the last refusal in the message, and no draft is made. A turn that already had a
  proposal accepted keeps that draft. Each call, a read tool's included, also counts against `maxToolCallsPerTurn`.
- **Out of the history.** The conversation stores a patch under about 4 KiB as the model wrote it. A larger patch is stored
  as `{"patch":"[a patch of N bytes was proposed here, sha256:…]"}`, and the message's provider replay is dropped with it.
  The turn that proposed it still sees the whole call. The next turn is given the current draft in its context, so edits
  build on the last accepted one, and a long conversation of edits does not fill `maxConversationBytes`.
- **Does not save.** The tool never calls `save`. The person's press of **Save as…** does.

## The client

The client half is four framework pieces and the generic client's wiring
([`../client/client.md`](../client/client.md#custom-screens)). Drafts themselves are the document host's
([`host.md`](host.md), "Drafts").

**`CustomScreenSync`** talks to `morph.customScreens` for one application through the interpreter's own `Dispatcher`,
as `PreferenceSync` talks to the preferences model, so its calls carry the screens' session and go where their models
go. No call names a person.

- **Discovery.** `probe` calls `list`. An answer makes the offer `Offered`; any refusal makes it `Off`. A server that
  never registered the model refuses its registration with `unknown model type: morph.customScreens`, the ordinary way
  to say custom screens are not offered, and is not logged. Any other refusal (a server that has the model and refuses
  this client, a wrong `app`) is logged at warn, because it is a misconfiguration someone has to hear about, and custom
  screens are off for the person all the same. `offRefusal()` keeps the text.
- **The cache.** `CustomDocuments` keeps what `get` answered, loaded against the client's catalog, by id. It is what
  `DocumentScreenHostOptions::documents` answers for a `custom:` id, and that lookup is synchronous, so whatever opens a
  custom screen (a navigator entry, **Open as**, a restored route) fetches it first (`open` answers a kept document
  or `fetch`es one). A list that shows a kept screen with another `hash`, or a whole list that no longer shows it,
  drops it; the next open fetches the stored document again. A document that does not load in the client, or names
  another id, is not kept.
- **Lists stay current.** Once offered, the sync adds the record `morph.customScreens` to the session's extra watches.
  The session sends them through the source's watcher with `replace: false`, and again after each reconnect and each
  `resync`, since both replace the connection's watch set; the extra watch outlives the cache's own interest in the
  record. A `changes` event naming the record relists. An in-process source has no watcher and sends no events, so its
  lists refresh at a frame, a reconnect and this client's own saves. A refused watch is logged at warn; an admitted
  one is silent.
- **Calls.** `check` and `save` send the document compact (`compactJson`: the white space between tokens removed, members
  in their order), since the server bounds the text before it parses it and the Source view's indentation would otherwise
  count; a refusal's JSON Pointer is the same for either text, a byte offset in it is into the compact one. `check` answers
  the server's first refusal with its pointer; `save` passes a store's refusal through as
  the store worded it, and relists after an answer; `get` answers without keeping, for a broken screen's source.
- **Remove.** `remove(id, done)` calls `remove`; a store's refusal reaches `done` as the store worded it. An answer
  forgets the kept document and relists, so no list or `documents` lookup keeps a screen that is gone.
- **Restore.** `prefetchRoutes(routes, done)` fetches every custom screen a record or a history names that is not
  kept, and answers the ids no document came for, so a start can tell the person which were not restored.
- **Sessions.** `reset` forgets the offer, the list, the cache and the binding, and drops replies in flight: what a
  sign-in or sign-out does before probing again.

**`CustomScreenController`** is the chrome's logic without a view, for an application that draws its own controls
over the same drafts. It holds, per tab, the draft's text, a generation that changes with every draft mounted or
ended, whether a save is in flight and the last refusal of a save (`message`), and runs the commands: `source` and
`storedSource` (the text a source view opens with), `preview`, `checkAndSave`, `proposeDraft`, `saveAs` (with the
attributes the store is asked to record), `save`, `discard`, `openAs`, `openCustom`, `remove`, and `layoutChangesOf` and
`applyLayoutChoices` ([Versions](#versions)). Its state is
reactive, so a view's bindings follow `drafted`, `saving`, `message`, `offered` and `canChange`. A view calls
`follow(tab)` when it draws a tab, as the chrome does from `WorkspaceOptions::chrome`.

- **Outcomes and refusals are data.** A check from a source view answers a `CustomScreenCheckResult`: `Done`,
  `Refused` with the refusal's pointer and reason, or `Dropped`. A save or removal answers the store's refusal as it
  gave it. What the person should be told that no command's answer carries is a `CustomScreenOutcome` through
  `onOutcome`: an event (saved, saved but the draft is gone or replaced or the tab closed, saved but it stays a draft,
  removed, a tab that stayed open on a removed screen, Open as cancelled, a failed open, an unreadable source, a
  proposal not shown), which save it was about, the screen's title, the reason, and a tone. Wording them is the view's.
- **Stale answers.** An answer acts only on what it was asked about. A save's answer acts only while the tab still
  holds the draft of the generation it was asked under, and tells how it ended otherwise. A check's answer is
  `Dropped`, and mounts nothing, when the tab's draft changed meanwhile (an assistant's proposal, a discard) or the
  view dropped its answers with `dropSourceAnswers`, as the chrome does when its source view closes or is replaced.
  Answers asked under a workspace that `attach` has since replaced, or after the controller is gone, do nothing.

**`CustomScreenChrome`** is the window chrome a shell mounts at the top of each document screen's page
(`WorkspaceOptions::chrome`), built from view nodes so every frontend shows it. A host screen gets none. It is a view
over a `CustomScreenController` it owns (`controller()`): it draws the controller's state, keeps its own dialogs, and
words the controller's outcomes (`describe`) for `notify`.

| Part | Test id | What it does |
|---|---|---|
| The draft bar | `draft_bar_<id>` | Shown while the tab holds a draft: "Draft — not saved", that forms on a draft change real data, the last refusal of a save (`draft_message_<id>`), **Save** (`draft_save_<id>`, for a draft of the person's own custom screen), **Save as…**, **Source**, **Discard** |
| The tools row | `screen_tools_<id>` | Shown while the tab holds none: **Open as** (`open_as_<id>`), **Source** (`source_button_<id>`) and **Remove** (`remove_<id>`, for a screen of the person's own) |
| The source view | `source_dialog` | A sheet, so the editor has the window: the document's JSON pretty-printed (`prettyJson`: indent 2, a container that fits in about 100 columns on one line, members in their order, scalars as written) in a monospace editor (`source_text`, `wrap: false`: its lines stay whole and it scrolls sideways), the refusal (`source_refusal`), a status line ("Checking…", "Saving…"), **Preview**, **Save as…**, **Check and save** (`source_save`, a stored screen of the person's own), **Close** |
| The save dialog | `save_as_dialog` | The name (`save_as_title`), the refusal, **Save**, **Cancel** |
| The removal | `remove_dialog` | Asks "Remove <title>?" once: the refusal (`remove_message`), **Remove** (`remove_confirm`), **Cancel** (`remove_cancel`) |

- **Source** shows the open draft's text, a custom screen's stored text (fetched again when the cache dropped it), or
  an application screen's as the bundle has it, through `prettyJson`. While custom screens are not offered it is read only, with no **Preview** and no
  **Save as…**.
- **Preview** loads the text in the client (`loadDocumentText`), asks the server's `check` against the screen's base,
  and mounts what passes with `DocumentScreenHost::mountDraft`, closing the view. A draft keeps the screen's id, so
  the host refuses a document with another `id` at `/id`, and that refusal's pointer is shown as any other. While the
  check runs the editor is read only and **Preview** is disabled, so the draft is exactly the text that was checked,
  and the answer is taken only by the view it was asked for. A refusal shows "Line N: reason", the line its
  pointer names in the text as the person left it (`pointerOffset`, `lineAt`); choosing it moves the cursor there
  (`Mounted::placeCursor`); a refusal about the document as a whole names no place and leaves the cursor. The client's checks are fast feedback; the server's decide.
- **Save as…** asks a name, saves the draft (from the source view, it previews first, so nothing is saved the person
  has not seen mounted), ends the draft and opens the saved screen in the tab's place, with the same params
  (`WorkspaceModel::openInPlace`). The saved document is stored, so the old screen closes without asking when the
  draft was all that kept it dirty; otherwise its close guard decides. When the screen stays open the tab stays a
  draft as it was (bar, text, generation) and the person is told the document is saved but stays a draft there until
  the screen can close. **Save** overwrites the person's own custom screen with the draft, fetches the
  stored document again and ends the draft; both tell the person. **Save** and **Check and save** send the screen's
  listed `attributes` back with it, so an overwrite keeps what the store recorded; the chrome's **Save as…** asks for
  none. A refusal shows in the bar and in the dialog, and
  the draft stays. While a save is in flight the bar's **Save** and **Save as…** are disabled, and its answer acts
  only on the draft that was saved: if the tab holds a newer draft, none, or is closed by then, the tab is left as it
  is and the person is told the save completed. Tab ids are issued per workspace, so a chrome attached to another
  workspace ignores every answer asked under the old one.
- **Remove** is offered on a listed custom screen of the person's own (`mine`) while custom screens are offered, the
  screens **Save** may overwrite; whether the removal is allowed is still the store's. One confirmation asks first.
  On the answer the confirmation closes and every tab that shows the screen opens its base in its place, with the
  tab's params (`WorkspaceModel::openInPlace`), so a tab never shows a screen that is gone. A tab that is dirty goes to
  its close guard; one the person keeps open stays on the removed screen and the person is told. A refusal stays in
  the confirmation, as the store worded it, and nothing changes.
- **Discard** mounts the screen's own document again (`discardDraft`), fetching a custom screen's first when the cache
  no longer holds it.
- **The assistant's draft.** `proposeDraft(tab, screenId, text)` mounts a document the assistant proposed as the
  tab's draft the way **Preview** mounts an edit: the client loads it, and the draft bar, the generation and the
  unsaved state are the same, so a second proposal replaces the first and **Discard** restores the screen. The server
  checked the document before proposing it, so it is not asked again. It does nothing, and tells nobody, when custom
  screens are not offered, the tab is closed, or the tab no longer shows `screenId`; a document the client cannot load
  or mount is reported with a warning toast and the tab stays as it was. A source view open on the tab is closed, since
  its text is the one the draft replaced. The shell calls it only for the tab the turn was started in. `draftOf(tab)`
  gives the text of the draft a tab holds, or nothing for a tab with no draft or a closed one; the shell sends it as the
  next turn's draft, bounded by the default `ChatLimits::maxDraftBytes` ([`chat.md`](chat.md#the-panel)).
- **One draft per window**: a new preview replaces the tab's draft. Closing a tab with a draft goes to the shell's
  close guard, since the handle is dirty while a draft is up.
- **Open as** is offered on a screen whose base takes params, when the list holds custom screens of that base: the
  base first, then each of them by its store title, "· by <savedBy>" when not the person's, "· broken" when broken. A
  choice opens in the tab's place with the tab's params. A tab that holds a draft does not offer **Open as**: the
  person saves or discards first. When the tab's screen is dirty without a draft and the person keeps it open at the
  close guard, the choice is cancelled and the person is told "Open as cancelled: <screen> stayed open", which is not
  an error.
- **The navigator's entries** (`navigatorEntries`) are the listed screens whose base takes no params, read from the
  base rather than from `opensWithoutParams`, which a broken screen does not have, so a broken screen is still listed
  with its reason.
- **A broken screen** is never mounted: opening it from the navigator or **Open as** opens its source view as `get`
  answers it, the breakage shown as the refusal. For the person's own screen while offered, **Check and save**
  (`source_save`) checks the text on the server and saves it over the screen.

## Not yet

- **The chrome's words** ("Draft", "Save as…", "Source", "Open as", the refusal's "Line N", and `describe`'s outcome
  sentences) are English; resolving them through the bundle's message catalogues, as the shell frame's `morph.shell.*`
  labels are, is still to come. They belong to the view: the controller carries no words, only outcomes as data.
- **The terminal's source view** is the same dialog; it is best effort, as the spec says.

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
| The client's draft logic | A headless `CustomScreenController`, with `CustomScreenChrome` a view over it | An application with its own controls reuses the drafts, the saves and the stale-answer checks instead of rebuilding them; the words stay in the view, the one place that localises them. |
| Servability | A bound on `maxDocumentBytes` that fits a file and the escaped `get` reply, rechecked on the rewritten text | Any stored document is then served within the envelope, so no per-document check is needed at serve time. |

# Custom screens

A person using a morph GUI makes their own version of a screen. They open a screen, ask the built-in assistant to
change it or edit its `ui/1` source themselves, watch the result live as a draft, and save it under a name. The saved
screen is offered beside the application's own screens and opens like any of them, on any device.

## Why

An application's screens fit the work most people do, and never exactly the work of each person. A person who always
ignores one panel, or who needs two fields side by side, today waits for a developer. The screens are already data, a
JSON document the client mounts, and the assistant already runs on the server beside them, so a person can be given
the document itself, provided the document can then do nothing the screen it came from could not.

## The experience

1. On the sample screen, the person tells the chat "put the notes field at the top and drop the audit panel".
2. The window changes in place, under a bar saying it is a draft and not saved. Its forms still work on real data.
3. They say "make the notes field wider". The draft changes again.
4. They open **Source**, see the draft's JSON, rename a heading by hand and press **Preview**. The draft shows it.
5. They press **Save as…**, type "My sample", and the draft becomes a saved custom screen.
6. On another workstation, after signing in, any sample's window offers **Open as → My sample**.

Success is that sequence on the LIMS sample screen, with no screen-specific code for it in LIMS.

## Goals

- A store seam for custom screens on the server, whose implementation is the application's, and a framework model that
  serves it to the client under the verified person.
- A custom screen is checked when it is saved and whenever it is loaded, and can do nothing its base screen cannot.
- **Source** on every open document screen: its `ui/1` JSON, editable, previewed as a draft, saved as a custom screen.
- One chat tool that proposes a whole new document for the open screen, shown as a draft.
- Custom screens in the navigator, and offered on the screen they were made from.

## Non-goals

- **Who may see, save, share or delete a custom screen.** That is the application's policy, decided in its store. The
  framework decides only what is the same for every application: what a custom document may contain, and that it is
  checked.
- A second notation for documents. The source is the `ui/1` JSON the server sends.
- Source for host screens. The explorer, the inspector and other C++ host screens have no document.
- Custom screens following later changes to the screen they came from. A custom screen is a copy.
- A custom screen doing more than its base: another screen's actions, a new component, a file or host command its base
  does not use. A person rearranges, removes and restyles; a developer adds capability.
- Offline editing. Saving needs the server.

## Anti-goals

This has failed, even if it works in a demo, if:

- a custom document gets past any check an application screen's calls get: authentication, `authorize`,
  `authorizeInstance`, limits or the journal;
- opening a custom screen someone else saved can do anything opening its base screen could not;
- a custom screen can take the place of an application screen, of an application component, or of another person's
  custom screen;
- a document an application screen would be refused for can be saved as a custom screen;
- the store refuses something and the person is not told;
- the assistant's document reaches the client before the server has checked it.

## Decided by the owner

- Custom screens are stored on the server and served by it.
- Visibility and every other policy over custom screens is decided outside the framework, by the application's store.
- The source the person sees and edits is the `ui/1` JSON.
- An assistant's change is a live draft in place of the window. Nothing is stored until the person saves.
- The UI document design's "free layout editing" exclusion is narrowed by this design: a person still cannot move or
  size panels on an application screen, and may make a screen of their own.

## Decided in design

- **No more than its base.** Everything a custom document can cause, it shares with the application screen it is
  based on (see [The base rule](#the-base-rule)). This is the framework's answer to sharing: a store may show Alice's
  screen to Bob, and Bob opening it can do only what Bob opening the base screen can. It needs no policy and no trust in
  who saved it.
- **A copy, checked every load.** A custom screen keeps `basedOn` and nothing else of its origin. The checks run again
  whenever it loads, against the current bundle and the person's current permissions, so a document the application
  has since outgrown is reported, not mounted.
- **Save overwrites; Save as… creates.** Saving a draft of a custom screen replaces that screen. **Save as…** always
  makes a new one.
- **Whole documents, not patches.** The assistant and the source view both produce a complete document, which is what
  the checks take and what the store keeps.
- **A model, not new envelope kinds.** Custom screens travel as the actions of a framework model, as preferences do, so
  the session, `authorize`, effects and the client's transport are the ones every model already has.
- **One spec, three plans:** the server (store, model, checks), the client (drafts, source view, navigator, **Open
  as**), and the assistant (`propose_screen`). Each lands and is tested on its own, in that order.

## How it fits what exists

| Exists today | Used for |
|---|---|
| `ui::loadDocument`, `UiRegistry`'s screen checks, `author`'s accessibility rules | The document checks |
| `UiService::catalogAnswer`'s `permitted` loop over `allActions` | Which actions a person may run, shared by the model |
| `DocumentActions` (`src/ui/service.cpp`) | The actions, components and commands a document uses, for the base rule |
| `PreferencesModel`, `IPreferencesStore` (`preferences.md`) | The pattern: a store seam, a model registered once, the person from the session |
| Effects and fan-out (`docs/KEEPING-SCREENS-CURRENT.md`) | Lists of custom screens refresh after a save or a delete |
| `DocumentScreenHost::remount` (`host.hpp`) | Mounting a draft in place, keeping state by name, and discarding it |
| `DocumentScreenHostOptions::documents` | Where the client finds a custom screen's document |
| `ChatService`, `screenTools`, `TurnState` (`chat.md`) | The assistant's draft tool and how a draft reaches the client |
| `Workspace`, `Navigator` (`workspace.md`) | A tab for a custom screen, the navigator's custom section, **Open as** |

## What has to change in what exists

- **`permitted` becomes a function.** `permittedActions(bundle, session, authorizer)` is the loop
  `UiService::catalogAnswer` runs today, so the catalog and the custom screens model answer the same set. `permitted`
  lists only actions some served document names; that is also the most a custom screen could name, under the base rule.
- **`ServedBundle` keeps its checking catalog.** The `Catalog` the registry loaded each screen with is kept on the
  served bundle, so the model loads a custom document with the same facts.
- **The model sees the current bundle.** A development reload replaces the served bundle. The model is given a getter
  for the current one, not a snapshot.
- **`remount` is not only for development.** Its contract already fits a draft: same params, same place, state carried
  by name, a refused mount keeping the old screen. Its documentation states drafts as a use.
- **`UiRegistry` reserves `custom:`.** An application screen id starting with `custom:` is refused at registration.

## The store

```cpp
struct CustomScreenKey { std::string person, app, id; };
struct CustomScreen {
    std::string id;          // custom:<uuid>, issued by the model
    std::string title;       // the person's text, naming the entry
    std::string basedOn;     // the application screen id it was made from
    std::string document;    // the ui/1 JSON text
    time::DateTime savedAt;
    std::string savedBy;     // the principal that saved it
};
struct CustomScreenRefusal { std::string reason; };   // shown to the person as it is

class ICustomScreenStore {
public:
    virtual async::Completion<std::vector<CustomScreen>> list(std::string person, std::string app) = 0;
    virtual async::Completion<std::expected<CustomScreen, CustomScreenRefusal>> get(CustomScreenKey) = 0;
    virtual async::Completion<std::expected<void, CustomScreenRefusal>> put(CustomScreenKey, CustomScreen) = 0;
    virtual async::Completion<std::expected<void, CustomScreenRefusal>> remove(CustomScreenKey) = 0;
};
```

- **The store holds the policy.** `list` answers the screens this person may open, which may include screens other
  people saved. `put` and `remove` refuse what this person may not do, with a reason for the person: overwriting a
  screen they may only view, a count over the application's limit, anything the application decides. Sharing, roles,
  approval and limits on how many screens a person keeps belong here.
- **The store is given checked values only.** The model calls it with a person the server derived and a document that
  has passed every check. Atomicity of its own rules, such as a count, is the store's.
- **`MemoryCustomScreenStore(base)`** is the framework's store, for tests and examples: a map on an `OwnerStrand`, each
  person seeing and changing only their own screens. Its entries last as long as it does.

## The model

`CustomScreensModel` is registered under `morph.customScreens` by `registerCustomScreens(store, bundle, authorizer,
dispatcher, registry, options)`, which a server that offers custom screens calls once before it serves. `bundle` is a
getter for the current `ServedBundle`; `authorizer` is the server's, for `permittedActions`. Its action ids are
`morph.customScreens.<action>`, so they cannot meet an application's. It refuses a `maxDocumentBytes` above
`kMaxFileBytes`, or one whose document, escaped into a reply with its summary, could exceed the envelope bound, so
every document it accepts is one a reply can carry.

| Action | Kind | Body | `ok` reply |
|---|---|---|---|
| `list` | query | `{app}` | `{screens: [{id, title, basedOn, savedAt, savedBy, mine, opensWithoutParams, broken?}]}` |
| `get` | query | `{app, id}` | `{summary, document}`: the `list` entry of the screen, and its document text |
| `check` | query | `{app, basedOn, document}` | `{refusal?: {pointer, reason}}`; absent when the document would be saved |
| `save` | mutation | `{app, id?, title, basedOn, document}` | `{id}`; a refusal is the error's message, its pointer in the text |
| `remove` | mutation | `{app, id}` | `{removed}` |

- **The person** is `session::current()->principal`, as for preferences. A body naming a person has it ignored. With no
  verified principal, `list` answers nothing and the other actions are refused with
  `custom screens are stored for a signed-in person`.
- **`save` with an `id`** overwrites only a screen `get` answers for this person; any other id is refused with
  `no such custom screen`, so a client cannot choose an id. With no `id`, the model issues a new id.
- **`app`** must be the served bundle's application id; a request naming another is refused before anything else, a
  `list` without a principal included, so a screen checked against one application is never stored under another.
- **A store's refusal reaches the person as the store gave it**, from `get` as from `put` and `remove`; the model adds no wording of its own over it.
- **`mine`** is whether `savedBy` is the caller, so a screen someone else saved is shown as theirs.
- **`broken`** is the refusal the checks give for a stored document today, with its pointer. A broken screen is still
  listed, so the person can open its source and fix it, and is never mounted.
- **Effects.** `save` and `remove` declare `changes` on the record type `morph.customScreens`, the whole type: a save
  can add a screen to lists no key names yet. Effects fan out by record type to every connection watching it, not by person, so every client with a list of custom
  screens open refetches its own `list` after anyone's save. Saves are a person's deliberate act, so this is rare; the
  verdict cache below keeps each refetch cheap.
- **Journal.** The actions are `Loggable::No`. A screen is not application data; the store keeps who saved it and when.

## The checks

`save` and `check` run them in this order, and `list` and `get` run them on every stored document they answer. Each
stops at its first refusal, with its JSON Pointer and reason, as `loadDocument` does: `check` and `broken` carry the
pointer as a field, and a refused `save` carries both in its error message.

1. **Bounds.** The document is at most `CustomScreensOptions::maxDocumentBytes` bytes, 256 KiB by default, and parses
   within `kMaxJsonDepth`. `title` is 1 to `kMaxNameBytes` bytes.
2. **The base.** `basedOn` is a screen document of the current bundle that the person's catalog lists. A custom screen
   based on something else, or on a screen the person can no longer open, is refused.
3. **The application screen checks.** `ui::loadDocument` with the served bundle's catalog; the document is a screen,
   not the app document.
4. **The accessibility rules**, `author`'s `checkAccessibility`, which application screens built with the author
   builders already pass at registration.
5. **The base rule**, below.
6. **Permission.** Every action the document names is in `permittedActions` for this person.
7. **The id.** The document's screen id is replaced by the custom screen's own id before it is stored or answered.
   The document is written out again for this, so the stored text keeps its content, not its key order or spacing.

**The verdict cache.** The result of checks 3 to 6 is kept by the model, keyed by the document's content hash, its
`basedOn`, the bundle digest, and a hash of the person's permitted set, in a cache bounded by `CustomScreensOptions::verdictCacheSize`
entries. Check 2 runs before the cache on every call, because what it reads, a catalog entry's availability, is not part of the bundle digest. A `list` over unchanged documents reads verdicts; a reload, a permission change or an edit misses the cache
and checks again.

The checks are a guide for the person and a bound on what a document contains. The boundary on what it does is still
the server's checks on every call a mounted document makes, and the base rule for what the client does without one.

## The base rule

A custom document's capabilities are a subset of its base document's, as `DocumentActions` and the document walk find
them:

| Capability | The custom document may use |
|---|---|
| Actions: queries, mutations, forms, collections, wizards, wherever declared, component fallbacks included | Only actions the base uses, each on a model type the base binds it to and in a position of the same kind: a query, a mutation, a form the person submits, a form that submits itself (any `submit` but `"explicit"`: a form with none keeps its schema's mode, which is automatic unless the schema says otherwise), a model-backed table, a collection, a wizard |
| Commands that run without the person: `onMount`, `onUnmount`, `onBackendChange`, `onClose`, `onSave`, watches, a form whose `submit` is not `"explicit"`, a delivered or host component's `on` events (its own code decides when they fire), and the `onSuccess` and `onError` of a mutation one of those runs, an `upload` or a `signIn`, followed through every chain, each name resolved in the scope that declares it | Only the actions and commands the base runs from the same trigger, so an unattended run cannot chain to an action the base only runs on a press |
| Client-side commands: `upload`, `download`, `switchBackend`, `signIn`, `signOut`, `resetPrefs` | Only the command kinds the base uses anywhere, and a `switchBackend` only to a backend the base names; an `upload`'s file only as `{"ref": "event"}` directly in the `onPick` of a `filePicker` that opens files (no `mode`, or the literal `"open"`) or a `dropZone`'s `onDrop`, so it is always a file the person just chose |
| `host` commands | Only a command identical to one of the base's, name and arguments, with literal arguments: an argument that names a state, a let or an element would resolve in the custom document, whose author chose its value. A host command's handler still treats its arguments as untrusted input |
| Delivered components | Only the components the base declares, by name and content hash |
| Host names: host components and host values, and the names of `host` commands, as one set of every string member named `host` | Only the names the base uses |
| `navigate` | Any screen the person's catalog lists, as a navigator entry already would |
| Messages (`t` keys), styles, layout, literals, expressions, states | Anything the vocabulary allows |

Why each row is there:

- A custom screen someone else saved is code the viewer runs. Anything that acts without a press, or acts on the
  viewer's machine rather than through the server, must already be something the viewer accepts by opening the base.
- A component or host name the base does not use could register under a name another screen relies on, or reach a
  host feature no shipped screen exposes.
- A button the person presses runs an action through the server under their own session, so the server's checks
  bound it. Restricting it to the base's actions still keeps a shared screen from putting an action, say a delete,
  where its base never offers one.

**Params match the base.** The custom document declares exactly the params the base declares, with the same types, so
it opens wherever its base opens with the params the base was given.

## Ids and where a screen opens

- **The namespace.** A custom screen's id is `custom:` and 32 random hexadecimal digits issued by the model. A client resolves a `custom:` id
  through its custom document cache and every other id through the bundle.
- **The client's cache.** `CustomDocuments` holds loaded custom documents by id, filled by `get`, and answers
  `DocumentScreenHostOptions::documents` for `custom:` ids. It drops an entry when its `list` refetch shows the screen
  changed or gone. Because `documents` is synchronous, anything that opens a custom screen, a navigator entry, **Open
  as** or a restored tab, awaits `get` into the cache first and then opens.
- **No params.** A custom screen whose base takes no params is an entry in the navigator's **Custom** section, after
  the application's groups, under the store's `title`. Every custom entry carries a custom-screen mark, and one that is
  not `mine` names who saved it, so it never passes for an application entry. A broken one carries a badge with its
  reason.
- **With params.** A custom screen whose base takes params opens where its base opens: the base's window chrome
  offers **Open as**, listing the custom screens based on it and the base itself. Choosing one opens it with the same
  params in the same tab.
- **Titles.** The store's `title` names the entry in the navigator and in **Open as**. The tab caption is the
  document's own title, which may show the record.
- **Routes and tabs.** A custom screen's route is its id and params, so it is recorded with the workspace's tabs and
  restored with them. A recorded custom screen that `get` does not answer is not restored, and the person is told.

## Drafts

A draft is an unsaved document mounted in place of a window's screen. Both the source view and the assistant make one,
and it behaves the same whichever made it.

- **Mounted by `remount`.** The client runs `loadDocument`, then `DocumentScreenHost::remount(handle, draft)`. The draft
  keeps the screen's params and place, and each state starts from the screen's value of the same name and type, so a
  half-filled field survives a draft. A refused mount keeps the screen and shows the refusal.
- **Identity.** A draft keeps the screen id, params, identity and route of the screen it replaced, so the tab's record
  names the saved screen, never the draft.
- **The bar.** A draft shows a bar above the screen: "Draft — not saved", **Save** (for a draft of a custom screen
  that is `mine`), **Save as…**, **Source** and **Discard**. It says that forms on the draft change real data.
- **A real screen.** A draft has the same models and params as the screen it replaces, so it shows live data and its
  actions run as on any screen.
- **Discard** remounts the replaced screen's document the same way. Closing a tab with a draft open asks first, as a
  tab with unsaved edits does.
- **One draft per window.** A new draft replaces the current one.
- **The bundle changes under a draft.** A development reload that changes the base leaves the draft mounted. The draft
  is checked against the new bundle at its next **Preview** or save.

## The source view

**Source** is in the window chrome of every open document screen, and in a draft's bar.

- It opens as an overlay holding the document's JSON, pretty-printed: an application screen's document as the bundle
  has it, a custom screen's as `get` answers it, or the open draft's. The overlay is a plain monospace text editor and
  the refusal, so every frontend can host it; the terminal frontend's is best effort.
- **Preview** runs `loadDocument` at once in the client, and the server's `check` after it. A refusal is shown with
  its pointer, and choosing it moves the cursor to its line. A document with none becomes the window's draft.
- **Save as…** from the source view previews first and then saves, so it never saves a document the person has not
  seen mounted.
- The client's checks are fast feedback. The server's are the ones that decide.

## The assistant

The chat on a document screen offers one more tool beside the screen's action tools: `propose_screen`.

- **Its context.** With a draft open, the client sends the draft's document in the `TurnRequest`. The server bounds and
  parses it as a `check` would before using it. With no draft, the server reads the screen's own document from the
  bundle, or a custom screen's from the store. The system text gains the `ui/1` vocabulary reference and what the base
  rule allows this screen to use.
- **The document is data.** The document in the context is wrapped and named as the screen being edited, never as
  instructions, because a shared custom screen or a hand-edited draft is text someone else may have written, in a turn
  whose tools run as the person.
- **Size.** The assistant edits documents up to `ChatLimits::maxProposalBytes`, 64 KiB by default, which fits the
  conversation bound and a provider's output. For a larger screen the tool is not offered and the chat says so; the
  source view still works.
- **Its argument** is a complete `ui/1` document for the same base. The tool runs the checks of `check`. A refused
  document comes back to the model as a tool error with the refusal and its pointer.
- **Its limit.** At most `ChatLimits::maxProposalsPerTurn` refused proposals, 3 by default, each also counting against
  `maxToolCallsPerTurn`. Past it, the turn stops and tells the person why, in the chat, and the window stays as it was.
- **Out of the history.** A proposal's document is not kept in the conversation; the history records that a draft was
  proposed and its content hash. Each turn gets the current draft fresh as its context, so a long conversation of edits
  does not fill `maxConversationBytes`.
- **A draft reaches the client only checked.** An accepted proposal becomes `TurnState::draft`, the document text, and
  a `draft` event on the `chat` topic. The client mounts it as the window's draft. Events are hints, as for every chat
  event: a client that misses one reads the draft from `TurnState`.
- **Conversations.** A conversation stays keyed by the screen id and record the tab shows. A draft does not change the
  tab's screen id, so drafting continues one conversation. Opening a saved custom screen starts that screen's own.
- **What the assistant still does not do.** It does not save, fill in forms on the person's behalf, or navigate. Saving
  is the person's press of **Save as…**. The chat design's non-goal "the assistant does not operate the open GUI"
  holds, except that it may propose a draft of the screen's document.

## Errors

| What happens | What the person sees |
|---|---|
| The store refuses a save or a delete | The store's reason, in the draft's bar or the source view; the draft stays open |
| A stored screen fails the checks today | The navigator's badge, and **Open as** marking it broken; opening it opens its source |
| A draft does not load in the client | The refusal in the source view, or a chat message for an assistant draft; the window is unchanged |
| The server's `check` refuses what the client loaded | The server's refusal, in the source view |
| The assistant runs out of proposals | A chat message with the last refusal; the window is unchanged |
| A screen is over `maxProposalBytes` | A chat message that the assistant cannot edit this screen; **Source** still works |
| A recorded custom screen is gone | A notice that it was not restored |
| No custom screen store is registered | **Source** shows the document read-only; no save buttons, no **Open as**, no custom section |

## Testing

- **Store and model.** `save` then `list` and `get` answer the screen; `remove` removes it; with no verified principal,
  nothing is answered and writes are refused; a body naming another person reads only the caller's screens; `save`
  with an id `get` does not answer is refused.
- **Every check refuses.** One document per check, from a document that passes, mutated so it fails that check alone:
  too large, an unknown base, one `loadDocument` rule, the app document, one accessibility rule, one action outside
  `permittedActions`.
- **Every base rule row refuses.** One mutation per row of the base rule table, each adding to a passing custom
  document something its base does not have: another screen's action, an `onMount` run, an `upload`, a host command, a
  delivered component, a host component, a changed param. Each confirmed to pass with its row's check removed, so the
  test fails without the rule.
- **The id cannot take a place.** A document claiming an application screen's id is stored under its own `custom:` id;
  `UiRegistry` refuses an application screen id in `custom:`.
- **Permission now, not at save.** A saved screen whose base or action the person later loses is reported broken by
  `list`; a call a mounted draft makes that `authorizeInstance` refuses is refused as on any screen.
- **The verdict cache.** A changed permitted set or bundle digest misses it; the cache stays within its bound.
- **Refusals reach the person.** A store whose `put` refuses shows its reason in the draft's bar.
- **Drafts.** A draft keeps a state the screen had; **Discard** restores the screen; a draft that fails to load leaves
  the screen mounted.
- **The assistant.** With a scripted provider: an accepted proposal becomes `TurnState::draft`; a refused one comes back
  as a tool error; three refusals end the turn with a message and no draft; a turn with a draft open sends it, bounded,
  as the context; a proposal's document is not in the stored conversation.
- **The proof.** The LIMS sample screen, in a GUI test: the assistant drops a panel, the draft shows it, the source is
  edited by hand and previewed, **Save as…** "My sample", and after a reconnect **Open as** offers it on a sample and
  opens it.

## Specs this changes

- `2026-10-07-ui-document-design.md` §15: free layout editing stays out of scope for application screens; a person's
  own screen is this design's.
- `2026-11-07-server-hosted-chat-design.md` and `docs/spec/ui/chat.md`: the non-goal on operating the GUI admits a
  proposed draft; `TurnRequest`, `TurnState` and the `chat` topic carry a draft; `ChatLimits` gains
  `maxProposalBytes` and `maxProposalsPerTurn`.
- `docs/spec/ui/service.md`: `permittedActions`, the reserved `custom:` prefix, and the catalog kept on
  `ServedBundle`.
- `docs/spec/ui/host.md` or its section of `interpreter.md`: `remount` for drafts.
- `docs/spec/ui/workspace.md`: the navigator's custom section, **Open as**, and custom routes.
- A new `docs/spec/ui/custom_screens.md` for the store, the model, the checks, the base rule and drafts.

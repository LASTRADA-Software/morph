# Custom screens

A person using a morph GUI makes their own version of a screen. They open a screen, ask the built-in assistant to
change it or edit its `ui/1` source themselves, watch the result live as a draft, and save it under a name. The saved
screen is listed beside the application's own screens and opens like any of them, on any device.

## Why

An application's screens fit the work most people do, and never exactly the work of each person. A person who always
ignores one panel, or who needs two fields side by side, today waits for a developer. The screens are already data, a
JSON document the client mounts, and the assistant already runs on the server beside them, so a person can be given
the document itself without opening a route to anything the document cannot already do.

## The experience

1. On the sample screen, the person tells the chat "put the notes field at the top and drop the audit panel".
2. The window changes in place, under a bar saying it is a draft and not saved. Its forms still work on real data.
3. They say "make the notes field wider". The draft changes again.
4. They open **Source**, see the draft's JSON, rename a heading by hand and press **Preview**. The draft shows it.
5. They press **Save as…**, type "My sample", and the draft becomes a saved custom screen.
6. On another workstation, after signing in, "My sample" is in the navigator, and opens on any sample.

Success is that sequence on the LIMS sample screen, with no screen-specific code for it in LIMS.

## Goals

- A store seam for custom screens on the server, whose implementation is the application's, and a framework model that
  serves it to the client under the verified person.
- A custom screen is checked like an application screen, when it is saved and again whenever it is loaded.
- **Source** on every open document screen: its `ui/1` JSON, editable, previewed as a draft, saved as a custom screen.
- One chat tool that proposes a whole new document for the open screen, shown as a draft.
- Custom screens in the navigator, and offered on the screen they were made from.

## Non-goals

- **Who may see, save, share or delete a custom screen.** That is the application's policy, decided in its store. The
  framework decides only what is identical for every application: that a document grants nothing and is checked.
- A second notation for documents. The source is the `ui/1` JSON the server sends.
- Source for host screens. The explorer, the inspector and other C++ host screens have no document.
- Custom screens following later changes to the screen they came from. A custom screen is a copy.
- New actions, or actions a person cannot already run. A custom screen calls only actions its person is permitted.
- Custom host components or QML. A custom document may use the components the verified bundle carries, and nothing
  else.
- Offline editing. Saving needs the server.

## Anti-goals

This has failed, even if it works in a demo, if:

- a custom document gets past any check an application screen's calls get: authentication, `authorize`,
  `authorizeInstance`, limits or the journal;
- a custom screen can take the place of an application screen, or of another person's custom screen, by its id;
- a document refused at registration can be saved as a custom screen;
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

- **A copy, checked every load.** A custom screen keeps `basedOn`, the screen id it came from, and nothing else of its
  origin. The checks run again whenever it loads, against the current catalog, so a document the server has since
  outgrown is reported, not mounted.
- **Save overwrites; Save as… creates.** Saving a draft of a custom screen replaces that screen. **Save as…** always
  makes a new one.
- **Whole documents, not patches.** The assistant and the source view both produce a complete document, which is what
  the checks take and what the store keeps.
- **A model, not new envelope kinds.** Custom screens travel as the actions of a framework model, as preferences do, so
  the session, `authorize`, effects and the client's transport are the ones every model already has.

## How it fits what exists

| Exists today | Used for |
|---|---|
| `ui::loadDocument` and the accessibility rules (`document.md`, `author.md`) | The checks on save and on load |
| `ui-catalog`'s `permitted` (`service.md`) | Which actions a person's custom document may name |
| `PreferencesModel`, `IPreferencesStore` (`preferences.md`) | The pattern: a store seam, a model registered once, the person from the session |
| Effects and fan-out (`docs/KEEPING-SCREENS-CURRENT.md`) | The navigator and every open list of custom screens refresh after a save or a delete |
| `ChatService`, `screenTools`, `TurnState` (`chat.md`) | The assistant's draft tool and how a draft reaches the client |
| `DocumentScreenHost`, `Workspace`, `Navigator` (`workspace.md`) | Mounting a draft, a tab for a custom screen, the navigator's custom group |

## The store

```cpp
struct CustomScreenKey { std::string person, app, id; };
struct CustomScreen {
    std::string id;          // custom:<uuid>, issued by the model
    std::string title;       // the person's text, shown as it is
    std::string basedOn;     // the screen id it was made from
    std::string group;       // the navigator group; empty is "Custom"
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
  people saved. `put` and `remove` refuse what this person may not do, with a reason for the person. A store with its
  own sharing, roles or approval puts them here; the framework never second-guesses an answer.
- **The store is given checked values only.** The model calls it with a person the server derived and a document that
  has passed every check. A store needs no bound or check of its own.
- **`MemoryCustomScreenStore(base)`** is the framework's store, for tests and examples: a map on an `OwnerStrand`, each
  person seeing and changing only their own screens. Its entries last as long as it does.

## The model

`CustomScreensModel` is registered under `morph.customScreens` by `registerCustomScreens(store, served, dispatcher,
registry, options)`, which a server that offers custom screens calls once before it serves. `served` is the
`ServedBundle`, for the vocabularies, components and catalog a document is checked against.

| Action | Body | `ok` reply |
|---|---|---|
| `list` | `{app}` | `{screens: [{id, title, basedOn, group, savedAt, savedBy, params, broken?}]}` |
| `get` | `{app, id}` | `{screen: CustomScreen, broken?}` |
| `check` | `{app, document}` | `{refusals: [{pointer, reason}]}`; empty when the document would be saved |
| `save` | `{app, id?, title, basedOn, group?, document}` | `{id}`; with no `id`, a new id is issued |
| `remove` | `{app, id}` | `{removed}` |

- **The person** is `session::current()->principal`, as for preferences. A body naming a person has it ignored. With no
  verified principal, `list` answers nothing and the other actions are refused with
  `custom screens are stored for a signed-in person`.
- **Effects.** `save` and `remove` declare an effect on the person's custom screen list and on the screen they name, so
  the navigator and an open copy of that screen refresh, on every device the person has open.
- **Journal.** The actions are `Loggable::No`. A screen is not application data; the store keeps who saved it and when.
- **`broken`** is the first refusal the checks give for a stored document today, with its pointer. A broken screen is
  still listed, so the person can open its source and fix it, and is never mounted.
- **`params`** are the document's declared params, so the client knows where a screen can open (below).

## The checks

`save` and `check` run them in this order, and `list` and `get` run them on every stored document they answer:

1. **Bounds.** The document is at most `CustomScreensOptions::maxDocumentBytes` bytes, 256 KiB by default, and parses
   within `kMaxJsonDepth`. `title` and `group` are 1 to `kMaxNameBytes` bytes. A person holds at most
   `maxScreensPerPerson` saved screens, 200 by default, counted by `list`.
2. **`ui::loadDocument`**, with the bundle's vocabularies and components: exactly the load-time rules of an application
   screen.
3. **The accessibility rules**, as registration runs them.
4. **Permission.** Every action the document names, as `actionUses` finds them, is in the person's `permitted`. A
   document naming an action the person may not run is refused, rather than saved and failing at the first call.
5. **The id.** A document's screen id is replaced by the custom screen's own `custom:<uuid>` id before it is stored, so
   a document claiming an application screen's id, or another custom screen's, stores under its own.

The checks are a person's guide, not the security boundary. The boundary is the server's checks on every call a mounted
document makes, which a custom document cannot reach around, because its calls are ordinary envelopes under the
person's session. A check that passes at save does not make a later call permitted.

Each refusal carries the JSON Pointer and the reason, as `loadDocument`'s do.

## Ids and where a screen opens

- **The namespace.** A custom screen's id is `custom:` and a UUID issued by the model. `UiRegistry` refuses an
  application screen id starting with `custom:`, so the two can never meet. A client resolves a `custom:` id through
  `get` and every other id through the bundle.
- **No params.** A custom screen whose document declares no params is an entry of the navigator, in its `group` or
  "Custom", after the application's groups. A broken one carries a badge with its reason.
- **With params.** A custom screen that takes params, such as one made from a record screen, opens where its base
  opens: the window chrome of a screen offers **Open as** listing the custom screens based on it, and choosing one
  opens it with the same params in the same tab. The navigator does not list it, because it has nothing to open it
  with.
- **Routes and tabs.** A custom screen's route is its id and params, so it is recorded with the workspace's tabs and
  restored with them. A recorded custom screen that `get` no longer answers is not restored, and the person is told.

## Drafts

A draft is an unsaved document mounted in place of a window's screen. Both the source view and the assistant make one,
and it behaves the same whichever made it.

- **The bar.** A draft shows a bar above the screen: "Draft — not saved", and **Save**, for a draft of a custom screen,
  **Save as…**, **Source** and **Discard**. It says that forms on the draft change real data.
- **A real screen.** A draft is mounted by the same interpreter with the same models and params as the screen it
  replaces, so it shows live data and its actions run as on any screen. It is never routed or recorded.
- **Mounting.** The client runs `loadDocument` before it mounts a draft. A draft that does not load leaves the window
  as it was and shows the refusals.
- **Discard** remounts the screen the draft replaced. Closing the tab with a draft open asks first, as a tab with unsaved
  edits does.
- **One draft per window.** A new draft replaces the current one.

## The source view

**Source** is in the window chrome of every open document screen, and in a draft's bar.

- It opens as an overlay holding the document's JSON, pretty-printed: an application screen's document as the bundle
  has it, a custom screen's as `get` answers it, or the open draft's. The overlay is a plain monospace text editor and
  a list of refusals, so every frontend can host it; the terminal frontend's is best effort.
- **Preview** runs `loadDocument` at once in the client, and the server's `check` after it. Refusals are listed with
  their pointers, and choosing one moves the cursor to its line. A document with none becomes the window's draft.
- **Save as…** from the source view previews first and then saves, so it never saves a document the person has not
  seen mounted.
- The client's checks are fast feedback. The server's are the ones that decide.

## The assistant

The chat on a document screen offers one more tool beside the screen's action tools: `propose_screen`.

- **Its context.** When a turn starts, the client sends the open draft's document in the `TurnRequest`, within
  `maxDocumentBytes`. With no draft, the server reads the screen's own document: from the bundle, or from the store for
  a custom screen. The system text gains the `ui/1` vocabulary reference and the person's permitted actions with their
  schemas, so the model can write a document that passes.
- **Its argument** is a complete `ui/1` document. The tool runs the checks of `check`. A refused document comes back to
  the model as a tool error with every refusal and its pointer, so it can correct it.
- **Its limit.** At most `ChatLimits::maxProposalsPerTurn` refused proposals, 3 by default, each also counting against
  `maxToolCallsPerTurn`. Past it, the turn stops and tells the person why, in the chat, and the window stays as it was.
- **A draft reaches the client only checked.** An accepted proposal becomes `TurnState::draft`, the document text, and
  a `draft` event on the `chat` topic. The client mounts it as the window's draft. Events are hints, as for every chat
  event: a client that misses one reads the draft from `TurnState`.
- **Later turns edit the draft.** The next turn sends the draft as the context, so "now make it wider" changes the
  draft and not the original.
- **What the assistant still does not do.** It does not save, fill in forms on the person's behalf, or navigate. Saving
  is the person's press of **Save as…**. The chat design's non-goal "the assistant does not operate the open GUI"
  holds, except that it may propose a draft of the screen's document.

## Errors

| What happens | What the person sees |
|---|---|
| The store refuses a save or a delete | The store's reason, in the draft's bar or the source view; the draft stays open |
| A stored screen fails the checks today | The navigator's badge, and **Open as** marking it broken; opening it opens its source |
| A draft does not load in the client | The refusals in the source view, or a chat message for an assistant draft; the window is unchanged |
| The server's `check` refuses what the client loaded | The server's refusals, in the source view |
| The assistant runs out of proposals | A chat message with the last refusal; the window is unchanged |
| A recorded custom screen is gone | A notice that it was not restored |
| No custom screen store is registered | No **Source** save buttons, no **Open as**, no custom group; **Source** shows the document read-only |

## Testing

- **Store and model.** `save` then `list` and `get` answer the screen; `remove` removes it; with no verified principal,
  nothing is answered and writes are refused; a body naming another person reads only the caller's screens.
- **Every check refuses.** One document per check, from a document that passes, mutated so it fails that check alone:
  too large, one `loadDocument` rule, one accessibility rule, one action outside `permitted`.
- **The id cannot take a place.** A document claiming an application screen's id is stored under its own `custom:` id;
  `UiRegistry` refuses an application screen id in `custom:`. Each test is confirmed to fail with its check removed.
- **Permission at call time.** A saved screen whose action the person later loses is reported broken by `list`; a call
  a mounted draft makes that `authorizeInstance` refuses is refused as on any screen.
- **Refusals reach the person.** A store whose `put` refuses shows its reason in the draft's bar.
- **Effects.** A save on one connection refreshes the navigator on a second connection of the same person.
- **The assistant.** With a scripted provider: an accepted proposal becomes `TurnState::draft`; a refused one comes back
  as a tool error; three refusals end the turn with a message and no draft; a turn with a draft open sends it as the
  context.
- **The proof.** The LIMS sample screen, in a GUI test: the assistant drops a panel, the draft shows it, the source is
  edited by hand and previewed, **Save as…** "My sample", and after a reconnect **Open as** offers it on a sample and
  opens it.

## Specs this changes

- `2026-10-07-ui-document-design.md` §15: free layout editing stays out of scope for application screens; a person's
  own screen is this design's.
- `2026-11-07-server-hosted-chat-design.md` and `docs/spec/ui/chat.md`: the non-goal on operating the GUI admits a
  proposed draft; `TurnRequest`, `TurnState` and the `chat` topic carry a draft.
- `docs/spec/ui/service.md`: `UiRegistry` reserves the `custom:` screen id prefix.
- `docs/spec/ui/workspace.md`: the navigator's custom group, **Open as**, and custom routes.
- A new `docs/spec/ui/custom_screens.md` for the store, the model, the checks and drafts.

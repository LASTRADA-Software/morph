# Guided tours — design

A guided tour is a short sequence of steps an application declares. Each step points at one part of the GUI: it opens
the screen the part is on, dims the rest of the window, rings the part, and shows a card that says what the part is
for. The person starts a tour, moves through it with Next and Back, and can end it at any step. A step can also wait
for the person to do something, such as open a screen from the navigator, so the tour teaches the move rather than
making it for them.

Tours belong to a screen or to the whole application. A screen can have several tours, one per workflow: a samples
list might have "Register a sample", "Find a sample" and "Release results". While a screen is shown, the shell's
**Tours** button lists that screen's tours, followed by the application's general ones.

## Contents

- [Goals](#goals)
- [Decisions](#decisions)
- [Authoring a tour](#authoring-a-tour)
- [Registering tours at runtime](#registering-tours-at-runtime)
- [Starting a tour](#starting-a-tour)
- [Running a tour](#running-a-tour)
- [The spotlight widget](#the-spotlight-widget)
- [The card](#the-card)
- [Checks](#checks)
- [Testing](#testing)
- [Proof in lims](#proof-in-lims)
- [Out of scope](#out-of-scope)

## Goals

- Tours can also be registered and removed while the server runs, and connected clients pick up the change without
  reconnecting.
- The application's server writes tours, as it writes screens. A client shows the tours of whatever application it
  connects to and needs no per-application code.
- Tours are translated like every other text: a tour's texts are expressions in the app scope, so a change of locale
  re-evaluates them while a tour runs.
- A screen can carry several tours, one per workflow, and the person finds them on that screen without the screen
  author adding any buttons.
- A step can point at the shell (search, navigator entry, help button, chat) or at an element of a screen.
- It works on every frontend. Qt Quick (desktop and WASM) dims the window and rings the target. The terminal shows the
  card beside the target and marks the target, without dimming.
- A tour that names something that does not exist is refused when the bundle is built. It is never left to fail in
  front of a person.

## Decisions

Each decision is listed with the obvious alternative that was rejected.

| Decision | Why, and why not the alternative |
|---|---|
| Tours are a `tours` member of the **app document** | The app document already has a bundle slot, fetch rules, a signed-out gate and a reload path. That is the same reason `shell` lives there. A new manifest kind would repeat all four. |
| A step names its target by **`testId`** | Shell and chrome ids are documented and stable already (`shell_search`, `nav_<screenId>`, `chat_input`). Screen authors set `testId` deliberately. A JSON Pointer would break whenever a screen is edited, and an element `id` is scoped, so it is not unique in a window. |
| The **backend** resolves the target and tracks its geometry | Geometry belongs to the toolkit. The generic layer has no geometry API, and adding one would put a frame-by-frame layout question on the seam. Each backend gains a per-context index from `testId` to its live widgets, with change notification ([The spotlight widget](#the-spotlight-widget)). |
| The card is a **client-generated ui/1 document** that reads a `tour.*` host vocabulary | This is the chat panel's pattern. The card is themed, translated, keyboard-reachable and testable like any screen, and its layout needs no new widget code. |
| A tour may name the **screen it belongs to** (`screen` on the tour), and a screen may have any number of tours | Workflows are per screen: one screen supports several tasks, and each task is its own short tour. Long tours that cover everything are hard to follow and are rarely finished. A tour with no `screen` is general and is offered on every screen. |
| The shell shows a **Tours button** whenever the active screen has tours or the app has general ones | The person discovers a screen's workflows where they need them, and a screen author does not have to add a button to each screen. The button is a client part, like the toast. It is not a declared shell part, because declaring a tour already says that it should be offered. |
| A tour can also start from a document, through the **host command `tour.start`** | A screen can offer a tour in context: an empty state ("New here? Take the tour"), a welcome banner, a help screen. A host command, rather than a new ui/1 command, keeps clients and servers independently releasable. An older client reports a missing host command and keeps the screen, and `{"can": "host:tour.start"}` lets the author hide the button there. A new built-in command would make every older client refuse the whole screen. The help button keeps opening one destination, as its spec requires. |
| A waiting step advances when **a screen becomes active** (`advanceOn: {screen}`) | Every frontend observes that event the same way, through the workspace's active tab. "Advance when this element is clicked" would need click observation in every backend, and with clicks the tour cannot tell an accidental press from a deliberate one. |
| A target that is not on screen shows the card **centred, without a ring**, and logs a warning | Checking at build time catches misspelt ids. What remains at run time is an element hidden by state (a collapsed panel, an empty table). The card text still helps, and ending the tour would strand the person mid-way. The warning keeps the miss visible to the developer. |
| Runtime tours live in a framework model, **`morph.tours`**, beside the app document's static ones | Some tours are not known when the bundle is built: a module enabled later, a tour an administrator writes, a release note's "what's new" walk. `morph.customScreens` already shows how a server-side list reaches every client and stays current through a record watch, so tours reuse that shape rather than reloading the app document, which only takes effect at the next sign-in. |
| Runtime registration is a **server C++ API**; the wire offers only `list` | Who may add a tour is application policy. An application that wants an editor or an admin screen exposes its own action, checks its own permission, and calls the registry. The framework adds no write action whose authorisation it would have to guess. |
| One tour runs at a time | Starting a tour ends the one that is running. Two spotlights cannot share one window. |
| The tour belongs to the **frame** | The frame is rebuilt on sign-in and sign-out, and the controller and spotlight are rebuilt with it. That is what ends a tour then. A reconnect is transient and leaves the screens mounted, so a tour survives it. A reloaded app document takes effect at the next frame build, like every other app-document member. |
| No "already seen" state, and no auto-start | The generic client does not wire preferences yet. Auto-start needs per-person state, so it waits for that work rather than adding a second store just for tours. |

## Authoring a tour

```json
"tours": [
  {
    "id": "getting-started",
    "title":   {"t": "tours.start.title"},
    "summary": {"t": "tours.start.summary"},
    "steps": [
      { "target": "shell_search",
        "title": {"t": "tours.start.search.title"}, "body": {"t": "tours.start.search.body"} },
      { "target": "nav_samples", "advanceOn": {"screen": "samples"},
        "title": {"t": "tours.start.nav.title"},    "body": {"t": "tours.start.nav.body"} },
      { "screen": "samples", "target": "samples_table",
        "title": {"t": "tours.start.table.title"},  "body": {"t": "tours.start.table.body"} },
      { "title": {"t": "tours.start.done.title"},   "body": {"t": "tours.start.done.body"} }
    ]
  },
  {
    "id": "register-sample",
    "screen": "samples",
    "title":   {"t": "tours.register.title"},
    "summary": {"t": "tours.register.summary"},
    "steps": [
      { "target": "action_new",
        "title": {"t": "tours.register.new.title"},  "body": {"t": "tours.register.new.body"} },
      { "target": "samples_filter",
        "title": {"t": "tours.register.find.title"}, "body": {"t": "tours.register.find.body"} }
    ]
  },
  {
    "id": "release-results",
    "screen": "samples",
    "title":   {"t": "tours.release.title"},
    "summary": {"t": "tours.release.summary"},
    "steps": [ ... ]
  }
]
```

In this example, `getting-started` is general. `register-sample` and `release-results` both belong to `samples`, so
the samples screen offers both of them as well as `getting-started`.

| Member | Required | Meaning |
|---|---|---|
| `id` | yes | Unique among the app's tours: `[a-z0-9-]+`, at most 64 characters |
| `screen` | no | The screen the tour belongs to. The tour is offered while that screen is active, and starting it opens that screen first. Without `screen`, the tour is general and is offered on every screen |
| `title`, `summary` | `title` | Text expressions in the app scope. The Tours list shows both. The card shows `title` as its eyebrow on every step |
| `steps` | yes | One step or more, and at most 50 |

| Step member | Required | Meaning |
|---|---|---|
| `title`, `body` | `title` | Text expressions in the app scope. `body` may hold several paragraphs |
| `screen` | no | A screen id, with optional `params` (a literal record). The step opens it with `Workspace::open` before it shows, the same as a navigator click. A screen whose tab is already active, compared by `identityOf`, is not reopened |
| `target` | no | A `testId`. With no target, the card is centred and nothing is ringed |
| `advanceOn` | no | `{"screen": id}`: the card shows no Next button, and the step advances as soon as the active tab shows that screen, or a custom screen based on it. Back and End still work |

Texts are written `{"t": key}`, the catalogue's translation form. The `AppRoot` lowers them as it lowers the
navigation badges, into one `AppRoot::tours()` computed value that re-evaluates on a locale change.

The app document is written as JSON, so the author API adds `author::Tour`, `author::TourStep` and
`author::withTours(appText, tours)`, which returns the app document's text with `tours` filled in. A tour is data the
server builds once. It has no lifecycle.

## Registering tours at runtime

`ui::TourRegistry` (`include/morph/ui/tour_registry.hpp`) holds the tours added while the server runs. The app
document's `tours` stay where they are, and the registry adds to them.

```cpp
morph::ui::TourRegistry tours{registry};                 // checks against this UiRegistry's bundle
morph::ui::registerTours(models, tours);                 // serves morph.tours: the read action `list`
auto added = tours.add(tourJson);                        // std::expected<void, TourRefusal>
auto again = tours.replace(tourJson);                    // the same id, new content
auto gone  = tours.remove("whats-new-2026-12");
```

- **Same format, same checks.** A runtime tour is the same JSON as one entry of the app document's `tours`, built the
  same way with `author::Tour`. `add` and `replace` run every check in [Checks](#checks) against the bundle being
  served, and refuse with a `TourRefusal` that names the tour and the step. An id already used by the app document or
  by another runtime tour is refused by `add`. `replace` and `remove` refuse an id the registry does not hold, and
  neither can touch an app-document tour.
- **Translations.** A runtime tour's texts use the same `{"t": key}` form. Its keys resolve through the client's layered
  provider, so a tour whose keys are not in the bundle's catalogues ships them with
  `UiRegistry::setFieldTexts`, as server-sent texts are shipped today. A key found in neither is reported by `add`.
- **Who sees which tour.** `list` answers the runtime tours in registration order, leaving out any tour whose `screen`,
  or any step's `screen`, the asking person may not open (`screenAdmitted`). The tour is not cut short: it is either
  offered whole or not offered at all.
- **Keeping clients current.** Every `add`, `replace` and `remove` touches the record `morph.tours`, and its effects
  reach every client watching it. On the client, `TourSync` (`include/morph/ui/tour_sync.hpp`) lists at each frame
  build. It adds `morph.tours` to the session's extra watches, as `CustomScreenSync` does, and lists again on each
  `changes` event naming the record, after a reconnect and after a `resync`. An in-process source sends no events, so
  its list refreshes at a frame and a reconnect.
- **Merging.** The controller's tours are the app document's, then the runtime ones, each group in its own order. The
  Tours menu shows them that way within each screen group. When a running tour is replaced or removed, it ends and the
  shell shows a toast saying the tour changed. It is not swapped mid-way, because its step positions may no longer
  correspond.
- **Bundle reload.** When the server's bundle changes (a development reload), the registry checks every runtime tour
  again. A tour that no longer passes is removed, logged at error with its refusal, and reported through
  `TourRegistry::onDropped`. It is never kept and left to fail on a client.
- **Lifetime.** The registry keeps its tours in memory. An application that wants them to survive a restart stores
  them itself and adds them again at start-up. A store seam would only matter once tours are written by people,
  which is out of scope.
- **A client without the model.** A server that never calls `registerTours` answers `list` as an unknown model. The
  sync logs that once at debug and offers the app document's tours alone.

## Starting a tour

- **From the Tours button.** The button is `shell_tours`, captioned "Tours" and named "Guided tours". It sits in the
  top bar's trailing tools, after the help button and before the principal, or last when neither is declared. A bundle
  that declares any tour counts as one that declares a top-bar part. The shell therefore mounts the in-page head for
  it, as it does for search, inbox, help or principal. The button is client-made, like the chat column: declaring a
  tour already says that it should be offered, so there is no separate shell part to declare. It is shown whenever
  the active screen has tours or the app has general ones, and hidden otherwise.

  It is a button-style `MenuWidget`, like the principal menu. Its entries are:
  - the active screen's tours, under the screen's title as a `heading`;
  - then the general tours, under a "General" heading;
  - each tour showing its title, with its summary as the entry's `description`;
  - tours in declaration order within each group.

  Menu entries have no `testId`, so tests choose them by path (`["General", "Getting started"]`), as they do the
  principal menu. On a custom screen, the list holds the tours of the screen it is based on. In the compact form, a
  "Tours" submenu with the same entries joins `shell_nav_menu`, after the help entry.
- **From a document.** `{"host": "tour.start", "args": {"obj": {"id": "getting-started"}}}`. The shell registers the
  command for every document. It is safe for untrusted connections, because it changes nothing on the server and
  shows only what the app declared. An id the app does not declare is refused before the command runs, through a new
  `HostCommand::refusal` check, because `run` has no way to answer that it did not succeed. The interpreter reports the
  refusal like any other.
- **From client code.** `ui::MountedShell` gains `virtual std::expected<void, std::string> startTour(std::string_view
  id)`. The error is a plain message, so `frontend.hpp` stays free of the document model. Its default refuses, and the
  generic shell implements it.

## Running a tour

`ui::TourController` (`include/morph/ui/tours.hpp`) reads the tours from `AppRoot::tours()` and runs at most one of
them. The shell builds it with each frame.

| Event | What happens |
|---|---|
| `start(id)` | Ends any running tour. If the tour has a `screen` and the active tab is not showing it, opens that screen. Then shows step 0 |
| Showing step *n* | 1. If the step names a `screen` the active tab is not showing, calls `Workspace::open`. If that answers a `MountError` (the screen is unavailable to this person, say), the tour ends and the shell shows a toast naming the screen. 2. Prepares a shell target (below). 3. Sets the host values and points the spotlight at `target` |
| `next` | Shows step *n*+1. On the last step the card's button is captioned Done and ends the tour |
| `back` | Shows step *n*−1. Back does not undo a screen the tour opened; the earlier step's own `screen` reopens it if it named one |
| Active tab changes | If the step has `advanceOn` and the new screen matches it, or is a custom screen based on it, the tour moves to the next step. The shell maps a custom screen to its base through the custom-screens list it already holds |
| `end`, Escape, Done | Closes the spotlight, clears the host values, and returns focus to where it was when the tour started |
| Frame rebuilt (sign-in, sign-out) or shell torn down | The controller goes with the frame, so the tour ends |

**Preparing a shell target.** A shell id may name something that exists but is not drawn. Before such a step shows,
the controller makes it drawable:

| Target | Wide form | Compact form |
|---|---|---|
| `nav_<id>` | Clears the navigator's search, and opens the module that holds the entry (`Navigator::setOpenModule`) | Rings `shell_nav_menu` instead |
| `shell_search` | — | Rings `shell_search_open` instead |
| Any other `shell_*` | — | Rings `shell_nav_menu` if the part has moved into it |

The controller reaches the workspace, navigator and frame through a small interface: open a screen, the active
screen, the compact flag, and the navigator calls above. Its tests use a fake of that interface, an `AppRoot` over a
test app document, and `RecordingBackend`, with no shell.

## The spotlight widget

`IViewBackend::createSpotlight(ContainerWidget* parent)` returns a `SpotlightWidget`. Like dialogs, popovers and
drawers, it is a container: its children are the card's content. The shell frame creates it on the first tour, as a
child of the main column (a child of the root row would become a third column, which is why the toast lives there too).
Every backend draws it in a window-level layer, so it still covers the sidebar, the top bar and the page. It is
destroyed with the frame. `createSpotlight` has a default that throws, so a third-party backend keeps compiling and a
tour on it fails loudly.

```cpp
class SpotlightWidget : public ContainerWidget {
public:
    virtual void setOpen(bool open) = 0;
    virtual void setTarget(std::optional<std::string> testId) = 0;
    [[nodiscard]] virtual bool targetShown() const = 0;
    virtual void setOnTargetShownChanged(std::function<void(bool)> handler) = 0;
    virtual void setTargetFocusable(bool focusable) = 0;   // a waiting step: the target joins the Tab order
};
```

**Finding the target.** Each backend keeps a per-context index from `testId` to the widgets carrying it. The index is
updated when a widget's test id is set and when the widget is destroyed, and it notifies listeners of each change.
Several widgets can carry one id: a hidden tab showing the same screen, or a repeated row. The spotlight picks the
first, in creation order, that is effectively shown: visible itself, every ancestor visible, and inside the active tab.
It picks again on every index change and on every visibility change of its candidates. Qt Quick's process-wide
`findWidget` list becomes this index, kept per context, so that the inspector window's items are never candidates.

- **Qt Quick.**
  - **Drawing.** Four scrim rectangles around the target's scene rectangle dim everything else. A ring in the theme's
    focus colour, padded by one spacing unit, surrounds the target. The card sits on the side of the target with the
    most free space and is clamped inside the window.
  - **Geometry.** The spotlight follows the target as it resizes, as layout passes move it, and as it scrolls. When the
    target is clipped, the nearest `ScrollWidget` is scrolled until the target is in view.
  - **Pointer.** Presses on the scrim are swallowed. Presses inside the cut-out reach the target.
  - **Missing target.** When no candidate is shown, `targetShown()` is false and the card is centred with no cut-out.
- **Terminal.**
  - **Finding the target.** A walk from the root over the widgets' stored test ids fills the same index.
  - **Placement.** The card is a bordered box placed below the target's drawn rectangle, or above it when there is no
    room. This needs placement code beside another widget; today's dialogs are only centred.
  - **Drawing.** The target is drawn in reverse video. Nothing is dimmed.
- **Recording backend.** It keeps a real index over the test ids its widgets record, so `targetShown()` follows mounts,
  unmounts and `setVisible`. It records `open`, `target` and the children. A test therefore observes the spotlight
  finding a late target rather than setting the result by hand.
- **Echoing test backend** (`tests/ui_echoing_backend.hpp`). It implements the widget as a plain container.
- **Conformance suite.** The probe gains a lookup by test id. The new cases are: the widget opens and closes; a target
  mounted after `setTarget` is found; hiding the target or unmounting it flips `targetShown` and calls the handler; of
  two widgets with one id, the shown one is picked. Each case is checked to fail when the spotlight ignores its
  target.

## The card

The card is `kTourCardDocument`, with id `morph.tour`. The server may never serve that id. Today the chat panel's id is
reserved by hand in two places, `servedScreenRefusal` and `UiRegistry::checkNewEntry`. Both move to one set of
reserved client ids holding both ids, and `host_vocabulary.md` is updated to match. The card is mounted into `SpotlightWidget::card()` with `DocumentScreenHost::mountInto`. Its
host vocabulary is scoped to `morph.tour`:

| Values | Commands |
|---|---|
| `tour.title`, `tour.stepTitle`, `tour.body`, `tour.waitingFor` (string: the awaited screen's title), `tour.position`, `tour.count` (int), `tour.waiting`, `tour.canBack`, `tour.last` (bool) | `tour.next`, `tour.back`, `tour.end` |

| testId | What it is |
|---|---|
| `tour_card` | The card |
| `tour_eyebrow` | The tour's title |
| `tour_title`, `tour_body` | The step's texts |
| `tour_progress` | "2 of 5" |
| `tour_back` | Back. Hidden on the first step |
| `tour_next` | Next, or Done on the last step. On a waiting step it is replaced by a muted hint, such as "Open Samples to continue" |
| `tour_end` | End tour |

Keys: Escape runs `tour.end`, Right runs `tour.next` (except on a waiting step), and Left runs `tour.back`. The card's
document declares them as its `keys`, and the backend delivers these three before the focused button sees them.
Enter and Space stay with the focused button, so Enter on Back or End does what the button says. When the card opens,
it focuses its first shown button: Next on an ordinary step, End tour on a waiting one.

Focus is trapped in the card on ordinary steps: Tab cycles through its buttons and never reaches the page under the
scrim. On a waiting step, the focus chain is the card's buttons followed by the target. The person can then finish
the step from the keyboard (Tab to the ringed navigator entry, then Enter) on every frontend. Accessibility: the card is
a dialog named after the step title. ui/1 has no accessible description on a column, so the body follows the title
as text. The captions come from the client's own
catalogue, as the chat panel's captions do.

## Checks

Tours are checked in two places. Adding the app document (`addFile`) refuses malformed tours: an empty, malformed or
repeated id, a step count out of range, and `params` that are not a literal record. `UiRegistry::build` then parses
each registered screen document (it keeps their text today) and refuses the bundle for the cross-references. Every
error names the tour and the step. The full list:

- A tour id that is empty, malformed or repeated.
- A tour with no steps, or more than 50.
- A tour's `screen`, or a step's `screen` or `advanceOn.screen`, that is not a registered screen.
- `params` that are not a literal record.
- A `target` that names nothing the window can show.

  The step's *effective screen* is the screen the active tab shows when the step appears: its own `screen`, else the
  most recent earlier `screen` or `advanceOn.screen`, else the tour's `screen`.

  A target is accepted when it is one of these:
  - a shell id the declared parts produce: `shell_search`, `shell_inbox`, `shell_help`, `shell_principal_menu`,
    `shell_brand_title` and `shell_context` when declared; `shell_tours`, always (a tour is declared); `nav_<id>` for
    a registered screen that has a document and is not public, since only those appear in the navigator;
    `chat_input`, always: whether chat is offered belongs to the chat service, which the registry cannot see, so a
    server without chat shows that step's card centred, with the warning;
  - a literal `testId` anywhere in the effective screen's document, including inside its `components`;
  - `action_<id>` for an action in the effective screen's head `actions`, which is the id the interpreter generates for
    it.

  A step whose effective screen is a host screen (registered with `addEntry`, so it has no document) may target only
  shell ids. Client chrome ids, such as the custom-screens draft bar, are not accepted: the server cannot see them.
- A `tour.start` host command whose `args` is a literal record naming a tour the app does not declare, in any
  registered document. An `args` that is computed is not checked; it is reported at run time instead.

The client's interpreter checks the shape of `tours` when it loads the app document (members, types and the id
pattern), and refuses a screen that declares `tours`. Unknown step members are ignored, and members starting with `!`
are must-understand, as everywhere else in ui/1.

## Testing

- **Tours list, headless:** a screen with two tours lists both, in order, before the general ones; a screen with none
  lists only the general ones; with no tours anywhere the button is hidden; switching tabs refreshes the list; starting
  a screen's tour from another screen opens that screen first; the compact form shows the same entries under
  `shell_nav_menu`.
- **Controller, headless:** start, next, back, Done; a step that opens its screen; `advanceOn` on a navigator open;
  `advanceOn` ignoring a non-matching screen; `advanceOn` matching a custom screen based on its screen; a missing
  target (card centred, one warning per step); a `nav_<id>` target in a closed module (the module opens); the compact
  substitutions; a screen that fails to open (the tour ends with a toast); Escape; sign-out ending the tour and a
  reconnect not ending it; focus returning when the tour ends; a new tour replacing a running one; a locale change re-evaluating the texts while a
  tour runs.
- **Load and build:** each refusal in [Checks](#checks) has its own case, including a target inside a component, an
  `action_<id>` target, a host-screen step and a shell id whose part is not declared. A control case shows that a
  valid tour builds. A mutation check confirms that the target check refuses a misspelt `testId`.
- **Backend conformance:** the spotlight cases above, on every backend.
- **Qt Quick, offscreen,** based on `tests/qt_quick/test_qt_quick_chat.cpp`:
  - the cut-out equals the target's scene rectangle plus its padding;
  - the card does not overlap the target and stays inside the window at 1280×800 and in the compact width;
  - a click inside the cut-out reaches the target, and a click on the scrim reaches nothing;
  - a target inside a scrolled-away `ScrollWidget` is scrolled into view.
- **Terminal:** a render test of the card box and of the target in reverse video, and Tab reaching the target on a
  waiting step.
- **Runtime registration, server:** `add` with each refusal (unknown screen, unknown target, duplicate id against the
  app document and against another runtime tour, missing text key); `replace` and `remove` of an unknown id; `list`
  in order and filtered by `screenAdmitted`; each change touching `morph.tours`; a bundle reload dropping a tour that
  no longer passes and calling `onDropped`.
- **Runtime registration, client:** a tour added on the server appears in the Tours menu after the `changes` event;
  a removed one disappears; a running tour that is replaced ends with a toast; the list is fetched again after a
  reconnect; a server without the model offers the app document's tours alone.
- **Host command:** `tour.start` from a document starts the tour; an unknown id is reported; `{"can":
  "host:tour.start"}` is true on this client.

## Proof in lims

The framework ships first as its own pull request. The lims proof follows in a separate change.

- **A general tour,** "Getting started", covering the navigator, search, the inbox and the assistant. It has a
  waiting step that asks the person to open Samples.
- **Several tours on the samples screen,** one per workflow: "Register a sample", "Find a sample" and "Release
  results". One billing screen also gets its own workflow tour, "Issue an invoice".
- All tours are in English and German.
- **Runtime:** the lims server registers a "What's new" tour at start-up through `TourRegistry`, and a GUI test adds
  and removes a tour while a client is connected.
- **Starting from a screen:** the samples list's empty state runs `tour.start` as well.
- **A lims GUI test** opens the samples screen, checks that the Tours list shows its three workflows before
  "Getting started", and runs "Register a sample" end to end in the headless shell. A render-smoke test captures the
  spotlight over the samples table.

## Out of scope

- **"Seen" state and first-run auto-start.** These need app-scope preferences wired into the generic client, which is
  a separate piece of work.
- **An assistant `start_tour` tool.** Today a chat tool cannot command the client: its only client effect is a draft.
  Giving tools a client-command channel is its own design.
- **Targets in dynamic rows** (such as "the first row of this table"), and **arbitrary click conditions**.
- **Tours authored by people in the client,** as custom screens are.

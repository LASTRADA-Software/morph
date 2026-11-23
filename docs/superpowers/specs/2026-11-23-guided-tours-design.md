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
- [Starting a tour](#starting-a-tour)
- [Running a tour](#running-a-tour)
- [The spotlight widget](#the-spotlight-widget)
- [The card](#the-card)
- [Checks](#checks)
- [Testing](#testing)
- [Proof in lims](#proof-in-lims)
- [Out of scope](#out-of-scope)

## Goals

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
| The **backend** resolves the target and tracks its geometry | Geometry belongs to the toolkit. The generic layer has no geometry API, and adding one would put a frame-by-frame layout question on the seam. The backend already holds a registry from `testId` to item (Qt Quick's `findWidget`). |
| The card is a **client-generated ui/1 document** that reads a `tour.*` host vocabulary | This is the chat panel's pattern. The card is themed, translated, keyboard-reachable and testable like any screen, and its layout needs no new widget code. |
| A tour may name the **screen it belongs to** (`screen` on the tour), and a screen may have any number of tours | Workflows are per screen: one screen supports several tasks, and each task is its own short tour. Long tours that cover everything are hard to follow and are rarely finished. A tour with no `screen` is general and is offered on every screen. |
| The shell shows a **Tours button** whenever the active screen has tours or the app has general ones | The person discovers a screen's workflows where they need them, and a screen author does not have to add a button to each screen. The button is a client part, like the toast. It is not a declared shell part, because declaring a tour already says that it should be offered. |
| A tour can also start from a ui/1 command, **`startTour`** | A screen can offer a tour in context: an empty state ("New here? Take the tour"), a welcome banner, a help screen. The help button keeps opening one destination, as its spec requires. |
| A waiting step advances when **a screen becomes active** (`advanceOn: {screen}`) | Every frontend observes that event the same way, through the workspace's active tab. "Advance when this element is clicked" would need click observation in every backend, and with clicks the tour cannot tell an accidental press from a deliberate one. |
| A target that is not on screen shows the card **centred, without a ring**, and logs a warning | Checking at build time catches misspelt ids. What remains at run time is an element hidden by state (a collapsed panel, an empty table). The card text still helps, and ending the tour would strand the person mid-way. The warning keeps the miss visible to the developer. |
| One tour runs at a time | Starting a tour ends the one that is running. Two spotlights cannot share one window. |
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

The JSON above writes a text as `{"t": key}` for brevity. In the document, a text is written exactly as a screen's
`title` is written.

| Member | Required | Meaning |
|---|---|---|
| `id` | yes | Unique among the app's tours: `[a-z0-9-]+`, at most 64 characters |
| `screen` | no | The screen the tour belongs to. The tour is offered while that screen is active, and starting it opens that screen first. Without `screen`, the tour is general and is offered on every screen |
| `title`, `summary` | `title` | Text expressions in the app scope. The Tours list shows both. The card shows `title` as its eyebrow on every step |
| `steps` | yes | One step or more, and at most 50 |

| Step member | Required | Meaning |
|---|---|---|
| `title`, `body` | `title` | Text expressions in the app scope. `body` may hold several paragraphs |
| `screen` | no | A screen id, with optional `params` (a literal record). The step opens it with `Workspace::open` before it shows, the same as a navigator click |
| `target` | no | A `testId`. With no target, the card is centred and nothing is ringed |
| `advanceOn` | no | `{"screen": id}`: the card shows no Next button, and the step advances as soon as the active tab shows that screen. Back and End still work |

The author API adds `author::Tour` and `author::TourStep`, plus `AppDocument::tour(...)`, beside `ShellParts`. A tour
is data the server builds once. It has no lifecycle.

## Starting a tour

- **From the Tours button:** the shell's top bar has a button, `shell_tours`, captioned "Tours", just before the help
  button. It is shown whenever the active screen has tours or the app has general ones, and hidden otherwise. It opens
  a popover (`PopoverWidget`, as search does) with these entries:
  - the active screen's tours first, under the screen's title as a heading;
  - then the general tours, under "General";
  - each tour as an entry `tour_<id>`, showing its title with its summary as the second line;
  - tours in declaration order within each group.

  Choosing an entry closes the popover and starts the tour. On a custom screen, the button lists the tours of the
  screen it is based on. Their targets may be missing there, and a missing target falls back as
  [Running a tour](#running-a-tour) describes. In the compact form, the button moves into the overflow menu as a
  "Tours" submenu.
- **From a document:** `{"startTour": "getting-started"}` is a command with a literal tour id. The shell handles it
  itself, as it handles `setContext`. No owner round trip is needed, because starting a tour changes nothing on the
  server.
- **From client code:** `Shell::startTour(id)` returns `std::expected<void, Error>`. It refuses an id the app does not
  declare.

## Running a tour

`ui::TourController` (`include/morph/ui/tours.hpp`) holds the tour definitions parsed from the app document. It runs
at most one tour.

| Event | What happens |
|---|---|
| `start(id)` | Ends any running tour. If the tour has a `screen` and the active tab is not showing it, opens that screen. Then shows step 0 |
| Showing step *n* | If the step names a `screen` and the active tab is not showing it with those params, it calls `Workspace::open`. It sets the host values and points the spotlight at `target` |
| `next` | Shows step *n*+1. On the last step the card's button is captioned Done and ends the tour |
| `back` | Shows step *n*−1. Back does not undo a screen the tour opened; the earlier step's own `screen` reopens it if it named one |
| Active tab changes | If the step has `advanceOn` and the new screen matches, the tour moves to the next step |
| `end`, Escape, Done | Closes the spotlight and clears the host values |
| App document reloaded, disconnect, sign-out | Ends the tour, because its definitions or its screens may no longer hold |

The controller sees the workspace only through what it needs: the `open` call and an active-screen signal. Its tests
therefore run on `RecordingBackend` and `MemoryHistory` without a shell.

## The spotlight widget

`Backend::createSpotlight(ContainerWidget* parent)` returns a `SpotlightWidget`. The shell frame creates it once, over
the whole window, in the same way it creates its toast.

```cpp
class SpotlightWidget : public Widget {
public:
    virtual void setOpen(bool open) = 0;
    virtual void setTarget(std::optional<std::string> testId) = 0;
    [[nodiscard]] virtual bool targetShown() const = 0;
    virtual void setOnTargetShownChanged(std::function<void(bool)> handler) = 0;
    virtual ContainerWidget& card() = 0;
};
```

- **Qt Quick.** A scrim made of four rectangles around the target's scene rectangle dims everything else. A ring in
  the theme's focus colour, padded by one spacing unit, surrounds the target. The card sits on the side of the target
  with the most free space and is clamped inside the window.
  - The widget follows the target's geometry as it changes: resizes, layout passes and scrolling. When the target is
    clipped, the widget scrolls the nearest `ScrollWidget` until the target is in view.
  - Presses on the scrim are swallowed. Presses inside the cut-out reach the target, so a waiting step can be finished
    by clicking the ringed navigator entry.
  - The target is found through the existing `testId` registry, and found again whenever that registry changes. When
    no item has the `testId`, or its item is invisible, `targetShown()` is false and the card is centred with no
    cut-out.
- **Terminal.** The card is a bordered box beside the target's rectangle; the box sits below the target or, if there is
  no room, above it. The target is drawn in reverse video. Nothing is dimmed. Tab cycles through the card's buttons
  while the tour runs.
- **Recording backend.** It records `open`, `target`, `targetShown` (which a test sets) and the card's children.
- **Conformance suite.** New cases cover these behaviours: the widget opens and closes; a target that registers late
  is picked up; a target that unmounts flips `targetShown` and calls the handler.

## The card

The card is `kTourCardDocument`, with id `morph.tour`. The server may never serve that id; it is reserved the same way
the chat panel's id is. The card is mounted into `SpotlightWidget::card()` with `DocumentScreenHost::mountInto`. Its
host vocabulary is scoped to `morph.tour`:

| Values | Commands |
|---|---|
| `tour.title`, `tour.stepTitle`, `tour.body` (string), `tour.position`, `tour.count` (int), `tour.waiting`, `tour.canBack` (bool) | `tour.next`, `tour.back`, `tour.end` |

| testId | What it is |
|---|---|
| `tour_card` | The card |
| `tour_eyebrow` | The tour's title |
| `tour_title`, `tour_body` | The step's texts |
| `tour_progress` | "2 of 5" |
| `tour_back` | Back. Hidden on the first step |
| `tour_next` | Next, or Done on the last step. On a waiting step it is replaced by a muted hint, such as "Open Samples to continue" |
| `tour_end` | End tour |

Keys: Escape runs `tour.end`. Enter and Right run `tour.next`, except on a waiting step. Left runs `tour.back`. The card
takes focus when it opens and returns focus to the element that had it when the tour ends. Accessibility: the card is
a dialog named after the step title, and its body is the description. The captions come from the client's own
catalogue, as the chat panel's captions do.

## Checks

When the server's `UiRegistry::build` runs, it refuses the bundle, with an error naming the tour and step, for any of
these:

- A tour id that is empty, malformed or repeated.
- A tour with no steps, or more than 50.
- A tour's `screen`, or a step's `screen` or `advanceOn.screen`, that is not a registered screen.
- `params` that are not a literal record.
- A `target` that names nothing the window can show. The step's *effective screen* is its own `screen`, or failing
  that the last `screen` named by an earlier step, or failing that the `advanceOn.screen` of the step just before it,
  or failing that the tour's `screen`.
  A target is accepted when it is one of these:
  - a documented shell id (the frame's fixed ids, or `nav_<id>` for a registered screen);
  - a `testId` set in the effective screen's document;
  - a chrome id that the effective screen shows.
- Any `startTour` command, in any registered document, that names a tour the app does not declare.

The client's interpreter checks the shape of `tours` when it loads the app document (members, types and the id
pattern), and refuses a screen that declares `tours`. Unknown step members are ignored, and members starting with `!`
are must-understand, as everywhere else in ui/1.

## Testing

- **Tours list, headless:** a screen with two tours lists both, in order, before the general ones; a screen with none
  lists only the general ones; with no tours anywhere the button is hidden; switching tabs refreshes the list; starting
  a screen's tour from another screen opens that screen first.
- **Controller, headless:** start, next, back, Done; a step that opens its screen; `advanceOn` on a navigator open;
  `advanceOn` ignoring a non-matching screen; a missing target (card centred, one warning per step); Escape; reload
  and disconnect ending the tour; a new tour replacing a running one; a locale change re-evaluating the texts while a
  tour runs.
- **Load and build:** each refusal in [Checks](#checks) has its own case. A control case shows that a valid tour
  builds. A mutation check confirms that the target check refuses a misspelt `testId`.
- **Backend conformance:** the spotlight cases above, on every backend.
- **Qt Quick, offscreen,** based on `tests/qt_quick/test_qt_quick_chat.cpp`:
  - the cut-out equals the target's scene rectangle plus its padding;
  - the card does not overlap the target and stays inside the window at 1280×800 and in the compact width;
  - a click inside the cut-out reaches the target, and a click on the scrim reaches nothing;
  - a target inside a scrolled-away `ScrollWidget` is scrolled into view.
- **Terminal:** a render test of the card box and of the target in reverse video.

## Proof in lims

The framework ships first as its own pull request. The lims proof follows in a separate change.

- **A general tour,** "Getting started", covering the navigator, search, the inbox and the assistant. It has a
  waiting step that asks the person to open Samples.
- **Several tours on the samples screen,** one per workflow: "Register a sample", "Find a sample" and "Release
  results". One billing screen also gets its own workflow tour, "Issue an invoice".
- All tours are in English and German.
- **Starting from a screen:** the samples list's empty state runs `startTour` as well.
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

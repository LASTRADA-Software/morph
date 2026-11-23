# Guided tours

A guided tour is a short sequence of steps an application declares in its app document. Each step points at one part
of the GUI: it opens the screen the part is on, dims the rest of the window, rings the part, and shows a card that says
what the part is for. Tours belong to a screen or to the whole application, and a screen may have several, one per
workflow. Specified across `include/morph/ui/tours.hpp` (the controller), `tour_card.hpp` (the card),
`shell_frame.hpp` (the Tours button) and the generic client.

## Contents

- [The declaration](#the-declaration)
- [Registering tours at runtime](#registering-tours-at-runtime)
- [Runtime tours on the client](#runtime-tours-on-the-client)
- [The tours of a session](#the-tours-of-a-session)
- [The controller](#the-controller)
- [The card](#the-card)
- [The generic client](#the-generic-client)
- [Design decisions](#design-decisions)
- [Not yet](#not-yet)

## The declaration

The app document's `tours` member ([document.md](document.md#the-app-documents-tours)): tours of `{id, screen?, title,
summary?, steps}`, steps of `{title, body?, screen?, params?, target?, advanceOn?}`. A server writes it by hand or with
`author::withTours` ([author.md](author.md)). The bundle refuses a tour that names a screen, a target or a tour it
does not have ([service.md](service.md#the-registry)).

## Registering tours at runtime

`TourRegistry` (`tour_registry.hpp`) holds the tours a server adds while it runs, beside the app document's. It reads
the bundle served now through a getter, as `CustomScreens` does, so a development reload is seen.

- **Same form, same checks.** `add(json)` and `replace(json)` take one tour in the form of an entry of the app
  document's `tours` (`author::tourJson` writes it). The tour is checked as if appended to the served app document's
  `tours`: the loader checks its shape and its texts in the app scope and refuses an id the app document uses, and
  `checkTour`, the check the bundle's build runs, checks its screens and targets. A `{"t": key}` whose key is in no
  message catalogue of the bundle and not among the server's texts (`UiRegistry::setFieldTexts`) is refused. A refusal
  is a `TourRefusal{tourId, path, reason}`: its `tourId` is the runtime tour's id, and its path is a JSON Pointer into the
  submitted tour that names the step (`/steps/2/target`), never into the app document. An id the app document already
  declares is refused at `/id` in those words. `add` refuses an id another runtime tour
  holds; `replace` and `remove` refuse one the registry does not hold, so neither touches an app-document tour. A
  bundle with no app document is refused: a runtime tour's texts are read in the app scope.
- **`list`.** `registerTours(tours, authorizer, dispatcher, registry)` serves the model `morph.tours` with one read
  action, `morph.tours.list`, which answers the runtime tours in registration order as `ListedTour{id, revision,
  tour}`, leaving out a tour whose `screen` or any step's `screen` the caller may not open (`screenAdmitted`): a tour
  is offered whole or not at all. `revision` grows with each `add` and `replace`. There is no write action: who may
  add a tour is the application's policy, and an application that offers an editor exposes its own action and calls
  the registry.
- **Changes.** Every change, a drop included, is told to `setOnChanged`'s handler outside the registry's lock;
  `publishTourChanges(tours, server)` makes it a whole-type change of the record `morph.tours` (`toursChanged()`)
  fanned out from the server's strand to every connection watching it. A server that does not call it sends no event,
  and its clients see runtime tours change at their next frame or reconnect; the first change made while no handler is
  set is logged as a warning, once, so that omission is heard.
- **Bundle reload.** Every call compares the served bundle's digest with the one last checked; when it differs, every
  runtime tour is checked again first, and one that fails is dropped, logged at error with its refusal and told to
  `setOnDropped`'s handler. `recheck()` does it at once, for an application that swaps its bundle.
- **Locking.** The bundle getter, the server's texts and the authoriser given to `list` are called with the registry's
  lock held, so none of them calls back into the registry or holds a lock around a call into it. `recheck()` with a
  getter that answers no bundle throws `std::logic_error`, as every other call does.
- **Lifetime.** In memory only. An application that wants its runtime tours to survive a restart stores them and adds
  them again at start-up.

## Runtime tours on the client

`TourSync` (`tour_sync.hpp`) is the client of `morph.tours`, shaped like `CustomScreenSync`. `list` asks
`morph.tours.list`; an answer makes the tours offered, loads each listed tour with `loadTourText` (one that does not
load is logged at warn and left out) with its `revision`, tells `setOnListed`, and adds `morph.tours` to the session's
extra watches. While offered, a `changes` event naming the record, a `resync` and a reconnect list again; `relist(done)`
calls back once its own reply was handled, and one private channel carries every call, so replies arrive in order. A
server that never called `registerTours` answers `unknown model type: morph.tours`: the sync offers no runtime tours and
logs that once at debug; any other failure is logged at warn and keeps the last list. `reset` forgets the list, stops
the watch and drops replies in flight.

## The tours of a session

`AppRoot::tours()` evaluates every text in the app scope, inside one computed value, so a change of locale re-evaluates
them ([interpreter.md](interpreter.md#the-app-root-scope)). `offeredTours(tours, screenId)` splits them into the tours
of a screen (those whose `screen` is it, in declaration order) and the general ones (no `screen`, in declaration
order): what the Tours menu lists, the screen's first.

Runtime tours (a server registers them while it runs) reach the client as a list of texts. Each is loaded with
`loadTourText`, which reads one tour as an entry of `tours` is read and checks its shape, and the list is given to
`AppRoot::setRuntimeTours`. `tours()` then lists the app document's tours and after them the runtime ones, each group in
its own order; a runtime tour with an app tour's id is left out, so a literal `tour.start` names the app document's tour.
Each `Tour` carries the server's `revision`, 0 for an app tour.

## The controller

`TourController` runs at most one tour. A shell builds one with each frame and destroys it with the frame, so a
sign-in or a sign-out, which rebuild the frame, end a running tour; a reconnect leaves the frame and the tour. It
reaches the shell through `TourShell`: open a screen (`openScreen`, as a navigator click does, focusing a tab that
already shows it), whether the active tab shows a screen with given params (`shows`, by identity; a custom screen counts as the screen it is based on, so a step naming that screen leaves the person on their own custom screen), the active screen
(`activeScreen`, a custom screen named by the screen it is based on), a screen's title, whether the frame is compact,
`revealNavEntry`, the spotlight, a toast, and the shell's own texts (`text`): the two toasts below are the built-in `morph.shell.tourChanged` and `morph.shell.tourScreenFailed` messages, English and German, which an application's translations may override like every `morph.shell.*` key; `{title}` in them is the tour's or screen's title.

| Event | What happens |
|---|---|
| `start(id)` | An id the app does not declare is refused and starts nothing. Otherwise a running tour ends; the tour's `screen`, when the active screen is not it, is opened; step 0 shows; the spotlight opens once the card shows that step (`TourShell::afterCardUpdated`: at once by default, after the card's next update in the generic client), so focus moves to the card's first shown button of that step; a tour that ended or was replaced in between opens nothing |
| Showing step *n* | The step's `screen`, when the active tab does not show it with those params, is opened; a refusal ends the tour and the shell shows a toast naming the screen. A shell target is prepared (below). The spotlight's target is set, and `setTargetFocusable` is true on a waiting step. A step whose `advanceOn` screen is already active is shown as an ordinary step, not waiting: no change of the active screen would move it on, so Next does. A target not shown leaves the card centred and is reported once per step and tour run through `TourControllerOptions::warn` |
| `next()` | Shows step *n*+1, or ends the tour on the last step. A waiting step ignores it |
| `back()` | Shows step *n*-1. A screen an earlier step opened is not closed |
| `activeChanged()` | When the step waits for a screen (`advanceOn`) and the active screen is it, the tour moves on. A screen the controller is opening itself does not count |
| The running tour changes | The controller follows `AppRoot::tours()` while a tour runs. A tour that is gone, whose `revision` differs from the one it started with (replaced on the server), or that no longer has the step on show ends, and the shell shows the toast "The tour <title> changed, so it ended" (in the person's language): its steps may no longer match the one shown. A change of locale, a change of another tour, and a runtime update that leaves an app-document tour alone keep it running; the texts are re-evaluated |
| `end()` | The spotlight's target is cleared and it closes, which hands focus back to where it was when it opened; the view empties |

`view()` is what the card shows: the tour's title, the step's title and body, its position and the count, whether it
waits and for which screen's title, whether Back is possible, and whether it is the last step. `onChanged` is called
after each change of it, a change of locale included, which the controller hears by following `AppRoot::tours()` while a
tour runs.

**Preparing a shell target.** In the wide form a `nav_<id>` target is revealed first: the navigator's search is emptied
and the module holding the entry opened (`Navigator::reveal`). In the compact form `tourTargetFor` rings what the frame
draws instead: `nav_<id>`, `shell_context`, `shell_help`, `shell_principal_menu` and `shell_tours` as `shell_nav_menu`,
which holds them; `shell_search` as `shell_search_open`. When the frame goes between its forms while a step shows, the shell
calls `formChanged()`, which rings the step's target again in the new form without opening or moving anything.

## The card

`kTourCardDocument` (`tour_card.hpp`), id `morph.tour` (`kTourCardId`, reserved like the chat panel's: a server may
never serve it). The shell mounts it into the frame's spotlight with `DocumentScreenHost::mountInto`. A
`TourCardBinding` connects it to the frame's controller through the shell's `HostVocabulary`:

| Values (the card only) | Commands |
|---|---|
| `tour.title`, `tour.stepTitle`, `tour.body`, `tour.waitingFor` (string), `tour.position`, `tour.count` (int), `tour.waiting`, `tour.canBack`, `tour.last` (bool) | `tour.next`, `tour.back`, `tour.end` (the card only); `tour.start` with `{id}` (every document) |

| testId | What it is |
|---|---|
| `tour_card` | the card: a column named after the step's title, role `dialog` |
| `tour_eyebrow` | the tour's title |
| `tour_title`, `tour_body` | the step's texts; the body hides while empty |
| `tour_progress` | "2 of 5" (`morph.tour.progress`) |
| `tour_back` | Back; hidden on the first step |
| `tour_next` | Next, or Done on the last step; hidden on a waiting step |
| `tour_waiting` | on a waiting step, "Open Samples to continue" (`morph.tour.waiting`) |
| `tour_end` | End tour |

The captions are `t` messages (`morph.tour.*`) with English fallbacks, so a bundle's catalogue translates them. The
document's `keys` are Esc (`tour.end`), Enter and Right (`tour.next`) and Left (`tour.back`); a waiting step ignores
`tour.next`. The spotlight delivers the card's chords before a focused control's own keys, Enter and Space excepted, so
Enter on a focused button activates that button. Opening the spotlight focuses the card's first shown button.

`tour.start` is registered for every document and is safe for an untrusted connection: it changes nothing on the
server and shows only what the application declared. An id the app does not declare is refused through the command's
`refusal` check, so it answers as a command that did not succeed and the interpreter reports it; the bundle check
already refuses a literal one. `{"can": "host:tour.start"}` is true on a client that runs tours, so a screen can hide
its button on one that does not. The commands' work is posted: a step can open a screen, which must not be replaced
inside one of its widgets' handlers.

## The generic client

The generic client builds a controller and the Tours menu with each frame and mounts the card into the frame's
spotlight; see [client.md](../client/client.md#tours). `MountedShell::startTour(id)` starts a tour from client code;
its default refuses. Its `TourSync` lists the runtime tours at each frame and keeps them current
([client.md](../client/client.md#tours)).

## Design decisions

| Decision | Why, and why not the alternative |
|---|---|
| Tours are a member of the app document | It already has a bundle slot, fetch rules, a signed-out gate and a reload path, as `shell` does; a manifest kind of its own would repeat all four. |
| A step names its target by `testId` | Shell ids are stable and documented, and screen authors set `testId` deliberately. A JSON Pointer breaks with every edit of a screen, and an element `id` is scoped, so not unique in a window. |
| The backend resolves and tracks the target | Geometry is the toolkit's. The generic layer has no geometry API, and one would put a frame-by-frame layout question on the seam. |
| A waiting step advances when a screen becomes active | Every frontend observes the active tab the same way. Watching clicks would need click observation in every backend, and could not tell an accidental press from a deliberate one. |
| A target not shown centres the card and warns | The bundle check catches misspelt ids; what remains is a part hidden by state. The card still helps, and ending the tour would strand the person. |
| One tour at a time, owned by the frame | Two spotlights cannot share a window. The frame is rebuilt on sign-in and sign-out, which is what ends a tour then; a reconnect keeps the screens, and the tour. |

## Not yet

- "Seen" state and first-run auto-start: they need app-scope preferences in the generic client.
- A chat tool that starts a tour: a chat tool has no channel to command the client.
- Targets in dynamic rows, and arbitrary click conditions.
- Tours authored by people in the client.
```

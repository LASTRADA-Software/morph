# Guided tours

A guided tour is a short sequence of steps an application declares in its app document. Each step points at one part
of the GUI: it opens the screen the part is on, dims the rest of the window, rings the part, and shows a card that says
what the part is for. Tours belong to a screen or to the whole application, and a screen may have several, one per
workflow. Specified across `include/morph/ui/tours.hpp` (the controller), `tour_card.hpp` (the card),
`shell_frame.hpp` (the Tours button) and the generic client.

## Contents

- [The declaration](#the-declaration)
- [The tours of a session](#the-tours-of-a-session)
- [The controller](#the-controller)
- [Design decisions](#design-decisions)
- [Not yet](#not-yet)

## The declaration

The app document's `tours` member ([document.md](document.md#the-app-documents-tours)): tours of `{id, screen?, title,
summary?, steps}`, steps of `{title, body?, screen?, params?, target?, advanceOn?}`. A server writes it by hand or with
`author::withTours` ([author.md](author.md)). The bundle refuses a tour that names a screen, a target or a tour it
does not have ([service.md](service.md#the-registry)).

## The tours of a session

`AppRoot::tours()` evaluates every text in the app scope, inside one computed value, so a change of locale re-evaluates
them ([interpreter.md](interpreter.md#the-app-root-scope)). `offeredTours(tours, screenId)` splits them into the tours
of a screen (those whose `screen` is it, in declaration order) and the general ones (no `screen`, in declaration
order): what the Tours menu lists, the screen's first.

## The controller

`TourController` runs at most one tour. A shell builds one with each frame and destroys it with the frame, so a
sign-in or a sign-out, which rebuild the frame, end a running tour; a reconnect leaves the frame and the tour. It
reaches the shell through `TourShell`: open a screen (`openScreen`, as a navigator click does, focusing a tab that
already shows it), whether the active tab shows a screen with given params (`shows`, by identity), the active screen
(`activeScreen`, a custom screen named by the screen it is based on), a screen's title, whether the frame is compact,
`revealNavEntry`, the spotlight, and a toast.

| Event | What happens |
|---|---|
| `start(id)` | An id the app does not declare is refused and starts nothing. Otherwise a running tour ends; the tour's `screen`, when the active screen is not it, is opened; the spotlight opens; step 0 shows |
| Showing step *n* | The step's `screen`, when the active tab does not show it with those params, is opened; a refusal ends the tour and the shell shows a toast naming the screen. A shell target is prepared (below). The spotlight's target is set, and `setTargetFocusable` is true on a waiting step. A step whose `advanceOn` screen is already active is shown as an ordinary step, not waiting: no change of the active screen would move it on, so Next does. A target not shown leaves the card centred and is reported once per step and tour run through `TourControllerOptions::warn` |
| `next()` | Shows step *n*+1, or ends the tour on the last step. A waiting step ignores it |
| `back()` | Shows step *n*-1. A screen an earlier step opened is not closed |
| `activeChanged()` | When the step waits for a screen (`advanceOn`) and the active screen is it, the tour moves on. A screen the controller is opening itself does not count |
| `end()` | The spotlight's target is cleared and it closes, which hands focus back to where it was when the tour started; the view empties |

`view()` is what the card shows: the tour's title, the step's title and body, its position and the count, whether it
waits and for which screen's title, whether Back is possible, and whether it is the last step. `onChanged` is called
after each change of it, a change of locale included, which the controller hears by following `AppRoot::tours()` while a
tour runs.

**Preparing a shell target.** In the wide form a `nav_<id>` target is revealed first: the navigator's search is emptied
and the module holding the entry opened (`Navigator::reveal`). In the compact form `tourTargetFor` rings what the frame
draws instead: `nav_<id>`, `shell_context`, `shell_help`, `shell_principal_menu` and `shell_tours` as `shell_nav_menu`,
which holds them; `shell_search` as `shell_search_open`.

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

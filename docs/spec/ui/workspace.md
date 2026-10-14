# The workspace shell (`ui::Workspace`)

`include/morph/ui/workspace.hpp`. Spec 6 §7 and §15: tabs over screens, each one screen instance, mounted through a
`DocumentScreenHost` into the tab's own page.

## Contents

- [Shape](#shape)
- [Opening](#opening)
- [Closing](#closing)
- [Switching](#switching)
- [Captions](#captions)
- [Start and record](#start-and-record)
- [Host screens](#host-screens)
- [Navigator and pins](#navigator-and-pins)
- [Design decisions](#design-decisions)

## Shape

- `Workspace(runtime, backend, host, options, parent)` makes a `TabsWidget` appended to `parent` (a backend root
  when it is null): the shell's root, which the host places. Each tab is a page slot of that bar, and its screen is
  mounted into it with `DocumentScreenHost::mountInto`.
- `WorkspaceOptions::screens` gives a screen id's catalog facts, `WorkspaceScreen{singleton, reuseEmpty, host,
  available}`; empty treats every screen as a plain, available document screen.
- Tabs keep the order they were opened in, so a page slot's index is its tab's. `tabs()` lists them as
  `WorkspaceTab{screenId, identity, caption}`; `active()` is the shown tab's index, `std::nullopt` while none is open.
- An application routes its host's `onNavigate` to `open`, so a document's `navigate` opens or focuses a tab.
- The destructor unmounts every screen without asking, newest first.

## Opening

`open(screenId, params)` answers the index of the tab it opened or focused, which becomes the active one:

1. the identity the screen would have is `host.identityOf(screenId, params)`;
2. the first tab of that screen with that identity is focused, and a `singleton` screen's tab is focused whatever
   the params;
3. a `reuseEmpty` screen opened with an identity replaces its tab that has no identity and no unsaved changes: the
   new screen takes that tab's place, and the placeholder is closed with `forceClose`, which is safe because it
   holds nothing;
4. otherwise a new tab is appended.

A refused mount answers its `MountError` and opens nothing; its page is destroyed with it.

## Closing

- `close(index, done)`, `closeOthers(index, done)`, `closeAll(done)` and `closeSaved(done)` (the tabs without
  unsaved changes) run each screen's `requestClose` (spec 6 §3), one tab at a time, and stop at the first screen that
  stays open: `done(false)`. `done(true)` when every tab asked closed. A dirty screen without `onClose` goes to the
  `CloseGuard` the application gave the host.
- A closed tab's reactive scope, screen and page are destroyed in that order. When the active tab closes, the most
  recently used of the rest is shown.
- Ctrl+W closes the active tab.

## Switching

- `activate(index)` shows a tab, keeps exactly its page visible and makes it the most recently used. A user's pick on
  the bar does the same.
- `next()` (Ctrl+Tab) and `previous()` (Ctrl+Shift+Tab) walk the tabs in most-recently-used order. Repeated presses
  walk the order as it was at the first of them, so the user can reach every tab; any other activation starts a new
  walk. The bar takes the three chords through `Widget::setKeys`; a document cannot declare them (`document.hpp`
  refuses reserved chords at load).

## Captions

A tab's caption is its screen's `title`, followed by ` •` while `dirty` is true. Each tab has an effect over its
handle's `title` and `dirty` that re-sends every caption and the selection with `setTabs` and `setSelected`. A tab is
re-keyed without any action: `open` reads each tab's identity from its handle when it looks.

## Start and record

- **The record.** `WorkspaceOptions::onRecord` receives the open tabs' routes, in order, each time the tabs change:
  when a tab opens, closes or is replaced.
  - A document screen's route is `DocumentScreenHost::routeOf`, and a host screen's is its catalog id.
  - A tab whose params have no route form is left out.
  - With `WorkspaceOptions::scheduler`, the record is written once, `recordDelay` after the last change of a
    burst. `flushRecord()` writes it at once and cancels the pending write, as sign-out does before the session
    is cleared. `record()` is the list as it would be written now.
- **The start.** `start(WorkspaceStart{start, fallback, recorded})` answers the number of tabs it opened, and makes
  the first of them active.
  - With `start == "resume"` it opens every recorded route that reads back and whose screen is `available`, in
    order. Skipped: an unknown screen, a param that no longer parses, and an unavailable entry. The `fallback`
    opens when none did.
  - Any other `start` is the screen id to open.

## Host screens

A screen whose `WorkspaceScreen::host` names a host screen is mounted by the factory registered under that name
in `WorkspaceOptions::hosts` ([host.md](host.md#host-screens)), into the tab's page. Its tab is like any other:
- the caption follows the handle's `title` and `dirty`;
- closing runs the handle's `requestClose`;
- it takes part in the recent order and the record.

A name with no factory refuses the mount with `no host screen `<name>` is registered`.

## Navigator and pins

`ui::Navigator` (`include/morph/ui/navigator.hpp`) lists the catalog for a person to open screens from.
- **Grouping.** Entries are grouped by module, then group, in the order each first appears. Each group has a
  heading, `navigator_group_<module>/<group>`, and each entry is a button, `nav_<id>`.
- **Unavailable entries** are disabled, with their reason as the tooltip, and neither open nor prefetch.
- **Search.** The search (`navigator_search`) shows the entries one of whose titles contains the text, ignoring
  ASCII case, and the headings of the groups that still show an entry.
  - `NavigatorOptions::titles` gives an entry's titles, one per locale (spec 6 §19), and the first one is shown.
  - Without it, the title is the entry's string, or a message reference's `fallback`.
  - `focusSearch()` is what the navigator's shortcut calls.
- **Pins** are the person's ordered shortcuts, the buttons `pin_<id>` in `navigator_pins`.
  - The stored list is read once, dropping unknown and repeated ids and those past `maxPins`.
  - `pin(id)` appends one; it is refused for an unknown or pinned id, or when the pins are full. `unpin(id)`
    removes one.
  - Each change sends the whole new list to `onPins`. The application stores it, under the app document's `pins`
    point.
- **Open and prefetch.** Choosing an available entry or pin calls `onOpen(id)`, which an application routes to
  `Workspace::open`. Hovering one calls `onPrefetch(id)`, which it routes to `DocumentScreenHost::prefetch`.

## Design decisions

- **The shell drives the `TabsWidget` itself**, as `Mounted` does for a `Tabs` node, rather than mounting a view
  tree: the view tree's `Tabs` fixes its pages at mount, and a workspace opens, closes and renames them. The
  `TabsWidget` contract already allows it: `setTabs` replaces the captions, and the owner keeps exactly the
  selected page slot visible.
- **Tabs are tracked by an id, not an index**, in the recent order, the walk and a close in progress, so a tab that
  closes or moves while a walk or a close is under way does not shift which tab the rest refers to.
- **In `morph::ui`, not the forms engine.** The shell needs only the host contract, the view backend and the
  catalog's facts.

- **A record is routes, and its debounce is the caller's clock.** A route reads back against the screen's declared
  params, so a recorded tab reopens as the instance it was. A host screen has no params, so its route is its id.
  Without a scheduler the record is written on every change; with one, once per burst.
- **A host screen factory mounts into a container, not a `QQuickItem`**, so one registry serves every backend
  ([host screens](host.md#host-screens)).

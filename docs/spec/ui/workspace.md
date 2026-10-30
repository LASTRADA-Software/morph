# The workspace shell (`ui::WorkspaceModel`, `ui::Workspace`)

`include/morph/ui/workspace_model.hpp` and `include/morph/ui/workspace.hpp`. Spec 6 §7 and §15: tabs over screens,
each one screen instance, mounted through a `DocumentScreenHost` into the tab's own page. `WorkspaceModel` holds the
tab logic with no widgets, for an application's own tab strip; `Workspace` is the framework's tab bar over it.

## Contents

- [Model](#model)
- [Shape](#shape)
- [Opening](#opening)
- [Closing](#closing)
- [Switching](#switching)
- [Captions](#captions)
- [Start and record](#start-and-record)
- [Overlays](#overlays)
- [Host screens](#host-screens)
- [Navigator and pins](#navigator-and-pins)
- [Design decisions](#design-decisions)

## Model

`WorkspaceModel(runtime, host, WorkspaceModelOptions)` holds the tabs: which screens are open, in which order, which
one is active, and how they open, close and are recorded. The rules of the sections below are its own; `Workspace`
answers them by index over its order.

- **Options.** `WorkspaceModelOptions` is `WorkspaceOptions` (`screens`, `hosts`, `onRecord`, `scheduler`,
  `recordDelay`, `onChanged`, `diagnostics`) and two callbacks. `mountPage(tab, screenId, params)` makes the tab's page and mounts
  the screen in it, a document screen or a host screen alike, and answers its handle, which the model owns.
  `unmountPage(tab)` is called after the tab's handle is destroyed, so the application destroys the page.
- **Tabs.** A tab is a `WorkspaceTabId`, issued when it opens and never reused. `open` answers the tab it opened or
  focused, and `openTab` the same as a `WorkspaceOpened{tab, mounted}`, `mounted` false when an open tab was focused; `activate`, `close`, `closeOthers` take one; `closeAll`, `closeSaved`, `forceCloseAll`, `start`, `next`,
  `previous`, `record` and `flushRecord` are as below.
- **Observed values.** `tabs()` is a `reactive::Computed` of the open tabs in order, each a
  `WorkspaceTabState{id, screenId, identity, title, dirty}` that follows its handle; `active()` one of the active
  tab. Both are equality-gated, so an Effect over `tabs()` runs once per open, close, move, and change of a tab's
  title, unsaved state or identity. `order()` lists the open tabs' ids without tracking, and `handleOf(tab)` is a
  tab's handle. `onChanged` is called after each such change, at once.
- **Identity and routes.** `DocumentScreenHost` is borrowed for `identityOf`, `routeOf` and `parseRoute`; mounting
  is `mountPage`'s.
- **Misuse.** An empty `mountPage` is `std::invalid_argument` at construction; a `mountPage` that throws is answered
  as a `MountError` with the exception's text, and opens nothing; a `WorkspaceTabId` the model does not hold (never
  issued, or closed) is `std::out_of_range` from every member that takes one, except `handleOf`, which answers null.
- The destructor unmounts every screen without asking, newest first, each followed by its `unmountPage`, then
  cancels every call no mount adopted (`DocumentScreenHost::cancelUnclaimedCalls`), as `forceCloseAll` does.

## Shape

- `Workspace(runtime, backend, host, options, parent)` builds a `WorkspaceModel` whose `mountPage` makes a page slot
  of its bar, mounts the screen into it (a host screen from `hosts`) and the head row above it, and whose
  `unmountPage` destroys the page; `model()` answers it. It makes a `TabsWidget` appended to `parent` (a backend root
  when it is null): the shell's root, which the host places. Each tab is a page slot of that bar, and its screen is
  mounted into it with `DocumentScreenHost::mountInto`. On each change of the model the bar shows its tabs in order,
  with their captions, the active one selected and its page alone visible, and moves a page the model moved. The
  strip of tab buttons shows with two tabs or more and hides with one (`setBarShown(tabs > 1)`): one tab needs no
  strip to choose it by, and the chords work with it hidden. There is no option to show it for one tab; an
  application that wants a strip of its own builds it over `WorkspaceModel`. The bar
  and every page stretch both ways, so a screen whose root stretches fills the page and lays itself out. Every page
  slot is scrollable (`SlotWidget::setScrollable(true)`): a screen whose root keeps its content height is shown at
  that height and scrolls inside the page, so no screen is cut off at the window's edge. Scrolling every page
  instead would leave a stretching root no height to stretch to. A stretching root is never shorter than its
  `minHeight`: while the page is shorter than that, the page scrolls it at that height.
- `WorkspaceOptions::screens` gives a screen id's catalog facts, `WorkspaceScreen{singleton, reuseEmpty, host,
  available}`; empty treats every screen as a plain, available document screen.
- Tabs keep the order they were opened in, so a page slot's index is its tab's. `tabs()` lists them as
  `WorkspaceTab{screenId, identity, caption}`; `active()` is the shown tab's index, `std::nullopt` while none is open.
- An application routes its host's `onNavigate` to `openTab`, so a document's `navigate` opens or focuses a tab, or
  to `openOverlay` when `as` asks for a dialog or a drawer ([Overlays](#overlays)), and settles the call of a
  navigation that mounted nothing ([host.md](host.md#documentscreenhost)).
- The destructor closes every overlay without asking, newest first, then unmounts every tab's screen the same way.

- `handleOf(index)` is a tab's `ScreenHandle`, and `activeHandle()` the active tab's; null past the end or with no
  tab. A client remounts a tab's screen through it on a development reload and reaches its inspector.
- `forceCloseAll()` closes every overlay, then every tab, without running a close guard, newest first: what signing
  out does, since no guard may keep a screen of the principal who is leaving. A callee goes before the opener its
  call answers, so the opener's `onCancel` runs in a live scope, as closing tabs newest first already does; then
  every call no mount adopted is cancelled.
- `WorkspaceOptions::onChanged` is called after the open tabs, their captions or the active tab changed, so a client's
  top bar can follow the active tab's title.

## Opening

`open(screenId, params)` answers the index of the tab it opened or focused, which becomes the active one:

1. the identity the screen would have is `host.identityOf(screenId, params)`;
2. the first tab of that screen with that identity is focused, and a `singleton` screen's tab is focused whatever
   the params;
3. a `reuseEmpty` screen opened with an identity replaces its tab that has no identity and no unsaved changes: the
   new screen takes that tab's place, and the placeholder is closed with `forceClose`, which is safe because it
   holds nothing;
4. otherwise a new tab is appended.

A refused mount answers its `MountError` and opens nothing; its page is destroyed with it. `openTab(screenId,
params)` opens the same way and answers `WorkspaceOpened{tab, mounted}`: `mounted` is false when step 2 focused an
open tab, which is when a host settles the navigation's call rather than leave it for a later mount.

## Closing

- `close(index, done)`, `closeOthers(index, done)`, `closeAll(done)` and `closeSaved(done)` (the tabs without
  unsaved changes) run each screen's `requestClose` (spec 6 §3), one tab at a time, and stop at the first screen that
  stays open: `done(false)`. `done(true)` when every tab asked closed. A dirty screen without `onClose` goes to the
  `CloseGuard` the application gave the host.
- A closed tab's reactive scope, screen and page are destroyed in that order: the model destroys the first two,
  then `unmountPage` lets the page go. When the active tab closes, the most
  recently used of the rest is shown.
- Ctrl+W closes the active tab.

## Switching

- `activate(index)` shows a tab, keeps exactly its page visible and makes it the most recently used. A user's pick on
  the bar does the same.
- `next()` (Ctrl+Tab) and `previous()` (Ctrl+Shift+Tab) walk the tabs in most-recently-used order. Repeated presses
  walk the order as it was at the first of them, so the user can reach every tab; any other activation starts a new
  walk. The bar takes the three chords through `Widget::setKeys`; a document cannot declare them (`document.hpp`
  refuses reserved chords at load).

## The head row

With `HeadPlacement::InPage`, the default, the shell mounts a row over the handle's `head()` above each tab's
screen, in its page: one wrapping row (`head`, gap 2, `align: start`, `justify: end`) of a lead and a trail. The lead (`head_lead`) is a
row of gap 1 that wraps, `align: center`, and stretches from `kHeadLeadMinimum` (32) units: the subtitle (muted; the screen's `subtitleSpans` as the text's runs when it gives them, else its plain subtitle as one run) and the badges, whose holder wraps too, so they sit beside one another while they fit and wrap under one another where they do not. (A wrapping row holding a wrapping row is sized from its children's one-line widths, so the width a row was last laid out at never feeds back into the width it asks for.) The trail (`head_trail`) is a row of gap 1:
the tools, the actions as buttons and the head's own menu. Where the trail fits beside the lead (32 units, or its natural width where that is less), the lead takes
the rest of the width; where it does not, the trail drops to a line of its own at the trailing edge. The test ids are
`head`, `head_lead`, `head_trail`, `head_subtitle`, `head_status_<n>` (the badge's place among the visible
ones), `head_tool_<id>`, `head_action_<id>` and `head_menu`. A tool is a `TextInput` (a search) or a dropdown
`Select`, keyed by its id: its value is bound to the tool's, it draws no caption, its label is its accessible name,
its width is the tool's (none for 0), and it is disabled while the tool is; an edit or a choice goes to
`ScreenHead::edit`, Enter in a search to `ScreenHead::submit`, and the field shows what the document binds. A button is disabled while its action is, and a click runs the action through
`ScreenHead::run`. The menu is a button-style `menu` drawing the framework icon `more` alone, its accessible name and
tooltip the head's `menuLabel()` (`More actions` for a head that has none); it is drawn again whenever the entries'
shape (their ids, separators and submenus) changes, and each entry's label, description, check mark and enablement
are bound to the head's. Choosing an entry calls `ScreenHead::choose` with the id of the entry that was drawn, so an
entry the screen has hidden since is refused by name and never mistaken for its neighbour. The row is hidden while
the head has no subtitle, badges, tools, actions or menu entries, so a screen without them looks as it would without the
row; a handle without a
head (a host screen) gets no row. The row is mounted after the screen, since it reads the handle, and moved above it.
It reads the head through the model, a tracked read of `tabs()`, so a row mounted while the model is still opening
the tab reads the head once the model holds it, and one whose handle has gone reads nothing.

With `HeadPlacement::External`, `mountPage` mounts no head row and a page holds only its screen: the host mounts
`headPartsForActive()` where it chooses.

`Workspace::headPartsForActive()` builds the same head as two parts, the lead and the trail (`HeadParts`), for whichever
tab is active, as public `Node`s a host can mount somewhere else entirely (a client's own shell chrome) —
`morph::client::Shell` constructs its `Workspace` with `HeadPlacement::External` and mounts the lead into
`ShellFrame::headLeadSlot()` and the trail into `ShellFrame::headSlot()`, on the top bar's one line, and shows the slots
while `activeHeadShown()` holds, so the head shows once. `activeHeadShown()` is a tracked read: true while the active
tab's head has a subtitle, a badge, a tool, an action or a menu entry. It and the parts' own visibility share one rule,
so the two cannot disagree. It reads
`WorkspaceModel::active()` directly, not the public `Workspace::active()` index, since the index is a position in
the tab bar, not an identity, and is unsafe to compare across a close. With no tab open it returns two defined,
invisible rows — never a null `Node` — so a caller mounting them need not handle a null case itself.

## Captions

A tab's caption is its screen's `title`, followed by ` •` while `dirty` is true. Each tab of the model has an effect
over its handle's `title`, `dirty` and `identity` that bumps `tabs()` and calls `onChanged`, on which the bar re-sends
every caption and the selection with `setTabs` and `setSelected`. A tab is
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

## Overlays

`openOverlay(request)` shows a document screen over the tabs, as `NavigateRequest::as` asks: `dialog` a
`DialogWidget`, `drawer` a `DrawerWidget` on the end side, made under the workspace's parent (a backend root when it
has none). It is what a host's `onNavigate` calls for those two, and stays inside the call protocol of
[host.md](host.md#documentscreenhost):

- **The call.** The screen is mounted into the overlay with `DocumentScreenHost::mountInto`, so the mount adopts the
  navigation's call. An overlay always mounts a new instance and never focuses an open tab, so a call is never left
  unclaimed; a catalog `singleton` limits tabs, and a singleton screen open in a tab may be open in an overlay too.
- **Documents only.** An `as` other than `dialog` or `drawer`, and a screen whose catalog entry names a `host`
  screen, are refused with a `MountError` and show nothing, as is a refused mount. A host screen adopts no call, so a
  host opens it as a tab and settles its call.
- **What it shows.** The overlay's title follows the screen's `title()`. Its footer holds a button per visible head
  action (`overlay_action_<id>` in a row `overlay_actions`, hidden while there is none), each running the action by
  its id, as the head row's do. Its badges are not shown, nor its head menu or tools: the first time the head has
  either, the workspace reports it once to `WorkspaceOptions::diagnostics` (`a dialog or drawer does not show the
  head menu or tools of <screen>`).
- **Sheets.** A dialog is a sheet (`DialogWidget::setSheet`) while the backend's `widthClass()` is `compact` when it
  opens, and follows `setWidthClass(widthClass)`, which a host calls from its backend's width-class listener, while
  it is open. A drawer has no sheet. This is the host's own chrome, so it follows the class directly; a dialog node in
  a document is a sheet only where its `sheet` says.
- **Closing.** A user's dismissal closes the widget first (`DialogWidget`'s contract). The workspace opens it again
  at once, inside the dismissal, so the screen stays reachable, and calls `requestClose`; the overlay goes only when
  that answers `true`. So a host never destroys a handle whose close is pending, a dirty screen without a guard stays,
  and a close the screen refuses leaves the overlay as it was.
- **Answers.** A screen that `return`s or runs `close: self` reaches the host's `onScreenClosed(handle)`, which a host
  forwards to `screenClosed(handle)`: the overlay of that handle goes, or, for a tab's screen, its tab (the closed
  screen answers its close at once).
- **Lifetimes.** An overlay's handle is destroyed before its widget, as `mountInto` requires of any container. The
  destructor and `forceCloseAll` close the overlays without asking, newest first, before the tabs, so a callee goes
  before the opener its call answers. `closeAll`, `closeOthers` and `closeSaved` walk tabs only: a dialog is modal
  over them, so no user reaches those walks while one is open.
- **Not a tab.** An overlay adds no tab, no record entry and no route. `overlays()` lists the open overlays' screen
  ids, oldest first. Overlays stack: a screen in an overlay may open another, and each closes on its own.

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
  heading in the `Section` text role, `navigator_group_<module>/<group>`, and each entry is a row as wide as the
  navigator, `nav_row_<id>`, holding a list item (`ButtonStyle::Item`) that stretches, `nav_<id>`, and a badge,
  `nav_badge_<id>`, hidden until the entry has a count. The search shows and hides the row.
- **Counts.** `setBadge(id, NavigatorBadge{text, tone})` shows a count at the end of a screen's entry, and
  `setBadge(id, std::nullopt)` takes it away; `badgeOf(id)` answers it. The entry stays one button to assistive
  technology: its accessible name carries the count, "Review, 4", and is its title again without one. A screen the
  navigator does not list answers `false` and changes nothing. The client copies the app document's `navigation`
  onto the entries ([client.md](../client/client.md#the-shell)).
- **As a menu.** `menuEntries()` is the navigator as the entries of a menu, for a frame too narrow for it: the pins
  first, a separator, then each group as a submenu labelled by its heading (a group with no heading puts its entries
  at the top level). An entry's label is its title with its count after it, "Review · 4"; the current screen's entry
  is checked, and an unavailable one is disabled. `choose(path)` opens the entry at that path, separators counted, as
  choosing it in the navigator does, and answers whether it named an available entry. `setOnChanged(handler)` is
  called after anything the menu lists changed: a count, the current entry, a pin.
- **The current entry.** `setCurrent(id)` marks the entry of the screen that is open and clears the one marked
  before; an id the navigator does not list, or empty, leaves none marked, and a pin is never marked. `current()`
  answers the marked one. The client shell calls it whenever the active tab changes.
- **Unavailable entries** are disabled, with their reason as the tooltip, and neither open nor prefetch.
- **Search.** The search (`navigator_search`) shows the entries one of whose titles contains the text, ignoring
  ASCII case, and the headings of the groups that still show an entry.
  - `NavigatorOptions::titles` gives an entry's titles, one per locale (spec 6 §19), and the first one is shown.
  - Without it, the title is `catalogTitles(entry, nullptr)`: the entry's string, or a message reference's
    `fallback`, else the entry's id.
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

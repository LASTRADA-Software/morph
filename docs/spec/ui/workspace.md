# The workspace shell (`ui::Workspace`)

`include/morph/ui/workspace.hpp`. Spec 6 §7 and §15: tabs over screens, each one screen instance, mounted through a
`DocumentScreenHost` into the tab's own page.

## Contents

- [Shape](#shape)
- [Opening](#opening)
- [Closing](#closing)
- [Switching](#switching)
- [Captions](#captions)
- [Design decisions](#design-decisions)

## Shape

- `Workspace(runtime, backend, host, options, parent)` makes a `TabsWidget` appended to `parent` (a backend root
  when it is null): the shell's root, which the host places. Each tab is a page slot of that bar, and its screen is
  mounted into it with `DocumentScreenHost::mountInto`.
- `WorkspaceOptions::screens` gives a screen id's catalog facts, `WorkspaceScreen{singleton, reuseEmpty}`; empty
  treats every screen as plain.
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

## Design decisions

- **The shell drives the `TabsWidget` itself**, as `Mounted` does for a `Tabs` node, rather than mounting a view
  tree: the view tree's `Tabs` fixes its pages at mount, and a workspace opens, closes and renames them. The
  `TabsWidget` contract already allows it: `setTabs` replaces the captions, and the owner keeps exactly the
  selected page slot visible.
- **Tabs are tracked by an id, not an index**, in the recent order, the walk and a close in progress, so a tab that
  closes or moves while a walk or a close is under way does not shift which tab the rest refers to.
- **In `morph::ui`, not the forms engine.** The shell needs only the host contract, the view backend and the
  catalog's facts.

Not here yet: the navigator, pins, prefetch on hover, the `resume` start and its session record (§7), and host
screens from a registry (§15).

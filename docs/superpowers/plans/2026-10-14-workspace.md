# The workspace shell: implementation plan (part 8, slice d1, #893)

**Goal.** Spec 6 §7 and §15's tabs over screens: a screen opened by id focuses the tab whose `identity` it would
have, or opens a new one; a `singleton` screen has one tab; closing runs each screen's close protocol, one tab at a
time, and close-others and close-all stop at the first veto; Ctrl+Tab walks tabs most recently used first; a tab
shows the screen's title and an unsaved mark; a tab is re-keyed when its screen's identity changes; and a
`reuseEmpty` placeholder is replaced rather than stacked.

**Specs.** Spec 6 §7, §15 ("Tab menu", "Re-keying", "Placeholders"), §17 (reserved chords). In-tree:
`docs/spec/ui/host.md`, `docs/spec/ui/view_tree.md`, `docs/spec/ui/backend_contract.md` (`TabsWidget`).

## What planning found

1. **The view tree's `Tabs` node is fixed at mount.** Its pages and their labels are set once, but a workspace
   opens, closes and renames tabs. The `TabsWidget` contract already allows what a workspace needs: `setTabs`
   replaces the captions, the bar's children are page slots in the order first shown, and the owner keeps exactly
   the selected slot visible. So the shell drives a `TabsWidget` itself, as `Mounted` does for a `Tabs` node: one
   slot per open tab, created when the tab opens and destroyed when it closes, and captions re-sent from the
   screens' `title` and `dirty`. Tab order is the order tabs were opened, so a slot's index is its tab's.
2. **A screen must mount inside its tab.** `DocumentScreenHost::mountInto` (this part, (c2)) mounts a screen with
   its root under a container; each tab's slot is that container.
3. **Reserved chords are already refused in documents** (`document.hpp`: Ctrl+Tab, Ctrl+Shift+Tab, Ctrl+W), so the
   shell can take them on its own container through `Widget::setKeys`.
4. **Where it lives.** Spec 6 §7 places the shell in `forms/engine/workspace.hpp` beside an `app_shell.hpp` that
   does not exist. The shell needs nothing of the forms engine: only the host contract, the view backend and the
   catalog. It goes in `include/morph/ui/workspace.hpp`, and spec 6 §7 says so.

## Design

- `ui::WorkspaceOptions`: what the shell needs of each screen id, `singleton` and `reuseEmpty`, from the catalog.
- `ui::Workspace(runtime, backend, host, options, parent)`: a column with a `TabsWidget`, a `DocumentScreenHost`
  the application made, which the shell mounts through `mountInto`.
  - `open(screenId, params)`: the identity from `host.identityOf`, then the first tab with that screen and
    identity is activated; else a singleton's tab is activated; else a `reuseEmpty` tab of the same screen with no
    identity and not dirty is closed and replaced; else a new tab is opened and activated. A refused mount opens
    nothing and is answered as a `MountError`.
  - `close(index, done)`, `closeOthers(index, done)`, `closeAll(done)`, `closeSaved(done)`: `requestClose` per tab,
    one at a time; a veto stops the walk and answers `false`.
  - `activate(index)`; `next()` / `previous()` over the most-recently-used order (Ctrl+Tab, Ctrl+Shift+Tab);
    Ctrl+W closes the active tab.
  - A tab's caption is its title, with ` •` while it is dirty; it is re-sent on any change. Re-keying needs no
    action: a tab's identity is read from its handle each time `open` looks.
  - `tabs()`: each tab's screen id, identity and caption, for tests and an application's tab menu.
- The application routes `DocumentScreenHost::onNavigate` to `Workspace::open`.

## Not in this slice

- The navigator, pins, prefetch on hover and `resume` start (§7), and host screens from `ScreenHostRegistry`
  (§15): slice d2.
- The three-choice close guard dialog (§15): the shell answers through the `CloseGuard` the application gives
  `DocumentScreenHost`, as today.

## Tasks (one commit each)

1. This plan.
2. `ui/workspace.hpp` and its tests on `RecordingBackend`: open and focus by identity, singleton, reuseEmpty,
   close and the veto walk, MRU, captions, chords.
3. Docs: `docs/spec/ui/workspace.md`, spec 6 §7's location, CHANGELOG.

## Verification

Mutations against freshly linked binaries: open ignoring identity (a second tab), close-others not stopping at a
veto, the caption not following `dirty`, MRU walking open order. GCC full, Qt Quick, Doxygen, clang-tidy on changed
lines with the file list split.

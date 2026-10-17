# The app-shell style — design

A client built on morph looks like a desktop application: a sidebar navigator, a top bar, tabbed screens on a tinted
page, and controls styled from the theme, on Qt Quick; the same frame on the terminal. This is direction C of the
2026-10-17 review: modern, card-based controls plus a desktop app shell.

It builds on the layout fixes (`2026-10-17-qt-quick-layout-fixes-design.md`): spacing steps, content filling the cross
axis, a sized splitter.

## What a person sees

```
┌──────────────┬───────────────────────────────────────────────────┐
│ Morph Bank   │ Accounts                              alice  ⏻    │  top bar (surface "toolbar")
│ ──────────── ├───────────────────────────────────────────────────┤
│ Search…      │ [Accounts ×] [Transfers ×]                        │  workspace tabs
│ BANKING      │ ┌───────────────────────────────────────────────┐ │
│ ▌Accounts    │ │ Account   Owner       Balance                 │ │  a card (surface "card")
│  Transfers   │ │ CHK-001   Alice      1 240.00                 │ │  on the page (surface "page")
│  Statements  │ └───────────────────────────────────────────────┘ │
│ ADMIN        │ [ New transfer ]                                  │
│  Users       │                                                   │
└──────────────┴───────────────────────────────────────────────────┘
  sidebar (surface "sidebar")
```

Before sign-in there is no sidebar and no top bar: the start screen is shown on its own, its content in a card.

## 1. Roots fill the window

`MorphRoots` anchors its column to the top, left and right only, so a root sized `stretch` in height has no height to
take, and a frame cannot fill the window. The roots column fills the host item's height when a shown root's height
stretches, and that root takes what the others leave; without one, the roots keep their implicit heights at the top.
(The terminal gives the last shown root what is left whatever its sizing; a frame stretches, so both agree on it.)

## 2. Tokens

The closed `ui-theme/1` set gains, with these defaults:

| Tokens | Kind | Defaults |
|---|---|---|
| `color.background` | colour | `#f2f3f7` (the window behind every surface) |
| `radius.control`, `radius.card` | dimension | 6 px, 10 px |
| `control.height` | dimension | 32 px (inputs, buttons, select) |
| `font.size.body`, `font.size.heading` | dimension | 0 (the platform font's size), 0 (the body size ×1.3) |
| `elevation.card` | shadow | `{color: #00000014, offsetX: 0px, offsetY: 2px, blur: 10px, spread: 0px}` |

- A **0** for a font size means "derived": `font.size.body` 0 is the platform font's size and `font.size.heading` 0 is
  the body size ×1.3. A theme that sets either in px replaces the derivation. Units (`Theme.unitWidth`,
  `unitHeight`) stay the default font's; a body size does not move them.
- `ThemeTokenKind` gains `Shadow`. `parseTokens` checks a `shadow` value's shape (an object of `color` and the
  dimensions `offsetX`, `offsetY`, `blur`, `spread`, or a non-empty list of them) instead of accepting it unchecked;
  of a list, the first layer is read. `ThemeValues` gains
  `shadows` (colour and four px lengths); `ThemeTokenSpec` gains a shadow default.
- `qt_quick::applyTheme` sets `Theme.background`, `Theme.radiusControl`, `Theme.radiusCard`, `Theme.controlHeight`,
  `Theme.bodySize`, `Theme.headingSize` and `Theme.cardShadow`.
- The heading text role uses `font.size.heading`, semi-bold. No new text role is added.

## 3. Controls

Every `Morph*.qml` component draws itself from the theme instead of the Qt Quick Controls `Basic` look:

- **Inputs** (text input, text area, date-time, file picker, select): `control.height` tall (a text area at least
  that), `radius.control` corners, a 1 px border in the border colour that turns the focus colour on focus, the
  label above in the muted colour, errors below in the error colour.
- **Buttons**: `control.height` tall, `radius.control`, outlined in the border colour on the surface colour.
- **Checkbox, tabs, table, menu, banner, badge, panel**: theme colours and radii; a table's rows are dense (the
  control height less 8 px) with a hairline between rows and its header in the muted colour; a panel is drawn as a
  card (`radius.card`, `elevation.card`, the surface colour).
- The window's background is `color.background`.

## 4. Button emphasis

`ButtonWidget::setEmphasis(ButtonEmphasis)` with `ButtonEmphasis { Normal, Primary, Danger }`, the view tree's
`Button::emphasis` and the document key `emphasis` (`"normal"`, `"primary"`, `"danger"`). The default is `Normal`
and the mount calls the setter only for another value, as for every setter that changes a default
(`backend_contract.md`), so every existing document and dump is unchanged.

- Qt Quick: `Primary` is accent-filled with white text; `Danger` error-filled with white text.
- Terminal: `Primary` bold; `Danger` in the error colour, bold.
- `RecordingBackend` records `emphasis=Primary|Danger`; a conformance case mounts each and reads it back.

## 5. Surfaces

The theme index already defines a surface: a named token file that overrides the variant for a subtree
(`ThemeIndex::surfaces`, `resolveTheme(index, files, variant, density, surface)`; spec 6 §6). That model stays.

- **Built-in surfaces.** `page`, `card`, `sidebar` and `toolbar` are built in, so an application gets the shell's look
  with no theme file. A theme index's surface of the same name replaces the built-in one. `page` and `card` derive
  from the theme, so a dark variant gets a dark page and card; `sidebar` and `toolbar` are fixed sets, which suit a
  light theme (a dark theme that wants others declares its own surfaces of those names):

| Surface | surface | text | muted | accent | selection | border |
|---|---|---|---|---|---|---|
| `page` | `color.background` | — | — | — | — | — |
| `card` | `color.surface` | — | — | — | — | — |
| `sidebar` | `#1f2430` | `#c9ccd6` | `#7d8396` | `#4c8dff` | `#2f3646` | `#2f3646` |
| `toolbar` | `color.surface` | — | — | — | — | `#e3e5ea` |

  A "—" is not set: a role a surface does not set is the theme's own value, not an enclosing surface's. Surfaces do
  not nest their colours; a card inside the sidebar is a card.
- **Qt Quick.** `applyTheme(engine, values, surfaces = {})` takes the surfaces too, a map from name to
  `ThemeValues`; the default keeps today's two-argument calls. `Theme` keeps one palette object per surface name
  (`Theme.paletteOf(name)`, the theme itself for the empty name) and updates each in place when a theme is applied
  again, since widgets hold them. Every `MorphWidget` has `palette`, bound as: its own surface's palette when it has
  a surface, otherwise `inheritedPalette`, which its container writes. `ContainerCore` gets a virtual `palette()`:
  `Roots` gives the theme's, a container widget its item's `palette`; `ContainerCore::adopt` writes it onto the
  child's `inheritedPalette`, as it writes `fillsWidth`. The mount applies a node's common properties, its surface
  included, before it mounts the node's children, so a container's palette is known when its children are adopted;
  a child's own surface arrives after its adoption, which is why its own surface wins in the binding. Every
  `Morph*.qml` reads `root.palette.<role>` instead of `Theme.<role>`.
- **Where the theme comes from.** `ui::Bundle` gains the theme: the theme index and its token files by id, empty
  when the bundle has none. The Qt Quick frontend, once a bundle is open and before the shell mounts, resolves the
  theme (`resolveTheme` with no variant or density, and once more for each of the index's surfaces) and applies it
  with the built-in surfaces underneath. A bundle with no theme gets the defaults and the built-ins. Re-applying on a
  development reload of a theme file is not part of this change.
- A widget with a surface draws that surface's colour behind its content, and for `card` the card radius and shadow.
- An unknown surface name is neither built in nor in the theme: it draws nothing and changes no colour. Nothing
  reports it, since a surface is resolved by the renderer and a document cannot know which theme a client applies.
- **Terminal.** `sidebar` and `toolbar` draw their content in the inverse of the terminal's default colours;
  `card` and `page` draw nothing. A surface changes no size on the terminal. The enclosing surface reaches a terminal
  widget as it does on Qt Quick: its container passes its effective surface at adoption, a widget's own surface wins,
  and a widget whose effective surface is `sidebar` or `toolbar` adds `inverse` to the style it draws with.

## 6. The frame

`ui::ShellFrame` (`include/morph/ui/shell_frame.hpp`) builds the frame from backend widgets, so both frontends get it:

- a row, sized stretch both ways: the **sidebar** (a column, surface `sidebar`, `fixed(28)` wide, stretch height)
  holding the application's name and a `ui::Navigator`; then the **main** column (stretch), holding the **top bar**
  (a row, surface `toolbar`: the shown screen's title, a stretch spacer, the principal's name and a sign-out button)
  and the **workspace** (`ui::Workspace`'s tabs, stretch, surface `page`);
- `showSignedIn(bool)`: signed out, the sidebar and the top bar are hidden;
- each tab's content is padded by step 4.

`Workspace` gains `handleOf(index)` and `activeHandle()`, giving the `ScreenHandle` of a tab, so a client can remount a
tab's screen for development reload and reach its inspector, and `forceCloseAll()`, which unmounts every tab without
asking, for sign-out.

## 7. The client on the frame

The generic client's shell (`examples/common/app/client_shell.cpp`) uses `ShellFrame`:

- Screens are tabs: `navigate` and the navigator's choices go to `Workspace::open`; the start screen is the first
  tab; there is no `resume` (the client has no record store).
- **Routes.** On WebAssembly the browser history follows the active tab's route, pushed when a tab is opened and
  when one is activated; back and forward open or activate the tab of the route.
- **Sign-out.** `Workspace::forceCloseAll()` closes every tab without running a close guard, then the existing posted
  sign-out runs, and the frame shows the signed-out start screen.
- **Development reload** remounts every tab whose document changed, through `handleOf`. **Inspector**:
  `inspectFocused` visits the active tab's screen through `activeHandle`.
- Bank's own sidebar (`examples/bank/screens/bank_screens.cpp`, the `nav-*` buttons and `signOut`) is removed: the
  frame is that seam now. Bank's GUI behaviour test and the client shell's tests that assumed one screen at a time
  (`examples/common/app/tests/test_sources.cpp` "navigate replaces the screen", the history-per-mount case,
  `render_smoke.hpp`) are rewritten for tabs.

## 8. The examples

Bank's sign-in column becomes a `card` with the sign-in button `primary`; screens that show a list put it in a `card`;
a destructive action is `danger`.

## Tests

- Roots: a root sized `stretch` in height fills the window; two content roots sit at the top.
- Theme: the new tokens' defaults; a `shadow` token reads and a malformed one is refused at registration; font sizes
  0 derive.
- Qt Quick: an input is `control.height` tall with `radius.control`; a `primary` button's background is the accent;
  a widget with surface `card` draws the card colour and radius; a text inside a `sidebar` is the sidebar text
  colour and one outside it is not; a theme index surface replaces a built-in one.
- Terminal: `primary` is bold; a `sidebar` surface draws inverse.
- `ShellFrame` on `RecordingBackend`: the widget tree, and that signed out hides the sidebar and the top bar; on Qt
  Quick, the sidebar is 28 units wide and the workspace takes the rest of the window.
- `Workspace::handleOf` and `activeHandle` give the handles of open tabs and nothing past the end; `forceCloseAll`
  unmounts every tab and runs no close guard.
- The Qt Quick frontend applies a bundle's theme: a bundle whose theme sets `color.accent` and a `sidebar` surface
  shows that accent on a primary button and that sidebar colour behind a `sidebar` widget.
- The client: two navigator choices give two tabs; sign-out closes every tab and shows the start screen with no
  sidebar; reload remounts a changed tab; the inspector visits the active tab.
- Screenshots of bank signed out and signed in, recorded in the pull request. They are not the evidence; the tests
  above are.

## The specs that change

`docs/spec/ui/theme.md` (closed set, kinds, surfaces, On Qt Quick, Not yet), `backend_contract.md` (emphasis, surface
meaning), `view_tree.md`, `document.md`, `workspace.md` (handles), `host.md`, a new `docs/spec/ui/shell_frame.md`,
`docs/spec/qt_quick/backend.md` (palettes, roots), `docs/spec/tui/frontend.md` ("a surface changes nothing drawn"
becomes the inverse rule), and spec 6 §6 and §7 (`2026-10-08-app-hosting-design.md`): the built-in surfaces, and that
the frame's top bar with the principal and sign-out is the library's, not the host's.

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Surface model | The theme index's surface files, plus four built-in layers | Spec 6 §6 already defines surfaces this way; a second model in the closed set would contradict it. |
| Surface inheritance | A surface's unset role is the theme's | `ThemeTokenSpec` has no "unset"; the theme's value is predictable and a nested card reads as a card. |
| Propagation on Qt Quick | The container writes its palette onto each adopted child | A QML child cannot see its container; the C++ container can, as it already does for the cross-axis fill. |
| Unknown surface | Draws nothing, reported nowhere | The renderer resolves surfaces; a document cannot know the client's theme. |
| Button emphasis | A widget property, default `Normal` | A primary action is a document fact; the default keeps every document unchanged. |
| The frame | A library composition of backend widgets | The explorer and every client need it; built from widgets, it runs on both frontends. |
| Max content width | None; the content is padded | No max-width sizing exists, and a fixed column would be a minimum, which an 80-column terminal or a compact window cannot fit. |

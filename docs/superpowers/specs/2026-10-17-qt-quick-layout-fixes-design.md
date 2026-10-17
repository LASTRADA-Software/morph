# Qt Quick layout fixes — design

The Qt Quick renderer lays out three things wrongly, and every example screen shows it: children sit far apart, fields
shrink to the toolkit's minimum, and a splitter's panes draw over each other. This change fixes those three, and makes
the spacing of a document mean the same thing on Qt Quick and on the terminal.

It is the first of the steps toward a usable client (the layout, the development tools in every build, the app-shell
style, the app explorer); each later step builds on screens that lay out correctly. It lands after the ladder is cut
to three rungs, so the documents it rewrites are those of bookmarks, bank, lims, the gallery, the forms lab and the
forms engine.

## What is wrong, measured

Measured on `master` at 003cc536, `linux-everything` with `MORPH_BUILD_QT_QUICK=ON`, by dumping the item tree of
`morph_examples_client --app bank` and by a probe on the real engine.

| Symptom | Cause |
|---|---|
| Bank's sign-in fields are 157 px apart (25 px field, 136 px gap) | The column says `gap = 8`, and a gap is in units: a line height (17 px) on Qt Quick, a cell on the terminal. 8 units is 136 px, or eight blank rows on the terminal. |
| Fields are 120 px wide in a 1024 px window | A child sized `content` gets its natural width across a Qt Quick stack. The terminal gives it the full width (`layout::crossExtent`, and `slotOf` for single-child containers). The two backends disagree. |
| A splitter's "Left" and "Right" overlap | `MorphSplitter`'s implicit size is its `SplitView`'s, which is 0×0, so a splitter sized `content` has no area and both panes are drawn at its origin. |

The authors are not at fault for the first. Counting `.gap =` and `.padding =` in `examples/` at 003cc536: 35 gaps of
4, 8, 12 or 16 and 9 paddings of 4 or 8 (written in pixels), against one gap of 2 (written in lines). The forms engine
(`include/morph/forms/engine/form_view.hpp`, `wizard_view.hpp`) writes gap and padding 1, meaning one line. The same
mistake in that many places is a missing framework seam, so the fix is in the vocabulary.

## Spacing is a step on a scale

`gap` (column, row, grid, forEach) and `padding` (panel) name a **spacing step**, an integer from 0 to 5:

| Step | Qt Quick (px, the `space.<step>` default) | Terminal, vertical (rows) | Terminal, horizontal (columns) |
|---|---|---|---|
| 0 | 0 | 0 | 0 |
| 1 | 4 | 0 | 1 |
| 2 | 8 | 0 | 1 |
| 3 | 12 | 1 | 2 |
| 4 | 16 | 1 | 2 |
| 5 | 24 | 2 | 3 |

- **The clamp is in the mount.** `ui::spacingStep(value)` maps a document's value to 0..5 (below 0 is 0, above 5 is
  5), and `Mount` passes only that to `setGap` and `setPadding`. The backend contract says the argument is a step in
  0..5; a backend never sees another value, and `RecordingBackend` records the clamped step. A conformance case
  mounts a column with gap 99 and -3 and checks the backend received 5 and 0.
- **Qt Quick** reads a step from the `Theme` singleton's `space` list (six lengths in px), so a theme can change it.
  `MorphStack`, `MorphGrid` and `MorphPanel` multiply by nothing any more: spacing is `Theme.space[gap]`.
- **The terminal** maps a step through the fixed table above, by axis. A cell is about twice as tall as it is wide,
  so the vertical column is roughly half the horizontal one. The mapping is `layout::stepCells(step, Axis)` in the
  terminal's solver. The solver's single gap becomes two where both axes occur: `Track.gap` stays per axis,
  `planGrid` takes a row gap and a column gap (and `GridPlan` reports both), and `pad` takes vertical and horizontal
  padding. Stacks, grids and panels convert at `setGap` / `setPadding`.
- **Sizes stay in units.** `Sizing::fixed(n)` is still `n` units: how wide a field is is a unit question; how far apart
  two fields sit is a spacing question, and only the second needs to be finer than a line.

### Theme tokens

The six `space.0` … `space.5` tokens (DTCG `dimension`) join the closed `ui-theme/1` set, since a built-in component
now reads them. This is the numeric naming `docs/spec/ui/theme.md` already uses in its example (`space.2`).

- `ThemeTokenKind` gains `Dimension`; `ThemeTokenSpec` gains a dimension default in px; `ThemeValues` gains
  `dimensions` (px). A `rem` value is 16 px.
- `themeValues` takes a dimension from a token whose type is `dimension`, as it takes a number from `number`.
- `qt_quick::applyTheme` sets `Theme.space` from them, and reports a dimension it could not set as it does a colour.

### The rewrite

Every document is rewritten to steps in the same change:

| Written as | Becomes | Why |
|---|---|---|
| gap or padding 4, 8, 12, 16 (pixels) | 1, 2, 3, 4 | The nearest step. |
| gap 1 or padding 1 in the forms engine (one line) | 4 | 16 px is a line on Qt Quick; one row on the terminal, as today. |
| gap 2 (two lines) | 5 | |

The `gap = 8` columns become step 2: 8 px on Qt Quick, no blank row on the terminal. A form whose fields carry
labels reads without blank rows between them, and the forms engine's own spacing (step 4) still separates its
sections.

## The cross axis

A child sized `content` fills its container across the axis it is laid out along, on Qt Quick as on the terminal:

- in a vertical stack (column, vertical forEach), every content child is as wide as the stack;
- in a horizontal stack (row, horizontal forEach), every content child is as tall as the row;
- in a grid, a content child fills its cell's width;
- in a single-child or page container (panel, collapsible body, slot, scroll area, tab page, dialog, drawer, the
  roots), a content child fills its width.

`fixed(n)` still gives exactly `n` units, at most the space there is; `stretch` is unchanged. This rule moves into
`docs/spec/ui/backend_contract.md` and `docs/spec/tui/frontend.md` (where the solver already implements it without
saying so).

**The mechanism on Qt Quick.** A child cannot see its parent's axis, so the container tells it: `ContainerCore::adopt`
sets two properties on the child's `MorphWidget`, `fillsWidth` and `fillsHeight`, from the container's kind and axis.
`MorphWidget` binds `Layout.fillWidth: widthKind === 2 || (widthKind === 0 && fillsWidth)`, and the same for height.
A host component that roots in `MorphWidget` fills as every built-in kind does, which is the terminal's behaviour too.

A text that fills a column keeps its left alignment and wraps later; a field's edge is what moves. `MorphField` already
binds its control to `root.width`.

## The splitter's size

`MorphSplitter`'s implicit size is computed from its panes: along the axis, the sum of each pane's extent (its `sizes`
entry in units when `sizes` matches the pane count, else its implicit extent) and the handles between them; across
it, the largest pane's implicit extent. A splitter sized `content` is then as large as its panes ask for; one sized
`fixed` or `stretch` is unaffected.

## The specs that change

- `docs/spec/ui/backend_contract.md`: `setGap` / `setPadding` take a step in 0..5; the cross-axis rule.
- `docs/spec/ui/view_tree.md` (sizing and the container table), `docs/spec/ui/document.md` (gap and padding),
  `docs/spec/ui/interpreter.md` (where it passes them).
- `docs/spec/tui/frontend.md`: the step tables, the per-axis gaps, the cross-axis rule.
- `docs/spec/qt_quick/backend.md`: spacing from `Theme.space`, not from units; `fillsWidth`/`fillsHeight`.
- `docs/spec/ui/theme.md`: the closed set gains `space.0`–`space.5`; `Dimension`; drop "scale tokens" from Not yet.
- Doxygen on `setGap` / `setPadding` / the options structs in `include/morph/ui/backend.hpp` and `view.hpp`
  ("backend units" becomes "a spacing step").

## Tests

Each defect gets a failing test before its fix, on the real Qt Quick engine; each runs on the unfixed code first and
must fail there.

- A column of a text and two inputs with gap step 2 places them 8 px apart (today 34 px).
- A text input sized `content` in a column nested under a `fixed(40)` parent is 40 units wide (today 120 px).
- A splitter sized `content` with two texts is at least as wide as both texts, and the right pane starts at or after
  the left one's end (today 0×0); with `sizes` {10, 30} it is 40 units plus the handle wide.
- A row gives a content child the row's height.
- A theme that sets `space.3` to 20 px makes a gap of step 3 20 px wide; `applyTheme` reports nothing unset.
- The terminal maps every step through its table, both axes (`tests/tui/test_tui_layout.cpp`, including `pad` and
  `planGrid` with distinct row and column gaps).
- The conformance case for the clamp, run by `RecordingBackend`, the terminal and Qt Quick.
- `tests/test_ui_theme.cpp`: the set's size counts dimensions; `space.2` is now in the set.

## Not in this change

- Padding around a screen, a readable maximum content width, and the look of the controls belong to the app-shell
  style (a later step).

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| The unit of a gap | A step on a theme scale | A pixel value is one renderer's model in a backend-neutral document; a unit (a line) is too coarse for compact rows. A step is meaningful on both, and a theme can tune it. |
| Where values are clamped | The mount | One place for every backend, and `RecordingBackend` then records what every backend receives. |
| The terminal's table | Fixed in the solver, per axis | A terminal has no theme dimensions to read, and a cell's two extents differ. |
| Token names | `space.0` … `space.5` | The step is the name; it matches the numeric example the theme spec already shows. |
| Content across a container | Fills, told by the container | It is the terminal rule already and what a form needs; a QML child cannot see its parent's axis, the C++ container can. |
| Changing `gap`'s meaning in place | No new key, no vocabulary bump | Morph is `0.x` (`docs/spec/VERSIONING.md`): any release may change anything. Every document that sets a gap is in this repository and is rewritten in the same change. |

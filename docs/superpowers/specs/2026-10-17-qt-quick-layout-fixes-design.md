# Qt Quick layout fixes — design

The Qt Quick renderer lays out three things wrongly, and every example screen shows it: children sit far apart, fields
shrink to the toolkit's minimum, and a splitter's panes draw over each other. This change fixes those three, and makes
the spacing of a document mean the same thing on Qt Quick and on the terminal.

It is the first of four steps toward a usable client (the layout, the development tools in every build, the app-shell
style, the app explorer); each later step builds on screens that lay out correctly.

## What is wrong, measured

Measured on `master` at 003cc536, `linux-everything` with `MORPH_BUILD_QT_QUICK=ON`, by dumping the item tree of
`morph_examples_client --app bank` and by a probe in `tests/qt_quick/test_qt_quick_containers.cpp`.

| Symptom | Cause |
|---|---|
| Bank's sign-in fields are about 157 px apart | The column says `gap = 8`, and a gap is in units: a line height (17 px) on Qt Quick, a cell on the terminal. 8 units is 136 px, or eight blank rows on the terminal. |
| Fields are 120 px wide in a 1024 px window | A child sized `content` gets its natural width across a Qt Quick stack. The terminal solver gives it the full width (`layout::crossExtent`). The two backends disagree. |
| A splitter's "Left" and "Right" overlap | `MorphSplitter`'s implicit size is its `SplitView`'s, which is 0×0, so a splitter sized `content` has no area and both panes are drawn at its origin. |

The authors are not at fault for the first. Across the examples, `gap` is 1 or 2 in thirteen places and 4, 8, 12 or 16
in thirty-eight, and `padding` is 1 in four places and 4 or 8 in nine: half the documents were written in pixels. The
same mistake in that many places is a missing framework seam, not thirty-eight bugs, so the fix is in the vocabulary.

## Spacing is a step on a scale

`gap` (column, row, grid, forEach) and `padding` (panel) name a **spacing step**, an integer from 0 to 5:

| Step | Name | Qt Quick (px, the `space.*` default) | Terminal rows (vertical) | Terminal columns (horizontal) |
|---|---|---|---|---|
| 0 | none | 0 | 0 | 0 |
| 1 | xs | 4 | 0 | 1 |
| 2 | s | 8 | 0 | 1 |
| 3 | m | 12 | 1 | 2 |
| 4 | l | 16 | 1 | 2 |
| 5 | xl | 24 | 2 | 3 |

- A step above 5 is 5; a negative step is 0, as a negative gap is today.
- On Qt Quick a step is read from the `Theme` singleton's `space` list, so a theme can change it. The six `space.*`
  tokens (`space.none` … `space.xl`, dimensions) join the closed `ui-theme/1` set with these defaults, since a built-in
  component now reads them (`docs/spec/ui/theme.md`, "The closed set").
- On the terminal a step maps through the fixed table above. A cell is about twice as tall as it is wide, so the
  vertical table is about half the horizontal one, and both round a pixel distance to the nearest cell.
- Widths and heights (`Sizing::fixed(n)`) stay in units. A field 30 characters wide is a unit question; how far apart
  two fields sit is a spacing question, and only the second needs to be finer than a line.

The examples are rewritten to steps: a pixel-ish 4, 8, 12, 16 becomes 1, 2, 3, 4; a unit-ish 1 or 2 (a line, or two)
becomes 4 or 5.

## The cross axis

A child sized `content` across a stack fills the stack's cross extent on Qt Quick too, which is the terminal rule
already. In a column every child is as wide as the column; in a row every child is as tall as the row. `fixed(n)`
still gives exactly `n` units, at most the space there is. This rule moves from the terminal spec into
`docs/spec/ui/backend_contract.md`, since it is a property of the contract and not of one backend.

A text that fills a column's width keeps its alignment, so this changes no text's look; it changes where a field's
edge is.

## The splitter's size

`MorphSplitter`'s implicit size is computed from its panes: along the axis, the sum of the panes' implicit extents and
the handles between them; across it, the largest pane's. A splitter sized `content` is then as large as its panes ask
for, and one given `fixed` or `stretch` is unaffected.

## Tests

Each defect gets a failing test before the fix, on the real Qt Quick engine:

- a column of a text and two inputs with gap step 2 places them 8 px apart (fails today at 34 px);
- a text input sized `content` in a 400 px column is 400 px wide (fails today at 120 px);
- a splitter sized `content` with two texts is as wide as both texts and the handle, and the right pane starts after
  the left one ends (fails today at 0×0);
- the terminal maps every step through its table, vertically and horizontally (a solver test);
- a theme that sets `space.m` to 20 makes a gap of step 3 20 px wide.

The control for each is the current behaviour: every test is run on the unfixed code first and must fail there.

## Not in this change

- Padding around a screen, a readable maximum content width, and the look of the controls belong to the app-shell
  style (step 3).
- Margins between groups of a form, and anything a `form` node lays out itself, are unchanged.

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| The unit of a gap | A step on a theme scale | A pixel value is one renderer's model in a backend-neutral document; a unit (a line) is too coarse for compact rows. A step is meaningful on both, and a theme can tune it. |
| The terminal's table | Fixed in the backend | A terminal has no theme dimensions to read, and a cell is the only unit it has. |
| Content across a stack | Fills the cross extent | It is the terminal rule already, it is what a form needs, and two backends disagreeing on it is the defect. |
| Changing `gap`'s meaning in place | No new key, no vocabulary bump | Morph is `0.x` (`docs/spec/VERSIONING.md`): any release may change anything. Every document that sets a gap is in this repository and is rewritten in the same change. |

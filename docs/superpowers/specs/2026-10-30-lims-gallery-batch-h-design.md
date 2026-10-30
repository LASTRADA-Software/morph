# LIMS gallery batch H: page edges, glyph buttons, selected cards and head text

Batch H of the LIMS gallery visual review (`examples/lims_gallery/design/visual-review.md`, "After batch G"). It
closes every item that list leaves for later:

- H1, a pinned bar that bleeds to the page's edge;
- H2, a square, glyph-sized button;
- H3, a control inside a selected entry;
- H4, runs in a head's title;
- H5, an eyebrow line above a head's title, and section labels in small capitals.

It adds two framework gaps that a fresh comparison found and that no screen can fix:

- H6, the shell head's lead keeps a 32-unit minimum it should cap at its natural width;
- H7, a pinned bar whose nodes are all hidden still draws an empty strip.

The aim is the review's: ui/1 itself produces each mockup's look. A difference is a vocabulary or frontend change,
and a screen changes only where the vocabulary already says what the design says. The rules of batch G hold here:

- A look is a token or a rule, never a screen's choice of pixels.
- A key means the same thing on every kind that takes it.
- The strong weight is the theme's one emphasis weight, never a literal 500 or 600.
- The terminal keeps its cells. A purely visual change says what it leaves unchanged there, and a change to what a
  user can read or reach is drawn on both frontends.

Every claim below was checked against `c4df1b3e9` (`lg-h-design`, built as `build/linux-everything` from that
revision before grabbing). Each item says whether its premise was **reproduced** (run or measured here) or **read**
(inferred from the code). Grabs are `morph_examples_client --app lims-gallery --screen lims-gallery.<id>
--grab-size WxH --grab FILE`; mockups are the `design/mockups/*.dc.html` files rendered in headless Firefox
(`lg-cmp/mock/*.png`). Pixel figures for the mockups are read from their CSS where it gives them and measured in the
renders otherwise.

## Probes

Three throwaway probes were run with `qml6` (Qt 6.12.0, offscreen) and kept out of the tree (`lg-hd-probe1.qml` to
`lg-hd-probe3.qml` in the session scratchpad). Qt 6.8 itself was not run.

**1. A pager glyph's size.** `TextMetrics` on `‹` and `⋯` in the gallery's sans face:

```
PROBE ‹ 13px/400 tight 3.0x7.0 advance 4.3
PROBE ‹ 13px/700 tight 4.0x7.0 advance 4.3
PROBE ‹ 16px/700 tight 4.0x8.0 advance 5.3
PROBE ⋯ 13px/400 tight 11.0x5.0 advance 13.0
PROBE ⋯ 16px/700 tight 14.0x6.0 advance 16.0
```

In the audit grab at 1000×830 the `‹` of the pager has a dark core (all channels below 120) of 2×3 px inside a
61×32 px button. The mockup's has a 3×6 px core inside a 31×30 px button (`04-Audit.png`, `.btn{height:30px;
padding:0 12px; font:500 13px Inter}`). At the body size and regular weight the glyph is about 4 px of ink, which
reads as a speck in a 32 px square.

**2. Capitals, letter spacing and a selected group.**

```
PROBE plain text=Record type w=63.7
PROBE caps text=Record type w=85.0 letterSpacing=0.546875 cap=1
PROBE a11y selectable=true selected=true
```

- `font.capitalization: Font.AllUppercase` draws the text in capitals and leaves `text`, and so the accessible name,
  as written.
- `font.letterSpacing` takes a fractional pixel value and quantises it (0.55 asked, 0.546875 read).
- `Accessible.selectable` and `Accessible.selected` are settable on an item whose role is `Grouping` and read back.

All three properties exist on Qt 6.8 (they predate Qt 6), so none of them raises the floor.

**3. The console's compact head.** The console grabbed at a height of 300 and widths 400 to 420, reading the first
row of page colour under the top bar at x = 5:

```
400 page starts y 116
405 page starts y 60
410 page starts y 60
```

The top bar holds the menu (☰), the title "Overview" over the "Connected" badge, and Scan. Below 405 px, Scan drops
to a second line. Title and badge are each about 75 px wide, so the line would fit from about 240 px if the lead took
its natural width. It drops at the width a 32-unit lead (about 248 px) gives: 12 + 32 + 12 + 248 + 12 + 82 + 12 px.
At 390×844, the size the review grabs, Scan sits on a line of its own. The mockup's compact console draws one line
(`10-Console.png`).

## The items

| Item | Where it lands | Verdict on the review |
|---|---|---|
| H1 | a screen's `pageInset`; `pinned`'s inset; a two-step `padding` on stacks; `ShellFrame::setPageInset` | Right (reproduced: every compact bar's white run is x = 16 to 373 at 390 px) |
| H2 | `shape: square` on `button`, `badge` and a button-style `menu`; the framework's own icon triggers | Right (reproduced, probe 1). **Wider than listed:** the head's `⋯` menu and samples' bulk `⋯` are 62 px too. **Narrower:** results' pager is two labelled buttons in the mockup, so it keeps them |
| H3 | `selected` on `column` and `row`: the accent bar, with no click and no fill | Right (read: the item button refuses a check box, `document.md`, the `button` row) |
| H4 | the screen's `titleSpans`; `ShellFrame::setTitleSpans` | Right. It is the shell head G9 deferred to |
| H5 | the screen's `eyebrow` and `eyebrowSpans`; the text role `overline`; the navigator's group headings | Right. The eyebrow is not in capitals in any mockup; the capitals are a separate role |
| H6 | the head lead built for a shell drops its own minimum | Found here (reproduced, probe 3) |
| H7 | `pinned` hides a bar while none of its nodes shows | Found here (read, not run) |

## Parts and lanes

### Framework files

Two lanes, each in its own worktree, each owning the files it changes:

| Lane | Items, in order | Owns |
|---|---|---|
| H-C, controls and stacks | H1's stack half (two-step `padding`, `pinned`'s inset), H7, H3, H2, H5's `overline` | `control_shape.hpp`; `view.hpp`; `backend.hpp`; `mount.hpp`; `testing/recording_backend.hpp`; `testing/backend_conformance.hpp`; `EchoingBackend` in `tests/test_ui_common.cpp`; Qt Quick `MorphStack`, `MorphWidget`, `MorphButton`, `MorphButtonBackground`, `MorphMenu`, `MorphBadge`, `MorphText`, `kinds.hpp`, `containers.cpp`, `leaves.cpp`, `fields.cpp`; terminal `container_widgets.*`, `leaf_widgets.*`, `list_widgets.*` (the menu trigger) |
| H-S, shell and head | H1's page half (`pageInset`, the shell, the client), H6, H4, H5's eyebrow, then the framework triggers of H2 and the navigator's headings of H5 | `shell_frame.hpp`; `workspace.hpp`; `host.hpp`; `navigator.hpp`; `src/client/shell.cpp` |

**Landing order: H-C, then H-S.** The lanes work in parallel. Each lands on `feat/lims-gallery` with one commit per
item, green on its own, and H-S rebases onto H-C before it lands. H-S's last two framework commits use what H-C adds:
`MenuTrigger::shape` (the head menu and the navigation menu go square) and `TextRole::Overline` (the navigator's group
headings). H-S writes those two commits after its rebase.

### Shared files

Every file both lanes touch has a named region per lane. The later lane rebases onto the earlier lane's version.
Within a region, rows and cases are appended, never reordered.

| Shared file | H-C's region | H-S's region |
|---|---|---|
| `document.hpp` | the `KeySpec` arrays (`kContainerKeys`, `kMenuKeys`), the role names, and the node checks | the `Document` struct, its top-level loading (`pageInset`, `titleSpans`, `eyebrow`, `eyebrowSpans`), the app document's refusal list, and their expression checks |
| `interpreter.hpp` | `roleNamed`, and the lowering of stacks, buttons, badges and menus | the screen's head evaluation (`titleSpans()`, `eyebrow()`, `eyebrowSpans()`, `pageInset()`) |
| `author.hpp` | `ContainerOptions`, `padding(…)`, `PinnedOptions` and `pinned`, `MenuButtonOptions` and `menuButton` | `Scope`'s head methods |
| `document.md` | the View nodes rows (`column, row`, `button`, `badge`, `menu`, `text`), and "A bar that stays in view" | "The document" table and "The head" |
| `author.md` | containers, `pinned`, `menuButton` | `Scope` |
| `tests/test_ui_document.cpp`, `tests/qt_quick/test_qt_quick_containers.cpp`, `tests/qt_quick/test_qt_quick_widgets.cpp` | new `TEST_CASE`s appended at the end | new `TEST_CASE`s appended after H-C's |
| `tests/ui/corpus/` | new files only | new files only |

H-S adds no pure virtual, so it does not touch `backend.hpp`, the recording backend, `EchoingBackend` or the
conformance suite. The shell draws its new parts with widgets every backend already has: a text's runs
(`TextWidget::setSpans`), a stack, and visibility.

### Screens

Each screen file, and its test under `examples/lims_gallery/tests/`, has one owning lane. The owner makes every edit
on it, after the items it uses have landed. The landing order guarantees that, because H-S lands last.

| Screen file | Owner | Uses |
|---|---|---|
| `audit.cpp` | H-C | H2 (the pager), H5 (`overline` facet labels) |
| `console.cpp` | H-C | H2 (the sample pager) |
| `receipt.cpp` | H-S | H1 |
| `samples.cpp` | H-S | H1, H2 (the pager, the bulk `⋯`), H3 (the cards), H7 (the bulk bar) |
| `stability.cpp` | H-S | H1; H5 (the compact step line as the eyebrow) |
| `results.cpp` | H-S | H1; H5 (the worksheet line as the eyebrow) |
| `review.cpp` | H-S | H1 (the splitter at the page's edges; the panes inset themselves) |
| `instrument.cpp` | H-S | H5 (the breadcrumb as the eyebrow); H2 reaches its head `⋯` through the workspace |
| `plate.cpp` | H-S | H4 |

H-C verifies H1 with uncommitted screen changes. H-S runs the final re-grab pass after both lanes have landed.

## H1: a pinned page's bars span the page

**Problem (reproduced; read).**

- **The bars stop 16 px short.** At 390×844 the white run of each compact bottom bar is x = 16 to 373, on receipt,
  samples, stability, results and review. At 1000×830 stability's and review's bars run to x = 983. The mockups' bars
  span the window:
  - compact: `padding:12px 16px; border-top:1px solid #E1E5EA` across the whole 390 px (`02`, `06` and `09` compact);
  - review expanded: the worksheet pane's bar spans the pane (`09`, x = 327 to 1039 in a card whose edge is the
    window's), with its content 16 px in.
- **The cause (read).** `ShellFrame` pads the page by spacing step 4 with zero-sized spacers above and either side of
  `_content` (`shell_frame.hpp`, the `_top`, `_left` and `_right` spacers), and the workspace mounts every screen
  inside that inset. `pinned` (`author.hpp`) is a helper that writes a divided column, so nothing marks a page as
  pinned. Neither the shell nor the workspace can tell a bar from the content.
- **Review's panes (reproduced).** The splitter sits inside the page inset, so the queue pane's surface stops 16 px
  from the window's left, bottom and right edges. The worksheet pane carries its own `padding: 4` column inside
  `pinned`'s scroll to inset its content.

**Decision.** The page inset moves to where it belongs on a pinned page: around the scrolling body and inside the
bars, not around the bars.

1. **A screen says the shell does not inset its page.** The document member `pageInset` is a condition, default
   `true`, evaluated in the screen scope like the head. While it is false, a shell that insets its page lays the
   screen at the page's edges, and the screen insets its own content.
   - It is a screen's only. The app document refuses it, as it refuses the head.
   - `ScreenHead::pageInset()` is a virtual returning `reactive::Computed<bool> const*`. Its default, `nullptr`, means
     inset, so a host screen's head compiles unchanged and keeps the inset. A closed screen keeps its last value, as
     the subtitle does.
   - `Workspace::activePageInset()` is a tracked read of the active tab's value, `true` with no active tab.
   - `ShellFrame::setPageInset(bool)` shows or hides the three inset spacers. A hidden spacer takes no gap, so the
     content then fills the page. A new frame insets.
   - The generic client (`src/client/shell.cpp`) sets it from an effect beside the head's `setHeadShown` effect,
     reading `activePageInset()`.
   - With `HeadPlacement::InPage` there is no shell inset, and `pageInset` changes nothing.
   - **Why a screen member, not a node key.** A root is usually a `switch` by width class, so the node that is
     pinned is not the document's root, and it changes with the width. A screen member is evaluated where the head
     is, and the client already follows the head reactively. A node key that cancelled its ancestors' padding would
     need every frontend's layout to know the page's inset and where a node touches it.
2. **`pinned` insets its content by the page's step.** `PinnedOptions::inset` is a spacing step, default 4, the
   shell's page inset:
   - the scroll's content sits in a `column` padded by `inset` on every side;
   - each bar is a `column` padded by step 2 at its top and bottom and by `inset` at its sides, so the bar's content
     lines up with the body's while its surface and its rule span the whole column;
   - `.inset = 0` writes the content and the bars as they are written today, with step 2 on every side of a bar.

   So a pinned column at a page's edges, or in a splitter pane, draws its bars across that page or pane, and its
   body is inset as the page was. A screen whose root is pinned says `pageInset: false`, so the inset is drawn once.
3. **A stack's padding takes two steps.** `padding` on `column` and `row` is a spacing step (every side) or a literal
   two-item list `[vertical, horizontal]` of steps, as CSS's `padding: 8px 16px` is.
   - A literal list of any other length, or with a step outside 0 to 5, is refused.
   - A computed padding is a step, as today. One that evaluates to a list is reported once and taken as 0.
   - `panel`'s `padding` stays one step, and a list there is refused.
   - In the view tree, `Column::padding` and `Row::padding` become `Padding{int vertical; int horizontal;}`, with
     both members defaulting to 0. `StackWidget::setPadding(int)` becomes `setPadding(Padding)`. The mount still
     calls it only for a stack that has padding.
   - Author: `ui::padding(int vertical, int horizontal)` returns the literal list as an `AnyHandle`.
     `.padding = 2` keeps meaning every side.

**Every pinned screen, at both sizes.** With H1, each of these screens declares `pageInset: false` at both sizes.

| Screen | Compact (390×844) | Expanded (1000×830) |
|---|---|---|
| receipt | bottom bar (Reject, Receive) spans x = 0 to 390, content at 16 | `pinned` with no bar: the body is inset 16, as today |
| samples | bulk bar spans the width (with H7, only while a sample is selected) | `pinned` with no bar: the body as today |
| stability | top bar (the indicator) and bottom bar (Back, Continue) span the width | the stepper and the footer span the page, content at 16 |
| results | bottom bar spans the width | bottom bar spans the page |
| review | bottom bar spans the width; the queue case's `pinned` insets the queue | the splitter fills the page edge to edge. The queue pane is padded by step 3 (the mockup's 12 px). The worksheet pane's `pinned` insets its body by 16, so the screen's own `padding: 4` column goes, and its bar spans the pane from the splitter handle to the window's edge |

**Per backend.**

- **Qt Quick:** `MorphStack` insets by `Theme.spaceOf(vertical)` above and below and by `Theme.spaceOf(horizontal)`
  at the sides. Its implicit size adds both.
- **Terminal:**
  - A two-step padding maps each step by its axis (`layout::stepCells`): `[2, 4]` is 0 rows and 2 columns.
  - With the shell's spacers hidden, the page loses its 1 row and 2 columns. The pinned body's step-4 padding puts
    them back: 1 row above and below, 2 columns at the sides.
  - So a pinned page reads as it does today, with three differences:
    - a top bar sits directly under the shell's top bar, not one row below it;
    - the body ends one row above the bottom bar;
    - review's queue pane gains 1 row and 2 columns of padding, and the worksheet pane gains 2 columns.
  - The bars have no rule and no inverse on the terminal (`bar` is not inverted), so their width shows only through
    where their content starts, which is unchanged.
- **`RecordingBackend`:** records `padding=2/4` for a two-step padding and `padding=3` for one step, as today.

**Spec.**

- `document.md`:
  - "The document": the `pageInset` row;
  - the `column, row` row: the two-step padding;
  - "A bar that stays in view": `pinned` insets its body and its bars' sides by the page's step, and a screen whose
    root is pinned declares `pageInset: false`.
- `view_tree.md`, Column and Row (`Padding`); `backend_contract.md`, `StackWidget::setPadding(Padding)`.
- `author.md`: `pinned` (`inset`), `padding(…)`, `Scope::pageInset`.
- `host.md`, the head's members; `workspace.md`, `activePageInset`.
- `shell_frame.md`:
  - the Shape table's `shell_page` row: the inset spacers hide while `setPageInset(false)`;
  - the Design decisions row "The padding": "Zero-sized spacers a gap away, hidden for a screen that insets its own
    page".
- `qt_quick/backend.md` and `tui/frontend.md`: the Column, Row and Stack rows.

**Tests.**

- `tests/test_ui_author.cpp`:
  - `pinned(content, {.bottom = …})` writes the scroll's content in a column with padding 4, and the bar with
    padding `[2, 4]`.
  - `.inset = 0` writes the bar with padding 2 and no wrapping column.
  - **Control:** today's helper writes neither, and both checks fail.
- **Layout case** "a stack's two-step padding insets its ends and its sides apart": a column with padding `[0, 4]`
  holding a text. The text's top equals the column's, and its left is one step-4 inset in.
  - **Control:** a backend that applies the first step to every side puts the text's left at the column's.
- Corpus (`tests/ui/corpus/`, new files):
  - `padding: [2, 4]` on a column loads;
  - `[2]`, `[2, 4, 1]`, `[2, 9]` and `[2, 4]` on a panel are refused;
  - `pageInset: false` loads on a screen and is refused on the app document.
- `tests/test_ui_workspace.cpp`: `activePageInset()` follows the active tab, is `true` with no tab, and is `true` for a
  host screen whose head gives none.
  - **Control:** answer `true` always, and the first check fails.
- `tests/test_ui_shell_frame.cpp` and `tests/tui/test_tui_shell_frame.cpp`: after `setPageInset(false)` the three
  spacers are hidden, and on the terminal the page's content starts at the page's first row and column.
  - **Control:** a no-op `setPageInset`.
- `tests/client/test_shell.cpp`: opening a screen whose `pageInset` is false hides the frame's inset, and switching
  to one that does not declare it shows it again.
  - **Control:** drop the effect.
- `tests/qt_quick/test_qt_quick_containers.cpp`, at 390 px: a `ShellFrame` whose page holds a `pinned` with a bottom
  bar, with the inset hidden. The bar's surface `x` is the page's, its width is the page's, and its first child's
  `x` is `Theme.spaceOf(4)`.
  - **Control:** keep the inset, and the bar's `x` is 16.

**Gallery** (H-S): `screen.pageInset(false)` on receipt, samples, stability, results and review. Review's queue pane
takes `padding: 3`, and its worksheet column drops `padding: 4`.

**Verify.** Re-grab the five screens at both sizes. At 390 px each bottom bar's white run is x = 0 to 389 with a rule
across its top, and its first button starts at x = 16. At 1000 px review's queue surface reaches x = 0 and the
window's bottom. Compare with the compact halves of `02`, `05`, `06`, `08` and `09`, and with `09`'s expanded card.

**Risk.** A screen that declares `pageInset: false` without a pinned root, or without padding of its own, draws
against the window's edge. That is the screen's choice, and the member says so in its name.

## H2: a square button for a glyph or an icon

**Problem (reproduced, probe 1; read).**

- **The width.** Audit's, samples' and console's pagers are push buttons labelled `‹` and `›`
  (`audit.cpp:362-367`, `samples.cpp:571-578`, `console.cpp:739-744`). Each is 61×32 px in the grabs:
  - `MorphButtonBackground` gives every framed button a least width of 8 units;
  - `MorphButton` pads a push button by `space.4` on each side.
- **The glyph.** It is drawn at the body size and regular weight: about 4 px of ink.
- **The design.** 30×30 squares with a 13 px/500 glyph (`04`, `05`, and `10`'s `style="width:30px;padding:0"`).
- **The `⋯` triggers.** The head's own menu (`workspace.hpp`, `buildHeadTrail`, icon `more`) and samples' compact bulk
  menu are button-style menus padded by `space.4`, 62 px wide in the grabs. The designs draw them square: 30 px
  beside instrument's head actions (`03`), and 44 px on compact (`03`, `05`).
- **Results.** Results' compact pager is "‹ Previous" and "Next ›", each half the width (`08`, `.btn.lg; flex:1`),
  which the screen already draws. It is not a square button, and it does not change.

**Decision.** A third `ControlShape`, shared as G8's `round` is.

- **`ControlShape::Square`**, document name `square`, on `button`, `badge` and a button-style `menu`:
  - the corners are the theme's (`radius.control` for a button and a trigger, `radius.badge` for a badge);
  - there is no least width and no side padding beyond `space.1`;
  - the node is at least as wide as it is tall. A glyph or an icon alone gives a square, at the control height on
    expanded and the touch height on compact (32 and 44 px at the default scale). A longer caption gives a wider
    node, as `round`'s stadium does.
- **The caption.** A square button's or trigger's caption is drawn at `Theme.headingPixels` in `Theme.strongWeight`.
  A square holds a glyph, and a glyph at the body size reads as a speck (probe 1: 3×7 against 4×8 px). This is the
  same rule as G8's dense button: the size is a type step, and the weight is the strong weight. An icon from the
  theme's set is drawn at its icon size, as now.
- **`menu` takes `shape`** (a literal name) with `style: button`. On a bar-style menu it is refused, since a bar has
  no trigger. `MenuTrigger` gains `ControlShape shape = ControlShape::Default`, so the mount passes it through
  `setTrigger` and no new virtual is needed.
- **The framework's own icon triggers are square:**
  - the head's menu (`more`, in `buildHeadTrail`);
  - the compact shell's navigation menu (`nav.menu`, ghost). A square ghost trigger has no frame, so for it the
    shape only removes the padding.

**Vocabulary.**

- `kControlShapes` gains `{"square", ControlShape::Square}` and grows from 2 entries to 3.
- `kMenuKeys` gains `shape`.
- Author:
  - `ButtonOptions::shape` and `BadgeOptions::shape` take `Square`;
  - `MenuButtonOptions` gains `shape`;
  - a pager writes `button({.label = "‹", .shape = ControlShape::Square, …}).a11y("Previous page")`.

**Per backend.**

- **Qt Quick:**
  - `MorphWidget` gains `square` beside `round`, set by `setShape`;
  - `MorphButtonBackground` drops the least width for a square button, as for a compact one;
  - `MorphButton` and `MorphMenu`'s trigger set `implicitWidth` to at least `implicitHeight`, pad by `space.1`, and
    draw the caption as decided;
  - `MorphBadge` gives a square badge `radiusBadge` corners and a width of at least its height.
- **Terminal:** a square button or trigger drops the push button's inner spaces, as `small` does: `[‹]`, `[⋯]`. A
  square badge draws as now. A cell has no corners.
- **`RecordingBackend`:** records `shape=square` on a button and a badge, and `trigger … shape=square` on a menu.

**Spec.**

- `control_shape.hpp`'s doc comments;
- `document.md`, the `button`, `badge` and `menu` rows;
- `backend_contract.md`, the `ButtonWidget` and `BadgeWidget` rows and `MenuWidget::setTrigger`;
- `author.md`, `menuButton`;
- `workspace.md`, the head's menu trigger; `shell_frame.md`, `shell_nav_menu`;
- `qt_quick/backend.md` (Button, Menu, Badge) and `tui/frontend.md` (Button, Menu).

**Tests.**

- **Layout case** "a square push button is narrower than a normal one with the same label", on both frontends: `‹`
  in each.
  - **Control:** a backend that ignores `shape`.
- `tests/qt_quick/test_qt_quick_widgets.cpp`:
  - A square `‹` button's width equals its height equals `Theme.targetHeight`, its background radius is
    `Theme.radiusControl`, and its caption's font is `Theme.headingPixels` at `Theme.strongWeight`.
    - **Control:** draw `square` as `default`, and the width is 8 units.
  - A square button-style menu with icon `more` is as wide as it is tall.
    - **Control:** ignore the trigger's shape.
- `tests/tui/test_tui_widgets.cpp`: a square `‹` draws `[‹]`.
  - **Control:** `[ ‹ ]`.
- Corpus:
  - `shape: square` loads on a button, a badge and a button-style menu;
  - `shape` on a bar-style menu is refused.
- `tests/test_ui_workspace.cpp` (H-S): the head menu's recorded trigger has `shape=square`.
  - **Control:** the default trigger.
- `tests/test_ui_shell_frame.cpp` (H-S): `shell_nav_menu`'s trigger is square.

**Gallery.**

- Audit and console (H-C): the pager buttons take `.shape = ControlShape::Square`.
- Samples (H-S): its pager and its compact bulk `menuButton` take `Square`.
- Instrument's `⋯` is the head's menu, so it changes with the workspace.

**Verify.** Re-grab audit at 1000×830, samples and console at 1000×830, and instrument and samples at 390×844. The
pager buttons are 32×32 with a legible glyph, and the `⋯` triggers are 32 or 44 px squares. Compare with `04`, `05`,
`10` and `03`.

**Residual.** 32 px against the design's 30, as G8 sizes every control by the control height.

## H3: a selected card that holds a check box

**Problem (read; the grabs show it).**

- **The design.** The design's compact samples card (`05` compact) is a plain card with a 22 px check box, and a
  checked card draws a 3 px accent bar at its leading edge (`box-shadow: inset 3px 0 0 #17784A`).
- **The card has no click.** Only the check box is interactive.
- **The marks compose.** The first card is both out of specification and selected, so it shows the `#FFF5F5` tint and
  the bar together.
- **Today.** Samples' card is a padded `column` on a conditional `outOfSpec` surface, holding the check box
  (`samples.cpp:594-615`). Nothing in ui/1 draws the selection's bar on it:
  - The bar is the selected **item button**'s (`MorphButtonBackground`).
  - An item button refuses a check box among its children: "an entry's content is read-only; put the control beside
    the entry" (`document.md`, the `button` row).

**The alternatives weighed.**

1. **An item button that may hold a control.** A button containing a check box is a nested interactive control.
   Assistive technology reads the button as one leaf named by its label, so the box inside is unreachable or
   announced twice. A click on the box would also be a click on the button. This is why the refusal exists, and it
   stays.
2. **A selectable list row that is not a button** (a focusable `ListItem` whose content may hold controls). ARIA's
   listbox forbids interactive children, and a grid row that may hold them needs cell navigation, a row selection
   model and focus management across rows. That is a new list kind for one card.
3. **A click on the card's non-control area.** This needs a pointer target that is not reachable by the keyboard,
   which ui/1 has never had (every click has a key). The design draws none: the check box is the card's one target,
   at the touch size on compact.
4. **The selection's mark as a stack property.** The card stays a plain container. The check box keeps the state,
   the focus and the name, and the stack draws the mark.

**Decision: 4.**

- **`selected` on `column` and `row`**, a condition. While true, the stack draws the accent's 3 px bar at its leading
  edge, down its whole height, over its surface and under its children. This is the bar a selected item button and a
  selected table row draw (G6's `selectionBar`), in the same colour.
- **No fill.** The stack's `surface` stays its fill, so a tint and the bar compose, as the design's first card does.
  A screen that wants the selection's fill names a surface by condition, which it can already do. The key means what
  it means on a button ("this is the chosen entry") and draws the part of the mark a stack does not already have.
- **Room for the bar.** It lies inside the stack's leading padding. A stack with `selected` and no leading padding
  draws it over its first 3 px. The spec says to give such a stack padding; the frontends do not move the content.
- **Accessibility.** While the key is given, the stack's item has `Accessible.selectable: true`, and
  `Accessible.selected` follows the condition (probe 2). Its role stays what it is; a named stack is a group. The
  check box inside keeps its own role, name and state, so assistive technology meets one control per card and no
  nesting.
- **No click.** Alternative 3 is not taken (Decisions).

**Vocabulary.**

- `kContainerKeys` gains `selected` (a condition), on `column` and `row`. It is refused on `grid`.
- In the view tree, `Column` and `Row` gain `std::optional<Prop<bool>> selected`, appended after their existing
  members so every designated initializer keeps its order on MSVC.
- `StackWidget::setSelected(bool)` is pure. The mount calls it only for a stack that gives the key, at mount and on
  every change.
- Author: `ContainerOptions::selected` (an `AnyHandle`; empty writes no key).

**Per backend.**

- **Qt Quick:** `MorphStack` gains `selectable` and `selected`, and a `Rectangle` `selectionBar`, 3 px wide, in
  `scheme.accent`, at x = 0, mirrored under right-to-left layout.
- **Terminal:**
  - A stack that takes `selected` reserves its first column, inside its padding when its horizontal padding gives
    one, and adds one otherwise.
  - While selected, every row of that column draws `▌` in the list's selected-item style.
  - The column is reserved whether or not the stack is selected, so the content does not move when the selection
    changes.
- **`RecordingBackend`:** records `selected=true` or `selected=false`.

**Spec.**

- `document.md`, the `column, row` row;
- `view_tree.md`, Column and Row;
- `backend_contract.md`, `StackWidget`;
- `author.md`, containers;
- `qt_quick/backend.md` and `tui/frontend.md`, the stack rows.

**Tests.**

- `tests/qt_quick/test_qt_quick_containers.cpp`:
  - A padded column with `selected: true` on a tinted surface shows a visible `selectionBar` 3 px wide, its colour
    `scheme.accent`, at x = 0. A pixel just right of the bar reads the surface's colour.
  - Its `Accessible.selected` is true.
  - With `selected: false` the bar is hidden.
  - **Control:** a stack that ignores the key draws no bar.
- `tests/tui/test_tui_containers.cpp`: a selected column of two text rows draws `▌` in column 0 of both rows. An
  unselected column with the key keeps its text in the same column as the selected one.
  - **Control:** reserve the column only while selected, and the text moves.
- `tests/test_ui_mount.cpp`: the condition reaches `setSelected` on change, and a stack without the key is never told.
- Corpus: `selected` loads on `column` and `row`, and is refused on `grid`.

**Gallery** (samples, H-S). The card (`sampleCard`) takes `.selected = ui::includes(list.selected, ui::row("id"))`,
the condition its check box already reads. Its padding stays step 2 (8 px), which holds the 3 px bar. The card's check
box, its `outOfSpec` surface and the divided list stay as they are.

**Verify.** Re-grab samples at 390×844. The checked cards show the green bar at their leading edge, and the first
also keeps its tint. Compare with `05` compact.

## H4: runs in the head's title

**Problem (read; the grabs show it).** Plate's title is "Plate PL-0193" with the ID in mono (`07`: `Plate
<span class="mono">PL-0193</span>`, both halves 16 px/600). The title is the document's `title` expression, which is
also the tab's caption, the navigator's entry and the window's title, all plain text. The shell draws it with
`ShellFrame::setTitle(string)`. G9 left this to the shell head, which is now the shell's top bar.

**Decision.** Mirror `subtitleSpans`:

- **The screen member `titleSpans`** is an expression of runs, as a `text`'s `spans`. A shell that knows it draws the
  runs in place of `title` in its top bar. `title` stays the caption, the navigator entry, the window title and what
  an older shell draws.
- The runs are not compared with `title`, as `subtitleSpans` is not compared with `subtitle`.
- `ScreenHead::titleSpans()` is a virtual with a default that answers none, so a host screen's head compiles
  unchanged.
- `Workspace::activeTitleSpans()` is a tracked read.
- `ShellFrame::setTitleSpans(std::vector<TextSpan> const&)` sets the title text's runs (`TextWidget::setSpans`);
  empty draws `setTitle`'s text.
- The client sets it in the same effect that follows the active tab.
- The title's role (`heading`) gives every run its size and weight, so a mono run is the mono face at the heading
  size, and the accessible name is the runs' joined text.

**Per backend.** Nothing new: the top bar's title is a text widget, and both frontends draw runs (G9). On the
terminal a mono run is drawn as any run.

**Spec.** `document.md` ("The document", "The head"); `host.md`; `workspace.md`; `shell_frame.md` (`setTitleSpans`);
`author.md` (`Scope::titleSpans`).

**Tests.**

- Corpus: `titleSpans` loads on a screen and is refused on the app document. A run the head cannot read is reported
  once, as for `subtitleSpans`.
- `tests/test_ui_workspace.cpp`: `activeTitleSpans()` follows the active tab.
- `tests/test_ui_shell_frame.cpp`: after `setTitleSpans({"Plate ", mono "PL-0193"})`, `shell_title`'s recorded runs
  are those two. After `setTitleSpans({})` it draws the plain title again.
  - **Control:** ignore the runs.
- `tests/client/test_shell.cpp`: the top bar's title follows a screen's `titleSpans`.
  - **Control:** the client never calls `setTitleSpans`.

**Gallery** (plate, H-S): `screen.titleSpans(ui::spans({"Plate ", ui::mono("PL-0193")}))` beside its `title`.

**Verify.** Re-grab plate at both sizes, and compare the title with `07`'s.

## H5: an eyebrow line, and labels in small capitals

### The eyebrow

**Problem (read; the grabs show it).** Instrument's mockup puts "Instruments › EQ-0142" above the title, with
the badges under the title (`03` expanded, `<p class="muted">` before the `h1`; compact "EQ-0142" likewise).
Results puts "Worksheet WS-2026-0412 · HPLC-02 · method TM-ASSAY-07 v3" above its title (`08`). Stability's compact
head puts "Step 2 of 4 · Paracetamol 500 mg" above its title (`06`). The head has one text line, `subtitle`, and the
shell draws it under the title, beside the badges. So each screen either puts the line under the title (instrument,
results) or in its own content (stability's top bar).

**Decision.** A second head line, placed above the title:

- **The screen members `eyebrow`** (an expression) **and `eyebrowSpans`** (runs drawn in place of `eyebrow`), named
  and shaped as `subtitle` and `subtitleSpans` are, so one rule covers both lines. Both are a screen's only.
- **The shell draws it above the title**, muted, in the lead block. `HeadParts` gains `eyebrow`, a muted text node
  (`head_eyebrow`) that is hidden while the eyebrow is empty. `ShellFrame::headEyebrowSlot()` is a container at the
  top of the lead block, above `shell_title`, shown with the other slots by `setHeadShown`. The client mounts the
  part there.
- **`headHasContent` counts the eyebrow**, so a head with only an eyebrow shows.
- **In the page** (`HeadPlacement::InPage`), the eyebrow text is a line above the head row, mounted with it. The tab
  bar carries the title there.

**Per backend.** Nothing new. On the terminal the eyebrow is one more muted row above the title in the top bar.

### Labels in small capitals

**Problem (read; the grabs show it).** Audit's facet labels are `.fh{font-size:11px; font-weight:600;
letter-spacing:.04em; text-transform:uppercase; color:#4D5461}` (`04`). The console's sidebar group headings are the
same look (`.navh`, `.06em`, `10`). Audit writes "RECORD TYPE" in capitals in a `section` text (`audit.cpp:256`),
which draws at the body size in bold: 13 px against 11. The navigator draws its group headings as `section`
(`navigator.hpp:406`). No role draws the small, letter-spaced capitals, and `section`'s other uses are strong
body-size lines ("System suitability", "6 wells selected"), which the designs draw that way, so `section` cannot
change.

**Decision.** A text role, **`overline`**: a short label over a group.

- **Qt Quick:**
  - the small step (`Theme.smallPixels`) in the strong weight (`Theme.smallStrongFont`);
  - `scheme.muted`;
  - `font.capitalization: Font.AllUppercase`;
  - `font.letterSpacing` of `0.05 × Theme.smallPixels`, between the designs' `.04em` and `.06em`, as a rule rather
    than a token.
- **The text is written as it reads**, "Record type", and the frontend draws the capitals. The accessible name stays
  as written (probe 2), so a screen reader does not spell a capitalised word out letter by letter.
- **`TextRole::Overline`**, document name `overline`. `roleNamed`, the role names `document.hpp` lists, and a table
  column's `role` take it.
- **The navigator's group headings take `overline`**, which is the design's sidebar.

**Per backend.**

- **Qt Quick:** as decided, in `MorphText` (role 9) and in `textColorOf`.
- **Terminal:** drawn as `section` is, bold in the muted style, **as written**. Uppercasing beyond ASCII needs case
  data the terminal frontend does not carry, and the capitals are a look.
- **`RecordingBackend`:** records `role=overline`.

**Spec.**

- `document.md`:
  - "The document" and "The head" (`eyebrow`, `eyebrowSpans`);
  - the `text` row (`overline`);
- `view_tree.md`, `TextRole`;
- `host.md`; `workspace.md` (the eyebrow part, the in-page line);
- `shell_frame.md`:
  - `headEyebrowSlot`;
  - the navigator's headings: "its group headings are overlines and its entries body text";
- `author.md`, `Scope::eyebrow`, `eyebrowSpans`;
- `qt_quick/backend.md` and `tui/frontend.md`, the Text rows.

**Tests.**

- `tests/qt_quick/test_qt_quick_widgets.cpp`: an `overline` text "Record type":
  - its label's font has `Theme.smallPixels`, `Theme.strongWeight`, `Font.AllUppercase` and a letter spacing above
    0;
  - its colour is `scheme.muted`;
  - its `Accessible.name` is "Record type".
  - **Control:** draw it as `section`, and the size check fails.
- `tests/tui/test_tui_display.cpp`: an `overline` text draws "Record type" in bold muted cells.
  - **Control:** draw it as `normal`.
- Corpus:
  - `role: overline` loads;
  - `eyebrow` and `eyebrowSpans` load on a screen and are refused on the app document.
- `tests/test_ui_workspace.cpp`:
  - `headPartsForActive().eyebrow` shows the eyebrow's runs, and is hidden while there is none;
  - a head with only an eyebrow has content;
  - the in-page head shows `head_eyebrow` above `head`.
  - **Control:** leave the eyebrow out of `headHasContent`.
- `tests/test_ui_shell_frame.cpp`: the lead block's children are, in order, the eyebrow slot, `shell_title` and
  `shell_head_lead`.
  - **Control:** the slot after the title.
- `tests/test_ui_shell_frame.cpp` (or a navigator test): a group heading is recorded with `role=overline`.
  - **Control:** `section`.

**Gallery** (H-S, except audit's labels, which H-C writes):

- **Audit:** the facet labels become `ui::text({.text = "Record type", .role = "overline"})` and "Action".
- **Instrument:** `eyebrowSpans` takes what `subtitleSpans` holds today ("Instruments › " with a mono `EQ-0142`, or
  the mono ID alone on compact), and `subtitle` and `subtitleSpans` go. The badges stay under the title.
- **Results:** the worksheet line moves from `subtitle` and `subtitleSpans` to `eyebrow` and `eyebrowSpans`, and
  "In progress" stays a badge.
- **Stability compact:** "Step 2 of 4 · Paracetamol 500 mg" moves from the pinned top bar to `eyebrow`, and the top
  bar keeps the indicator.

**Verify.** Re-grab instrument and results at both sizes, stability at 390×844, and audit at 1000×830. The eyebrow
sits above the title, and the facet labels are small grey capitals. Compare with `03`, `08`, `06` compact and `04`.

**Residuals.**

- The label's colour is the gallery's `muted` (`#5F6673`), against the design's `#4D5461`.
- The weight is the strong weight (700), against the design's 600, as G decided for every strong text.

## H6: the shell head's lead takes its natural width when that is less

**Problem (reproduced, probe 3; cause read).**

- **The spec.** `shell_frame.md` and `workspace.md` say the trail drops to its own line "where it does not fit beside
  the lead block's 32 units (or its natural width where that is less)". The console's compact head drops Scan below
  405 px, where its lead's natural width is about 75 px.
- **The cause.**
  - `headPartsForActive()` builds the lead with `buildHeadLead`, which gives the lead row `minWidth =
    kHeadLeadMinimum`. That minimum is the in-page head's rule.
  - In the shell, that row sits in `_leadBlock`, a column that already has the 32-unit minimum. A column's natural
    width is at least its children's minimum widths, so the block's natural width becomes 32 units whenever the lead
    row shows.
  - The wrapping line then takes `min(32 units, natural)`, which is 32 units (`MorphStack.naturalBasisOf`).
  - Samples keeps one line only because its head has no lead.

**Decision.** The lead built for a shell has no minimum of its own. `headPartsForActive()` clears the lead row's
`minWidth`, and the shell's lead block keeps the one 32-unit minimum, capped at its natural width as specified. The
in-page head row keeps its lead's minimum, which is the rule there.

**Per backend.** Both frontends lay the shell out from the same widgets, so both change.

**Spec.** `workspace.md`, `headPartsForActive`: "the lead has no minimum width of its own; the host places it".

**Tests.**

- `tests/test_ui_workspace.cpp`: `headPartsForActive()`'s lead is recorded with layout `stretch(1)/content`, with no
  `min=32`. The in-page head's lead keeps `min=32`.
  - **Control:** restore the minimum, and the first check fails.
- `tests/qt_quick/test_qt_quick_containers.cpp`: a `ShellFrame` 390 px wide, compact. Its head lead holds one badge
  and its trail one button labelled "Scan". The button's `y` is the title's line's `y`.
  - **Control:** the minimum restored puts the button on a second line.

**Gallery.** None.

**Verify.** Re-grab console at 390×844. ☰, the title over "Connected", and Scan sit on one line, as in `10` compact.

## H7: a pinned bar with nothing shown takes no room

**Problem (read, not run).**

- **What `pinned` does.** It leaves out a bar whose node list is empty when the screen is built. A bar whose nodes
  are all hidden at run time is still a column with step-2 padding on the `bar` surface, and the divided column
  draws a rule above it. That is an empty 16 px strip with a rule.
- **Where it shows.** Samples' compact bulk bar is shown only while a sample is selected (`compactBulkBar`,
  `.visible(list.selectedCount > 0)`), so with no selection the strip shows.
- **Why it is not run.** The grabs open samples with three samples selected, and a grab cannot clear them. Nothing
  here was run with the selection empty.

**Decision.** `pinned` gives each bar a `visible` condition derived from its nodes, when every node in the list
carries one: the bar is visible while any of them is. A list with a node that has no `visible` condition gives the
bar none, and the bar always shows, as today. The helper reads the nodes' JSON it already holds. No backend changes,
and the divided column draws no rule beside a hidden child.

**Spec.** `author.md`, `pinned`.

**Tests.** `tests/test_ui_author.cpp`:

- A bottom list of one node with `.visible(c)` writes the bar column with `visible` equal to `c`.
- Two nodes with conditions `a` and `b` write `a || b`.
- A list with an unconditioned node writes no `visible`.
- **Control:** today's helper writes no `visible`.

**Gallery** (samples, H-S): see Screen fixes.

## Screen fixes

Each lands in the lane that owns the screen, after the items it uses.

| Fix | How | Owner |
|---|---|---|
| The five pinned screens bleed (H1) | `screen.pageInset(false)` in receipt, samples, stability, results and review | H-S |
| Review's panes inset themselves (H1) | The queue pane takes `padding: 3`. The worksheet column inside `pinned` drops `padding: 4`, since `pinned` now insets it | H-S |
| Samples' compact bulk bar is a bar, not a tinted panel inside one | `compactBulkBar` returns its row with `.visible(selectedCount > 0)`, without the `selected`-surface panel. The bar is white with a rule, as `05` compact draws it, and H7 hides it with no selection | H-S |
| Pagers (H2) | `.shape = ControlShape::Square` on `‹` and `›`: audit and console (H-C), samples (H-S). Samples' compact bulk `⋯` also takes `Square` | H-C, H-S |
| Samples' selected cards (H3) | `sampleCard` passes the box's condition as `.selected` | H-S |
| Plate's mono title (H4) | `titleSpans` | H-S |
| Eyebrows (H5) | Instrument and results move their line to `eyebrow` and `eyebrowSpans`; stability compact moves its step line there | H-S |
| Audit's facet labels (H5) | `role: "overline"`, written "Record type" and "Action" | H-C |

## Totals

**New document keys and members:**

- the screen's `pageInset`, `titleSpans`, `eyebrow` and `eyebrowSpans`;
- `column` and `row`: `selected`, and `padding` as `[vertical, horizontal]`;
- `menu`: `shape`;
- the shape name `square` on `button`, `badge` and `menu`;
- the text role `overline`.

**New or changed pure virtuals**, each with four implementations (Qt Quick, terminal, `RecordingBackend`,
`EchoingBackend`):

- `StackWidget::setSelected(bool)`, new;
- `StackWidget::setPadding(Padding)`, which replaces `setPadding(int)`.

**Other API:**

- `ControlShape::Square`; `MenuTrigger::shape`; `TextRole::Overline`; `ui::Padding`.
- `ScreenHead::pageInset()`, `titleSpans()`, `eyebrow()` and `eyebrowSpans()`, each with a default.
- `Workspace`: `activePageInset()`, `activeTitleSpans()`, and `HeadParts::eyebrow`.
- `ShellFrame`: `setPageInset(bool)`, `setTitleSpans(…)`, `headEyebrowSlot()`.
- Author:
  - `PinnedOptions::inset`;
  - `ContainerOptions::selected`;
  - `ui::padding(int, int)`;
  - `MenuButtonOptions::shape`;
  - `Scope::pageInset`, `titleSpans`, `eyebrow`, `eyebrowSpans`.

**Conformance:** two new layout cases, H1's two-step padding and H2's square button.

**Toolchain notes for both lanes.**

- New members go at the end of the view-tree structs, so designated initializers keep their order on MSVC.
- No parameter is named after a member function it sits beside (GCC 15 `-Wshadow`). For example,
  `setPageInset(bool inset)` stays clear of the frame's members.
- Every QML property used here exists on Qt 6.8, and `qmllint` runs over the changed files.

## Not in this batch

- **A click on a card's non-control area** (H3, alternative 3). ui/1 has no pointer target without a key, and the
  design draws none.
- **A selectable list row that may hold controls** (H3, alternative 2). That is a grid-like kind with cell
  navigation, and nothing in the designs needs it.
- **The bars' 12 px vertical inset.** Bars keep step 2 (8 px), for G3's reason: step 3 costs a terminal row above
  and below every bar.
- **The rules' colours.** The mockups use `#E1E5EA`, `#EDF0F3`, and samples' green `#BCDFC9` above its bulk bar. The
  theme has one divider. That is a theme value, not a vocabulary gap, as G3 found.
- **30 px controls.** Controls are sized by the control height (32 px), as G8 decided.
- **The mono face.** JetBrains Mono is not installed on this machine, which makes it environmental.
- **Comparing `titleSpans` with `title`.** Neither head line's runs are checked against their plain text.

## Decisions

Taken on 2026-10-30, at this design's defaults:

- **H1's mechanism.** The opt-out is a screen member, `pageInset` (a condition, default `true`), not a node key.
  `pinned` insets its body and its bars' sides by step 4 by default, and `.inset = 0` keeps today's helper.
- **The two-step padding** is `padding: [vertical, horizontal]` on stacks only, not a new key.
- **A square caption** is drawn at the heading size in the strong weight.
- **The framework's icon triggers are square:** the head's `⋯` and the compact shell's ☰.
- **H3's `selected` on a stack** draws the bar only. It has no fill and no click, and the terminal reserves one
  column for it.
- **H5.** The eyebrow mirrors `subtitle` (`eyebrow`, `eyebrowSpans`). The capitals are a role named `overline`. The
  terminal draws it as written, and the navigator's group headings take it.
- **Results' pager stays labelled.** The design draws "‹ Previous" and "Next ›".

## Defects this batch fixes

- **The shell head's lead ignores its natural-width cap** (H6). Reproduced at 390 to 405 px; the cause is read.
- **A pinned bar whose nodes are all hidden draws an empty strip** (H7). Read, not run.

Neither is filed, since each is fixed by the change that touches it.

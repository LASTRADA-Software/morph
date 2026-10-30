# LIMS gallery batch H: page edges, glyph buttons, selected cards and head text

Batch H of the LIMS gallery visual review (`examples/lims_gallery/design/visual-review.md`, "After batch G"). It
closes every item that list leaves for later:

- H1, a pinned bar that bleeds to the page's edge;
- H2, a square, glyph-sized button;
- H3, a control inside a selected entry;
- H4, runs in a head's title;
- H5, an eyebrow line above a head's title, and section labels in small capitals.

It adds three framework gaps that a fresh comparison and the design's review found, none of which a screen can fix:

- H6, the shell head's lead keeps a 32-unit minimum it should cap at its natural width;
- H7, a pinned bar whose nodes are all hidden still draws an empty strip;
- H8, instrument's tabs sit in the head's white block in the mockup, and in the grey page in the grab.

The aim is the review's: ui/1 itself produces each mockup's look. A difference is a vocabulary or frontend change,
and a screen changes only where the vocabulary already says what the design says. The rules of batch G hold here:

- A look is a token or a rule, never a screen's choice of pixels.
- A key means the same thing on every kind that takes it.
- The strong weight is the theme's one emphasis weight, never a literal 500 or 600.
- The terminal keeps its cells. A purely visual change says what it leaves unchanged there, and a change to what a
  user can read or reach is drawn on both frontends.
- **A look that changes every application's shell is the theme's to switch.** The square icon triggers and the
  small-capital group headings are off in the default theme and on in the gallery's (Theme switches).

Every claim below was checked against `c4df1b3e9` (`lg-h-design`, built as `build/linux-everything` from that
revision before grabbing). Each item says whether its premise was **reproduced** (run or measured here) or **read**
(inferred from the code). Grabs are `morph_examples_client --app lims-gallery --screen lims-gallery.<id>
--grab-size WxH --grab FILE`; mockups are the `design/mockups/*.dc.html` files rendered in headless Firefox
(`lg-cmp/mock/*.png`). Pixel figures for the mockups are read from their CSS where it gives them and measured in the
renders otherwise.

## Probes

Four throwaway probes were run with `qml6` (Qt 6.12.0, offscreen) and kept out of the tree (`lg-hd-probe1.qml` to
`lg-hd-probe4.qml` in the session scratchpad). Qt 6.8 itself was not run; every property below predates Qt 6.

**1. A pager glyph's size.** `TextMetrics` on `‹` and `⋯` in the gallery's sans face:

```
PROBE ‹ 13px/400 tight 3.0x7.0 advance 4.3
PROBE ‹ 13px/700 tight 4.0x7.0 advance 4.3
PROBE ‹ 16px/700 tight 4.0x8.0 advance 5.3
PROBE ⋯ 13px/400 tight 11.0x5.0 advance 13.0
PROBE ⋯ 16px/700 tight 14.0x6.0 advance 16.0
```

In the audit grab at 1000×830 the pager's `‹` has a dark core (every channel below 120) of 2×3 px in a 61×32 px
button. The mockup's has a 3×6 px core in a 31×30 px button, drawn 13 px/500 (`04-Audit.dc.html`, `.btn{font:500
13px Inter}`). The strong weight at the body size widens the stroke from 3 to 4 px without changing the size, which is
what the design's medium weight does.

**2. Capitals, letter spacing and a selected group.**

```
PROBE plain text=Record type w=63.7
PROBE caps text=Record type w=85.0 letterSpacing=0.546875 cap=1
PROBE a11y selectable=true selected=true
```

`font.capitalization: Font.AllUppercase` draws capitals and leaves `text`, and so the accessible name, as written.
`font.letterSpacing` takes a fractional pixel value and quantises it. `Accessible.selectable` and `selected` are
settable on a `Grouping` item.

**3. The console's compact head.** The console grabbed at a height of 300 and widths 400 to 420, reading the first
row of page colour under the top bar at x = 5:

```
400 page starts y 116
405 page starts y 60
410 page starts y 60
```

The top bar holds ☰, the title "Overview" over the "Connected" badge, and Scan. Below 405 px Scan drops to a second
line, at the width a 32-unit lead (about 248 px) gives: 12 + 32 + 12 + 248 + 12 + 82 + 12 px. The title and badge are
each about 75 px, so the line would fit from about 240 px if the lead took its natural width. The mockup's compact
console draws one line (`10-Console.png`).

**4. A selected client item, and a font built with `Qt.font`.**

```
PROBE client role=10 selectable=true selected=true name=''
PROBE group role=20 selected=true
PROBE Qt.font drops cap=0 spacing=0
PROBE Qt.font keeps cap=1 spacing=0.546875
```

- The selection states also read back on an item whose role is `Client` (10), the role every `MorphWidget` has by
  default. A probe cannot show what a screen reader does with them. An unnamed `Client` node is one that screen
  readers pass over, so H3 does not rely on it (H3, Accessibility).
- A font rebuilt with `Qt.font({family, pixelSize, weight})`, as `Theme.figuresOf` rebuilds one for tabular
  figures, loses `capitalization` and `letterSpacing`. Passing both keeps them. H5 relies on this.

## The items

| Item | Where it lands | Verdict on the review |
|---|---|---|
| H1 | `pageEdge` on stacks; `pinned` writes it and insets itself; a two-step `padding`; the shell follows the shown root | Right (reproduced: every compact bar's white run is x = 16 to 373 at 390 px) |
| H2 | `shape: square` on `button`; the theme switch `shape.iconOnly` for icon-only triggers | Right (reproduced, probe 1). **Wider than listed:** the head's `⋯`, the compact ☰ and samples' bulk `⋯` are 62 px too. **Narrower:** results' pager is two labelled buttons in the mockup, so it keeps them |
| H3 | `selected` on `column` and `row`: the accent bar, with no click and no fill | Right (read: the item button refuses a check box, `document.md`, the `button` row) |
| H4 | the screen's `titleSpans`; `ShellFrame::setTitleSpans` | Right. It is the shell head G9 deferred to |
| H5 | the screen's `eyebrow` and `eyebrowSpans`; the text role `overline` and the theme switch `font.overline.case`; the navigator's group headings | Right. The eyebrow is not in capitals in any mockup; the capitals are a separate role |
| H6 | the head lead built for a shell drops its own minimum | Found here (reproduced, probe 3) |
| H7 | `pinned` hides a bar while none of its nodes shows | Found here (read, not run) |
| H8 | `inset` and `stripSurface` on `tabs` | Found by the design's review (reproduced in the grabs) |

## Theme switches

Two looks change every application's shell or sidebar. Each is a token of the closed set, off by default, so an
application without the token looks as it does today. The gallery's bundle (`lims_gallery.cpp`) turns both on.

| Token | Values (default first) | What it switches | Read by |
|---|---|---|---|
| `shape.iconOnly` | `default`, `square` | A push button or a button-style menu whose caption is an icon alone (no label) and which gives no `shape` of its own is drawn square (H2). This covers the head's `⋯`, the compact shell's ☰ and samples' bulk `⋯` | `MorphButton`, `MorphMenu`'s trigger |
| `font.overline.case` | `none`, `uppercase` | The `overline` role (H5). With `none` it draws as `section` does: body size, strong, muted. With `uppercase` it draws at the small step, strong, muted, in capitals, letter-spaced by `0.05 × Theme.smallPixels` | `MorphText` (through `Theme.overlineFont`) |

- **A new token kind, `Keyword`.** A keyword names one of a list the token's spec gives. It is read into a new map,
  `ThemeValues::keywords`, by path. Its `$type` is `keyword`, a `ui-theme/1` type listed in `theme.md` beside the DTCG
  types: DTCG has no type for a switch, and `strokeStyle` names stroke dashes only. Any other value is refused at
  `addFile` (`shape.iconOnly: only default and square are drawn`), the vocal choice over drawing the default.
- **Read once for the whole theme, not per surface.** No design varies them by surface.
- **The framework sets nothing per application.**
  - The workspace and the shell keep building their triggers icon-only, and the navigator marks its headings as
    `overline`.
  - The frontend draws what the theme says.
  - So the default look is today's, and no screen or client code changes with the switch.
- **Terminal:** the terminal frontend reads no theme tokens, and both switches draw its default look there:
  - an icon-only trigger is `[ ⋯ ]`;
  - an overline is bold in the muted style, as written.

  An explicit `shape: square` still draws `[‹]` (H2).
- **Spec:** `theme.md`, the closed set and the On Qt Quick table, the new kind and the `keyword` type.
  `qt_quick/backend.md` names who reads each token.
- **Tests:**
  - `tests/test_ui_theme_values.cpp`: `shape.iconOnly: square` reads `square`, and `round` is refused at `addFile`.
    `font.overline.case: uppercase` reads `uppercase`.
    - **Control:** accept any string, and the refusal check fails.
  - `tests/qt_quick/test_qt_quick_theme.cpp`: the values reach `Theme.iconOnlySquare` and `Theme.overlineCaps`.
    - **Control:** leave the paths out of `kThemeTokens`, and both read their defaults.

## Parts and lanes

### Framework files

Two lanes, each in its own worktree, each owning the files it changes:

| Lane | Items, in order | Owns |
|---|---|---|
| H-C, controls, stacks and theme | the theme switches; H1's stack half (two-step `padding`, `pageEdge`, `pinned`); H7; H3; H2; H5's `overline`; H8 | `theme.hpp`; `control_shape.hpp`; `view.hpp`; `backend.hpp`; `mount.hpp`; `testing/recording_backend.hpp`; `testing/backend_conformance.hpp`; Qt Quick `Theme.qml`, `theme.cpp`, `MorphStack`, `MorphWidget`, `MorphButton`, `MorphButtonBackground`, `MorphMenu`, `MorphText`, `MorphTabs`, `kinds.hpp` (`StackImpl`, `TableRowImpl`, `TabsImpl`), `containers.cpp`, `leaves.cpp`, `fields.cpp`; terminal `container_widgets.*`, `leaf_widgets.*`, `list_widgets.*` (`TabsImpl`, the menu trigger) |
| H-S, shell and head | H1's page half (the shown root's `pageEdge`, the shell, the client, the workspace's tab strip); H6; H4; H5's eyebrow; then the navigator's headings | `shell_frame.hpp`; `workspace.hpp`; `host.hpp`; `navigator.hpp`; `src/client/shell.cpp` |

`tests/ui_echoing_backend.hpp` forwards only the `create…` calls, so neither lane changes it.

**Landing order: H-C, then H-S.** The lanes work in parallel. Each lands on `feat/lims-gallery` with one commit per
item, green on its own, and H-S rebases onto H-C before it lands. H-S uses three of H-C's additions:

- the `pageEdge` key that `pinned` writes (H1), which H-S's derivation reads;
- `TextRole::Overline`, for the navigator's headings;
- `pinned`'s inset, for the H1 Qt Quick test that needs both halves.

H-S writes the code that depends on them after its rebase.

### Shared files

Every file both lanes touch has a named region per lane. The later lane rebases onto the earlier lane's version.
Within a region, rows and cases are appended, never reordered.

| Shared file | H-C's region | H-S's region |
|---|---|---|
| `document.hpp` | the `KeySpec` arrays (`kContainerKeys`, `kTabsKeys`), the role names, and the node checks (padding, `pageEdge`, `selected`, `shape`) | the `Document` struct, its top-level loading (`titleSpans`, `eyebrow`, `eyebrowSpans`), the app document's refusal list, and their expression checks |
| `interpreter.hpp` | `roleNamed`, and the lowering of stacks, buttons and tabs | the screen's head evaluation (`titleSpans()`, `eyebrow()`, `eyebrowSpans()`) and `pageEdge()`, the shown root's flag |
| `author.hpp` | `ContainerOptions`, `padding(…)`, `PinnedOptions` and `pinned`, `TabsOptions` | `Scope`'s head methods |
| `document.md` | the View nodes rows (`column, row`, `button`, `tabs`, `text`), and "A bar that stays in view" | "The document" table and "The head" |
| `author.md` | containers, `pinned`, `tabs` | `Scope` |
| `tests/test_ui_document.cpp`, `tests/qt_quick/test_qt_quick_containers.cpp`, `tests/qt_quick/test_qt_quick_widgets.cpp` | new `TEST_CASE`s appended at the end | new `TEST_CASE`s appended after H-C's |
| `tests/ui/corpus/` | new files only | new files only |

H-S adds no pure virtual, so it does not touch `backend.hpp`, the recording backend or the conformance suite. The
shell draws its new parts with widgets every backend already has: a text's runs (`TextWidget::setSpans`), a stack,
and visibility.

### Screens

Each screen file, and its test under `examples/lims_gallery/tests/`, has one owning lane. The owner makes every edit
on it, after the items it uses have landed. The landing order guarantees that, because H-S lands last.

| File | Owner | Uses |
|---|---|---|
| `lims_gallery.cpp` (the theme bundle) | H-C | the theme switches |
| `audit.cpp` | H-C | H2 (the pager), H5 (`overline` facet labels) |
| `console.cpp` | H-C | H2 (the sample pager) |
| `receipt.cpp` | H-S | H1 (no screen code: `pinned` does it) |
| `samples.cpp` | H-S | H2 (the pager), H3 (the cards), H7 (the bulk bar) |
| `stability.cpp` | H-S | H5 (the compact step line as the eyebrow) |
| `results.cpp` | H-S | H5 (the worksheet line as the eyebrow) |
| `review.cpp` | H-S | H1 (`pageEdge` on the expanded root; the queue's head padded, its entries edge to edge) |
| `instrument.cpp` | H-S | H5 (the breadcrumb as the eyebrow), H8 (the tabs in the white block) |
| `plate.cpp` | H-S | H4 |

H-C verifies its items with uncommitted screen changes. H-S runs the final re-grab pass after both lanes have
landed.

## H1: a pinned page's bars span the page

**Problem (reproduced; read).**

- **The bars stop 16 px short.** At 390×844 the white run of each compact bottom bar is x = 16 to 373, on receipt,
  samples, stability, results and review. At 1000×830 stability's and review's bars run to x = 983. The mockups' bars
  span the window:
  - compact: `padding:12px 16px; border-top:1px solid #E1E5EA` across the whole 390 px (`02`, `06` and `09` compact);
  - review expanded: the worksheet pane's bar spans the pane (`09`, x = 327 to 1039 in a card whose edge is the
    window's), with its content 16 px in.
- **The cause (read).**
  - `ShellFrame` pads the page by spacing step 4, with zero-sized spacers above and either side of `_content` (the
    `_top`, `_left` and `_right` spacers). The workspace mounts every screen inside that inset.
  - `pinned` (`author.hpp`) is a helper that writes a divided column, so nothing marks a page as pinned. Neither the
    shell nor the workspace can tell a bar from the content.
- **Review's panes (reproduced).**
  - The splitter sits inside the page inset, so the queue pane's surface stops 16 px from the window's left, bottom
    and right edges.
  - The worksheet pane insets its content with its own `padding: 4` column inside `pinned`'s scroll.

**Decision.** The page inset moves to where it belongs on a pinned page: around the scrolling body and inside the
bars, not around the bars. A pinned page gets this with no key of its own. The node that draws the inset says so,
and the shell follows the node that is shown.

1. **`pageEdge`, a literal flag on `column` and `row`.** When the stack is the screen's shown root, the shell lays it
   at the page's edges and does not inset the page: the stack insets its own content. Anywhere else the flag means
   nothing; the loader does not refuse it there, since a component may be a root in one screen and a child in
   another.
2. **`pinned` writes `pageEdge: true` and insets itself.** `PinnedOptions::inset` is a spacing step, default 4, the
   shell's page inset:
   - the scroll's content sits in a `column` padded by `inset` on every side;
   - each bar is a `column` padded by step 2 at its top and bottom and by `inset` at its sides, so the bar's content
     lines up with the body's while its surface and its rule span the whole column;
   - `.inset = 0` writes the content and the bars as today (a bar padded by step 2 on every side) and no `pageEdge`,
     so the shell insets the page as before.

   A pinned page therefore looks like its mockup with no extra key, and its inset is drawn once: the shell drops its
   own exactly where `pinned` draws one. A pinned column inside a splitter pane is not the root. Its bars span the
   pane, and its body is inset by its own padding.
3. **The shown root, through switches.** The interpreter's `Screen::pageEdge()` is a tracked read:
   - it starts at the view's root;
   - while the node is a `switch`, it moves to the case shown now (the default when none matches), when that case
     holds exactly one node;
   - it answers that node's `pageEdge`.

   So `byWidth`'s `switch` and review's nested `switch` are seen through, and the answer follows a width change or a
   case change. Any other node, or a case of several nodes, is the root as it is.
4. **The shell follows it.**
   - `ScreenHead::pageEdge()` is a virtual returning `reactive::Computed<bool> const*`. Its default, `nullptr`, means
     inset, so a host screen's head compiles unchanged and keeps the inset. A closed screen keeps its last value.
   - `Workspace::activePageInset()` is a tracked read: `true` with no active tab, `true` while the workspace's tab
     strip shows (two tabs or more), and otherwise `!pageEdge`.
     - **The tab strip.** The strip is a child of the shell's page (`workspace.hpp`, `showTabs`, `setBarShown(captions
       .size() > 1)`). Without the inset it would touch the window's edges. A pinned page in a workspace of several
       tabs is therefore inset as today, bars included. The edge look is the one-tab look, which is how the gallery
       and the generic client open a screen.
   - `ShellFrame::setPageInset(bool)` shows or hides the three inset spacers. A hidden spacer takes no gap, so the
     content then fills the page. A new frame insets.
   - The generic client (`src/client/shell.cpp`) sets it from a `reactive::Effect` over `activePageInset()`, owned
     and reset with `_headShown`.
   - With `HeadPlacement::InPage` there is no shell inset, and `pageEdge` changes nothing.
5. **A stack's padding takes two steps.** `padding` on `column` and `row` is a spacing step (every side), or a
   constant list of two steps `[vertical, horizontal]` written as a `lit` constant, `{"lit": [2, 4]}`. A bare JSON
   array is not an expression (`expression.hpp`, "a constant list `{"lit": [...]}`").
   - The constant is checked at load: two integers, each 0 to 5. Any other `lit` list is refused.
   - A computed padding is a step, as today. One that evaluates to a list is reported once and taken as 0.
   - `panel`'s `padding` stays one step, and a list there is refused.
   - In the view tree, `Column::padding` and `Row::padding` become `Padding{int vertical; int horizontal;}`.
     - It has an implicit constructor from one `int` (both members) and `operator==`, so `.padding = 3` keeps
       compiling. Today `backend_conformance.hpp:1851` and `tests/test_ui_mount.cpp:74` write it so.
     - `view_tree.md` says so.
   - `StackWidget::setPadding(int)` becomes `setPadding(Padding)`. The mount still calls it only for a stack that has
     padding.
   - Author: `ui::padding(int vertical, int horizontal)` returns `AnyHandle{std::vector<std::int64_t>{vertical,
     horizontal}}`, which serialises as the `lit` constant, as review's splitter sizes do (`review.cpp:745`).
     `.padding = 2` keeps meaning every side.
   - **The four overrides of `StackWidget`:** Qt Quick's `StackImpl` and `TableRowImpl` (`kinds.hpp:601`), the
     terminal's `StackImpl`, and `RecordingBackend`. `TableRowImpl` takes `setPadding(Padding)` and `setSelected`
     (H3) as no-ops: the table places a row's cells, as it already ignores `setAlign` and `setWrap`.

**Every pinned screen, at both sizes.** No screen declares anything for H1 but review.

| Screen | Compact (390×844) | Expanded (1000×830) |
|---|---|---|
| receipt | bottom bar (Reject, Receive) spans x = 0 to 390, content at 16 | `pinned` with no bar: the body is inset 16, as today |
| samples | bulk bar spans the width (with H7, only while a sample is selected) | `pinned` with no bar: the body as today |
| stability | top bar (the indicator) and bottom bar (Back, Continue) span the width | the stepper and the footer span the page, content at 16 |
| results | bottom bar spans the width | bottom bar spans the page |
| review | the queue case and the worksheet case are each a `pinned` root: bars span the width | the root column takes `pageEdge` (Screen fixes), so the splitter fills the page edge to edge. The queue pane's head (title and search) is padded by step 3, the mockup's 12 px, and its entries span the pane, divided, with the selected entry's tint and bar at the pane's edge (`09`). The worksheet pane's `pinned` insets its body by 16, so the screen's own `padding: 4` column goes, and its bar spans the pane from the splitter handle to the window's edge |

**Per backend.**

- **Qt Quick:** `MorphStack` insets by `Theme.spaceOf(vertical)` above and below and `Theme.spaceOf(horizontal)` at
  the sides. Its implicit size adds both.
- **Terminal:**
  - A two-step padding maps each step by its axis (`layout::stepCells`): `[2, 4]` is 0 rows and 2 columns.
  - With the shell's spacers hidden, the page loses its 1 row and 2 columns. The pinned body's step-4 padding puts
    them back: 1 row above and below, 2 columns at the sides.
  - A pinned page then differs from today in four ways:
    - **a bar's content starts at column 2, not column 3.** Today it is 2 columns of page inset plus the bar's
      1-column step-2 padding; after, it is the bar's own 2 columns. A bar's content now lines up with the body's,
      as on Qt Quick;
    - a top bar sits directly under the shell's top bar, not one row below it;
    - the body ends one row above the bottom bar;
    - review's queue head gains 1 row and 2 columns of padding, and the worksheet pane gains 2 columns.
  - The bars have no rule and no inverse on the terminal (`bar` is not inverted).
- **`RecordingBackend`:** records `padding=2/4` for a two-step padding and `padding=3` for one step, as today, and
  `pageEdge=true`.

**Spec.**

- `document.md`:
  - the `column, row` row: `pageEdge` and the two-step padding;
  - "A bar that stays in view": `pinned` marks its column as the page's edge, and insets its body and its bars' sides
    by the page's step.
- `view_tree.md`, Column and Row (`Padding`, its conversion from `int`, `pageEdge`).
- `backend_contract.md`, `StackWidget::setPadding(Padding)`.
- `interpreter.md`, the shown root's `pageEdge`.
- `author.md`: `pinned` (`inset`, `pageEdge`), `padding(…)`.
- `host.md`, the head's members.
- `workspace.md`: `activePageInset`, and the tab strip's rule.
- `shell_frame.md`: the Shape table's `shell_page` row (the inset spacers hide while `setPageInset(false)`), and the
  Design decisions row "The padding".
- `qt_quick/backend.md` and `tui/frontend.md`: the Column, Row and Stack rows.

**Tests.**

- `tests/test_ui_author.cpp` (H-C):
  - `pinned(content, {.bottom = …})` writes `pageEdge: true`, the scroll's content in a column with padding 4, and
    the bar with padding `{"lit": [2, 4]}`.
  - `.inset = 0` writes the bar with padding 2, no wrapping column and no `pageEdge`.
  - **Control:** today's helper writes none of these, and every check fails.
- **Layout case** (H-C) "a stack's two-step padding insets its ends and its sides apart": a column with padding
  `[0, 4]` holding a text. The text's top equals the column's, and its left is one step-4 inset in.
  - **Control:** a backend that applies the first step to every side puts the text's left at the column's.
- Corpus (H-C):
  - `padding: {"lit": [2, 4]}` on a column loads;
  - a bare `[2, 4]`, `{"lit": [2]}`, `{"lit": [2, 4, 1]}`, `{"lit": [2, 9]}` and a list on a panel are refused;
  - `pageEdge: true` loads on a column, and a computed `pageEdge` is refused.
- `tests/test_ui_mount.cpp` (H-C): `.padding = 3` on a view-tree column still reaches `setPadding({3, 3})`.
- `tests/test_ui_host.cpp` (H-S): `pageEdge()` reads through a `byWidth`
  `switch` to a pinned column, follows a width change to a case that is not pinned, and answers `false` for a case
  of two nodes.
  - **Control:** read the root only, and the first check fails.
- `tests/test_ui_workspace.cpp` (H-S):
  - `activePageInset()` is `false` for a pinned screen alone, `true` once a second tab opens, and `true` for a host
    screen.
  - **Control:** ignore the strip, and the second check fails.
- `tests/test_ui_shell_frame.cpp` and `tests/tui/test_tui_shell_frame.cpp` (H-S): after `setPageInset(false)` the
  three spacers are hidden, and on the terminal the page's content starts at the page's first row and column.
  - **Control:** a no-op `setPageInset`.
- `tests/client/test_shell.cpp` (H-S): opening a pinned screen hides the frame's inset, and switching to a screen
  that is not pinned shows it again.
  - **Control:** drop the effect.
- `tests/qt_quick/test_qt_quick_containers.cpp` (H-S, after its rebase, since it needs both halves), at 390 px: a
  `ShellFrame` whose page holds a `pinned` with a bottom bar, with the inset hidden. The bar's surface `x` is the
  page's, its width is the page's, and its first child's `x` is `Theme.spaceOf(4)`.
  - **Control:** keep the inset, and the bar's `x` is 16.

**Verify.** Re-grab the five screens at both sizes.

- At 390 px each bottom bar's white run is x = 0 to 389 with a rule across its top, and its first button starts at
  x = 16.
- At 1000 px review's queue surface reaches x = 0 and the window's bottom, with its head 12 px in and its entries
  edge to edge.
- Compare with the compact halves of `02`, `05`, `06`, `08` and `09`, and with `09`'s expanded card.

## H2: a square button for a glyph

**Problem (reproduced, probe 1; read).**

- **The width.** Audit's, samples' and console's pagers are push buttons labelled `‹` and `›` (`audit.cpp:362-367`,
  `samples.cpp:571-578`, `console.cpp:739-744`), each 61×32 px in the grabs:
  - `MorphButtonBackground` gives a framed button a least width of 8 units;
  - `MorphButton` pads a push button by `space.4` on each side.
- **The glyph.** It is drawn at the body size and regular weight, about 4 px of ink.
- **The design.** 30×30 squares with a 13 px/500 glyph (`04`, `05`, and `10`'s `style="width:30px;padding:0"`).
- **The `⋯` triggers.** The head's menu (`workspace.hpp`, `buildHeadTrail`, icon `more`) and samples' compact bulk
  menu are button-style menus, 62 px wide in the grabs. The designs draw them square: 30 px beside instrument's head
  actions (`03`), and 44 px on compact (`03`, `05`).
- **Results.** Results' compact pager is "‹ Previous" and "Next ›", each half the width (`08`, `.btn.lg; flex:1`),
  which the screen already draws. It is not a square button, and it does not change.

**Decision.**

- **`ControlShape::Square`**, document name `square`, on `button`. It is an outline rule only:
  - the corners are the theme's (`radius.control`);
  - there is no least width, and the side padding is `space.1`;
  - the button is at least as wide as it is tall. A glyph or an icon alone gives a square, at the control height on
    expanded and the touch height on compact (32 and 44 px at the default scale). A longer caption gives a wider
    button, as `round`'s stadium does.
- **The caption** is the body size in `Theme.strongWeight`, through `MorphButton`'s existing `strongCaption` path,
  which a square button joins. This follows the mockup: 13 px at a heavier weight. Probe 1 shows the strong weight
  widens the glyph's stroke at the same size.
- **With other keys:**
  - `size: small` and `shape: square` combine: small sets the caption's step and square the outline;
  - `style: item` with `shape: square` is refused at load: an item fills its line, so it has no square outline;
  - `badge` does not take `square`. No design draws a square badge, so its shape names stay `default` and `round`.
- **Icon-only triggers follow the theme.** Under `shape.iconOnly: square` (Theme switches) a push button or a
  button-style menu whose caption is an icon alone, and which gives no `shape`, draws as `square`.
  - This covers the head's `⋯`, the compact shell's ☰ and samples' bulk `⋯` with no key in the workspace, the shell
    or the screen.
  - The ghost ☰ already pads by `space.1` with no least width (`MorphButton.qml:121`,
    `MorphButtonBackground.qml:37`). Square only makes it as wide as tall, which widens it to 44 px on a compact
    shell. H6's line still fits: 12 + 44 + 12 + 75 + 12 + 82 + 12 = 249 px at 390.
- **No `shape` on `menu`.** The theme switch covers every icon-only trigger, and no design draws a labelled square
  menu, so `menu` gains no key and `MenuTrigger` no member.

**Vocabulary.**

- `kControlShapes` gains `{"square", ControlShape::Square}` and grows from 2 to 3. The loader refuses `square` on a
  `badge` and beside `style: item`.
- Author: `ButtonOptions::shape` takes `Square`. A pager writes
  `button({.label = "‹", .shape = ControlShape::Square, …}).a11y("Previous page")`.

**Per backend.**

- **Qt Quick:**
  - `MorphWidget` gains `square` beside `round`, set by `setShape`;
  - `MorphButtonBackground` drops the least width for a square button, as for a compact one;
  - `MorphButton` sets `implicitWidth` to at least `implicitHeight`, pads by `space.1`, and draws a strong caption;
  - `MorphMenu`'s trigger does the same while `Theme.iconOnlySquare` holds and the trigger has no label.
- **Terminal:** a square button drops the push button's inner spaces, as `small` does: `[‹]`. Icon-only triggers keep
  `[ ⋯ ]` (the terminal reads no theme switch).
- **`RecordingBackend`:** records `shape=square`.

**Spec.**

- `control_shape.hpp`'s doc comments;
- `document.md`, the `button` and `badge` rows;
- `backend_contract.md`, the `ButtonWidget` row;
- `theme.md` (`shape.iconOnly`);
- `qt_quick/backend.md` (Button, Menu) and `tui/frontend.md` (Button).

**Tests.**

- **Layout case** "a square push button is narrower than a normal one with the same label", on both frontends: `‹`
  in each.
  - **Control:** a backend that ignores `shape`.
- `tests/qt_quick/test_qt_quick_widgets.cpp`:
  - A square `‹` button's width equals its height equals `Theme.targetHeight`. Its background radius is
    `Theme.radiusControl`. Its caption's font is `Theme.bodyPixels` at `Theme.strongWeight`.
    - **Control:** draw `square` as `default`, and the width is 8 units.
  - Under `shape.iconOnly: square`, an icon-only `more` menu trigger is as wide as it is tall. Under the default it
    keeps its padding.
    - **Control:** ignore the token.
- `tests/tui/test_tui_widgets.cpp`: a square `‹` draws `[‹]`.
  - **Control:** `[ ‹ ]`.
- Corpus:
  - `shape: square` loads on a button;
  - it is refused on a badge and beside `style: item`.

**Gallery.**

- Audit and console (H-C): the pager buttons take `.shape = ControlShape::Square`.
- Samples (H-S): the pager takes `Square`.
- The `⋯` and ☰ triggers change through the gallery theme's `shape.iconOnly`, with no screen code.

**Verify.** Re-grab audit, samples and console at 1000×830, and instrument and samples at 390×844. The pager buttons
are 32×32 with a legible glyph, and the `⋯` triggers are 32 or 44 px squares. Compare with `04`, `05`, `10` and `03`.

**Residual.** 32 px against the design's 30, as G8 sizes every control by the control height.

## H3: a selected card that holds a check box

**Problem (read; the grabs show it).**

- **The design.** The compact samples card (`05` compact) is a plain card with a 22 px check box, and a checked card
  draws a 3 px accent bar at its leading edge (`box-shadow: inset 3px 0 0 #17784A`).
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
   announced twice, and a click on the box would also be a click on the button. The refusal stays.
2. **A selectable list row that is not a button** (a focusable `ListItem` whose content may hold controls). ARIA's
   listbox forbids interactive children, and a grid row that may hold them needs cell navigation, a row selection
   model and focus management across rows: a new list kind for one card.
3. **A click on the card's non-control area.** ui/1 has no pointer target without a key, and the design draws none.
   The check box is the card's one target, at the touch size on compact.
4. **The selection's mark as a stack property.** The card stays a plain container, the check box keeps the state,
   the focus and the name, and the stack draws the mark.

**Decision: 4.** The check box alone selects, and the card body has no click.

- **`selected` on `column` and `row`**, a condition. While true, the stack draws the accent's 3 px bar at its leading
  edge, down its whole height, over its surface and under its children. It is the bar a selected item button and a
  selected table row draw (G6's `selectionBar`), in the same colour.
- **No fill.** The stack's `surface` stays its fill, so a tint and the bar compose, as the design's first card does.
  A screen that wants the selection's fill names a surface by condition. The key means what it means on a button
  ("this is the chosen entry") and draws the part of the mark a stack does not have.
- **Room for the bar.** It lies inside the stack's leading padding. A stack with `selected` and no leading padding
  draws the bar over its first 3 px. The spec says to give such a stack padding.
- **Its shape.** The bar is clipped to the stack's own surface shape: on a surface with a radius it follows the
  leading corners.
  - It is not clipped to an ancestor. Samples' cards sit in a panel with `card` corners (radius 6) and padding 0, and
    the panel does not clip its children. So the first and last cards' bar can show 1 px past the panel's rounded
    corner (residual).
- **Accessibility.**
  - **The state is the check box's.** Its role, its name (the row's label) and its checked state are what assistive
    technology meets, one control per card, with no nesting.
  - **The group.** A stack that takes `selected`, and that has no role of its own (`.a11y(name, role)`), takes the
    role `Grouping` in place of `MorphWidget`'s default `Client`. It reports `Accessible.selectable` and follows the
    condition with `Accessible.selected` (probes 2 and 4).
  - **Naming.** An unnamed group is one screen readers may pass over, so the samples card is named by its sample ID.
    The bar's state is then also read where a reader announces groups.
- **The table's row view refuses it.** The mount refuses `selected` on a stack that is a table's row view, as it
  refuses `tone` there outside a table. A table's selection is the table's, and a second mark on its row would
  disagree with it.
- **No click.** Alternative 3 is not taken.

**Vocabulary.**

- `kContainerKeys` gains `selected` (a condition), on `column` and `row`. It is refused on `grid`.
- In the view tree, `Column` and `Row` gain `std::optional<Prop<bool>> selected`, appended after their existing
  members so every designated initializer keeps its order on MSVC.
- `StackWidget::setSelected(bool)` is pure. The mount calls it for a stack that gives the key, at mount (possibly
  with `false`) and on every change. That first call is what tells the terminal to reserve its column.
  `TableRowImpl`'s is a no-op.
- Author: `ContainerOptions::selected` (an `AnyHandle`; empty writes no key).

**Per backend.**

- **Qt Quick:**
  - `MorphStack` gains `selectable` (set by the first `setSelected`) and `selected`;
  - it draws a `Rectangle` `selectionBar`, 3 px wide, in `scheme.accent`, at x = 0, inside an item clipped to the
    surface background's shape, mirrored under right-to-left layout;
  - its default role is `Grouping` while `selectable`.
- **Terminal:**
  - From its first `setSelected`, a stack reserves its first column. The column is inside its padding when its
    horizontal padding gives one, and one more column otherwise.
  - While selected, every row of that column draws `▌` in the list's selected-item style.
  - The column stays reserved whether or not the stack is selected, so the content does not move when the selection
    changes.
- **`RecordingBackend`:** records `selected=true` or `selected=false`.

**Spec.**

- `document.md`, the `column, row` row;
- `view_tree.md`, Column and Row (and the row view's refusal);
- `backend_contract.md`, `StackWidget`;
- `author.md`, containers;
- `qt_quick/backend.md` and `tui/frontend.md`, the stack rows.

**Tests.**

- `tests/qt_quick/test_qt_quick_containers.cpp`:
  - A padded column with `selected: true` on a tinted surface shows a visible `selectionBar` 3 px wide, its colour
    `scheme.accent`, at x = 0. A pixel just right of the bar reads the surface's colour.
  - Its `Accessible.role` is `Grouping` and its `Accessible.selected` is true.
  - With `selected: false` the bar is hidden.
  - **Control:** a stack that ignores the key draws no bar and keeps `Client`.
- `tests/tui/test_tui_containers.cpp`:
  - A selected column of two text rows draws `▌` in column 0 of both rows.
  - A column whose first call was `setSelected(false)` keeps its text in the same column as the selected one.
  - **Control:** reserve the column only while selected, and the text moves.
- `tests/test_ui_mount.cpp`:
  - the condition reaches `setSelected` at mount and on change;
  - a stack without the key is never told;
  - a table row view with `selected` is refused.
- Corpus: `selected` loads on `column` and `row`, and is refused on `grid`.

**Gallery** (samples, H-S). The card (`sampleCard`) takes `.selected = ui::includes(list.selected, ui::row("id"))`,
the condition its check box already reads, and `.a11y(ui::row("id"))`. Its padding stays step 2 (8 px), which holds
the 3 px bar. The card's check box, its `outOfSpec` surface and the divided list stay as they are.

**Verify.** Re-grab samples at 390×844. The checked cards show the green bar at their leading edge, and the first
also keeps its tint. Compare with `05` compact.

## H4: runs in the head's title

**Problem (read; the grabs show it).** Plate's title is "Plate PL-0193" with the ID in mono (`07`: `Plate
<span class="mono">PL-0193</span>`, both halves 16 px/600). The title is the document's `title` expression, which is
also the tab's caption, the navigator's entry and the window's title, all plain text. The shell draws it with
`ShellFrame::setTitle(string)`.

**Decision.** Mirror `subtitleSpans`:

- **The screen member `titleSpans`** is an expression of runs, as a `text`'s `spans`. A shell that knows it draws the
  runs in place of `title` in its top bar. `title` stays the caption, the navigator entry, the window title and what
  an older shell draws. The runs are not compared with `title`, as `subtitleSpans` is not compared with `subtitle`.
- `ScreenHead::titleSpans()` is a virtual with a default that answers none, so a host screen's head compiles
  unchanged.
- `Workspace::activeTitleSpans()` is a tracked read.
- `ShellFrame::setTitleSpans(std::vector<TextSpan> const&)` sets the title text's runs (`TextWidget::setSpans`);
  empty draws `setTitle`'s text.
- **The client follows the runs reactively.**
  - `showActiveTitle` (`src/client/shell.cpp`) is a plain callback that peeks at the tab's caption, and it does not
    run when a head's runs change.
  - So the client holds a `reactive::Effect` over `activeTitleSpans()` (`_titleSpans`). It is made in
    `showActiveHead` beside `_headShown` and reset with it in `resetHead`.
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
- `tests/test_ui_shell_frame.cpp`:
  - after `setTitleSpans({"Plate ", mono "PL-0193"})`, `shell_title`'s recorded runs are those two;
  - after `setTitleSpans({})` it draws the plain title again;
  - **Control:** ignore the runs.
- `tests/client/test_shell.cpp`: the top bar's title shows a screen's `titleSpans`. When the state the runs read
  changes, the title's runs follow without a tab switch.
  - **Control:** set the runs once in `showActiveTitle`, and the second check fails.

**Gallery** (plate, H-S): `screen.titleSpans(ui::spans({"Plate ", ui::mono("PL-0193")}))` beside its `title`.

**Verify.** Re-grab plate at both sizes, and compare the title with `07`'s.

## H5: an eyebrow line, and labels in small capitals

### The eyebrow

**Problem (read; the grabs show it).** Three mockups put a line above the title:

- instrument puts "Instruments › EQ-0142" above the title, with the badges under it (`03` expanded,
  `<p class="muted">` before the `h1`; compact "EQ-0142" likewise);
- results puts "Worksheet WS-2026-0412 · HPLC-02 · method TM-ASSAY-07 v3" above its title (`08`);
- stability's compact head puts "Step 2 of 4 · Paracetamol 500 mg" above its title (`06`).

The head has one text line, `subtitle`, and the shell draws it under the title.

**Decision.** A second head line, placed above the title:

- **The screen members `eyebrow`** (an expression) **and `eyebrowSpans`** (runs drawn in place of `eyebrow`), named
  and shaped as `subtitle` and `subtitleSpans` are. Both are a screen's only.
- **The shell draws it above the title**, muted, in the lead block.
  - `HeadParts` gains `eyebrow`, a muted text node (`head_eyebrow`) that is hidden while the eyebrow is empty.
  - `ShellFrame::headEyebrowSlot()` is a container at the top of the lead block, above `shell_title`, shown with the
    other slots by `setHeadShown`.
  - Its member `_eyebrow` is declared between `_leadBlock` and `_title`. Members are initialised in declaration
    order, which is the order the frame creates its widgets in (`shell_frame.hpp:78-80`), so the slot is created
    first in the block and `-Wreorder` stays quiet.
  - The client mounts the part there, owned and reset beside `_headLead`.
- **`headHasContent` counts the eyebrow**, so a head with only an eyebrow shows.
- **In the page** (`HeadPlacement::InPage`), the eyebrow text is a line above the head row, mounted with it. The tab
  bar carries the title there.

**Per backend.** Nothing new. On the terminal the eyebrow is one more muted row above the title in the top bar.

### Labels in small capitals

**Problem (read; the grabs show it).**

- **Audit's facet labels** are `.fh{font-size:11px; font-weight:600; letter-spacing:.04em; text-transform:uppercase;
  color:#4D5461}` (`04`). The console's sidebar group headings have the same look (`.navh`, `.06em`, `10`).
- **Today.** Audit writes "RECORD TYPE" in capitals in a `section` text (`audit.cpp:256`), which draws at the body
  size in bold: 13 px against 11. The navigator draws its group headings as `section` (`navigator.hpp:406`).
- **Why `section` cannot change.** No role draws the small, letter-spaced capitals. `section`'s other uses are strong
  body-size lines ("System suitability", "6 wells selected"), which the designs draw that way.

**Decision.** A text role, **`overline`**: a short label over a group, whose look is the theme's switch.

- **`TextRole::Overline`**, document name `overline`. `roleNamed`, the role names `document.hpp` lists, and a table
  column's `role` take it.
- **With `font.overline.case: none`** (the default) it draws as `section` does. **With `uppercase`** (the gallery) it
  draws at the small step, in the strong weight, muted, in capitals, letter-spaced by `0.05 × Theme.smallPixels`.
  That spacing lies between the designs' `.04em` and `.06em`, and it is a rule rather than a token.
- **One font, built whole.**
  - `Theme.overlineFont` is built as `fontOf` builds a font, passing `capitalization` and `letterSpacing` to
    `Qt.font` with the family, size and weight.
  - `Theme.figuresOf`, which rebuilds a font for tabular figures, copies both properties from its base (probe 4).
  - So an `overline` column in a table with tabular figures keeps its capitals. No binding sets
    `font.capitalization` beside a whole-`font` binding, a combination not tried on 6.8.
- **The text is written as it reads**, "Record type", and the frontend draws the capitals. The accessible name stays
  as written (probe 2), so a screen reader does not spell a capitalised word out letter by letter.
- **The navigator's group headings are `overline`.** Under the default theme they look as they do today, and under
  the gallery's they are the design's sidebar.

**Per backend.**

- **Qt Quick:** `MorphText` (role 9) draws `Theme.overlineFont` in `scheme.muted` (`textColorOf`).
- **Terminal:** bold in the muted style, **as written**, whatever the theme. The terminal reads no theme switch, and
  uppercasing beyond ASCII needs case data the terminal frontend does not carry.
- **`RecordingBackend`:** records `role=overline`.

**Spec.**

- `document.md`:
  - "The document" and "The head" (`eyebrow`, `eyebrowSpans`);
  - the `text` row (`overline`);
- `view_tree.md`, `TextRole`;
- `theme.md` (`font.overline.case`);
- `host.md`; `workspace.md` (the eyebrow part, the in-page line);
- `shell_frame.md`:
  - `headEyebrowSlot`;
  - the navigator's headings: "its group headings are overlines and its entries body text";
- `author.md`, `Scope::eyebrow`, `eyebrowSpans`;
- `qt_quick/backend.md` and `tui/frontend.md`, the Text rows.

**Tests.**

- `tests/qt_quick/test_qt_quick_widgets.cpp` (H-C):
  - Under `font.overline.case: uppercase`, an `overline` text "Record type":
    - its label's font has `Theme.smallPixels`, `Theme.strongWeight`, `Font.AllUppercase` and a letter spacing above
      0;
    - its colour is `scheme.muted`;
    - its `Accessible.name` is "Record type".
  - Under the default, its font is `Theme.strongFont`.
  - **Control:** draw it as `section` whatever the token, and the size check fails.
- `tests/qt_quick/test_qt_quick_table.cpp` (H-C): an `overline` column whose cells use tabular figures keeps
  `Font.AllUppercase` and its letter spacing.
  - **Control:** today's `figuresOf`, which drops them.
- `tests/tui/test_tui_display.cpp` (H-C): an `overline` text draws "Record type" in bold muted cells.
  - **Control:** draw it as `normal`.
- Corpus:
  - `role: overline` loads;
  - `eyebrow` and `eyebrowSpans` load on a screen and are refused on the app document.
- `tests/test_ui_workspace.cpp` (H-S):
  - `headPartsForActive().eyebrow` shows the eyebrow's runs, and is hidden while there is none;
  - a head with only an eyebrow has content;
  - the in-page head shows `head_eyebrow` above `head`.
  - **Control:** leave the eyebrow out of `headHasContent`.
- `tests/test_ui_shell_frame.cpp` (H-S):
  - the lead block's children are, in order, the eyebrow slot, `shell_title` and `shell_head_lead`;
    - **Control:** the slot after the title;
  - a navigator group heading is recorded with `role=overline`;
    - **Control:** `section`.
- `tests/client/test_shell.cpp` (H-S): with a screen that declares an eyebrow, `head_eyebrow` is mounted inside
  `headEyebrowSlot()` and shows its text.
  - **Control:** a client that never mounts the part.

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
- The weight is 700 against 600, as G decided for every strong text.

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
  - The wrapping line takes `min(32 units, natural)` (`MorphStack.naturalBasisOf`), which is then 32 units.
  - The terminal caps the same way (`layout.cpp:226`, read, not run).
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
- `tests/tui/test_tui_shell_frame.cpp`: the same frame at 50 columns draws Scan on the title's row.
  - **Control:** the same.

**Gallery.** None.

**Verify.** Re-grab console at 390×844. ☰, the title over "Connected", and Scan sit on one line, as in `10` compact.

## H7: a pinned bar with nothing shown takes no room

**Problem (read, not run).**

- **What `pinned` does.** It leaves out a bar whose node list is empty when the screen is built. A bar whose nodes
  are all hidden at run time is still a column with step-2 padding on the `bar` surface, and the divided column
  draws a rule above it: an empty strip with a rule.
- **Where it shows.** Samples' compact bulk bar is shown only while a sample is selected (`compactBulkBar`,
  `.visible(list.selectedCount > 0)`), so with no selection the strip shows.
- **Why it is not run.** The grabs open samples with three samples selected, and a grab cannot clear them.

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

## H8: tabs in the head's white block

**Problem (reproduced in the grabs; read).**

- **The design.** Instrument's tab strip sits in the white head block under the badges, and a rule under the strip
  spans the card. The page's grey begins below it, and the content is 16 px in (`03`: the strip at y ≈ 170, its rule
  at y = 189 across x = 41 to 1039).
- **The grab.** The strip sits in the grey page, inset 16 px, with its underline from x = 16 to 984
  (`instrument-1000x830.png`, y ≈ 94).
- **Why the vocabulary cannot say it.** A `tabs` node is one node, holding both the strip and the pages. So `pinned`
  cannot put the strip in its top bar and the page in its body. Its strip has no surface of its own, and it spans
  only the node's width. Compact (`03` compact) is the same, with the actions row above the strip in the white
  block.

**Decision.** `tabs` takes two literal keys, so a tabs node can be a page's edge-to-edge root:

- **`inset`**, a spacing step, default 0:
  - the strip spans the node's width, and its underline spans it too;
  - the first tab starts `inset` in from the leading edge;
  - each page is padded by `inset` on every side.
- **`stripSurface`**, a surface name, default none: the strip, and nothing else, is drawn on that surface.

Instrument's root is a column with `pageEdge: true` (H1) holding the tabs with `inset: 4` and `stripSurface: "bar"`.
On compact, the actions row before the tabs sits in a `column` padded `[2, 4]` on `bar`. The white block is then the
shell's top bar, the actions (compact) and the strip, and the page is inset 16 px below the strip's rule.

- `TabsWidget::setInset(int)` and `setStripSurface(std::string_view)` are pure. The mount calls each only when the
  document gives the key. Their implementations are Qt Quick's `TabsImpl`, the terminal's `TabsImpl` and
  `RecordingBackend`. The workspace's own tab strip calls neither.
- Author: `TabsOptions::inset` and `stripSurface`.

**Per backend.**

- **Qt Quick:**
  - `MorphTabs` draws the strip on the named surface's background across its width;
  - it insets the `TabBar` (and G10's More) by `Theme.spaceOf(inset)`;
  - it pads the page container by the same.
- **Terminal:**
  - the strip's tabs start `inset` columns in (step 4: 2 columns);
  - the pages are padded by the step's rows and columns;
  - `bar` is not inverted, so the strip's surface draws nothing new.

**Spec.** `document.md`, the `tabs` row; `view_tree.md`, Tabs; `backend_contract.md`, `TabsWidget`;
`author.md`, `tabs`; `qt_quick/backend.md` and `tui/frontend.md`, the Tabs rows.

**Tests.**

- `tests/qt_quick/test_qt_quick_containers.cpp`:
  - A 600 px tabs node with `inset: 4` and `stripSurface: "bar"`: the first tab button's `x` is `Theme.spaceOf(4)`;
    the strip's background spans x = 0 to 600 in `Theme.surface`; a page's first child is `Theme.spaceOf(4)` in from
    the page's left and top.
  - **Control:** ignore `inset`, and the button's `x` is 0.
- `tests/tui/test_tui_containers.cpp`: the same node at 60 columns draws its first tab label at column 2.
  - **Control:** the same.
- Corpus: `inset` and `stripSurface` load, and a computed `inset` is refused.

**Gallery** (instrument, H-S): as decided above, at both sizes.

**Verify.** Re-grab instrument at both sizes. The strip is white from the window's left to its right edge, with its
rule across. The Overview tab's label starts 16 px in, and the cards start 16 px below the rule. Compare with `03`.

**Residual.** The shell's top bar (`toolbar`) draws its bottom border, so a 1 px rule separates the badges from the
strip where the mockup's white block is unbroken. Dropping that border while a page has a top bar would also drop the
rule every other pinned page's mockup draws under the head (`02` compact, `05` compact). The rule is kept.

## Screen fixes

Each lands in the lane that owns the screen, after the items it uses.

| Fix | How | Owner |
|---|---|---|
| The gallery's theme switches | `shape.iconOnly: square` and `font.overline.case: uppercase` in the bundle | H-C |
| Review's root and panes (H1) | The expanded root column takes `pageEdge: true`. The queue pane's head column (title and search) takes `padding: 3`. Its entries are a divided `forEach` with no padding, so each entry's tint and bar reach the pane's edge (`09`). The worksheet column inside `pinned` drops `padding: 4`, since `pinned` now insets it | H-S |
| Samples' compact bulk bar is a bar, not a tinted panel inside one | `compactBulkBar` returns its row with `.visible(selectedCount > 0)`, without the `selected`-surface panel. The bar is white with a rule, as `05` compact draws it, and H7 hides it with no selection | H-S |
| Pagers (H2) | `.shape = ControlShape::Square` on `‹` and `›`: audit and console (H-C), samples (H-S) | H-C, H-S |
| Samples' selected cards (H3) | `sampleCard` passes the box's condition as `.selected` and its ID as `.a11y` | H-S |
| Plate's mono title (H4) | `titleSpans` | H-S |
| Eyebrows (H5) | Instrument and results move their line to `eyebrow` and `eyebrowSpans`; stability compact moves its step line there | H-S |
| Audit's facet labels (H5) | `role: "overline"`, written "Record type" and "Action" | H-C |
| Instrument's tabs (H8) | The root column takes `pageEdge: true`; the tabs take `inset: 4` and `stripSurface: "bar"`; compact's actions row sits in a `bar` column padded `[2, 4]` | H-S |

## Totals

**New document keys and members:**

- the screen's `titleSpans`, `eyebrow` and `eyebrowSpans`;
- `column` and `row`: `pageEdge`, `selected`, and `padding` as `{"lit": [vertical, horizontal]}`;
- `tabs`: `inset`, `stripSurface`;
- the shape name `square` on `button`;
- the text role `overline`.

**Theme:**

- the kind `Keyword`, with its `$type` `keyword`;
- the tokens `shape.iconOnly` and `font.overline.case`;
- `Theme.iconOnlySquare`, `Theme.overlineCaps` and `Theme.overlineFont`.

**New or changed pure virtuals**, each with every implementation (Qt Quick, terminal, `RecordingBackend`; for
`StackWidget` also Qt Quick's `TableRowImpl`):

- `StackWidget::setSelected(bool)`, new;
- `StackWidget::setPadding(Padding)`, which replaces `setPadding(int)`;
- `TabsWidget::setInset(int)` and `setStripSurface(std::string_view)`, new.

**Other API:**

- `ControlShape::Square`; `TextRole::Overline`; `ui::Padding` (from `int`, with `operator==`).
- `ScreenHead::pageEdge()`, `titleSpans()`, `eyebrow()` and `eyebrowSpans()`, each with a default.
- The interpreter's `Screen::pageEdge()`.
- `Workspace`: `activePageInset()`, `activeTitleSpans()`, and `HeadParts::eyebrow`.
- `ShellFrame`: `setPageInset(bool)`, `setTitleSpans(…)`, `headEyebrowSlot()`.
- Author:
  - `PinnedOptions::inset`;
  - `ContainerOptions::selected` and `pageEdge`;
  - `TabsOptions::inset` and `stripSurface`;
  - `ui::padding(int, int)`;
  - `Scope::titleSpans`, `eyebrow`, `eyebrowSpans`.

**Conformance:** two new layout cases, H1's two-step padding and H2's square button.

**Toolchain notes for both lanes.**

- New members go at the end of the view-tree structs, so designated initializers keep their order on MSVC.
- `ShellFrame`'s new member keeps declaration order equal to creation order (`-Wreorder`).
- No parameter is named after a member or member function beside it (GCC 15 `-Wshadow`).
- Every QML property used here exists on Qt 6.8, and `qmllint` runs over the changed files.

## Not in this batch

- **A click on a card's non-control area** (H3, alternative 3). ui/1 has no pointer target without a key, and the
  design draws none.
- **A selectable list row that may hold controls** (H3, alternative 2). That is a grid-like kind with cell
  navigation, and nothing in the designs needs it.
- **`shape` on `menu`.** The theme switch covers every icon-only trigger, and no design draws a labelled square menu.
- **A square badge.** No design draws one.
- **The bars' 12 px vertical inset.** Bars keep step 2 (8 px), for G3's reason: step 3 costs a terminal row above and
  below every bar.
- **The rules' colours.** The mockups use `#E1E5EA`, `#EDF0F3`, and samples' green `#BCDFC9` above its bulk bar. The
  theme has one divider. That is a theme value, as G3 found.
- **30 px controls.** Controls are sized by the control height (32 px), as G8 decided.
- **The mono face.** JetBrains Mono is not installed on this machine, which makes it environmental.
- **Comparing `titleSpans` with `title`.** Neither head line's runs are checked against their plain text.

## Decisions

Taken with the user on 2026-10-30:

- **`pinned` insets by default**, so every pinned page looks like its mockup with no extra key. The shell drops its
  page inset where the shown root is marked `pageEdge`, which `pinned` writes. There is no screen-level opt-in.
- **The square icon triggers and the small-capital group headings are the theme's switches**, off by default and on
  in the gallery's theme.
- **H3:** the check box alone selects, and the card body has no click.
- **H2:** legibility comes from text, not theme icons, at the mockup's body size in the strong weight.

Taken at this design's defaults:

- the two-step padding is a `lit` constant on stacks only;
- the edge look applies while the workspace's tab strip is hidden;
- `selected` on a stack draws the bar only, the terminal reserves its column from the first call, and the stack
  becomes a `Grouping`;
- the eyebrow mirrors `subtitle`; the capitals are a role named `overline`, drawn as written on the terminal;
- tabs get `inset` and `stripSurface` rather than a split into a strip node and a page node;
- results' pager stays labelled, as the design draws it.

## Review response

Every finding of the design's review was taken, except two nits, which a decision above made moot.

**Blockers.**

1. **A bare array is not an expression.** Two-step padding is written `{"lit": [2, 4]}`, a constant checked at load.
   Computed padding stays one step, and `ui::padding(v, h)` serialises as `AnyHandle{std::vector<std::int64_t>}` (H1,
   decision 5).
2. **The `StackWidget` overrides.** There are four: Qt Quick's `StackImpl` and `TableRowImpl`, the terminal's
   `StackImpl`, and `RecordingBackend`.
   - `TableRowImpl` takes `setPadding` and `setSelected` as no-ops.
   - `EchoingBackend` (`tests/ui_echoing_backend.hpp`) forwards only `createStack`, so it is out of both lanes.
   - The mount refuses `selected` on a table's row view (H1, H3, Parts and lanes).

**Should-fix.**

3. `Padding` has an implicit constructor from `int` and `operator==`, so `.padding = 3` keeps compiling.
   `view_tree.md` says so, and a mount test covers it (H1).
4. **The terminal's bars.** A bar's content starts at column 3 today and column 2 after, listed as a terminal
   difference (H1, Per backend).
5. **The double inset.** This is resolved by the user's decision. `pinned` writes `pageEdge` and insets itself, and
   the shell drops its inset where the shown root, seen through switches, has `pageEdge`. No screen says anything
   for a pinned page. The earlier screen-level `pageInset` member is gone (H1, decisions 1 to 4).
6. **Review's queue.** Only its head is padded by 12 px. Its entries are a divided, unpadded `forEach`, with the
   selection's tint and bar at the pane's edge (Screen fixes).
7. **The workspace's tab strip.** The edge look applies only while the strip is hidden.
   `activePageInset()` is `true` with two tabs or more, and a workspace test covers it (H1, decision 4).
8. **H2's caption.** The caption is the body size in the strong weight, through `strongCaption`. `square` is an
   outline rule only.
   - `square` with `small` combines.
   - `square` with `style: item` is refused.
   - `badge` does not take `square` (H2).
9. **H3's accessibility.** Probe 4 re-reads the states on a `Client` item.
   - A stack with `selected` and no role of its own becomes a `Grouping`, and the samples card is named by its ID.
   - The design states that the check box carries the state to assistive technology (H3, Accessibility).
10. **H4's title runs.** The client holds an effect over `activeTitleSpans()`, owned and reset with `_headShown`.
    `test_shell` changes the runs' value and checks that the title follows (H4).
11. **H5's font.** `Theme.overlineFont` is built whole with `capitalization` and `letterSpacing`, and `figuresOf`
    copies both (probe 4). A table test covers an overline column with tabular figures (H5).
12. **H5's mount.** A `test_shell` case checks that `head_eyebrow` is mounted in `headEyebrowSlot()` (H5, Tests).

**Nits.**

- **The ShellFrame-plus-pinned Qt Quick test** belongs to H-S, after its rebase (H1, Tests).
- **`Menu::shape` beside `MenuTrigger::shape`.** Not taken: `menu` gains no `shape`, because the theme switch
  `shape.iconOnly` covers every icon-only trigger. So neither member exists (H2; Not in this batch).
- **☰'s width.** Square ☰ already pads by `space.1` with no least width. Square only widens it to 44 px on compact.
  This is now under the theme's switch, and H6's line still fits at 390 px (H2).
- **The terminal's first `setSelected`.** The first call, possibly `false`, is what reserves the column.
- **The bar against a rounded corner.** The bar is clipped to the stack's own surface shape. The 1 px past the
  enclosing panel's corner is stated as a residual (H3).
- **The eyebrow slot's member order.** `_eyebrow` is declared between `_leadBlock` and `_title` (H5).
- **H6 on the terminal.** H6 has a terminal shell-frame test, and the terminal cap at `layout.cpp:226` is marked as
  read (H6).
- **The review's missing gap** is H8: instrument's tabs in the head's white block.

## Defects this batch fixes

- **The shell head's lead ignores its natural-width cap** (H6). Reproduced at 390 to 405 px; the cause is read.
- **A pinned bar whose nodes are all hidden draws an empty strip** (H7). Read, not run.

Neither is filed, since each is fixed by the change that touches it.

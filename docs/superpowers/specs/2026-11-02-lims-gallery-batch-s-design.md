# LIMS gallery batch S: the head's tools as a row of actions, a rule under a table's header, and five rows of buttons on screens

Batch S covers the seven items the after-R comparison left open (`lg-rr-findings-A.md`, `lg-rr-findings-B.md`). Each
item becomes one of three things:
- a framework item (S1, S2) for lane S-F;
- a screen item (U1 to U5) for lane S-S;
- an accepted difference (A15, A16).

The rules of batches G to R still hold:
- a look is a token or a rule, never a screen's choice of pixels;
- a key means the same thing on every kind that takes it;
- the terminal keeps its cells;
- vocal over silent.

**Sources.** Code was read with `git -C morph-lims-e2 show lg-fold-r:<path>` and in `morph-lims-e2` (`lg-rs`).
`morph-lims-e1` was not opened. Nothing was built.

**Evidence.** Pixel reads were taken from `lg-cmp/app-r/` and `lg-cmp/mock/` with PIL. The scripts and crops are in
`$S/lgs-exp/`. Premises are marked like this:
- **measured**: a pixel read;
- **read**: inferred from the code.

**Issues.** None are filed:
- S1 and S2 are vocabulary, not defects in shipped behaviour.
- The one defect S1 closes on the way is a fix smaller than an issue: a document's `actionBar` holding a `forEach` of
  buttons draws them with the `forEach`'s gap, which is 0 by default, so they touch.

## Findings

| # | Finding | Cause | Verdict |
|---|---|---|---|
| 1 | Button rows 4 px apart: receipt's phone footer, samples' bulk bars (phone, and the expanded one the review found), results' and review's expanded footers | **Read.** These are hand-written `ui::row(…, {.gap = 1})` at `receipt.cpp:468-470`, `samples.cpp:631-643`, `results.cpp:566-569` and `review.cpp` `worksheetPane` | U1 (screen). All four are rows of actions. |
| 2 | The head's tools and actions are 4 px apart on plate and console at 1215; the mockups have 8 | **Read.** `Workspace::buildHeadTrail` (`workspace.hpp:1058`) is a wrapping `Row` with `gap = 1`. Its tools, actions and menu are three `forEach` stacks, each with gap 1. The frame's `_trail` (gap 3) only separates that slot from the principal. No theme key reaches either gap | **S1** (framework), the same token |
| 3 | Stability's table header has "a grey band where the mockup is white" | **Measured: mis-framed.** The two bands are the same colour. The app draws `#FAFBFC` at y 305–332 (1215). The mockup draws `#FAFBFC` at y 393–418, and `06-Stability.dc.html:37` sets `.mx th{background:#FAFBFC}`. The real difference is a 1 px `#DDE1E6` rule **under** the mockup's header, at y 419. The app draws no rule under any table's header (audit, samples, review and stability were measured). Every mockup `th` has `border-bottom:1px solid #DDE1E6`. **Read:** `MorphTableHeaderBackground.qml` draws only the fill, and no token names a header rule | **S2** (framework) |
| 4 | Plate 390: the Assign… buttons end at x 369; the row's rule ends at 373 | **Read.** `runLine` (`plate.cpp:678`) pads its row `padding(withAssign ? 0 : 1, 1)`, which is 4 px at both sides. The comment says the inset "leaves room for a selected marker's ring at the page's edge". **Read:** nothing between the row and the page's scroll clips. The run lines sit in a `switchOn` slot, which is not scrollable and so does not clip, inside a collapsible, which has no `clip`; the compact column pads by step 4 inside the page's scroll. (Other kinds do clip: a scrolling `MorphSlot`, dialogs, drawers, interactive or virtualized tables, selects, progress bars and eliding badges. None of them is on this path.) So the ring has the 16 px gutter, as the mockup's `box-shadow` ring does (`07-Plate.dc.html:33`, `:39`) | U3 (screen) |
| 5 | Receipt 1215: the Outcome badges end 72 px from the pane's edge; the design has 33 | **Read.** Sample is `fixed(11)` (96 px). Outcome is the only stretch column, so it takes all the spare width | U2 (screen) |
| 6 | Instrument 390: "Calibration due in 3 days" wraps (+22 px head) | **Measured** by reviewer A: the line needs 322 px and has 318. This is R2's step-4 trailing inset beside the menu square, the same mechanism as A14 | A15 (accepted) |
| 7 | Stability's date field draws a picker glyph | Batch K's decision to keep the calendar button | A16 (accepted) |

## S1: the head's trail is a row of actions

### Decision

- **The head's trail takes its spacing from `actions.gap`,** the theme's token for the space between the actions of a
  row of actions. It gets no token of its own:
  - the mockups space head tools and button pairs alike, 8 px;
  - a head's trail holds the screen's own actions, the same ones an overlay's footer shows (`overlayActions`);
  - a second token would let two rows of the same actions disagree.
- **`ActionBar` gains `wrap`** in the view tree (`bool wrap = false`, set once), in `ActionBarOptions` and in ui/1
  (the `wrap` flag).
  - It means what a row's `wrap` means: actions that do not fit on one line continue on the next.
  - `checkActionBar` no longer refuses `wrap`. It still refuses `gap`.
  - The mount calls `setWrap(true)` before the first child. Like a wrapping `ForEach`, a wrapping bar registers no
    `ShareParent`.
  - **Not in an overlay's footer.** There the footer sizes the bar to its one-line width, so it would never wrap. The
    mount refuses a wrapping `ActionBar` whose `overlayFooter()` is non-null, with the message "an overlay's footer is
    one row of actions; `wrap` is for an action bar in place". The document loader cannot see the nesting at every
    depth, so the mount's refusal is the one that applies.
- **A horizontal `forEach` that is a direct child of an `actionBar` is part of the row of actions.**
  - The mount marks its stack `setActionRow()` and gives it gap step 2, both before its first row. It does not read
    the `forEach`'s own `gap`. `view_tree.md` says so for a C++ `ForEach`.
  - Only a *direct* child counts. A `forEach` inside a `row` inside an `actionBar` is unchanged.
  - ui/1 reports `gap` on such a `forEach`, with the bar's message:
    - at load, when its `axis` is the literal `"horizontal"`;
    - at lowering, when its `axis` is computed and evaluates to horizontal, as `wrap` and `divided` are reported
      (`interpreter.hpp:9121-9133`). The `actionBar`'s lowering checks its `forEach` children, so lowering a
      `forEach` does not have to know its parent.
  - A vertical `forEach` there is unchanged.
  - Implementation: `mountKind(ActionBar)` records its widget in a member while it mounts its children, and restores
    the previous value afterwards. `mountKind(ForEach)` compares that member with its `parent`.
- **The workspace.**
  - `buildHeadTrail` returns an `ActionBar{.wrap = true, .align = Center}` holding the same four children: the tools,
    the `End` lead, the actions and the menu.
  - `headPartsForActive` and `headRow` mount it with `ui::actionBar`.
  - **`overlayActions`** (`workspace.hpp:642-660`) becomes an `ActionBar` around its `forEach` too. Today it is a
    `Row` of gap 2, the footer's only child, so `actions.gap` never reaches it. It mounts in place, because its
    `Mounted` has no overlay, and the direct-child rule marks the `forEach`. A screen's actions are then the theme's
    gap apart whether they show in its head or in its overlay's footer (e-signature opened as an overlay is the case).
  - The trail is never mounted inside an overlay's content. The shell mounts it into `ShellFrame::headSlot()`, and the
    in-page head into `page->slot`, each as its own `Mounted`. An overlay screen's head actions go through
    `overlayActions` instead. So the bar stays where it is written.
  - The `head_trail` test id is kept.
- **Qt Quick:** no QML change, so nothing needs Qt newer than 6.8. **Read:** `MorphStack` already spaces an action row by `stepLength`
  (`Theme.actionSpacing`) in both its wrapping and its non-wrapping layout (`MorphStack.qml:40`, `:223`, `:354`,
  `:433`). A wrapping row gives no gap to a child with no width, so an empty tools or menu part still takes none.
- **Terminal:** unchanged. Steps 1 and 2 are both one column (`layout::stepCells`), and an action row keeps its `gap`
  there. A document's `forEach` of buttons inside an `actionBar` gains its one column. This is intended.
- **Comments (current behaviour only):**
  - at `buildHeadTrail`: "the trail is a row of actions: its parts are the theme's action gap apart, and wrapping, so
    a part with nothing in it takes none";
  - at the `ForEach` mount: "a horizontal `forEach` directly in an action bar is part of the row, so its rows are the
    action gap apart".

### Consequences

- Every expanded head with two or more trail items grows by 4 px per gap. The review counted five (read from each
  `declareHead`):

  | Screen | Gaps | Growth | Room left at 1000 (inferred from measured ink) |
  |---|---|---|---|
  | samples | 3 | +12 | about 12 px, the tightest |
  | plate | 2 | +8 | at 1215, about 36 px before the subtitle wraps |
  | console | 2 | +8 | over 60 px |
  | audit | 2 | +8 | over 60 px |
  | instrument | 2 | +8 | about 21 px |

  E-signature, receipt, results and review have one trail item, or show their actions only in an overlay.
- Compact heads are unchanged: each shows at most one trail item.
- A wrapped trail puts the action gap between its lines too (`MorphStack.qml:521`), as CSS `flex-wrap` with `gap`
  does.
- U4 re-measures all five.

### Spec

- `view_tree.md`: the ActionBar row (`wrap`; a horizontal `ForEach` child is part of the row).
- `document.md`: the `actionBar` row (`wrap` accepted) and the `forEach`-in-`actionBar` `gap` refusal.
- `author.md`: `ActionBarOptions::wrap`.
- `workspace.md:120-123`: the trail is an action bar spaced by `actions.gap`. Replace "a row of gap 1". The overlay
  footer's actions are an action bar as well.
- `view_tree.md`: a wrapping `ActionBar` in an overlay's footer is refused.
- `theme.md`: the `actions.gap` row also names the head's trail.
- `qt_quick/backend.md`: the Stack row.
- `tui/frontend.md`: one line saying the head's trail is unchanged.
- Doxygen on `ActionBar::wrap` and `ActionBarOptions::wrap`.

### Tests

- **Document (`test_ui_document*`):**
  - `actionBar` with `wrap` is accepted and lowers `wrap = true`;
  - `gap` on the bar is still refused;
  - a horizontal `forEach` with `gap` directly in an `actionBar` is refused at `/…/gap`;
  - a computed-axis `forEach` that evaluates to horizontal, with `gap`, in an `actionBar` is reported at `/…/gap` at
    lowering;
  - a vertical one is accepted.
- **Mount (`test_ui_mount*`, recording backend):**
  - an `ActionBar{wrap}` records `wrap` and `actionRow`;
  - its horizontal `ForEach` child, given gap 0, records `actionRow` and a gap of step 2. On the terminal, step 0 is 0
    columns and step 2 is 1, so the step shows there too;
  - **control:** the same `ForEach` inside a plain `Row` records neither;
  - **depth control:** a `ForEach` inside a `Row` inside an `ActionBar` records neither;
  - `ActionBar{.wrap = true}` inside a dialog's content throws. **Control:** the same bar outside the dialog mounts.
- **Workspace (`test_ui_workspace.cpp`, the tests at `:1052`, `:1101` and `:1411`):** `head_trail` records `actionRow`
  and `wrap`, and so do its tools and actions `forEach` stacks. An overlay screen's footer actions record `actionRow`.
- **Qt (discriminating), in the suite that holds the head geometry tests:**
  - With the stock theme, a head with two tools and two actions (plate's and samples' shape): each tool-to-tool,
    tool-to-action and action-to-action gap is 8 px.
  - With `actions.gap: 12`, every one of those gaps is 12.
  - **Empty parts, exact.** A head with actions and no tools or menu:
    - the last action's right edge equals `head_trail`'s right edge, within 0.5 px;
    - the first action's left edge equals the trail's left edge, within 0.5 px.

    The existing trailing-edge check (`actionRight >= slotRight - gap - 0.5`, near `test_qt_quick_containers.cpp:1725`)
    passes either way, so it is not the control.
  - With `actions.gap: 12`, an overlay screen's two footer actions are 12 px apart. **Control:** today's `Row`, at 8.
- **Terminal:** the existing head tests pass unchanged. That is the claim, so the lane reports that they ran.
- **Mutations, each reported as failing:**
  - `buildHeadTrail` back to a `Row` with gap 1 (fails Qt 8 px and workspace `actionRow`);
  - the `ForEach` mount without the action-bar check (fails the mount test and the Qt tool-to-tool and
    action-to-action gaps);
  - the check made "any `ActionBar` is mounting" instead of "my parent is that bar" (fails the depth control);
  - the bar mounted without `setWrap` (fails mount `wrap` and the exact empty-parts edges: a non-wrapping row gives
    an empty, visible child a gap, measured by the review in `gap.out`);
  - `checkActionBar` still refusing `wrap` (fails the document test);
  - no footer refusal for a wrapping bar (fails the dialog case);
  - no lowering-time `gap` report (fails the computed-axis case);
  - `overlayActions` left a `Row` (fails the overlay footer at 12).

## S2: a rule under a table's header

### Decision

- **Token `color.table.headerRule`** (colour, default `#00000000`). It follows `color.table.header` along every path
  that colour takes:
  - `theme.cpp`'s `kColorProperties` sets `Theme.tableHeaderRule`, so `Theme.qml` declares
    `property color tableHeaderRule: "#00000000"`.
  - Each of `Theme.qml`'s seven built-in schemes binds `tableHeaderRule` beside `tableHeader`:
    - `themeScheme`, `pageScheme`, `cardScheme`, `sidebarScheme`, `toolbarScheme` and `barScheme` take
      `theme.tableHeaderRule`;
    - `toolbarJoinedScheme` takes `theme.barScheme.tableHeaderRule`, as it does for `tableHeader`.
  - `kSchemeProperties`, the map for surfaces a theme declares (`setSurfaces`), maps `color.table.headerRule` to the
    scheme role `tableHeaderRule`.
  - `MorphScheme.qml` declares the role, and `MorphButton.qml:154`'s role-by-role copy gains it.
  - The rule for the lane: grep `tableHeader` in `src/qt_quick/qml` and add the new role beside every hit.
- **Qt Quick:** `MorphTableHeaderBackground.qml` draws a 1 px `Rectangle` (`objectName: "headerRule"`) at its bottom,
  in `scheme.tableHeaderRule`, over the fill. This is how `MorphTableRowBackground` draws its row rule.
  - The rule lies inside the header's height, so no layout moves.
  - It is in the `tableHeader` part, so a restyle that replaces the part replaces the rule with it.
- **Why transparent by default:** the stock theme has no header fill either (`color.table.header` is transparent), so
  the stock look does not change. A theme that draws a header band says how it closes.
- **Why its own colour, not `divider`:** the mockups close the header in `#DDE1E6`, darker than their row rules
  (`#EDF0F3`, which is the gallery's `divider`). A header rule in `divider` would be lighter than the design's.
- **Terminal:** unchanged. It reads no table colours, and its header is set apart by its role. `tui/frontend.md` says
  the token is not read there.
- **Comment:** "the rule under the header, in the theme's `color.table.headerRule`, over the band's last pixel row".

### Spec

- `theme.md`: the token row: it maps to `Theme.tableHeaderRule`, and from there to every built-in scheme and every
  declared surface. Also the scheme map row.
- `qt_quick/backend.md:339`: the header background draws the fill and the rule.
- `tui/frontend.md`: the token is not read.

### Tests

- **Theme:** `color.table.headerRule` parses as a colour and is refused when it is not one, as `color.table.header`
  is.
- **Qt (discriminating):** with the token `#DDE1E6`:
  - a table in a panel with no surface (`cardScheme`, where every gallery table draws): the header's `headerRule`
    child is visible, 1 px high, has that colour, and its bottom is at the header's bottom;
  - the first row's `y` is unchanged from the same table without the token;
  - a table on a surface the theme declares, which sets its own `color.table.headerRule`: the rule has the surface's
    colour.
  - **Control:** with the stock theme its colour is transparent.
- **Mutations, each reported as failing:**
  - the rule bound to `scheme.divider`;
  - the rule anchored at the header's top;
  - `cardScheme` without the binding (fails the panel case);
  - `kSchemeProperties` without the entry (fails the declared-surface case).

## Lane S-S: screen items

| Item | Where | Change | Measure | When |
|---|---|---|---|---|
| U1 | receipt 390 footer; samples 390 bulk bar (`compactBulkBar`); samples' expanded bulk bar (`bulkBar`, `samples.cpp:341-366`); results and review expanded footers | Each becomes `ui::actionBar({…}, {.align = Center})` with the same children and sizing: `fixed(14)`/`stretch()` on receipt's buttons, the stretch label, message or note kept in place. No `gap` is written. The note and message stay inside the bar, as the stepper footer keeps its `extra`. `bulkBar` keeps its text, Select all, its stretch spacer and Clear as children | 8 px between each pair: receipt 16–123 / 132–373; samples' Assign and ···; samples 1000's Assign to worksheet…, Change priority… and Print labels (4 px today, measured by the review); results' Save and Submit; review's Return and Reject. The mockup's 10 px on samples' bar is the theme's gap to set, not this screen's. No new wrap of review's note at 1000 (about 148 px left for its 117 px line, inferred). **Control:** revert one site and see 4 px | now |
| U2 | receipt 1215 session table | Sample `ui::fixed(11)` → `ui::stretch()`, with `role = "mono"` kept. Each stretch column starts from its content and shares the spare width equally | Sample about 126 px; the badges end about 33–40 px from the pane's edge. At 1000 every badge is whole, at 902–962 or better. At 390, nothing elides. The column is stretch on the terminal too: run the gallery's terminal receipt tests and report whether any of them pins a column position | now |
| U3 | plate 390 run lines | `padding(withAssign ? 0 : 1, 1)` → `padding(withAssign ? 0 : 1, 0)`. Update the comment: the selected ring stands in the page's gutter | Assign… ends at 373. A selected marker's ring is whole at the leading edge (select A3, grab). **If the ring is clipped,** keep the inset, report the clip, and this item becomes an accepted difference with that measurement | now |
| U4 | the five heads S1 widens: samples, plate, console, audit and instrument, at 1215 and 1000 | No code unless a head gains a line. If plate's subtitle wraps at 1215, narrow the template tool by the fewest units that put it back on one line with the user name shown. If samples' head wraps at 1000 (about 12 px left, inferred), narrow its search tool (23 units) the same way | Every trail gap 8 px. Plate's head is 52 px and its subtitle is on one line. Console's head line is unchanged in height (A3). Samples', audit's and instrument's heads keep their height at 1000. At 1000, nothing clips | after S1 |
| U5 | gallery theme (`lims_gallery.cpp:62`) | Add `"headerRule": { "$value": "#DDE1E6" }` beside `header`. The gallery has no dark variant | Stability's header (a table in a card) has a `#DDE1E6` row at its bottom; so do review's, audit's and receipt's. If a gallery table sits on a surface the theme declares, measure it too | after S2 |
| U6 | all ten screens at 1215, 1000 and 390, plus 1920 for receipt, audit, stability and review | Re-grab everything | The 1000 rule; the trail gaps of samples, plate, console, audit and instrument on their grabs; no head grew a line unless U4 had to act; e-signature as an overlay, its footer actions 8 px apart | after S-F |

## Accepted, no work

A1 to A14 still stand.

| # | Difference | Reason |
|---|---|---|
| A15 | Instrument 390, after R2: the third tag wraps, and the head grows by 22 px | The same mechanism as A14. The mockup's phone head has no menu square, so its text box is 356 px wide; ours keeps the navigation there and the page's 16 px gutter, which leaves 318 px for a 322 px line. Avoiding it would mean shortening the tag's text or tightening the badges' gap, which is shared by every head |
| A16 | Stability: the date field draws a picker glyph that the mockup lacks | Batch K kept the calendar button: the date can be picked without typing it |

## Lanes

- **S-F**, one commit per item:
  1. **S1:** `view.hpp` (`ActionBar::wrap`), `author.hpp` (`ActionBarOptions::wrap`), `document.hpp`,
     `interpreter.hpp` (lowering `wrap`, the lowering-time `gap` report), `mount.hpp` (the direct-child rule and the
     footer refusal), `workspace.hpp` (`buildHeadTrail`, `overlayActions`), plus the specs and tests above.
  2. **S2:** `theme.hpp`, `src/qt_quick/theme.cpp` (`kColorProperties` and `kSchemeProperties`), `Theme.qml` (the
     property and the seven schemes), `MorphScheme.qml`, `MorphButton.qml`, `MorphTableHeaderBackground.qml`, plus
     the specs and tests above.
  - Neither item needs Qt newer than 6.8: S1 changes no QML, and S2 adds one anchored `Rectangle` with an
    `objectName`. The tests use `findChild` and geometry.
  - Run the docs target under clang with `WARN_AS_ERROR`.
- **S-S:**
  - U1 to U3 now, in parallel with S-F. They touch only the gallery and its tests.
  - U4 and U5 after rebasing on S-F.
  - U6 last.
- **Shared files:** none. U5 uses the token only after the rebase.

## Risks

- **S1 widens every expanded head with several trail items by 4 px per gap.** Lines near their limit can wrap. U4 and
  U6 measure them. Terminal geometry does not move.
- **S1 changes how a document lays out** an `actionBar` that holds a horizontal `forEach`: its rows gain the action gap
  on Qt and one column on the terminal. A document that set `gap` on such a `forEach` is now refused. The review found
  none in `examples/`, `tests/` or `docs/` (read); the lane confirms with a grep.
- **A wrapping `actionBar` inside an overlay's content is now refused at mount.** No document has one: `wrap` was
  refused everywhere until this batch.
- **S2 changes nothing until a theme sets the token.**

## Review round 2

The review (`lg-s-review.md`: 1 blocker, 6 should-fixes) is taken in full.

| Item | Change |
|---|---|
| B1: the header rule never reaches a table in a card | S2's decision now names every path: `kColorProperties` → `Theme.tableHeaderRule`; the seven `Theme.qml` schemes (`toolbarJoinedScheme` from `barScheme`); `kSchemeProperties` for declared surfaces; `MorphScheme.qml`; and `MorphButton.qml:154`. The lane rule is to grep `tableHeader` in `src/qt_quick/qml`. The Qt tests now cover a table in a panel (`cardScheme`) and one on a declared surface. The mutations are those two bindings. The non-existent dark-scheme case and U5's dark-scheme sentence are gone. `Theme.qml` and `MorphButton.qml` are in S-F's file list |
| S1: the overlay footer is outside the rule | `overlayActions` becomes an `ActionBar`. Added to the S1 decision, `workspace.md`, the workspace and Qt tests (footer at 12 px with `actions.gap: 12`), a mutation, and U6 (e-signature as an overlay) |
| S2: `wrap` in an overlay's footer | Refused at mount, with a message, rather than defined. A footer sizes the bar to its one-line width, so a wrapping bar there has no width to wrap at, and nothing in the mockups wants one. Covered by a mount test with an outside-the-dialog control, a mutation, and a risk line |
| S3: a computed `axis` gets past the `gap` refusal | `gap` is reported at load for a literal horizontal axis, and at lowering for a computed axis that evaluates to horizontal, as `wrap` is. The `actionBar`'s lowering does the check. `view_tree.md` says a C++ `ForEach`'s `gap` is not read there. A document test and a mutation are added |
| S4: test and mutation corrections | (a) A depth control (a `ForEach` in a `Row` in an `ActionBar`) and an "any depth" mutation. (b) The Qt head now has two tools, and the mutation's failure names the tool and action gaps. (c) The empty-parts check is exact: the first and last action's edges against the trail's edges, within 0.5 px; the existing loose check is named as not being the control. (d) The mount test's `ForEach` has gap 0, so step 2 shows on the terminal as well |
| S5: five heads grow, not two | Consequences lists all five, with their growth and room. U4 measures samples, plate, console, audit and instrument at 1215 and 1000, and has a fallback for samples at 1000: narrow the search tool. U6 names the five |
| S6 (coordinator's decision): samples' desktop bulk bar | `bulkBar` (`samples.cpp:341-366`) joins U1 and keeps its text, Select all, spacer and Clear as children. Measure 8 px between its three framed buttons |
| Also taken | U2: report whether terminal receipt tests pin a column position. U3: the "only `MorphScroll` clips" sentence is corrected. U5: also measure a table on a declared surface, if the gallery has one. Qt 6.8: one line in Lanes |

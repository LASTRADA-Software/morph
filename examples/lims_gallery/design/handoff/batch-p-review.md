# Review of batch P design (`lg-p-design.md`)

I checked the design against `lg-fold-o` and against `/home/yaraslau/repo/morph-lims-e2` (`lg-os`, `d0e2013be`), reading
only. I did not build anything in either, and I edited nothing in either. Paths below are relative to that worktree
unless they start with `$S/`.
- Pixel numbers are **measured** with PIL on `$S/lg-cmp/app-o/` and `$S/lg-cmp/mock/`.
- Everything else is **read** from code, or **inferred** where I say so.
- The one run I attempted was the design's `GridLayout` probe on Qt 6.9.3 (`yaraslaut/static_qt:24.04-qt6.9.3`). It did
  not start: `libxkbcommon-x11.so.0` is missing in the image. So nothing here was run on a Qt older than 6.12.

**Verdict.** The five framework items are the right shape, and four of them are the right general vocabulary:
- **P4** (equal columns) is a real defect, and the right fix is "`columns` means `stretch(1)` tracks".
- **P1**, **P2** and **P5** are general.
- **P3** works, but its name says the opposite of what the gallery does with it (S5).

As written, P4 is not ready. Its natural width disagrees with the terminal's. That squeezes the content of every
content-sized `columns` grid on Qt, including one in a shipped example, and the planned conformance case cannot see it
(B1).

**Counts: 1 blocker, 12 should-fixes.** My should-fixes are S1 to S12. The design's own screen items are written "item S1" to "item S15".

**Note for the dispatcher (not a design defect).** `morph-lims-e2` has an uncommitted change to
`examples/lims_gallery/src/screens/instrument.cpp`, made at 00:09, two minutes after the design. It is items S1 and S3
already applied: `qualificationList` has `gap = 2, divided`, and all four cards have `padding = 3`. Someone is working
in e2. If that is not lane P-S, the change is stray. The design's claim that e2 equals `lg-os` holds only for the files
it quotes. Separately, `lg-fold-o` now points at `3150951bf`, a test-only commit after `74bb31a79`, which the grabs were
made on.

---

## Premises I verified

| Design claim | Status | Evidence |
|---|---|---|
| Q1: receipt's Condition column 2 at x 602 (1215) and x 998 (1920) | measured, holds | Checkbox fill starts at 604 and 1000, two px inside the box edge, which puts the box edges at 602 and 998. Column 1's box fill starts at 246, so the box edge is at 244. |
| Q1: `MorphGrid` uses a `GridLayout` without tracks; `place()` returns early | holds (read) | `MorphGrid.qml`: `content: tracked ? free : layout`; `place()` starts with `if (!root.tracked) return` |
| Q1: terminal `planGrid(int, …)` makes equal columns | holds (read) | `layout.cpp:329-333` gives `stretch(1)` with natural 0; `distribute` → `shareCapped` shares by weight only (`layout.cpp:91-121`, `:162-182`) |
| Q2: e-signature caption column about 58 px; design 96 | measured, holds | Record row: caption ink starts at 389 and value ink at 446, so 57 px. The mock: 334 and 439, so 105 px (96 plus a gap of about 9). |
| Q2: instrument values about 90 px after captions; design 130 | measured, holds | Model row: 241 to 331 is 90 px. The mock: 75 to 218 is 143 px (130 plus a 13 px gap). |
| Q2: `MorphKeyValue` caption column is the widest caption; no key | holds (read) | `MorphKeyValue.qml`: `NoWrap`, `Layout.minimumWidth: implicitWidth`; terminal `captionWidth()` is the widest label (`display_widgets.cpp:484-491`); `kKeyValueKeys` is `items`, `fieldLayout` (`document.hpp:1055`) |
| Q3: small button keeps the control height | holds (read) | `control_shape.hpp:30`; `MorphButtonBackground.qml:41` `implicitHeight: Theme.targetHeight` |
| Q3: only the plate's wells are `size: small` | holds (read) | the only `ButtonSize::Small` outside the library is `plate.cpp:369`, plus the conformance case at `backend_conformance.hpp:3369` |
| Q4: phone controls are 14 px in the mockups | holds (read) | `.btn.lg{height:44px;font-size:14px}` appears in 10 files and `.input.lg{…14px}` in 5. The design says "seven files"; the count is off, but the claim holds. |
| Q5: muted 12 px notes recur | holds, and recurs **more** than the design lists | `class="muted" style="font-size: 12px"`: review's self-review note **and** "Approving needs your signature"; console's four KPI notes, its three compact KPI notes and "mean 100.0 % · SD 0.4"; instrument's "(needs reason)" and its compact stacked key-value captions; plate's "dragging". See S9. |
| Q6: heads are 53 px, console's design 49 | measured, holds | the head rule is at y 53 on console-1215 and plate-1215; `10-Console.dc.html` has `header style="height: 48px"` |
| Cited line numbers | hold | `document.hpp:4617` (`checkFormColumns`), `:4232` (`kRoles`, 11 entries), `interpreter.hpp:8074`, `backend_contract.md:186`, `:375`, `:482`, `document.md:267`, `qt_quick/backend.md:169`, `MorphWidget.qml:253-284`, `MorphSelect.qml:55`, `:316`, `MorphDateTimeInput.qml:73` |

---

## Blockers

### B1. P4's natural width is the sum of the columns' naturals, not the terminal's columns × widest, so a content-sized grid squeezes its own cells

**Read.** `place()` computes the grid's natural width as `natural += track.kind === 2 ? naturals[t] : w`, where
`naturals[t]` is the widest one-track cell in column t. A spanning cell is not counted. The design makes `place()` lay
out every grid, so this becomes `free.implicitWidth` for a grid without tracks too.

The terminal's natural width for `columns` is different: `columns × unit + gaps`. Here `unit` is the widest cell's
width, and a spanning cell counts as its width divided by its span (`container_widgets.cpp`,
`GridImpl::naturalSize`, the branch after `if (!_tracks.empty())`).

**Inferred** (arithmetic on the code above). A grid that is given its natural width is then split into equal columns.
Any column wider than the average gets less than its content needs:
- **`examples/gallery/src/gallery.cpp:91`:** `ui::row({ui::grid({text "Cell", spacer}, {.gap = 1, .columns = 2}), …})`.
  In a row, a content-sized child gets its implicit width.
  - Qt today: about `n("Cell") + 4` px. After P4, "Cell" gets `n/2` and is cut or broken.
  - The terminal: `2 × 4 + gap` cells. "Cell" stays whole.
- **The forms engine** (`include/morph/forms/engine/form_view.hpp:493-503`, `groupGrid`). Every form, collection and
  wizard with `columns` is a `columns` grid, and its fields span with `x-colspan`. In a dialog with no fixed width,
  `MorphDialog` takes `min(implicitWidth, cap)` (`MorphDialog.qml:134`). So a form in such a dialog is squeezed the
  same way, and a spanning field's width is missing from the natural width altogether.
- **Tracked grids** already have this: a content-sized grid of `stretch` tracks. Both frontends agree there, so it is
  out of scope. P4 extends it to every `columns` grid on Qt alone.

**Why the tests miss it.**
- The conformance case uses a `fixed(60)` grid.
- The existing Qt tests use a root that fills an 800 px window.
- The gallery render tests check for warnings, not geometry.

Every check in the design passes with this defect in place.

**Fix:**
- **`MorphGrid.qml`.** When `tracks` is empty, the natural width is `columns × unit + gap × (columns − 1)`. `unit` is
  the largest of each shown cell's `(naturalWidth − gap × (span − 1)) / span`, rounded up, which is exactly
  `GridImpl::naturalSize`. State it in the header comment and in `qt_quick/backend.md:169`.
- **Conformance, a second case** (`gridColumnsNaturalIsWidestTimesColumns`): a content-sized `columns: 2` grid, gap 0,
  in a column with `align: start`, holding `sizedText("a")` and a 10-character `sizedText`.
  - Assert that the grid is 20 units wide within one.
  - Assert that the second cell's width is at least 10, which checks its text is not cut.
  - Control: the Σ-naturals rule gives 11 and 5.5, so the case fails.
  - Add a span case to the Qt test: a span-2 cell of 30 characters in a content-sized two-column grid makes it 30
    units wide.
- **Risks.** Name `examples/gallery` and the forms engine (`form`, `collection` and `wizard` with `columns`, and
  `examples/forms`). P-F's "every example's render tests" does not measure them, so P-F hands back a grab of the
  gallery example's Containers page and of a forms example with `columns` greater than 1.

---

## Should-fixes

### S1. The terminal control for P4 would pass with the mutation in place

The design says "terminal: `planGrid(int, …)` given the cells' natural widths fails the same assertion". It does not.
`distribute` → `shareCapped` starts every stretch item at 0 and shares the spare by weight; `Item::natural` is never
read for a stretch item (read: `layout.cpp:91-121`, `:162-171`). Naturals 1 and 30 on two `stretch(1)` items in 60 still
give 30 and 30, so the mutant survives.

**Fix:** the terminal control is `planGrid(int, …)` building `content()` items with the cells' naturals, which gives 1
and 30. Or drop the terminal control, and say that the terminal is the reference here, not under test.

### S2. P4's gallery acceptance is computed with the mockup's gap, not the grid's

Item S15 and the P4 tests say column 2 starts "within 2 px of 568 (1215) and of 917 (1920)". The receipt grid is `gap = 1`,
one step. The gallery sets no `space` token, so a step is 4 px.
- **Measured** at gap 2: the three-field row is 244–450, then 458, so two steps are 8 px.
- **Measured:** the grid spans 244–877 at 1215 and 244–1582 at 1920.

So equal columns start column 2 at 244 + (634 − 4) / 2 + 4 = **563** at 1215, and 244 + (1339 − 4) / 2 + 4 ≈ **916**
at 1920.

The 568 figure assumes the mockup's wider gap of about 14 px (in the mock, 389 − 76 = 313 in a 612 px grid). A correct
P4 fails the 1215 acceptance by about 5 px.

**Fix:** accept 563 ± 2 and 916 ± 2. If the mockup's position is the target, that is a screen gap change (step 3 is
12 px, giving about 567). State that as a separate item, not as P4's acceptance.

### S3. P3 shrinks the phone's well rows that have no Assign button, and item S12 says "no screen change"

**Read.** Since batch O (`ab797fe49`), a compact run line is `padding(0, 1)`, so "rows are as tall as their wells". The
row holds the well, the line and an Assign button that shows only when the well is empty (`.visible(empty)`).
- With Assign shown, the 44 px button keeps the row at 44, so the pitch is 45.
- Without it (A1, A2, A9 in row A), the row is the well's height.

**Inferred.** With `control.smallTouchHeight` at 36, those rows drop to about 37 px apart while the rows around them
stay at 45. The mockup's rows are 45 apart whether or not they have an Assign button (`07-Plate.png`: A1 at 256, A2 at
301).

**Fix:** item S12 also sets the run line's vertical padding back to one step. With a 36 px well that gives 36 + 8 = 44, plus
the rule, a 45 px pitch. Add the phone pitch of a row without Assign to item S12's measurements.

### S4. P3 at 1000: the well becomes taller than it is wide

Item S12 says "if a track is under 40 px, the well narrows into a stadium".
- **Measured** on plate-1000: the well pitch is about 39.4 px (596, 635, 675, 714, …), so a stretch track is about 35
  px (the pitch less the 4 px gap).
- **Read:** `place()` gives a label-width cell `min(natural, room)`, and the grid's `align: Center` keeps its natural
  height.

So each well is about 35 × 40: a vertical capsule, not a stadium and not a circle. The selected rings (3 px outside)
then overlap the next well's.

**Fix:** state the expected 35 × 40 in item S12, and decide it before the lane starts. Either accept it as a fit difference
at 785 px, or keep the desktop well at the track's width. One way to do the second: at 1000 the well's height is no
more than its track, which needs a rule in `place()`, not a screen workaround. Do not leave it to "a fit note".

### S5. P3's name inverts its meaning, and the gallery sets it for every small button

`ButtonSize::Small` is "a dense button" (`control_shape.hpp:30`). The gallery would set `control.smallHeight` 40, above
`control.height` 30. A "small" button then becomes taller than a normal one on the desktop, and shorter on the phone.

Only the wells are `size: small` today (premises table). But the token is theme-wide, so the next small button in the
gallery — a table row action, say — becomes 40 px without anyone choosing it.

**Fix (pick one, and record it):**
- (a) Keep the tokens. In `theme.md` and in `control_shape.hpp`'s Doxygen, say that the small height is independent of
  `control.height` and may exceed it. Add to Risks that the gallery's 40 px applies to every future small button.
- (b) Scope the height to what the well is: a surface. The wells already sit on the surfaces `standard`, `control`,
  `blank`, `sample` and `empty`. A surface that can carry `control.height`/`control.touchHeight` would put the height
  on the wells alone. This is larger, since surfaces carry colours today.

(a) is the small change and is acceptable, but the inversion must be written down, not discovered.

### S6. P1 names one font, but buttons and segments need strong variants at the touch size

- **Read:** `MorphButton.captionFont` returns `Theme.buttonFont` (body size, `buttonWeight`) for a strong caption. That
  covers primary and danger buttons, which are most phone footer buttons.
- **Read:** `MorphSelect.qml:316` gives the marked segment `Theme.strongFont`.

The design adds only `controlTextFont` at `Font.Normal`, and says "with `buttonFont`'s weight kept" without naming the
font that does it. Done literally:
- a filled button either loses its weight or stays 13 px;
- the marked segment stays 13 px strong beside 14 px neighbours.

**Fix.** Add `Theme.controlFontOf(weight)`: `bodyFont`/`strongFont`/`buttonFont` while `controlTextPixels` equals
`bodyPixels`, else `fontOf(controlTextPixels, weight)`. Name its three uses: the normal caption, the strong caption and
the marked segment. Add a strong (primary) button and a marked segment to the P1 test.

Two smaller points:
- The input's unit label (`MorphTextInput.qml:127-136`) sets no font. Say whether it follows; the prefix does, through
  `textFont`.
- A radio-style select's labels (`MorphSelect.qml:218`) are not in either list. Put them with the check boxes.

### S7. P2 should share its name with the forms engine's caption column, which already exists

The forms engine already has this concept: `ViewOptions::inlineLabelWidth`, in units, default 16, at least 1, else it
throws (`form_view.hpp:60-62`, `:417`, `:521-524`). It is the caption column of `fieldLayout: inline`, the same layout
name `keyValue` borrows. `form`, `collection` and `wizard` take `fieldLayout` in documents but have no width key.

Under "a key means the same thing on every kind that takes it":
- the design should say that `captionWidth` is the document spelling of that concept;
- `form`/`collection`/`wizard` should take it too, mapped to `inlineLabelWidth`, or the design should record that they
  will and why not now.

Otherwise there are two spellings of one rule, and the forms version has no wrap rule written down. It is a `fixed`
text, so on Qt it wraps; the design should state that this matches P2.

### S8. P2: the view-tree path and two document details

- **Vocal over silent.** The refusal of `captionWidth` with `stacked` exists only in the document checker. A C++
  `view::KeyValue{.captions = Above, .captionWidth = 12}` reaches the mount unchecked. Say what happens: the mount
  throws or reports it, as `setCaptionPlacement`'s neighbours do. Silently ignoring it is not an option.
- **`kKeyValueKeys` (`document.hpp:1055`)** must gain `captionWidth`, with the shape the literal-integer check expects.
  Otherwise the key is refused as unknown before `checkFormColumns`'s twin runs. Add it to P2's file list.
- **Test that passes if P2 did nothing.** "The value labels' `x` are equal across the pairs" holds today, since the
  `GridLayout` aligns column 1. Keep it as a guard and label it so; the width check is the discriminating one.

### S9. P5 is justified by three recurring sites but applied to one

Q5 cites console's KPI notes and instrument's menu hint as evidence. Lane P-S then applies `caption` only to review's
self-review note (item S14).

**Read**, in the code at `lg-os`:
- console's KPI notes are `layout::muted(note)` at body size (`console.cpp:487`);
- so is "mean 100.0 % · SD 0.4" (`console.cpp:524`);
- review's "Approving needs your signature" is also 12 px muted in `09-Review.dc.html`.

**Fix:**
- Add screen items after P5 for the console KPI notes (expanded and compact), the control-chart note, and review's
  signature note.
- Record the instrument menu hint as outside P5: it is a menu entry's description, not a text node.
- Record the instrument phone's stacked key-value captions (12 px muted in the mockup; `MorphKeyValue` draws stacked
  captions in `bodyFont`) as a separate L finding. It is **read** from the mockup HTML, not measured.

### S10. P5 omits a silent switch

`recording_backend.hpp:292-316`, `enumName(TextRole)`, ends in `default: return "unknown"`. A new enumerator compiles
without a warning and records as "unknown". The terminal's `roleStyle` (`leaf_widgets.cpp:30-48`) is an `if` chain that
falls through to `textNormal`. P5's terminal control covers that one; nothing covers the recording backend.

**Fix:** add `Caption` to `enumName`, with a `RecordingBackend` assertion that a caption text records `Caption`. Name
both files in P5's list.

### S11. P4: a gap or theme change after mount no longer re-places the grid

**Read.** The `GridLayout` bound `columnSpacing: Theme.spaceOf(root.gap)`, so it followed both the gap and the theme.
`place()` runs only on `width`, `tracks`, `children` and the C++ size signals, plus the `columns` signal P4 adds.
- `setGap` (`containers.cpp:114`) only writes the property.
- A theme re-applied at run time (`space`, or `font.size.body` through `unitWidth`) changes no width, so no grid
  re-places.

Tracked grids already have this gap in the code; P4 extends it to every grid.

**Fix:** `onGapChanged: place()`, and a `Connections` on `Theme` for `unitWidthChanged` and `unitHeightChanged` (and
on the spacing step, if that is a property). Add a Qt test: `setGap(3)` after the children are adopted moves the
second cell.

### S12. Smaller accuracy fixes

- **`kinds.hpp:344-366`** says "A grid with tracks places its cells itself" and "a grid without them leaves it to its
  GridLayout". The design says `GridImpl` is unchanged, but these two Doxygen comments go stale under P4. Add them to
  P4's file list.
- **P1's Qt theme test.** Without `font.family`, `bodyFont` is the platform font, not `fontOf(bodyPixels)`
  (`Theme.qml:66`). The "all of them are 13" assertions then depend on the platform's pixel size. Set a family in the
  test theme, or compare against `Theme.bodyFont.pixelSize`.
- **Item S15's re-grab list** should include 390 for the two compact `columns` grids:
  - receipt's containers/courier pair (`receipt.cpp:449`);
  - stability's three-column month boxes (`stability.cpp:628`).
- **Qt 6.8.1.** None of P1 to P5 uses an API newer than 6.8, as read: `Qt.font`, `Label.wrapMode`, `Layout.*`, plain
  JS layout. But P4's mutation control (the `GridLayout` restored) runs only locally on 6.12. The issue's verdict
  condition ("passes on 6.8.1 without the fix") therefore stays unverified after P-F. Say so in the issue, or run the
  plain-Qt probe in the CI toolchain image. The 6.9.3 image here lacks `libxkbcommon-x11`, so it could not run the
  probe.

---

## Answers to the focus questions

| Question | Answer |
|---|---|
| Premises re-measured | They hold (table above). The one exception is P4's acceptance arithmetic (S2) |
| P1 `control.touchTextSize`: right vocabulary? | Yes: a touch-class token beside `control.touchHeight`, not a per-node key. Needs the strong variants (S6) |
| P2 `keyValue.captionWidth` | Yes, in units, as `fixed` widths are. It should be one spelling with the forms engine's `inlineLabelWidth` (S7) and refuse vocally on the view-tree path (S8). The 12, 13 and 17 units come to about 108, 115 and 146 px including the 2-unit spacing, against the mockups' 105, about 112 and 143: within 3 px (measured for e-signature and instrument) |
| P3 `control.smallHeight` | It works, and fixed sizing truly cannot say 40 or 36 px or a circle (read). The name inverts its meaning and the token is theme-wide (S5). It causes a phone row-pitch regression (S3) and 35 × 40 wells at 1000 (S4) |
| P4 equal columns | The right rule and a real defect. The natural width is wrong (B1), and the terminal control does not discriminate (S1) |
| P5 `caption` role | Right. Apply it to every site the evidence names (S9), and cover the recording backend (S10) |
| Would each test fail if the feature did nothing? | P1, P3 and P5: yes. P2: yes, except the `x`-equality check (S8). P4: the Qt case yes; the terminal control no (S1); no case covers the natural width (B1) |
| Risks to other screens and examples | P4: the gallery example's Containers row and every forms-engine form with `columns` (B1); runtime gap and theme changes (S11). P1: phone buttons and segments about 7 % wider, which item S13 checks; the strong-font gap (S6). P3: every future small button in the gallery (S5) |
| Qt 6.8.1 | Nothing in the items needs a newer Qt (read). P4's version question stays open (S12) |
| Screen items | Items S1 and S3 are already applied, uncommitted, in e2 (the note above). Item S12 needs S3's padding and S4's decision. Item S15 needs S2's numbers and the 390 grabs (S12). Items S2, S8 and S10 are fine as stated |
| Accepted items | A1 to A6 are sound. **A3:** heads are 53 px on every screen (measured on console and plate). Console's design alone is a fixed 48 px header around a 15 px title, so `head.paddingY` cannot serve it without moving the eight padded designs. Accept |

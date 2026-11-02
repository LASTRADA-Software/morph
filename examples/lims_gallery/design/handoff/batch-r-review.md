# Review of batch R design (`lg-r-design.md`)

I checked the design against `lg-fold-q` (`dcefd3d15`) in `/home/yaraslau/repo/morph-lims-e2`, reading only, with
`git show lg-fold-q:<path>`. I built nothing and edited nothing in either repository, and I did not open
`morph-lims-e1`. Paths below are relative to that worktree unless they start with `$S/`.
- **Measured** pixel numbers are PIL reads of `$S/lg-cmp/app-q/`; crops are in `$S/lgr-rev-px/`.
- **Measured** probe results are plain QML on Qt 6.12.0 offscreen (`qml` and `qmltestrunner`, with
  `QT_FORCE_STDERR_LOGGING=1`), in `$S/lgr-rev-r1/` (R1) and `$S/lgr-rev-r4/` (R4). Each output file is next to its
  probe.
- Everything else is **read** from code, or **inferred** where I say so. Nothing ran on Qt 6.8.1: no 6.8 build is on
  this machine.

**Verdict.**
- **R4** is right with one change: align baselines, not tops (S7).
- **R2** is right for the gallery. Its condition is a proxy (S5), it misses one call (S4), and A14 understates the
  consequence.
- **R5** is the right tool for receipt, and T9's arithmetic holds against the grabs. On the terminal, the grid's natural
  height does not follow the columns shown, and no test would notice (B2).
- **R1** is the right rule. The overlay design breaks wherever a selection outlives the focus, and I reproduced that
  case (B1). The terminal's marks overdraw the "…" (S1).
- **R3** gets the gallery's look, but as a C++ constant that ui/1 documents and themes cannot see. Its reason for
  rejecting a theme value contradicts three existing tokens (S6).

**Counts: 2 blockers, 9 should-fixes** (S1 to S9).

---

## Premises I verified

| Design claim | Status | Evidence |
|---|---|---|
| R1: a read-only `TextField` clips with no mark | reproduced, holds | `$S/lgr-exp/readonly.out`, and the same in my probe (`elide.out`: contentWidth 145.98 against a 134 px room) |
| R1: receipt 1000 shows "Release, finished produc" | measured, holds | `rc1000.png` crop: Sample type cut at the field's right edge, no "…" |
| R1: Qt inputs draw their text in a `TextField` with `autoScroll: activeFocus` | read, holds | `MorphTextInput.qml:76`, `MorphDateTimeInput.qml:77`. Neither has an `inputMask`, so `text` is what is shown |
| R1: the terminal routes through `paintField` | read, holds for `TextInputImpl`. `DateTimeInputImpl::paintControl` calls `paintField` directly (`field_widgets.cpp:510`), so it needs its own route to the shared branch |
| R1: masked text is excluded | read, holds | Password is `TextInputMode::Password` (`field_widgets.hpp:85`), so "mode single-line" already excludes it |
| R2: title ink 57–382, line end 386 | measured, holds | My read: eyebrow 56–362, title 56–382, menu glyph 19–31. Line end = 390 − step 1 (`_topLine->setGap(1)` compact) |
| R2: the tags under the title | measured, **not in the design** | Badge boxes span 56–376 (rows 54–70). See S5 |
| R2: console's one-line head keeps step 1 | measured, holds | `co390-head.png`: the trailing scan icon is a ghost button whose glyph ends about 22 px from the edge |
| R3: stability 390, Back and Continue 4 px apart | measured, holds | Back's border at x 80, Continue's fill from 85 (y 800) |
| R3: the mockups space button pairs 8 px apart | read, holds | `06-Stability.dc.html:89,117`, `04-Audit.dc.html:97-98`, `09-Review.dc.html:76`. The mockups also use `gap: 8px` for most non-action rows (`.ck`, `.ss`, `.ins`, console's header) |
| R3: `stepperParts` footer at gap 1, the ActionBar at step 2 | read, holds | `author.hpp:5145`, `mount.hpp:1953` |
| R4: captions centred on two-line values | measured, holds | `st1215-kv.png`: Product, First pull and Containers are centred on two lines |
| R4: the terminal draws each pair on one row | read, holds | `KeyValueImpl::paint` (`display_widgets.cpp:524-545`). The value is never wrapped, so it has no second line |
| R4: forms are not affected | read, holds | `keyValue` is used only by `examples/gallery`, esignature, instrument (twice) and stability. No forms code uses it |
| R5: 1000 and 1215 are both `expanded` | read, holds | `unit.out`: 130 and 158 units |
| T9: 25 and 32 units give 2/1 columns at 1000 and 3/2 at 1215 | measured positions, inferred count | Card content is 244–729 (486 px) at 1000 and 244–878 (635 px) at 1215. Fields: floor(494/199.8) = 2 and floor(643/199.8) = 3. Checks: floor(490/249.5) = 1 and floor(639/249.5) = 2. At 33 % the 1215 card narrows by about 20 px, which still gives 3 columns (23 px margin) |
| A13: Results' `td` is 4 px; the others are 5–7 | read, holds | `08-Results.dc.html:35`; Receipt, Review and Console 6, Samples 5, Audit 7, Stability 5 |

---

## Blockers

### B1. R1's overlay draws over a selection that outlives the focus

**Measured** (`$S/lgr-rev-r1/tst_ctx.qml`, `ctx.out`, `ctx.png`). I built the design's overlay as specified:
- a `Label` at the left padding with `elide: ElideRight`;
- the field's `color` transparent while `shown`;
- `shown = !activeFocus && echoMode === Normal && overflow`.

Then I gave the field the focus, selected "inished product", and right-clicked:

```
before: sel "inished product" persist false
after right-click: activeFocus false sel "inished product" shown true
```

The grab draws the overlay's "Release, finished pr…" with the editor's selection highlight and its white selected
glyphs ("odu") on top of it, at the editor's scrolled position. Two texts are drawn over each other.

**Why.**
- `color: "transparent"` hides only the unselected glyphs. `selectionColor` and `selectedTextColor` still paint.
- On Qt 6.12, a `TextField` right-click opens its context menu, which takes the active focus and leaves the selection
  in place. That is the read-only field's most common interaction: copying the value.
- **Inferred, for 6.8.1**, which has no default context menu: `QQuickTextInput` keeps its selection when the focus
  leaves for a popup or because the window deactivates. That covers the date input's own picker popup and an alt-tab
  away from a selected field. I have not run this on 6.8.

**The design's claim** that "selection … [is] unchanged" holds for the model. It does not hold for the drawing.

**Fix:**
- `shown` also requires `field.selectionStart === field.selectionEnd`. A field holding a selection shows the editor,
  as it does with the focus.
- **Test (discriminating):**
  - focus the field, `select(…)`, then move the focus to a `Popup` with `focus: true`;
  - assert the overlay is hidden and the field's colour is the scheme's text colour;
  - **control:** the design's `shown` fails this.
- Add the mutation "no selection term".

### B2. R5 on the terminal: the grid's natural height stays at the all-columns height

**Read.**
- `GridImpl::naturalSize` (`container_widgets.cpp:391-421`) computes the height by `planGrid(columns, …)` at the
  all-columns width.
- The design keeps that ("`naturalSize` keeps `columns`") and changes only `planOf`, which `childAreas` uses.
- A parent stack therefore gives the grid the height of the all-columns plan. When the columns drop, `childAreas`
  places the extra rows below that height: they are cut, or they are drawn over the next sibling.
- The contract sentence the design writes ("its natural height follows the columns shown") is true on Qt, where
  `place()` sets `free.implicitHeight` from the rows it placed. It is false on the terminal.

**The tests do not catch it.** The three conformance cases assert only the cells' x and y ("cell 2 is under cell 0").
That still holds when cell 2 lies outside the grid's area.

**Fix:**
- Do what the wrapping `StackImpl` already does (`container_widgets.cpp:101-130, 225-229`):
  - keep the width natural at `columns`;
  - compute the height at `_drawnWidth` when the key is set and a width has been drawn;
  - in `paint`, update `_drawnWidth` and `refresh()` when the width changes.
- State that a nested grid settles one frame per level, as wrapping rows do.
- **Conformance case (`gridDropGrowsDown`), both frontends:**
  - a `fixed(25)` column of the grid (`columns: 3`, `minColumnWidth: 10`), followed by a `sizedText` sibling;
  - assert the sibling's y is at least the bottom of the grid's last cell;
  - **control:** today's `naturalSize` fails it on the terminal.

---

## Should-fixes

### S1. R1, terminal: the field's marks overdraw the "…"

**Read.**
- Without a unit, `paintText` gets a canvas as wide as the control (`field_widgets.cpp:390-391`).
- `paintMarks(control)` then draws `*`, `!` or `~` over the control's last cells (`widget.hpp:396`, `:491`), and
  `paintField` puts `!` on the last cell when the field is invalid.
- So an uncaptioned required, invalid, toned or stale field loses its "…" to the mark, and the cut is silent again.

**Fix:**
- Elide to `room − marksWidth()`.
- Add a terminal test: an uncaptioned required input whose value is too long paints "…" before its `*`.

### S2. R1, the tooltip on the terminal: the design states it for both frontends and builds it on one

**Read.**
- The terminal shows hover tooltips: `Hosted::onHover` returns `hoverText()` (`widget.hpp:316-325`), and `TuiField`
  adds its messages.
- The design's rule ("its tooltip is the full text, … on both frontends") is not in its terminal section.
- `hoverText()` is `const` and does not know the painted width, so building it would need paint-time state.
- A terminal elided `Text` has no full-text hover either (`leaf_widgets.cpp:235`). A focused input shows its whole
  value.

**Fix:** say the tooltip is Qt Quick's, as `MorphText`'s is. On the terminal the whole value is one Tab away. Put that
sentence in `tui/frontend.md`'s TextInput row.

### S3. R1, Qt: pin the overlay's geometry and colour to the field's, and make the tests readable

**Read and measured:**
- **The test as written is not readable.** "Its shown text ends in '…'" cannot be read: a `Label` exposes `truncated`,
  not the text it draws. Assert `truncated === true`, and the field's `color` transparent.
- **The overflow test.** `field.contentWidth` equals the `FontMetrics` advance, with tabular figures as well
  (`elide.out`: 145.98 = 145.98; tnum 126.33 = 126.33). Use `contentWidth`: it needs no second font binding that could
  drift from `textFont`.
- **The vertical position.** The design centres the label vertically on the field. The editor's text sits in
  `topPadding` / `availableHeight` with the field's `verticalAlignment`. Give the label that same box, and assert that
  `label.y + label.baselineOffset` equals `field.baselineOffset` within 0.5 px. Without that, a style with asymmetric
  padding moves the text by a pixel or two the moment the field takes the focus.
- **The colour.** Bind the label's colour to the same expression as the field's (one `textColor` property), not a
  restated `scheme.text`.
- **Window deactivation (inferred).** It clears `activeFocus`, so a field being edited shows its elided form while the
  window is in the background. State this as intended.
- **A note, not a fix (measured, `unfocused-scrolled-long.png`).** Today an unfocused field that was edited at its end
  stays scrolled to its tail, which contradicts the existing comment "a field not being edited shows its text from the
  start". R1's overlay makes that comment true wherever it matters, because a field can scroll only when its text
  overflows. A click maps to the start of the text (cursor 3 at x 30), so the overlay and the click agree.

**Value leak:** none, read on both frontends.
- On Qt the label is a separate item: `field.text`, `textEdited`, the commit on focus loss and the accessible value
  are untouched.
- On the terminal the branch only `putString`s, and `probeText` reads `_field->text()`.
- Forbid the alternative explicitly in the design: "never `setText` the elided form". Add to the terminal test that
  `probeText()` is the whole value while it is elided.

**IME, password, date-time:**
- **IME (read):** a preedit exists only with the focus, so the overlay is hidden.
- **Password:** excluded.
- **Date-time:** its picker popup takes the focus, so the field shows elided while the user picks. That is fine once
  B1's selection term is in.

### S4. R2: `setHeadOneLine` does not call `showTrail()`

**Read.**
- `setHeadOneLine` calls only `showBarInset()` (`shell_frame.hpp:278-282`).
- With the design's condition in `showTrail`, a screen change from a one-line head to a tall one at the same form
  leaves the inset at its old visibility until something else calls `showTrail`.
- The design's first test passes or fails by call order alone.

**Fix:**
- `setHeadOneLine` calls `showTrail()`. Say so in R-F's item list.
- Add a test: `setCompact(true)`, then `setHeadOneLine(false)`, and the inset is visible.
  **Control:** without the call, it is hidden.

### S5. R2: the condition is a proxy, and A14 understates the consequence

**Is the title inside its line?** Yes. **Measured:** the title's ink ends at 382, inside a line that ends at 386. R2's
reframing is right.

**Is the end-inset rule general? Not quite.**
- What decides the trailing inset is what ends the line, not how tall the head is:
  - a ghost icon button carries its own padding (console's scan icon, the menu square at the leading end);
  - text and framed buttons reach their box edge.
- **Read:** a compact one-line head with no trail, or with a trail that ends in text, still stands 4 px from the edge
  under R2.
- **Fix:** make the rule "the trailing end takes step 4 unless the line ends in an unframed button". The client already
  tells the frame about the last action (`setTrailEndInset`); widen that to {none, unframed, framed}, where none means
  text ends the line. The gallery's results are the same:
  - console ends unframed → step 1;
  - review has no trail → step 4.

  If the lane keeps `_headTall`, the design must name the one-line case and say why it is left at 4 px.

**A14 (measured, then inferred).**
- After R2 the lead block is 56–374 (318 px).
- The title needs 326 px, and the tag row's badge boxes span 56–376 (321 px). So the last tag wraps as well as the
  title.
- The head grows by two lines, not one.
- Correct A14 and T8: T8 should record the head's height. 374 for the tags is already in T8.

### S6. R3: `kActionGap` is not ui/1 vocabulary, and the reason for rejecting a theme value is contradicted by existing tokens

**Read.**
- `kActionGap` is a C++ constant that serialises as `"gap": 2`. A ui/1 document cannot name it: a server-emitted
  document writes the number. A theme cannot move the action gap without moving every step-2 gap. A frontend cannot
  tell a row of actions from any other row.
- T5 still writes it by hand at seven or more screen sites. Writing the same thing at that many sites is the signal, under AGENTS.md
  and the standing rule, that a framework seam is missing. Naming the literal does not supply that seam.
- **The rejection's reason is contradicted by the token set.** "The spacing scale names steps, not uses; a token for
  one use is a second scale" fails against three existing use-named tokens:
  - `head.paddingY` is a use token where 0 keeps a spacing step (`theme.hpp:1292`), the same shape a gap token would
    have;
  - `table.cellPaddingX/Y`;
  - `input.paddingX`.

  The gallery theme already sets two of them (`lims_gallery.cpp:72-77`).
- **The other rejected option.** "Allowing `actionBar` outside an overlay changes where an existing kind mounts." It
  does not: today it throws (`mount.hpp:1951`), so no valid document changes meaning.

**Fix (recommended):**
- A token `actions.gap` (Dimension, 0 keeps step 2), read by `MorphFooter` and by the stack an `actionBar` mounts.
- An `actionBar` outside an overlay mounts in place as a row of actions.
- `stepperParts`' footer is an `actionBar`, and T5's rows become `actionBar`s.
- The terminal keeps step 2 in cells: "the terminal keeps its cells".
- **Test:** a theme with `actions.gap` 12 moves a stepper footer's Continue by 4 px. **Control:** the constant.

If the lane keeps the constant, the design must record that the action gap is neither themable nor visible in a
document, and that T5's literals are the price.

### S7. R4: align baselines, not tops

**Measured** (`$S/lgr-rev-r4/kv.out`, `mono.out`):
- `Qt.AlignBaseline` works in `GridLayout` on 6.12. It aligns the caption's first line with the value's first line
  whatever the value's face.
- `Qt.AlignTop` does that only when the faces match. The 13 px caption's baseline is at 13.97. A 12 px mono value's,
  plain or as a run, is at 11.19.
- So top alignment leaves stability's Lot, Protocol and First pull rows **2.8 px off**. Those values are `mono` at
  `font.size.mono` 12 in the gallery theme.
- The design's sentence "both are drawn in body-size fonts, so their first lines share a baseline" is false for runs
  and mono values.

**Fix:**
- `Layout.alignment: Qt.AlignBaseline` on both.
- The Qt test asserts equal baselines (`y + baselineOffset`) for a mono value. **Control:** `AlignTop` misses it by
  about 3 px.
- **Read:** `AlignBaseline` is long-standing in Qt Quick Layouts (5.x); it was not run on 6.8.

**The terminal guard cannot fail.** A terminal value is one row and never wraps, so "a two-line value draws its
caption on the value's first row" cannot be constructed. Drop it, or replace it with the clause in
`tui/frontend.md`.

### S8. R5's mutation controls: two need sharpening

- **The floor ("no floor" → 0 columns).**
  - **Read:** on Qt, `count` 0 makes every span 0, so every cell lands at column 0, row 0.
  - "All three are in one column" would then pass on equal x alone.
  - Assert strictly increasing y as well.
- **The gap.** "A fourth case the lane adds with the computed width" is not specified. Here is one that discriminates
  on both frontends:
  - `fixed(31)`, `columns: 3`, `minColumnWidth: 10`, gap 1;
  - terminal: correct floor(32/11) = 2, mutant 3;
  - Qt: correct floor(241.8/80.7) = 2, mutant 3.

  It holds only while `unitWidth < spaceOf(1) × 2` (here 7.67 < 8). Write the derivation into the test so a theme
  change cannot turn it into a guard.
- **The floor of a ratio.** `gridKeepsColumnsWhenRoom` sits at exactly 3.0 on Qt. It passes because 7.671875 is exact
  in binary. Use `Math.floor(x + 1e-6)` in `place()`, and say why.
- **Vocal over silent.** `minColumnWidth` with no `columns`, or with `columns: 1`, is a no-op at the mount. Refuse it at
  load, as `tracks` is refused.

### S9. R5: the right answer for receipt; pane-width classes are the general one, and the options should say so

- **For receipt, `minColumnWidth` is right** (measured above: 2/1 at 1000, 3/2 at 1215):
  - it is CSS's `auto-fit, minmax`;
  - it is container-relative;
  - it is opt-in;
  - its loop direction (width → count → height) is the one wrapping rows already use;
  - its natural width is unchanged, so content-sized grids, table cells and batch Q's `W × max(n/w)` natural see the
    same number as today.
- **The general answer the design does not list:**
  - **The cases.** T3 (console's input) and T4 (audit's rail inset) are container-width questions about things other
    than columns: a padding and a sizing. The design answers them with screen-chosen bounds.
  - **The mechanism.** The general mechanism is a container-scoped width class, CSS's container queries: an
    `{"env": "paneWidthClass"}` that reads the nearest declared container's width class.
  - **Why it is not this batch.** It needs containment: the pane's width must not depend on its content, which a
    stretch or percent pane satisfies. It also needs re-evaluation per container. That makes it a design of its own.
- **Fix:**
  - Add it as option 4 in R5's options, with that cost.
  - Record it as the follow-up that would retire T3's and T4's screen bounds.
  - Keep `minColumnWidth`: it stays correct beside container classes.

---

## The screen items and the accepted items

| Item | Assessment |
|---|---|
| T1 | Fine. Measure the select's own elision, which R1 does not touch: a select already elides |
| T2 | Fine. The reasoning against B's proposal is right (read: Evaluation is the last non-shrink column) |
| T3 | Fine by batch Q's share and cap rule (inferred). 10 units is 76.7 px, inside 75–77 |
| T4 | Arithmetic holds: 23 units is 176 px, and 21 % of the 785 page is about 165. A container class (S9) is the general answer |
| T5 | Becomes `actionBar` under S6 |
| T8 | Record the two-line growth (S5) |
| T9 | Holds (measured positions). It needs B2 for the terminal render of receipt |
| T10 | Add: no input shows overlapping text after a select-and-right-click (B1) |
| A12 | Agree: `width: 90px` on one button is a screen's pixel choice |
| A13 | Agree (read). The "watched" rule is the right trigger |
| A14 | Agree with the principle; the description is wrong (two lines, S5) |

## Answers to the focus questions

| Question | Answer |
|---|---|
| Premises re-measured | They hold, apart from the tag row the design omits (table above) |
| R1: focus, editing, selection, IME, password/masked, date-time | Focus, editing, IME and password: fine. Selection: broken (B1, measured). Date-time: needs its own route on the terminal, and B1 on Qt (popup) |
| R1: the tooltip on the terminal | The terminal has hover tooltips, but the design builds none. State it as Qt only (S2) |
| R1: can the elision leak into the value? | No on both frontends (read). Forbid `setText` explicitly and test `probeText` (S3) |
| R2: is the title inside its line? Is the rule general? | Inside, yes (measured, 382 < 386). Not fully general: it should follow what ends the line (S5). `setHeadOneLine` must call `showTrail` (S4) |
| R3: is `kActionGap` the right vocabulary? | No. It is invisible to ui/1 and cannot be themed. Use a token like `head.paddingY`, plus `actionBar` in place (S6) |
| R4: effect on every keyValue | Only wrapping pairs move: stability, instrument, esignature, `examples/gallery`. Forms do not use keyValue. The terminal is unchanged. Use baseline (S7) |
| R5: interactions | `columns`: required, so refuse without it (S8). Tracks: refused. Spans: clamped. Natural width: unchanged, so P and Q's rules see the same number. Nesting: Qt via `place()`; terminal one frame per level once B2 is fixed. Terminal height: B2 |
| R5 or pane-width classes? | `minColumnWidth` for receipt. Container classes are the general follow-up for T3 and T4 (S9) |
| Mutation controls | R2, R3, R4 (Qt) and R5's key-ignored mutations would fail. Missing or weak: R1's selection (B1) and marks (S1); R2's call order (S4); R4's terminal guard cannot fail (S7); R5's floor and gap cases (S8) and terminal height (B2) |
| Qt 6.8.1 | Nothing needs an API newer than 6.8 (read): `FontMetrics`, `Label.elide`, `truncated`, `selectionStart`, `Qt.AlignBaseline`, `baselineOffset`. B1's context-menu path is 6.9+; its popup and window-deactivation paths apply to 6.8 (inferred). Probes ran on 6.12 only |

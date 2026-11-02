# Review of batch Q design (`lg-q-design.md`)

I checked the design against `lg-fold-p` (`e8b601a75`) in `/home/yaraslau/repo/morph-lims-e2`, reading only, with
`git show lg-fold-p:<path>`. I built nothing and edited nothing in either repository, and I did not open
`morph-lims-e1`. Paths below are relative to that worktree unless they start with `$S/`.
- Pixel numbers are **measured** with PIL on `$S/lg-cmp/app-p/`; scripts in `$S/lgq-rev-px/`.
- Two plain-QML probes are **measured** on Qt 6.12.0 offscreen (`$S/lgq-rev-q6/wrap.qml`, output in `wrap.out`). Note
  for anyone re-running probes: this `qml` binary prints `console.log` only with `QT_FORCE_STDERR_LOGGING=1`.
- Everything else is **read** from code, or **inferred** where I say so. Nothing ran on Qt 6.8.1: no 6.8 image is
  on this machine (`docker images` has 6.9.3 and 6.11.1 only).

**Verdict.**
- **Q2, Q4 and Q5** are right as designed, with small additions.
- **Q3** is right in scope, but it keeps the wrong weight on menu triggers (S8).
- **Q1** has the right rule and the right mechanism. It is not ready:
  - The new natural length is applied to `implicitWidth` but not to `widthFreeNatural`. Every text sets
    `widthFreeNatural`, and the design's own `naturalLength` reads it first. So a stretch row nested in a content-sized
    context, a table cell or a panel still gets the summing natural and squeezes its children. This is batch P's B1
    one level down (B1).
  - One terminal test pins a value that contradicts the formula (B2).
- **#1080 / Q6** can be built in this batch, as a Qt Quick rule that leaves the terminal unchanged. A10's premise, that
  it changes the contract on both frontends, is wrong. The terminal never wraps a text, so it has no word to keep
  whole. A design is under "Q6" below.

**Counts: 2 blockers, 12 should-fixes** (S1 to S12; S10 is the Q6 recommendation).

---

## Premises I verified

| Design claim | Status | Evidence |
|---|---|---|
| Q1: instrument 390, Book 132 and Log maintenance 174 | measured, holds | Book 16–147 and Log maintenance 152–325 (row y 95), so 132 and 174 |
| Q1: `MorphStack` uses a `GridLayout`; a stretch child is `fillWidth` with `horizontalStretchFactor` and no preferred width | read, holds | `MorphStack.qml` `GridLayout { id: layout }`; `MorphWidget.qml` `Layout.preferredWidth: widthKind === 1 ? … : widthKind === 3 ? shareWidth : -1` |
| Q1: the terminal starts a stretch item at 0 and its `stackNaturalSize` sums | read, holds | `layout.cpp` `distribute` (`isStretch ? 0`), `shareCapped`; `widget.cpp:627` `stackNaturalSize` sums `requestedSize` |
| Q1: the probes (`row.out`, `control.out`) | read the outputs, hold | 177/177 share-as-preferred; 174/288 = 22.6/37.4 units; 139/323; 122/198 |
| Q2: `bodyFont` is the platform font while `fontFamilies` is empty | read, holds | `Theme.qml:66-73`; `MorphText` role 0 uses `fontOf(bodyPixels)`, so a `text` node is already 13 |
| Q2: value "Assay and impurities, dissolution" is 12 px | measured, holds | ink 16–187, 172 px; `font2.out` 173 at 12, 188 at 13 |
| Q2: the gallery is the only theme that sets `font.size.body` without a family | read, holds | the only `font.size` token in `examples/` is `lims_gallery.cpp:63`; in `tests/qt_quick` only `test_qt_quick_theme.cpp:558`, which asserts no `bodyFont` size |
| Q3: banner, menu, file picker and empty state draw buttons in `bodyFont` | read, holds | `MorphBanner.qml:132`, `MorphMenu.qml:125-126`, `MorphFilePicker.qml:33,54`, `MorphEmptyState.qml:63` |
| Q4: `kRoles` holds 12 names; `Label` would be enumerator 11 | read, holds | `document.hpp:4247` (12 names, `title` among them); `view.hpp:239-254` (11 enumerators, `Caption` is 10) |
| Q4: the terminal `roleStyle` falls through to `textNormal` | read, holds | `leaf_widgets.cpp:31-48` |
| Q5: closed rows 50 apart, the A lines 45 | measured, holds | rules at 194, 239, 284, 329, 374 (45 each), then 424, 474, 524 … 724 (50 each). The step 374→424 is also 50: the open Row A's 5 px after its body |
| Q5: `MorphCollapsible`'s `ColumnLayout` has the default spacing | read, holds | `MorphCollapsible.qml:31`, no `spacing`; header, body, filler |
| Q6: console's "95.0–105.0" is one unbreakable unit for Qt | measured | `wrap.out`: at width 1 with `WordWrap`, Qt's widest line is 65.0 px, the same as the whitespace split: Qt does not break after the en dash between digits |

---

## Blockers

### B1. Q1's natural length is not applied to `widthFreeNatural`, and every text sets it, so nested stretch rows, table cells and panels still squeeze

**Read.**
- `MorphText.widthFreeNatural` is always 0 or more: `implicitWidth` for plain text, and a computed width for runs
  (`MorphText.qml:81`).
- `MorphStack.widthFreeNatural` is therefore 0 or more for nearly every stack. For a row it is the **sum** of the
  children's naturals (`MorphStack.qml:81-101`). `MorphPanel.widthFreeNatural` forwards the widest child's
  (`MorphPanel.qml:45-57`).
- The design changes `naturalLength` and `implicitWidth` only.
- The design's own `naturalLength` takes "a stretch child's natural" as "its `widthFreeNatural` when that is 0 or more,
  else its `implicitWidth`".

**Inferred** (arithmetic on the above). A row R holds two `stretch(1)` texts, of naturals 1 and 10.
- After Q1, `R.implicitWidth` is 20, but `R.widthFreeNatural` is still 11.
- Put R as a stretch child of a content-sized row. The outer `naturalLength` reads R's natural as 11, so R is
  placed at 11, and its texts get 5.5 each.
- A table cell holding R gets a column sized from `naturalOf(cell)`, which is `widthFreeNatural` first
  (`MorphTable.qml:234-239`). The same squeeze follows.
- So does a panel holding R, one level up.

The terminal has no second natural, so it would disagree with Qt here. None of the four conformance cases has a stretch
row inside a stretch row or a cell, so every check in the design passes with this in place.

**Fix:**
- `MorphStack.widthFreeNatural` along a row applies the same rule: others, plus gaps, plus `W × max(nᵢ/wᵢ)`, with each
  child's free natural. Make it one function that takes the child-natural accessor, used by `naturalLength` and
  `widthFreeNatural`, so the two cannot drift.
- For a column, `widthFreeNatural` is across the axis, so it is unchanged. The column's height natural is
  `naturalLength`.
- **Conformance, a fifth case** (`stretchRowNestedKeepsNaturals`):
  - a content-sized row (`alignSelf: start`) holding a `stretch(1)` `sizedText("a")` and a `stretch(1)` row of
    `sizedText("a")` and a 10-character `sizedText`, both `stretch(1)`;
  - assert that the 10-character text is at least 10 units;
  - **Qt control:** the summing `widthFreeNatural`.
- **Qt test:** a table column whose cell is that inner row is at least 20 units.

### B2. The terminal test pins 15 where the formula gives 30

The design's terminal test reads: "at `stretch(1)` and `stretch(2)` with naturals 10 and 10 it is 15. **Control:** the
summing rule gives … 20."

By the design's own formula, `W × max(nᵢ/wᵢ) = 3 × max(10/1, 10/2) = 30`.
- At 15, the shares are 5 and 10, so the weight-1 child (natural 10) is cut in half: the squeeze Q1 exists to remove.
- 15 is `W × min(n/w)`.
- **Read:** a lane that makes the test pass implements the inverse rule. A lane that implements the formula sees a
  correct implementation fail.

**Fix:**
- The expectation is 30.
- Add a case where the weights are not all 1 and the larger natural sits on the larger weight: naturals 10 and 10, at
  weights 2 and 1, give 30 too. Naturals 4 and 10, at weights 1 and 2, give 15. The expectation must be computed from
  the formula, not written by hand.

---

## Should-fixes

### S1. Q1 reaches `MorphStack` on Qt but every `stackNaturalSize` caller on the terminal

**Read.** `stackNaturalSize` has 13 callers. Changing it changes all of them:
- `ScrollImpl::naturalSize`;
- `ScrollViewport::contentAreas` and `paint`, which is the scroll extent;
- `SlotImpl`, `PanelImpl` and `CollapsibleImpl`;
- `TabsImpl` (`list_widgets.cpp:1231`);
- `FooterImpl`;
- `SplitterImpl`;
- `OverlayCore::frameSize`, which is the dialog's size;
- `DropZoneImpl`;
- `GridImpl`'s across natural;
- `StackImpl`.

On Qt, the panel's `column`, the collapsible's `body`, the scroll's `column`, the dialog's and drawer's
`contentColumn`, the slot, the roots and the tab pages are plain `ColumnLayout`s. A stretch-height child in them still
grows from its implicit height.

**Inferred.** After Q1, a scroll or panel with two stretch-height children of different naturals is laid out by weight
from zero on the terminal and from the naturals on Qt. The batch's "both frontends" holds only for `row`/`column`.

**Fix (pick one, and record it):**
- (a) Apply the rule only where `distribute` places the children, and list those callers. Keep the sum where a caller
  places children otherwise; `SplitterImpl` and `FooterImpl` look like such callers. State in Risks that panels,
  scrolls and collapsibles keep Qt's old stretch-height behaviour, and add it to #1078 as a remaining part.
- (b) Route those `ColumnLayout`s through the share computation too.

(a) is enough for this batch. Either way, the design must name the callers.

### S2. A content spacer is `stretch(1)` on the terminal but not in Qt's `stretchShares`

**Read.** On the terminal, `solvedAreas` turns a content child that `expandsByDefault()` (only `SpacerImpl`) into
`stretch(1)`. `MorphSpacer.qml` is `Layout.fillWidth/fillHeight: true` with `widthKind` 0. The design's
`stretchShares` counts `widthKind === 2` only.

**Inferred.** For a row `[text stretch(1), spacer]`, the terminal gives the text half. Qt's preferred shares would
give it all, and the spacer 0. Today's Qt splits by factors.

**Fix:** `stretchShares` counts a shown content-sized `MorphSpacer` as weight 1 with natural 0, as `solvedAreas`
does. Add a conformance case: a `fixed(40)` row of a `stretch(1)` text and a spacer gives the text 20.

### S3. Raising a share to a minimum takes the room back by an undefined rule on Qt, and the CSS claim is inexact

**Read.**
- The terminal's `distribute` raises a share to its minimum, then takes the deficit **from the last items first**
  (`shrinkFromLast`).
- The design's Qt side writes the raised share as the preferred width and leaves the deficit to `GridLayout`. That
  shrinks the others between their minimum and their preferred width, proportionally.
- **Inferred:** three `stretch(1)` children in 90, the first with minimum 50, give 50/30/10 on the terminal and
  50/20/20 on Qt.

The design also calls this "CSS `flex: w 1 0` … the terminal's `distribute` unchanged". CSS freezes a min-violating
item and re-shares the rest by weight. It also has `min-width: auto` (min-content), which ui/1 does not have (see S4
and Q6).

**Fix:**
- `shareByWeight` handles minimums itself, so the preferred widths sum to the room. Pick one rule for both frontends
  and say which: the terminal's take-from-last, or CSS's freeze and re-share, which is the better match for the
  mockups.
- Drop "CSS `flex: w 1 0`" as a description of the whole rule. Keep it for the from-zero basis only.
- Add a three-child conformance case.

### S4. Shares from zero remove Qt's de facto floor under a stretch button or select, and S10 would not notice a clipped label

**Read.**
- Neither frontend gives a button, a select or an input a content minimum. On Qt, `contentMinimumWidth` is set only by
  `MorphBadge`, `MorphPanel` and `MorphStack`; on the terminal by `StackImpl`, `PanelImpl` and badges.
- Today Qt grows a stretch child from its implicit width, so a stretch button narrower than its label never happened
  while the naturals fit.
- After Q1, a stretch button whose natural exceeds its weighted share is cut silently. Its minimum is 0, so no overflow
  is reported.

**Measured, then inferred.** The tightest case in the gallery is review 390's 1:1:2 decision row
(`review.cpp:846-848`).
- The row is 358 px inside its padding. The two 4 px gaps leave 350, so the shares are 87.5 / 87.5 / 175.
- "Return…" has 53 px of ink and "Reject…" 51 px (`ink.py`).
- `MorphButton` pads a style-0 push button by `spaceOf(4)` = 16 on each side (`MorphButton.qml:197`). That gives a
  natural of about 85–89 px against 87.5. It fits, or misses by about 1–2 px. I did not measure the button's
  `implicitWidth`.

S10 asks only that pairs be "equal within 1 px", which a clipped pair also satisfies.

**Fix:**
- Add to S10: each stretch button's caption ink is unchanged from P (for example, "Return…" 53 px, "Log maintenance"
  106 px) and is not elided.
- Add to Risks that a stretch control is now held to its share. A control that does not fit is the document's
  choice: `minWidth` or a wrap. Q6 gives a wrapping text its floor; a button keeps none.
- State the decision either way.

### S5. The no-loop test names an API the suite does not use and strings Qt does not print

**Read.**
- The Qt tests are Catch2, not QTest. They catch loops with `LoopWatch`, a `qInstallMessageHandler` that counts
  messages containing "Binding loop" (`test_qt_quick_containers.cpp:445-460`). It has a mutation control at `:522`.
- The design asks for `QTest::failOnWarning` on "Binding loop" and "Polish loop". The `const char*` overload matches a
  whole message exactly.

**Measured** (`strings` on Qt 6.12's libraries). The polish messages are:
- `possible QQuickItem::polish() loop` (`libQt6Quick`);
- `Layout polish loop detected for ` (`libQt6QuickLayouts`).

Neither contains "Polish loop". As written, the test passes whatever happens.

**Fix:**
- Extend `LoopWatch`, or `LayoutWatch` at `:2318`, to count case-insensitive `polish` + `loop` as well as
  `Binding loop`.
- Make the control the mutation "`implicitWidth` reads `layout.implicitWidth` while a child stretches", which must
  produce a counted message.
- Qt 6.8's wording is not verified here; the case-insensitive match is the hedge.

### S6. Q1's Qt cases: one assertion is too loose, and two pass without the new code

- **`stretchRowNaturalGivesEachItsNatural`** asserts "the second child at least 8". Its natural is 10, and 8 or 9
  means the text is cut. Assert at least 10 within rounding.
- **Cap and minimum.** The "cap" (`maxWidth: 10` → 10/50) and "minimum" Qt cases pass on today's code.
  - **Read:** `GridLayout` honours `Layout.maximumWidth` and `Layout.minimumWidth` itself, and hands the leftover to
    the other fill item by factor.
  - So the mutation "no cap in `shareByWeight`" survives on the non-wrapping path. It is caught only by the existing
    wrapping-row cap tests, because the function is shared.
  - Label both as guards. Name the wrapping-row test as the cap mutation's control.
- **Re-share** is the discriminating Qt case: it fails today (`row.out`, 165/189 at 359). Keep it as the control for
  the "no stretch branch" mutation.

### S7. The hand-back misses the content-sized places where the natural-length rule bites

The natural length matters only where a stack is given its natural. In the gallery, Q1's hand-back lists fill-width
rows, where only the shares change.

**Read:** the content-sized places are:
- dialogs, where `MorphDialog` takes `min(implicitWidth, overlay − 4 units)` (`MorphDialog.qml:134`). These are
  results' reason dialog, review's decision dialog, and plate's and console's dialogs;
- table cells that hold a row;
- a row with `align` other than stretch;
- `examples/gallery`'s rows, and forms in dialogs.

Also add `console.cpp:808-809`, the 17:10 chart and instruments panels.
- **Measured:** 231–838 and 844–1198 at 1215, so 607 and 354.
- **Inferred:** by weight from zero, 606 and 357.

That one moves by about 3 px, but it is the only weighted panel row besides instrument's.

**Fix:** add the gallery dialogs (1215 and 390), console 1215/1000, and a grab of `examples/gallery`'s Containers page
to Q-F's hand-back.

### S8. Q3 keeps the menu trigger's strong weight where a push button uses the button weight

**Read.**
- `MorphButton.captionFont` draws a filled or square push button at `Theme.buttonWeight`, and an outlined or
  danger-outlined one (emphasis 0 or 4) at the normal weight (`MorphButton.qml:73-92`).
- `MorphMenu`'s trigger draws in `strongFont` (`strongWeight`) for every emphasis but 0 and 3, and for a square trigger
  (`MorphMenu.qml:125-126`).
- The gallery sets `strong` 700 and `button` 500. A primary menu trigger is therefore bolder than a primary button
  beside it, and a danger-outlined trigger is bold where the button is regular.

The design maps the trigger to `controlFontOf(Theme.strongWeight)`, which keeps both differences.

**Fix:** the trigger takes `MorphButton`'s `strongCaption` rule, so `controlFontOf(Theme.buttonWeight)` for filled and
square triggers and `controlFontOf(Font.Normal)` for emphasis 0, 3 and 4. Test a primary trigger's weight against a
primary button's. The mutation is "`strongFont` in the strong branch", as the design lists. The trigger's padding
(`spaceOf(4)`, or 1 for ghost and square) already matches.

### S9. Runs are measured in body-size faces but drawn at the role's size; S5 and Q6 depend on it (file it)

**Read.**
- `MorphText` measures a strong run with `strongMetrics` (`Theme.strongFont`, the body size) and a mono run with
  `monoMetrics` (`Theme.monoFont`, `monoPixels`). This is in `wrapMinimumWidth`, `widthFreeNatural` and `elideRuns`.
- `runsHtml` writes only `font-weight` and `font-family`, so every run draws at the label's own size: the role's.

**Inferred:**
- S5's comment-card title, `label` role (12 px) with strong runs, is measured at 13 px strong after Q2, so it is about
  8 % too wide.
- A `heading` text with strong runs is measured narrow, so it wraps its last word when it is content-sized.
- A mono run in a body text is measured at 12 and drawn at 13 in the gallery.
- The error feeds Q1's `widthFreeNatural` and Q6's minimum.

This is a defect in `src/qt_quick`, outside Q's findings, so by AGENTS.md it gets an issue: verification "read",
with the closing condition "a run's measured advance equals its drawn advance for the `label`, `heading` and `normal`
roles".

**Fix in Q:** the faces are the label's font at the run's weight or family. That means `FontMetrics` bound to
`Qt.font` built from `label.font` with `weight` or `families` replaced. Do this in Q-F before Q6.

### S10. A10 should become Q6, built in this batch

See the "Q6" section below. A10's two reasons do not hold:
- "changes the contract on both frontends": the terminal does not wrap;
- "moves every screen at its narrowest width": the effect is bounded to the sites that break inside a word today.

### S11. Q2: units stay on the platform font, which also leaves `unitHeight` at a 12 px line

The design covers `fixed(n)` widths (about 8 % fewer characters).
- **Read:** the same holds for heights. `unitHeight` is `metrics.height`, 16.47 px for the 12 px platform font
  (`font.out`), while a 13 px line is about 1 px taller.
- **Inferred:** a `fixed(n)` height in lines that holds text now holds slightly less than n lines.

**Fix:** add "a `fixed(n)` height holding text" to S11's list, and say so in `theme.md`'s units paragraph next to the
width sentence.

### S12. Smaller accuracy fixes

- **#1080's body** says "Not verified: what the terminal draws for a text narrower than a word". It can be answered
  from the code (read): `TextImpl::paintControl` (`leaf_widgets.cpp:213-252`) draws each `\n` line and cuts it at the
  canvas width, and `paintRuns` breaks only at `\n`. The terminal never word-wraps (`table_widget.cpp:469`: "The
  terminal cannot wrap"). Add that to the issue with its verification status. It is also a defect in shipped
  behaviour, so `bug` fits better than `enhancement`.
- **#1078** should carry B1 and B2 when the design is corrected, since its body describes the terminal half.
- **The CSS citation.** "the flexbox intrinsic main size (css-flexbox-1 §9.9.1)": I would not cite it as what browsers
  draw. My understanding (not verified here) is that engines size a flex container's max-content from its items'
  contributions rather than by the flex-fraction algorithm. Cite it as the rule chosen, not as the mockups' behaviour.
- **Q5's neighbour.** `MorphPanel.qml:98` writes the same 5 px (`: 5 // the layout's default otherwise`) between a
  panel's header and its content. It is explicit, and none of the findings shows it, but it is the same untokened gap.
  List it under Watched.
- **Q4's terminal test** is a guard, as the design says. The discriminating Q4 checks are the Qt size (role 11 →
  Normal fails only because `captionPixels` differs from `bodyPixels`, which the default theme's 11 against 12
  provides) and `enumName`.

---

## Q6: a wrapping text is never laid out narrower than its widest unbreakable segment, on Qt Quick

### What was established

- **Measured** (`$S/lgq-rev-q6/wrap.out`, Qt 6.12, 13 px, `Text.WordWrap` at width 1 against `wrapMinimumWidth`'s
  `/\s+/` split):

  | text | Qt's widest unbreakable line | whitespace split |
  |---|---|---|
  | `Assay · 95.0–105.0` | 65.0 | 65.0 |
  | the same with U+200B after the dash (S4) | 35.0 | 65.0 |
  | `Paracetamol 500 mg`, joined by U+00A0 | **119.4** | **72.4** |
  | `a/b/c/d` | 11.2 | 39.3 |
  | `SOP‑CHR‑012.`, joined by U+2011 | 89.1 | 89.1 |

  So today's `wrapMinimumWidth` is wrong both ways:
  - JS `\s` matches NBSP, so it under-measures by 47 px: Qt would still break inside the word;
  - it ignores U+200B and slashes, so it over-measures.
- **Read:** the terminal draws plain text and runs line by line, cut at the cell width. It has no word to keep whole.
- **Read:** the propagation already exists. `contentMinimumWidth` feeds `baseMinimumWidth` → `Layout.minimumWidth` →
  a stack's `needWidth` (`layout.Layout.minimumWidth`) → its own `contentMinimumWidth`, and so on up. The overflow
  report fires at `needWidth ≥ haveWidth + 2`. A table cell sets `honoursContentMinimum: false`, and tables already
  use `shrinkWidth` for their least widths.

### Contract (recommended)

`backend_contract.md`, "Content that does not fit", after the badge sentence:

> On a frontend that wraps text, a text that wraps is never laid out narrower than its widest unbreakable segment, so
> a stack or a panel that holds one is raised with it, as with a badge. A one-line text, which elides, has no such
> minimum. The terminal does not wrap a text: it cuts each line at its width, so it keeps no minimum for a text.

This is a frontend-specific layout rule of the kind the terminal already has (it cuts and says nothing), not a new
key. The rule "the terminal keeps its cells" holds. No conformance case asserts it; it is a Qt test.

### Qt Quick

1. **`MorphText`.** `contentMinimumWidth: root.oneLine ? -1 : root.wrapMinimumWidth`.
2. **`wrapMinimumWidth` measures the segments Qt breaks at.** Two ways to do it:
   - **(a)** A C++ invokable on the backend: `QTextBoundaryFinder(Line)` over the plain text, with `QFontMetricsF` per
     face. It is exact and matches `QTextLayout`. Recommended.
   - **(b)** A JS split that joins at U+00A0, U+202F, U+2007, U+2060 and U+2011, and breaks at whitespace, U+200B and
     after `/`. It is cheaper and approximate.

   Either way, a Qt test asserts equality, within 1 px, with a hidden `Text { wrapMode: Text.WordWrap; width: 1 }`
   over the table above. That test is the control that fails on today's split, at NBSP.
3. **Runs** are measured in the drawn faces (S9).
4. **`MorphKeyValue`**, which is the stability site.
   - Its values are `Label`s, not `MorphText`. Each value's `Layout.minimumWidth` is its widest segment.
   - The kind's `contentMinimumWidth` is:
     - beside: caption column + `2 × unitWidth` + the widest value minimum;
     - stacked: the widest of each.
   - Today the kind sets none, so even its caption column's minimum does not reach its container (read).
5. **`Text.Wrap` stays** (`WrapAtWordBoundaryOrAnywhere`) as the last resort when a container cannot give the room.
   - The overflow is then reported (`morph.layout`), as for a badge: vocal, not silent.
   - Switching to `WordWrap` would draw an overlong word past its box and over its neighbour.
6. **With Q1.** A stretch text's share is raised to its widest segment. With S3's rule, Q1 and Q6 together are CSS
   `flex: w 1 0; min-width: auto` for text, which is what the mockups use.
7. **Out of scope, and said so:**
   - buttons, selects and inputs (no minimum, S4);
   - `MorphBanner`'s message, a dialog's description and a toast's label, which their own kinds size.

### What it does at the narrowest widths

**Inferred (argued, to be measured).**
- A layout uses minimums only when the room is below the preferred widths.
- Where no text is broken inside a segment today, every text's width is at least its widest segment, so no new
  minimum exceeds the width its container already has. Nothing moves, and nothing reports.
- At the judged widths, the after-P reports find mid-word breaks only at console 1000 (and stability before its pane
  minimum). Q6 should change those and nothing else.
- Narrower than 390 (for example 320) is not judged, and would report more overflows. That is the intended vocal
  failure.

**Two caveats:**
- **Qt shrinks the neighbours.** `GridLayout` shrinks children without a minimum when the room is short. Console's
  `fixed(10)` input then gives up the 1–6 px that "95.0–105.0" needs: 65 px at 13 px against the 64 px it has today.
  The terminal does the same to a `fixed` child (`shrinkFromLast`). Say that a `fixed` width has no minimum on either
  frontend. That is already true, but it becomes visible here.
- **Over-measuring raises minimums that are not needed.** If (b) is chosen and misses an opportunity, a minimum can
  exceed today's width where Qt breaks legitimately (`a/b/c/d`). The equality test above is the guard.

**The measurement:**
- Grab all 39 after Q1 to Q5, then again after Q6, and pixel-diff the two sets.
- Expected: only console 1000's first result line differs.
- The render smoke stays warning-free.
- Also run every Qt test suite: many resize to 400×200, and a new overflow warning there is a finding, not a test to
  relax.

### Tests

- **Qt:**
  - a `fixed(60)`-unit row of a `stretch(1)` text "Assay · 95.0–105.0" and a `fixed(52)` input. The text's width is at
    least its widest segment, and its second laid-out line starts with "95.0".
    **Control:** `contentMinimumWidth: -1` gives a narrower text and a line starting "0".
  - the segment-equality test (above), with NBSP as the control.
  - a `fixed`-width column too narrow for a 30-character word reports one overflow naming the text.
    **Control:** no report without Q6.
  - a stacked and an inline `keyValue` raise their container's minimum to the widest value segment.
    **Control:** the kind's minimum unset.
- **Terminal:** a guard, labelled as one: a text's `contentMinimumWidth()` stays 0.
- **Mutations:**
  - `contentMinimumWidth` unset;
  - the NBSP split;
  - the keyValue minimum unset;
  - runs measured in body faces (S9).

### Gallery consequences (Q-S)

- **S4 (U+200B) is dropped.** With Q6, "95.0–105.0" keeps whole without it. It is a gallery workaround for a framework
  gap, which the standing rule forbids once the gap is built.
- **Stability's 29-unit pane minimum** is re-measured without it. Keep it only if the mockup's width needs it, not to
  prevent breaks.
- **Watched:** a button label that clips under Q1 (S4).

### Spec and issue

- `backend_contract.md` (above);
- `qt_quick/backend.md`: the Text and KeyValue rows;
- `tui/frontend.md`: the Text row, "cut at its width, no minimum".

#1080 is closed by Q-F's Q6 commit, with S12's correction to its body.

---

## Answers to the focus questions

| Question | Answer |
|---|---|
| Premises re-measured | They hold (table). Q2's 12 px finding is right: values 172 px of ink, against 173 at 12 px |
| Q1: can a content-sized row or column squeeze a child? | At one level, no: `W × max(n/w)` gives each its natural. Nested, in table cells and in panels, yes, through `widthFreeNatural` (B1). The terminal test's 15 is the squeezing value (B2). Dialogs are the content-sized context the hand-back omits (S7). Scrolls, panels and collapsibles keep Qt's old stretch-height rule while the terminal's changes (S1). Spacers diverge (S2) |
| Q1: forms engine and other examples | `inlineCell` (`form_view.hpp:427-430`) has one stretch child per row, so it is unchanged. `explorer.hpp:474-475`'s two stretch scrolls sit in a splitter, not a stack, so they are unchanged on Qt; on the terminal `SplitterImpl::naturalSize` changes (S1) |
| Q2 blast radius | Only the gallery theme sets a body size without a family (read). It affects every `bodyFont`, `strongFont` and `buttonFont` text: tabs, check labels, key values, section headings and desktop controls. No other test pins those sizes (read: `:558` asserts caption, small and display only; P6's test uses the default theme). Add `fixed(n)` heights (S11) |
| Q3 | Right scope. The menu trigger's weight should follow `MorphButton`'s rule (S8) |
| Q4 `label` role | Right. A role, not a key; "a field's caption" is a real design element. S5 depends on run measurement (S9) |
| Q5 | Right, and measured: 45 + 5 = 50 exactly. Note the panel's explicit 5 (S12) |
| Mutation controls fail if the feature did nothing? | Q2, Q3, Q4 and Q5: yes. Q1: the conformance cases and re-share do. The cap and minimum Qt cases do not (S6). The no-loop test as written cannot fail (S5). The terminal test fails a correct implementation (B2) |
| Qt 6.8.1 | Nothing in Q1 to Q6 needs an API newer than 6.8 (read: `horizontalStretchFactor` 6.5, `?.`/`??`, `Qt.font` `families`, `FontMetrics`; for Q6(a), `QTextBoundaryFinder` is Qt 4). The loop-message strings are measured on 6.12 only. Q1's controls ran on 6.12 only, and no 6.8 image is on this machine |
| #1080: build it in Q? | Yes, as Q6 above. It is a Qt Quick rule plus a contract sentence. The terminal is unchanged because it does not wrap. The effect is bounded to today's mid-word sites at the judged widths. It needs S9 first, and it retires S4's U+200B |

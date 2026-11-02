# LIMS gallery batch Q: stretch shares by weight, a body font that follows the theme, touch text on every push button, a `label` text role, a closed collapsible as tall as its header, and no mid-word breaks on Qt Quick

Batch Q covers every finding the after-P comparison left open (`lg-pr-findings-A.md`, `lg-pr-findings-B.md`). Each
finding becomes one of three things:
- a framework item (Q1 to Q6), for lane Q-F;
- a screen item (S1 to S11), for lane Q-S;
- an accepted difference (A8 to A11), with its reason. A1 to A7 from batch P stand.

The review (`lg-q-review.md`: 2 blockers, 12 should-fixes) is taken in full. The coordinator's decision to build #1080
in this batch is item Q6. See "Review round 2" at the end. The review's should-fixes are written R-S1 to R-S12 there,
so they do not clash with the screen items S1 to S11.

The rules of batches G to P hold:
- a look is a token or a rule, never a screen's choice of pixels;
- a key means the same thing on every kind that takes it;
- the terminal keeps its cells, and a purely visual change says what it leaves unchanged there;
- vocal over silent.

The gallery is judged on the 1215×830 grab, where the page is about 1000 px wide. On the 1000×830 grab (a page of
about 785 px), nothing may clip, overlap or lose content, except where a column is told it may elide.

**Sources and evidence.**
- **Code.** Read in `/home/yaraslau/repo/morph-lims-e2` on `lg-ps`, whose head is the same commit as `lg-fold-p`
  (`e8b601a75`; `git diff lg-ps lg-fold-p` is empty). `morph-lims-e1` was not opened.
- **How each premise is marked:**
  - **measured**: a PIL read of `lg-cmp/app-p/` (by the reviewers or by me, as stated);
  - **reproduced**: run here, with the output shown;
  - **read**: inferred from the code.
- **What was run.** Plain-QML probes only, in `lgq-exp/` (Qt 6.12.0, `QT_QPA_PLATFORM=offscreen`, the platform the
  grabs use, on the same machine):
  - `row.qml` and `control.qml`: the stack's stretch mechanism;
  - `font.qml` and `font2.qml`: the platform font and reference widths;
  - `runs.qml`: run advances by size;
  - `ink*.py` and `edges.py`: the pixel reads.

  The reviewer's `lgq-rev-q6/wrap.qml` measured Qt's own unbreakable segments. Nothing in morph was built.
- **Issues.** #1078 (stretch), #1079 (body font) and #1080 (mid-word breaks, now a `bug`) are filed. The run
  measurement defect's body is `lg-q-issue-runmeasure.md`, for the coordinator to file.
  - #1078 gets the corrections at the end of `lg-q-issue-stretch.md`.
  - #1080 gets the rewritten `lg-q-issue-midword.md`.

## Probes

- **Q1. Two `stretch` siblings** (brief item 1).
  - **Measured:**
    - samples at 390: 158 / 196;
    - audit: 179 / 175;
    - results: about 170 / 186;
    - plate: 182 / 173;
    - instrument at 390, Book and Log maintenance: 132 / 174 (16–147 and 152–325, re-measured by the review);
    - console at 1215, the 17:10 panels: 607 / 354, where weight shares from zero give 606 / 357 (the review).
  - **Read:**
    - `MorphStack` lays a non-wrapping stack out in a `GridLayout`.
    - A stretch child is `Layout.fillWidth` with `horizontalStretchFactor` set to its weight and no preferred width,
      so it grows from its implicit width.
    - The terminal's `distribute` starts each Stretch item at 0. `StackImpl::innerNaturalSize` sums the naturals
      through `stackNaturalSize`.
  - **Reproduced** (plain QML):
    - 1:1 drifts and inverts: 165/189 at 359 px, 548/447 at 1000;
    - 1:2 gives 137/217 at 359;
    - a column gives 122/198;
    - writing the shares as preferred widths gives 177/177.
  - **Against #1077.** Same root, different site and symptom. P4's fix does not reach `MorphStack`.
  - **Verdict:** a defect (#1078).
- **Q2. P6 on instrument's phone details** (brief item 2).
  - **P6 reaches the stacked path:** `compactDetails()` sets `Stacked`, and that becomes `Above` and `captionsAbove`.
    Only that path draws the rules, and the grab shows them.
  - **The body does not follow the theme.** With `font.size.body` 13 and no `font.family`, `Theme.bodyFont` is the
    platform font, 9 pt = 12 px (**reproduced**). Values and captions are both 12 px, so P6 shows nothing.
  - **Measured:** the value "Assay and impurities, dissolution" has 172 px of ink (173 at 12 px, 188 at 13). The
    group legend, drawn by `MorphText` at `fontOf(bodyPixels)`, has 114 (116 at 13).
  - **Verdict:** a defect (#1079).
- **Q3. Console's "Open run"** (brief item 3).
  - **Read:** `MorphBanner`'s action, `MorphMenu`'s trigger, `MorphFilePicker`'s field and button, and
    `MorphEmptyState`'s action draw in `bodyFont`. P1 reached `MorphButton` only.
  - **Measured:** 47 px of ink, which is 12 px, not 13.
  - **Verdict:** a defect, not filed (a fix smaller than its issue).
- **Q4. The group legend** (brief item 4).
  - **Read:** `lowerGroup` draws the legend as an unroled `Text`.
  - **Read:** the mockups mark it `<legend class="lbl">`, the field-caption class.
  - The wells and the rings are A8 and A9.
- **Q5. Plate's collapsed rows, 50 against 45.**
  - **Read:** `MorphCollapsible`'s `ColumnLayout` has Qt's default `spacing: 5`.
  - **Measured** (the review): the closed rows are 50 apart. Row A's last line is 50 from Row B: the 5 px after an open
    body.
- **Q6. Mid-word breaks** (brief item 5; #1080).
  - **Measured:** stability's summary before its pane minimum, and console's "95.0–105. / 0" at 1000.
  - **Measured** (the review, `wrap.out`): today's whitespace split is wrong both ways:
    - it gives 72.4 against Qt's 119.4 for a U+00A0-joined phrase;
    - it gives 39.3 against 11.2 for `a/b/c/d`.
  - **Read:** the terminal never wraps a text (`paintControl` and `paintRuns` cut each `\n` line at the width), so it
    has no word to keep whole.
  - **Read** (R-S9): runs are measured at the body size and drawn at the role's size. **Reproduced** for one bold
    string: 132.5 at 12 px, 143.5 at 13 and 176.7 at 16 (`runs.out`).

## The items

| Item | Finding | Verdict | Lane |
|---|---|---|---|
| Q1: stretch children share by weight from zero; a stack's natural lengths give each its natural; a push button's minimum is its natural width | stretch pairs unequal on five screens | defect #1078: **measured**, **reproduced**, **read** | Q-F |
| Q2: the body fonts follow `font.size.body` | P6 invisible; body text at 12 and 13 px in one theme | defect #1079: **measured**, **read** | Q-F |
| Q3: every push button the library draws follows `control.touchTextSize`, at `MorphButton`'s weights | console's "Open run" | defect, not filed | Q-F |
| Q4: text role `label`; a group's legend uses it | receipt's legend 13 against 12 px | **measured**, **read** | Q-F |
| Q5: a collapsible lays out its header and body with no spacing | plate's collapsed rows 50 against 45 | defect, **measured** arithmetic; not filed | Q-F |
| Q6: runs measured in their drawn faces; a wrapping text and a key-value's value are never narrower than their widest unbreakable segment, on Qt Quick | console 1000's broken limit; stability's summary | defects: #1080, and the run-measure body | Q-F |

## Q1: stretch children share by weight

### Problem

Q1. Qt Quick grows a stretch child from its implicit length. The terminal counts a content-sized stack's natural
length so that equal shares cut a stretch child narrower than its natural.

### Options

1. **Drop the `GridLayout` from `MorphStack`.** Rejected. Every stack would change its placement path, which is more
   than the defect needs.
2. **`Layout.preferredWidth: 0` on stretch children.** Rejected:
   - it is raised to the minimum;
   - the implicit width falls to 0;
   - a cap leaves its share to no one.
3. **Chosen: the stack computes the shares and writes them as preferred lengths,** the mechanism a `percent` child
   uses. The shares sum to the room, so the `GridLayout` places them exactly (**reproduced**, 177 / 177).

### Decision

- **Scope (R-S1, option a).** The rule applies to `row` and `column` on both frontends:
  - `MorphStack` on Qt Quick;
  - `StackImpl` on the terminal, where `innerNaturalSize`'s non-extents path (`container_widgets.cpp:135`) changes.

  These keep the sum, and their Qt counterparts keep Qt's own stretch-height growth:
  - `stackNaturalSize`'s other twelve callers: the extents path at `:137`, `ScrollImpl`, `SlotImpl`, the slot check
    at `:315`, `PanelImpl` (`:539`), `:636` and `:750` (the scroll viewport's areas and paint), `FooterImpl`, the
    overlay's `frameSize` (`:992`), `TabsImpl` (`list_widgets.cpp:1231`), and `region_widgets.cpp:79`, `:259` and
    `:453`;
  - on Qt, the plain `ColumnLayout`s of panel, collapsible, scroll, slot, tab page, dialog and drawer.

  Two stretch-height children of a panel or scroll therefore still diverge. That is recorded in Risks and in #1078 as
  its remaining part.

  The terminal gets a `weightedNaturalSize(children, axis, gap)` beside `stackNaturalSize`, which only `StackImpl`
  calls. `stackNaturalSize` is unchanged.
- **Which children stretch (R-S2).** A child stretches along the axis when its sizing there is `stretch(w)`, or when
  it is a content-sized spacer. A spacer counts as weight 1 with natural 0, as the terminal's `solvedAreas` already
  treats `expandsByDefault`.
  - On Qt this is a shown `MorphSpacer` (a new `readonly property bool expandsByDefault: true` on it, read by the
    stack) with `widthKind` 0 in a row, or `heightKind` 0 in a column.
  - Both frontends read the same predicate in the share and in the natural length.
- **The share rule (both frontends; R-S3).**
  - Fixed, percent and content children take their lengths as today.
  - The room left after them and the gaps is shared among the stretching children by weight (`max(1, w)`), starting
    from zero.
  - Bounds are resolved as CSS flexbox resolves them (css-flexbox-1 §9.7, steps 4c–4d), in passes:
    - each pass shares the open room among the open children by weight and clamps each share into its bounds;
    - if the clamping added length in total, the children raised to their minimum are frozen there;
    - if it removed length, the children cut to their maximum are frozen there;
    - if it changed nothing, every violator is frozen;
    - the frozen lengths leave the room, and the next pass shares the rest.

    There is at most one pass per stretching child.
  - **Worked example:** three `stretch(1)` children in 90, the first with a minimum of 50, give 50 / 20 / 20.
  - If the frozen minimums alone exceed the room, the stack overflows as today: reported on Qt, cut from the last
    children on the terminal (`shrinkFromLast`, unchanged for that case).
  - **The terminal.** `layout::distribute`'s `shareCapped` becomes this loop: today it freezes maximums only, and
    raises minimums afterwards. That also changes grids with stretch tracks, splitters and wrapping lines, which call
    `distribute`, wherever a stretch share falls below its minimum. Those are the cases where the old rule took the
    room from the last items, fixed ones included. See Risks.
  - **"from zero"** is CSS `flex-basis: 0`. The bounds resolution is CSS's. ui/1 has no automatic `min-width`, except
    the floors this design names: a badge's, a push button's and a wrapping text's.
- **A push button's minimum is its natural width (R-S4, the coordinator's decision).**
  - Shares from zero remove Qt's de facto floor under a stretch button. Without a minimum, a button whose label does
    not fit its share would be cut silently.
  - A push button (`ButtonStyle::Push`, not an item-style entry) takes its natural width as its content minimum on
    both frontends, as a badge does:
    - **Qt:** `MorphButton.contentMinimumWidth` is its `implicitWidth` for style 0, and −1 for an item;
    - **terminal:** `ButtonImpl::contentMinimumWidth()` is `naturalSize().width` while `_style` is `Push`.
  - A table cell is not raised (`honoursContentMinimum: false`), as today.
  - A select, an input and a menu trigger keep no minimum. Their text is the value, not a label the user must read to
    act, and a `fixed` input must stay narrowable. The contract says so.
- **The natural length (both frontends; B1, B2).** One function, given the stack's children and an accessor for a
  child's natural, computes `Σ others + gaps + W × max(nᵢ / wᵢ)`:
  - `nᵢ` is a stretching child's natural, clamped into its bounds, and at least its content minimum;
  - `W` is the sum of their weights.

  Worked values, which the tests compute from the formula rather than write by hand:
  - naturals 10 and 10 at weights 1 and 2 give 3 × max(10, 5) = **30**;
  - naturals 4 and 10 at weights 1 and 2 give 3 × max(4, 5) = **15**;
  - 1 and 10 at weight 1 give 20.

  With one stretching child the rule is the plain sum. It is the rule chosen for ui/1, the stack's form of P4's grid
  rule. It is not a claim about what a browser draws (R-S12).
- **Qt Quick, `MorphStack.qml`, without `wrap`.**
  - **`stretchShares`** is a binding, an array aligned with `layout.children`, −1 for a child that does not stretch.
    It reads:
    - the axis length inside the padding;
    - the non-stretching children's clamped lengths;
    - `gapLength`;
    - each stretching child's weight and bounds: `Layout.minimum*`, which now includes a button's content minimum,
      and `Layout.maximum*`.

    It never reads a stretching child's preferred length.
  - **`shareByWeight(members, room)`** implements the passes above. The wrapping line in `place()` uses it too, so
    the two paths cannot drift.
  - **`naturalAlong(accessor)`** is the natural-length function. It is used:
    - by `naturalLength`, with each child's `implicitWidth` (in a row) or `implicitHeight` (in a column);
    - by `widthFreeNatural` along a row (B1), with each child's `widthFreeNatural` when it is 0 or more, else its
      `implicitWidth`.

    In a column, `widthFreeNatural` is across the axis and unchanged.
  - **`implicitWidth`** (a row) and **`implicitHeight`** (a column) read `naturalLength` while a child stretches.
  - **`MorphWidget`:** `stretchWidth` and `stretchHeight` are `stack.shareOf(root)`, duck-typed as `roomOf` is. They
    are `Layout.preferredWidth` and `preferredHeight` for a stretching dimension, a spacer's included. A `takesShare`
    child is unchanged.
- **Terminal.**
  - `weightedNaturalSize` for `StackImpl`.
  - `distribute`'s pass loop.
  - `ButtonImpl::contentMinimumWidth()`.
  - The one sizing predicate (`stretchSizingOf`, with `expandsByDefault`), which `solvedAreas` and
    `weightedNaturalSize` share.
- **Comments.** Each touched function states its rule in a sentence, with no history.

### Spec

- **`backend_contract.md`:**
  - the "cross axis" paragraph: the share rule, the bounds resolution, and the spacer;
  - the `StackWidget` row: the natural width and the free natural;
  - "Content that does not fit": "A badge's or a push button's minimum is its natural width";
  - **Bounds:** "a share outside its bounds is frozen at the bound, and the rest is re-shared by weight".
- **`document.md`**, sizing: `stretch` shares from zero by weight.
- **`tui/frontend.md`, Layout:**
  - the pass loop;
  - `weightedNaturalSize`, and which containers keep the sum;
  - a push button's minimum.
- **`qt_quick/backend.md`:**
  - `:384`, `stretchShares`;
  - `:397`, the natural and free natural;
  - the Button row, its minimum;
  - the Panel, Scroll, Collapsible, Slot, Dialog and Drawer rows, which keep Qt's stretch-height growth (the remaining
    part of #1078).

### Tests

- **Conformance**, in `layoutConformanceCases()`. The array grows by seven.
  - **`stretchSiblingsShareEqually`:**
    - **setup:** a `fixed(60)` row, gap 0, of `sizedText("a")` and a 30-character `sizedText`, both `stretch(1)`;
    - **assert:** 30 and 30, within one;
    - **Qt control:** 22.6 and 37.4 (**reproduced**).
  - **`stretchWeightsShareInProportion`:**
    - **setup:** the same, at weights 1 and 2;
    - **assert:** 20 and 40;
    - **Qt control:** 18.1 and 41.9 (**reproduced**).
  - **`stretchRowNaturalGivesEachItsNatural`:**
    - **setup:** a content-sized row of `sizedText("a")` and a 10-character `sizedText`, both `stretch(1)`;
    - **assert:** the row is 16 to 21 units wide (the formula's 20 where a character is a unit), and the second child
      is **at least its natural less one unit** (R-S6);
    - **terminal control:** the sum, 11 cells shared 5 or 6.
  - **`stretchRowNestedKeepsNaturals`** (B1):
    - **setup:** a content-sized row of a `stretch(1)` `sizedText("a")` and a `stretch(1)` row holding
      `sizedText("a")` and a 10-character `sizedText`, both `stretch(1)`;
    - **assert:** the 10-character text is at least its natural less one;
    - **Qt control:** the summing `widthFreeNatural`.
  - **`stretchSharesFreezeAtMinimum`** (R-S3):
    - **setup:** a `fixed(90)` row of three `stretch(1)` texts, the first with `minWidth: 50`;
    - **assert:** 50, 20 and 20;
    - **terminal control:** today's raise-then-take-from-last gives 50 / 30 / 10;
    - **Qt control:** the `GridLayout`'s own shrink.
  - **`spacerCountsAsStretch`** (R-S2):
    - **setup:** a `fixed(40)` row of a `stretch(1)` text and a content-sized spacer;
    - **assert:** the text is 20;
    - **Qt control:** the stack counting `widthKind === 2` only gives 40.
  - **`stretchHeightsShareByWeight`:**
    - **setup:** a 20-row `fixed` column of two `stretch(1)`-height children of 1 and 10 lines, using the helper of
      the "maximum height caps its share down a column" case;
    - **assert:** 10 and 10;
    - **Qt control:** **reproduced** at 122 / 198.
- **Conformance for the button minimum** (R-S4):
  - **setup:** a `fixed(20)` row of two `stretch(1)` push buttons, "a" and a 30-character label;
  - **assert:** the long button is at least its natural width.
  - **Control:** the minimum unset gives about 10 on both frontends.
- **Qt Quick** (`test_qt_quick_containers.cpp`), labelled as the review asks (R-S6):
  - **re-share (discriminating):** widening a row re-shares 1:1 equally. It fails today (165/189 in `row.out`) and is
    the control for the mutation "no stretch branch in `Layout.preferredWidth`";
  - **cap and minimum (guards):** `maxWidth: 10` gives 10 and 50, and a stretch badge keeps its natural. Both pass on
    today's code, because `GridLayout` honours bounds itself. They are kept as guards. The cap mutation in
    `shareByWeight` is caught by the existing wrapping-row cap test, named as its control;
  - **no loop** (R-S5): `LoopWatch` also counts messages containing "polish" and "loop" (case-insensitive), besides
    "Binding loop". A content-sized column holding two stretch rows, and a stretch row in a table cell, settle with no
    counted message. **Control:** `implicitWidth` reading `layout.implicitWidth` while a child stretches, which must
    produce a counted message. Qt 6.8's wording is not verified; the loose match is the hedge;
  - **table cell (B1):** a column whose cell is the nested row of `stretchRowNestedKeepsNaturals` is at least 20
    units.
- **Terminal** (`test_tui_layout.cpp`), with each expectation computed from the formula:
  - `weightedNaturalSize` gives:
    - naturals 1 and 10 at 1:1: 20;
    - naturals 10 and 10 at 1:2: **30**;
    - naturals 10 and 10 at 2:1: 30;
    - naturals 4 and 10 at 1:2: 15.
  - **Control:** the summing rule gives 11, 20, 20 and 14.
  - `distribute`'s pass loop gives 50 / 20 / 20. **Control:** 50 / 30 / 10.
  - `ButtonImpl::contentMinimumWidth()` is the label's width for a push button and 0 for an item.
- **Mutation controls, each reported as failing:**
  - no stretch branch in `Layout.preferredWidth` (fails re-share and the first two cases);
  - `naturalAlong` summing (fails the natural case);
  - a summing `widthFreeNatural` (fails the nested case and the table cell);
  - the terminal summing (fails the terminal values);
  - `distribute` raise-then-take (fails the freeze case);
  - the spacer not counted (fails the spacer case);
  - the button minimum unset (fails the button case);
  - no cap in `shareByWeight` (fails the wrapping-row cap test).

### What Q-F hands back for Q1 (R-S7)

- **Fill-width rows:** samples, audit, results, plate, instrument and review at 390, and instrument, stability and
  receipt at 1215 and 1000.
- **Content-sized places**, where the natural length decides:
  - the gallery dialogs at 1215 and 390: results' reason dialog, review's decision dialog, and plate's and console's
    dialogs;
  - console at 1215 and 1000 (the 17:10 panels: 606 / 357 expected);
  - `examples/gallery`'s Containers page;
  - a forms example with `columns` in a dialog.

## Q2: the body font follows `font.size.body`

### Decision

- **`Theme.qml`.**
  - `readonly property bool themedBody: fontFamilies.length > 0 || bodySize > 0`.
  - `bodyFont`, `strongFont` and `buttonFont` are `fontOf(bodyPixels, weight)` while `themedBody` holds. Otherwise
    they are the platform font untouched.
- **Units do not move.** `Theme.metrics` follows `fontFamilies` alone. Two things follow (R-S11), and `theme.md`'s
  units paragraph says both:
  - a `fixed(n)` width holds about 8 % fewer 13 px characters in the gallery;
  - a `fixed(n)` height holds slightly less than n 13 px lines, because `unitHeight` is the 12 px platform line,
    16.47 px.
- **`controlFontOf`** is unchanged, and now carries the theme's size at the body size.
- **Terminal.** Unchanged.
- **P6** is unchanged. Q2 makes it visible.

### Spec

`theme.md`:
- `:198`: the body-size row;
- `:287-292`: "the platform font untouched while the theme names no family and sets no `font.size.body`";
- `:307`: the units, widths and heights.

### Tests

- **`test_qt_quick_theme.cpp`, a theme with no family and `font.size.body` 17.** A guard asserts that
  `platformMetrics.font.pixelSize` is not 17.
  - `bodyFont`, `strongFont` and `buttonFont` are 17 px at their weights.
  - A key-value's value and its inline caption are 17 px.
  - A stacked caption is at `captionPixels`.
  - With neither token, `bodyFont` is the platform font.
- **Mutation controls:**
  - the family-only condition fails the 17 px checks;
  - P6's caption on `bodyFont` fails the stacked check in this variant.

## Q3: every push button the library draws follows `control.touchTextSize`

### Decision

These kinds draw a push button's caption with `controlFontOf`, at **`MorphButton`'s weight rule** (R-S8): the button
weight for a filled or square button, and the normal weight for emphasis 0 (outline), 3 (ghost) and 4 (danger outline).

| QML | Today | Becomes |
|---|---|---|
| `MorphBanner` `action` (style 0 ghost or 1 button; stretched or not) | `bodyFont` | `controlFontOf(Font.Normal)`: both styles are outlined or ghost |
| `MorphMenu` `trigger` in button style | `bodyFont` for emphasis 0 and 3; `strongFont` otherwise and for square | `controlFontOf(Theme.buttonWeight)` for a filled (primary, danger) or square trigger; `controlFontOf(Font.Normal)` for emphasis 0, 3 and 4 |
| `MorphFilePicker` path `TextField` and `browse` | `bodyFont` | `controlFontOf(Font.Normal)` |
| `MorphEmptyState` `action` | `bodyFont` | `controlFontOf(Font.Normal)` |

These stay at `bodyFont`, and `theme.md` lists them:
- the banner's message and a `link` action;
- the dismiss glyph;
- a sheet's `sheetDismiss`, an item-style header button;
- menu entries.

**Terminal.** Unchanged.

### Spec

- `theme.md`, the touch paragraph: the covered list and the left-alone list.
- `qt_quick/backend.md`: the Banner, Menu, FilePicker and EmptyState rows.

### Tests

- **`test_qt_quick_theme.cpp`** (a family, body 13, touch 14), with `touchTargets` on:
  - the banner's action at styles 0 and 1, and stretched, is 14 px;
  - a primary menu trigger is 14 at `buttonWeight` and equal in weight to a primary `MorphButton`;
  - an outline trigger is 14 at the normal weight;
  - the file picker's field and button, and an empty state's action, are 14;
  - the banner's message stays 13.

  Off, each is `bodyFont`'s size.
- **Mutation controls:**
  - the banner action on `bodyFont`;
  - the trigger's filled branch on `strongFont` (it fails the weight equality).

## Q4: the `label` text role, and a group's legend drawn in it

### Decision

- **The role.** `TextRole::Label` goes at the end of the enum (enumerator 11): "a field's or a group's caption:
  `font.size.caption`, regular, the text colour". Its document name is `label`.
  - It joins `kRoles` (`document.hpp:4247`, 12 to 13), the interpreter's role mapping, the author API's names and a
    table column's `role`.
  - `enumName` gains `Label`.
- **The group.** `lowerGroup` gives its legend `TextRole::Label`.
- **Qt Quick.** `MorphText` maps role 11 to `Theme.captionFont`. `textColorOf` keeps the text colour, and its comment
  gains `Label`.
- **Terminal.** `roleStyle` falls through to `textNormal`.

### Spec

- `document.md:259` and the `group` row;
- `view_tree.md`;
- `qt_quick/backend.md:161`;
- `tui/frontend.md`, Text;
- `theme.md:197`.

### Tests

**Discriminating:**
- **Document:** `label` is accepted, and unknown roles are refused.
- **Interpreter:** `label` lowers to `Label`, and a `group`'s legend is `Label`. **Control:** `Normal`.
- **`RecordingBackend`:** records `Label`. **Control:** "unknown".
- **Qt Quick:** a `label` text is `captionPixels` in `scheme.text`. **Control:** role 11 → `Normal`. That fails only
  because `captionPixels` differs from `bodyPixels`; the default test theme's 11 against 12 gives that (R-S12).

**Guard:** the terminal draws `label` as `textNormal`. It passes without Q4, and says so.

## Q5: a collapsible lays out its header and body with no spacing

### Decision

- `MorphCollapsible.qml`: `column` sets `spacing: 0`.
  - A closed collapsible is as tall as its header.
  - An open one is its header, then its body directly under it, as on the terminal.
- The comment states it in one clause.
- **Spec:** `qt_quick/backend.md`, the Collapsible row.
- **Watched** (R-S12): `MorphPanel.qml:98` writes an explicit 5 px between a panel's header and its content. That is
  the same untokened gap, which no finding shows.

### Tests

- **Qt:** a closed collapsible's height equals its header's, and an open one with a `fixed` body is header plus body.
- **Control:** `spacing: 5` fails both, by 5 and by 10 px.

## Q6: no mid-word breaks on Qt Quick (#1080), after measuring runs in their drawn faces

Q6 has two parts, in this order. Q6a is the run-measurement defect (R-S9; `lg-q-issue-runmeasure.md`). Q6b's minimum
is built from Q6a's measurements, so Q6a lands first.

### Q6a: runs measured in the faces they are drawn in

- **The defect.** `MorphText` measures a strong run in `Theme.strongFont`, which is at the body size, and a mono run
  in `Theme.monoFont`, at `font.size.mono`. `runsHtml` draws both at the label's own size. A run's width is therefore
  wrong:
  - in every role whose size is not the body's (`heading`, `caption`, `label`, `small`, `display`);
  - for every mono run in a body text.

  **Reproduced** for one bold string: 132.5 at 12 px against 143.5 at 13.
- **Fix.** `strongMetrics.font` and `monoMetrics.font` follow `label.font`, with only the weight or the family
  replaced:
  - strong: `Qt.font` with `label.font`'s families, pixel size and features, at `Theme.textWeightOf(strongWeight)`;
  - mono: the same size and weight in `Theme.monoFamilies`.

  `faceOf` is unchanged. Every caller (`implicitWidth`, `widthFreeNatural`, `wrapMinimumWidth`, `elideRuns`) follows.
- **Test:** texts in `heading`, `caption` and `normal`, each with a strong and a mono run. On one line, `implicitWidth`
  equals the label's `contentWidth` within 1 px. **Control:** the `Theme.strongFont` face fails heading and caption.
- **Spec:** `qt_quick/backend.md`, the Text row: "a run is measured in the face it is drawn in".

### Q6b: a wrapping text is never narrower than its widest unbreakable segment

- **The contract.** One sentence in `backend_contract.md`, "Content that does not fit", after the badge sentence:

  > On a frontend that wraps text, a text that wraps, and a key-value's value, are never laid out narrower than their
  > widest unbreakable segment, so a stack or a panel that holds one is raised with it, as with a badge; a one-line
  > text, which elides, has no such minimum. The terminal does not wrap a text: it cuts each line at its width and
  > keeps no minimum for it.

- **The measurement, in C++.**
  - **`TextMeasure`** is a `QObject` in `src/qt_quick` (`text_measure.hpp` and `.cpp`) with
    `Q_INVOKABLE qreal widestSegment(QVariantList pieces)`. Each piece is `{text, font}`, in order, which is a run
    with its drawn face.
  - It joins the pieces' plain text and walks `QTextBoundaryFinder(QTextBoundaryFinder::Line, …)` over it. A segment
    is the text between two line-break opportunities, with its trailing whitespace dropped, as Qt's line width drops
    it.
  - A segment's width is the sum of `QFontMetricsF(piece.font).horizontalAdvance` over the parts of it each piece
    holds. The result is the widest segment.
  - This is the analysis Qt's own line breaker uses (`QUnicodeTools`).
- **How QML reaches it.** The module declares no C++ types (`NO_PLUGIN`), so the backend writes one `TextMeasure` as
  `Theme.textMeasure` (a `QtObject` property, default null) when it starts. It writes it where it writes
  `platformMonoFamily`.
  - While the property is null, `MorphText` and `MorphKeyValue` keep no minimum. The backend always sets it, and a
    Qt test asserts that it is set.
- **`MorphText`.**
  - `wrapMinimumWidth` becomes `Theme.textMeasure.widestSegment(…)` over the text, or over the runs with
    `faceOf(run).font`, plus `wrapSlack` and the required mark. The JS `\s` split is removed.
  - `contentMinimumWidth: root.oneLine ? -1 : root.wrapMinimumWidth`.
  - `shrinkWidth` keeps reading `wrapMinimumWidth`.
- **`MorphKeyValue`.**
  - **Each value.** A `Value` label's `Layout.minimumWidth` is its widest segment, in its drawn font: `strongFont` for
    a toned value, else `bodyFont`, with runs in their faces.
  - **The kind's minimum.** `contentMinimumWidth` is:
    - **beside:** the caption column's minimum (`captionUnits × unitWidth` when set, else the widest caption, which is
      `NoWrap`), plus `2 × unitWidth`, plus the widest value minimum;
    - **stacked:** the widest of every caption and value minimum.
- **Unchanged, and said so in `qt_quick/backend.md`:**
  - `Text.Wrap` stays as the last resort when a container cannot give the room. The container then reports its
    overflow on `morph.layout`, as for a badge.
  - Buttons keep their natural minimum (Q1). Selects and inputs keep none.
  - A banner's message, a dialog's description and a toast's label are sized by their own kinds.
  - A `fixed` width has no minimum on either frontend. `GridLayout` shrinks a `fixed` neighbour, and the terminal's
    `shrinkFromLast` does the same.
- **Terminal.** Unchanged. `tui/frontend.md`'s Text row says: "cut at its width, with no minimum: the terminal does
  not wrap".

### Tests

- **Qt, the measurement against Qt's own wrap.** For each text of the review's table:
  - `widestSegment` equals, within 1 px, the widest line of a hidden `Text { wrapMode: Text.WordWrap; width: 1 }` in
    the same font;
  - the texts are `Assay · 95.0–105.0`, the same with U+200B, `Paracetamol 500 mg` joined by U+00A0, `a/b/c/d`, and
    `SOP‑CHR‑012.`;
  - a two-run case adds a strong half-word.

  **Control:** the old `/\s+/` split fails at U+00A0 (72.4 against 119.4) and at `a/b/c/d`.
- **Qt, console's shape:**
  - **setup:** a `fixed(30)`-unit row of a `stretch(1)` text "Assay · 95.0–105.0" and a `fixed(20)` input;
  - **assert:** the text is at least its widest segment, and no laid-out line starts with "0".
  - **Control:** `contentMinimumWidth: -1`.
- **Qt, vocal:** a `fixed(4)` column holding a 30-character word reports one overflow naming it. **Control:** no
  report without Q6b.
- **Qt, key-value:** a stacked and an inline `keyValue` in a content-sized column raise its minimum to their widest
  value segment, plus the caption column and gap when inline. **Control:** the kind's minimum unset.
- **Qt, setup:** `Theme.textMeasure` is not null after the backend starts.
- **Terminal (guard):** a text's `contentMinimumWidth()` stays 0.
- **Mutations:**
  - `contentMinimumWidth` unset;
  - the `/\s+/` split restored;
  - the key-value minimum unset;
  - runs measured in body faces (Q6a's).

### The measurement on the gallery

- Grab all 39 after Q1 to Q5 and again after Q6, then pixel-diff the two sets.
- **Expected** (**inferred**: a layout reads minimums only when it is short of the preferred widths, and at the judged
  widths only console 1000 breaks inside a segment): only console 1000's result lines change.
- The render smoke stays free of warnings.
- Every Qt test suite runs. Many resize to 400 × 200, and a new overflow warning there is a finding, not a test to
  relax.

## Lane Q-S: screen items

Q-S owns `examples/lims_gallery`. S1 to S3 start now. **S4 is dropped** by the coordinator: with Q6b, "95.0–105.0"
stays whole without a U+200B. The numbers are kept so that references stay stable.

| # | Screen | Finding | Change | When |
|---|---|---|---|---|
| S1 | instrument 1215/1000 | L: cards 8 px apart against 14 | `gap = 3` on the overview row and both columns (`instrument.cpp:335-343`). Measure 12 px | now |
| S2 | plate 390 | L: well to label 5 against 10 | `.gap = 2` on the run line's row (`plate.cpp:678`). Measure 8 px | now |
| S3 | results, review, plate at 390 | L: pairs 4 px apart against 8 | `.gap = 2`: `results.cpp:650` and `:639-647`, `review.cpp:846-848`, `plate.cpp:737` | now |
| S4 | — | dropped (Q6b) | none. Console 1000 is measured under S11 | — |
| S5 | review 1215 | L: the comment card's title is muted | `.role = "label"` with strong runs. Measure 12 px, strong, `scheme.text` | after Q4 and Q6a |
| S6 | receipt 1215 | L: legend 13 against 12 | no code. Measure against "Sample type" | after Q4 |
| S7 | instrument 390 | stacked captions | no code. Measure the caption (about 43 px for "Location") against the value at 13 px (about 187 px of ink) | after Q2 |
| S8 | console 390 | "Open run" | no code. Measure 14 px (about 55 px of ink) | after Q3 |
| S9 | plate 390 | L: collapsed rows 50 against 45 | no code. Measure Row B to H at 45, Row A to B at 45 | after Q5 |
| S10 | the Q1 hand-back list | M: stretch pairs unequal | no code. Measure (R-S4): (a) each 1:1 pair equal within 1 px, review's 1:1:2 in proportion, the 2:1 and 17:10 columns; (b) **every stretch button's caption ink unchanged from P**, for example "Return…" 53 px, "Reject…" 51 and "Log maintenance" 106, and none elided or cut; (c) a button raised to its natural width says so, with both widths | after Q1 |
| S11 | all ten screens at 1215, 1000 and 390, plus 1920 for receipt, audit, stability and review | Q2 moves `bodyFont` text from 12 to 13 px; Q6 changes minimums | re-grab everything. Then: the 1000 rule; console 1000's result lines whole (no "105. / 0"); stability's summary **without** its 29-unit pane minimum, keeping that minimum only if the mockup's width needs it; the `captionWidth` columns; P's pitches; `fixed(n)` inputs (console's results, receipt's fields) and `fixed(n)` heights holding text (R-S11). A row that now wraps, clips or reports an overflow is reported with its measurement, not narrowed by hand | after Q2, then after Q6 |

**Standing checks for every item:**
- the gallery's unit and render tests are green;
- the render smoke logs no Qt warning (an overflow report is one);
- each fix is measured on the grab it targets;
- 1000 is checked for clipping.

A step that misses the design by more than 2 px is reported, not tokenised. The gallery tests that pin P1's or P6's
sizes are updated in Q-S.

## Accepted, no work

A1 to A7 from batch P stand.

| # | Finding | Why |
|---|---|---|
| A8 | plate's legend chips are outline badges about 18 px tall; the design draws 26 px round wells | A 26 px swatch is a pixel size on one legend, the `diameter` option P3 rejected. The chips still name each state. **Watched:** a second sized swatch means a `swatch` kind |
| A9 | plate 1000: selected rings meet edge to edge | P3's criterion was no overlap, and they touch without overlapping. Keeping them apart would draw every round button's ring inside its circle, for one width that is not judged |
| A10 | e-signature's Flags on one line; the compact id is bold | The font face; not counted |
| A11 | audit's "Load 25 more" is grey | Disabled: the fixture has 4 entries |
| — | instrument's 27 against 29 px pitch; the fit wraps at 1000; the data | Accepted in P |

## Lanes

- **Q-F.** One Sonnet lane, in its own worktree from `lg-fold-p`. It owns `include/morph`, `src`, `tests` and
  `docs/spec`.
- **Q-S.** In `morph-lims-e2`. It does S1 to S3 now, rebases onto Q-F, then does S5 to S11.
- **Shared files:** none.
- **Builds:** under `flock $S/morph-build.lock`, `-j10` at most.
- **`morph-lims-e1`:** neither lane touches it.

### Q-F, one commit per item, in this order

1. **Q2:** `Theme.qml`, the theme tests (the no-family variant, with P6's test under it), `theme.md`. It hands back
   instrument at 390 and 1215 and receipt at 1215. Closes #1079.
2. **Q1:**
   - `MorphStack.qml`, `MorphWidget.qml`, `MorphSpacer.qml` and `MorphButton.qml`;
   - `layout.cpp`, `widget.cpp`, `container_widgets.cpp` and `leaf_widgets.hpp`/`.cpp`;
   - the conformance cases;
   - the Qt and terminal tests, with `LoopWatch` widened;
   - the specs.

   It hands back the R-S7 list. Closes the row/column part of #1078; the issue stays open for the containers named
   under Scope.
3. **Q3:** the four QML files, the tests and the specs.
4. **Q5:** `MorphCollapsible.qml`, a test and a spec line.
5. **Q4:** `view.hpp`, `document.hpp`, `interpreter.hpp`, `author.hpp`, `MorphText.qml`, `MorphWidget.qml`'s comment,
   `recording_backend.hpp`, the tests and the specs.
6. **Q6a:** `MorphText.qml`'s faces, a test and a spec line. Closes the run-measure issue.
7. **Q6b:**
   - `text_measure.hpp`/`.cpp` (in the `morph_qt_quick` target's sources) and the backend's start-up;
   - `Theme.qml`'s `textMeasure`, `MorphText.qml` and `MorphKeyValue.qml`;
   - the tests;
   - `backend_contract.md`, `qt_quick/backend.md` and `tui/frontend.md`.

   It hands back both grab sets and their diff. Closes #1080.

**Before reporting done,** Q-F re-runs:
- every touched suite;
- the three conformance runners;
- the gallery's render smoke;
- the docs target under clang with `WARN_AS_ERROR` (`TextMeasure` and `TextRole::Label` are public or documented);
- clang-tidy on the touched TUs;
- qmllint.

**Mutations, each reported as failing:**
- **Q1:** the eight listed under its tests;
- **Q2:**
  - the family-only condition;
  - P6's caption on `bodyFont`;
- **Q3:**
  - the banner action on `bodyFont`;
  - the trigger's filled branch on `strongFont`;
- **Q4:**
  - role 11 → `Normal`;
  - `enumName` without `Label`;
  - the legend as `Normal`;
- **Q5:** `spacing: 5`;
- **Q6a:** the strong face on `Theme.strongFont`;
- **Q6b:**
  - `contentMinimumWidth` unset;
  - the `\s` split;
  - the key-value minimum unset.

## Risks

- **Q2** moves body text from 12 to 13 px across the gallery, about 8 % wider.
  - S11 re-grabs everything.
  - Units stay on the platform font, so a `fixed(n)` holds fewer characters and slightly fewer lines.
  - Only the gallery sets a body size without a family (read by the review), so no other theme or test moves.
- **Q1** changes:
  - the natural lengths of rows and columns with two or more stretching children, a spacer counting as one;
  - the terminal's `distribute` wherever a stretch share falls below its minimum. That reaches grids with stretch
    tracks, splitters and wrapping lines as well as stacks: a minimum now freezes and re-shares instead of taking from
    the last items. Terminal tests that pin such a case get expectations computed from the rule, not re-recorded;
  - every stretch push button's floor. A button no longer gives up its label, so a row of buttons too wide for its
    container now reports an overflow on Qt (vocal) and raises its column's minimum on both frontends. S10 and S11
    look for it.
- **Q1's remaining part.** Panels, collapsibles, scrolls, slots, tab pages, dialogs and drawers keep Qt's own
  stretch-height growth while the terminal shares by weight. This is in #1078 and in `qt_quick/backend.md`.
- **Q1 and loops.** The shares follow the stack's length, and neither natural may read them. `LoopWatch` now counts
  polish loops as well.
- **Q4** changes every `group` legend, forms included. That is intended.
- **Q5** moves every open collapsible's body up 5 px and drops 5 px after it.
- **Q6b** gives every wrapping text and key-value value a minimum.
  - Where nothing breaks inside a segment today, nothing should move (**inferred**). The grab diff is the check.
  - At widths narrower than 390, which are not judged, more containers report overflows. That is the intended vocal
    failure.
  - An under-measured segment would leave a break, and an over-measured one would raise a minimum needlessly. The
    equality test against Qt's own wrap is the guard.
- **Qt 6.8.1.** No API used is newer than 6.8 (read by the review; `QTextBoundaryFinder` dates from Qt 4). Loop
  wording and Q1's controls are verified on 6.12 only.
- **CI traps:**
  - Doxygen under clang (`TextRole::Label`, `TextMeasure`);
  - MSVC `typename`;
  - Qt keywords as identifiers (`TextMeasure` must not name anything `slots` or `emit`);
  - GCC 15 `-O3`.

## Decisions

1. **`stretch` on rows and columns, on both frontends:**
   - stretching children share by weight from zero, with CSS's bounds resolution, and a content spacer counts as
     `stretch(1)`;
   - a stack's natural length and Qt's free natural give each child its natural (`Σ others + gaps + W × max(n/w)`);
   - a push button's minimum is its natural width.

   Other containers keep their stretch-height rule; that is the remaining part of #1078.
2. **The body fonts follow `font.size.body`** whenever a theme sets it. Units still follow the family alone.
3. **Every push button the library draws takes `controlFontOf` at `MorphButton`'s weights.**
4. **The `label` text role** is a field's caption, and a group's legend is drawn in it.
5. **A collapsible has no spacing of its own.**
6. **Runs are measured in their drawn faces.** On Qt Quick, a wrapping text and a key-value's value keep their widest
   unbreakable segment, measured in C++ with `QTextBoundaryFinder`. The terminal, which does not wrap, is unchanged.
   S4 is dropped.
7. **Accepted:** A8 to A11.

## Records

- **Filed:** #1078, #1079 and #1080.
  - #1078 gains the correction section of `lg-q-issue-stretch.md`: B1, B2, the bounds rule, spacers, and the scope with
    its remaining part.
  - #1080 is re-written from `lg-q-issue-midword.md`, as a `bug`, with the terminal answer read from the code.
- **To file:** `lg-q-issue-runmeasure.md` (`bug`, `area: qt`), closed by Q6a. None of these issues gets a `triage:`
  label.
- **Not filed:**
  - Q3 (its body is `lg-q-issue-banner.md`);
  - Q4 and Q5, each smaller than an issue;
  - the button minimum, which is part of Q1's change;
  - the screen items.
- **Corrections to the after-P reports.** `bodyFont` text in those grabs is 12 px, not 13. "Open run" is 12. P6 was
  masked, not stale.
- **Watched:**
  - A8;
  - `MorphPanel`'s explicit 5 px header gap;
  - P's list-row pitch.
- **Probes kept** in `$S/lgq-exp/`: `row`, `control`, `font`, `font2` and `runs`, as `.qml` and `.out`, with the pixel
  scripts.

## Review round 2

| Item | Change |
|---|---|
| **B1** Q1 not applied to `widthFreeNatural` | One `naturalAlong(accessor)` feeds `naturalLength` and a row's `widthFreeNatural`. New conformance case `stretchRowNestedKeepsNaturals`, and a Qt table-cell test. The summing `widthFreeNatural` is a listed mutation |
| **B2** the terminal test pinned 15 | The values are computed from the formula: 20, **30** (10/10 at 1:2), 30 (2:1) and 15 (4/10 at 1:2), against the sum's 11, 20, 20 and 14 |
| **R-S1** reach differs between frontends | Option (a), the same scope on both: `row` and `column` only. The terminal adds `weightedNaturalSize` for `StackImpl` and leaves `stackNaturalSize`'s twelve other callers, all named, on the sum. The Qt containers that keep their own growth are named in Risks, in the backend spec and in #1078's remaining part |
| **R-S2** spacers | A content spacer is `stretch(1)` with natural 0 on both frontends (`expandsByDefault` on `MorphSpacer`). New case `spacerCountsAsStretch` |
| **R-S3** the minimum rule | CSS's freeze-and-re-share, in passes, on both frontends. `shareCapped` becomes that loop. New case `stretchSharesFreezeAtMinimum` (50/20/20, against 50/30/10 today). "CSS `flex: w 1 0`" is kept for the from-zero basis only. The risk to grids, splitters and wrapping lines is stated |
| **R-S4** clipped stretch buttons | The coordinator's decision: a push button's minimum is its natural width on both frontends, with a conformance case and a mutation. Selects, inputs and menu triggers keep none, with the reason. S10 checks each stretch button's caption ink against P (for example "Return…" 53, "Log maintenance" 106) and that none is cut |
| **R-S5** the loop test | `LoopWatch` also counts case-insensitive "polish" + "loop". The control is `implicitWidth` reading `layout.implicitWidth`. `QTest::failOnWarning` is dropped |
| **R-S6** discriminating tests | The natural case asserts at least the natural less one unit. Cap and minimum Qt cases are labelled guards, with the wrapping-row cap test named as the cap mutation's control. Re-share is the discriminating Qt case |
| **R-S7** the hand-back | It adds the gallery dialogs at 1215 and 390, console at 1215 and 1000 (17:10 expected at 606 / 357), the Containers page and a forms dialog |
| **R-S8** the menu trigger's weight | The trigger follows `MorphButton`'s rule: `buttonWeight` filled or square, normal for 0, 3 and 4. A test compares a primary trigger's weight with a primary button's |
| **R-S9** run measurement | Q6a, before Q6b: the faces follow `label.font`. Issue body `lg-q-issue-runmeasure.md`, for the coordinator to file. S5 waits for it |
| **R-S10** build #1080 | Q6b, the coordinator's decision: `QTextBoundaryFinder` in C++ (`TextMeasure`), checked against Qt's `WordWrap`; `MorphKeyValue` covered; one contract sentence; the terminal unchanged. A10 is removed and S4 is dropped |
| **R-S11** Q2 line heights | `theme.md` and Q2 say that a `fixed(n)` height holds slightly fewer 13 px lines. S11 measures `fixed` heights holding text |
| **R-S12** accuracy | #1080 is re-written as a `bug`, with the terminal answer read from code. #1078 gains B1 and B2. The flexbox citation now names the rule chosen, not browser behaviour. `MorphPanel`'s 5 px is Watched. Q4's terminal test is labelled a guard, and its Qt control's dependence on the default theme's sizes is stated |

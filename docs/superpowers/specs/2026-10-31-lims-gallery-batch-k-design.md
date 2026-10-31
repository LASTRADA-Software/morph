# LIMS gallery batch K: a selection that keeps its text, a principal on the actions' line, gaps through a switch, and the controls' last insets

Batch K closes the framework gaps the two comparisons after batch J found (ten screens at 1000×830 and 390×844,
against the mockups and the batch-I grabs). The per-screen fixes those reports mark "screen" are lane K-S's work and
are not designed here. This design takes the nine items that need ui/1 itself to change, or that had to be settled
before a screen can rely on them.

The aim is still the review's: ui/1 produces each mockup's look directly, and a screen never works around the
framework. The rules of batches G to J hold:

- a look is a token or a rule, never a screen's choice of pixels;
- a key means the same thing on every kind that takes it;
- a look that changes every application's shell is the theme's to switch;
- the terminal keeps its cells. A purely visual change says what it leaves unchanged there;
- vocal over silent: where the framework cannot do what a document asks, it says so.

Every claim was checked against `300cee8c1` (batches A to J), with `build/linux-everything` rebuilt at that revision.
Each item says whether its premise was **measured** (run or probed here) or **read** (inferred from the code). The
comparison's grabs are in `lg-cmp/app-j`; probe output is in the session scratchpad (`lg-kd-probe/`). Neither is in the
tree.

## Probes

- **P1. The head line, instrumented.** `MorphStack.place()` was given a temporary `console.warn` that, for
  `shell_topbar_main` and `shell_title_line`, printed the line width, the unit, and each child's basis, width, x, y
  and implicit width. `MorphText` was given one that printed a text's widths when its line count changed. The client
  was rebuilt and every screen grabbed at 1000×830 with `--user "Dr. A. Meyer"`. Both files were then restored with
  `git checkout`, touched and rebuilt, and a re-grab printed no probe line. The final layout of each head (widths in px, unit `Theme.unitWidth` = 7.67 px, so 32 units = 245.5 px):

  | Screen | `shell_topbar_main` width | Lead block basis (natural) | Trail | Lines |
  |---|---|---|---|---|
  | samples | 676 | 67 (67) | 664 | **2** |
  | plate | 676 | 245.5 (420) | 461 | **2** |
  | instrument | 676 | 245.5 (404) | 230 | 1 |
  | console | 676 | 72 (72) | 469 | 1 |
  | audit | 676 | 76 (76) | 533 | 1 |
  | receipt | 676 | 130 (130) | 272 | 1 |
  | results | 676 | 245.5 (384) | 84 | 1 |
  | stability, review, esignature | 676 | 245.5, 159, 90 | none | 1 |

  The page beside the sidebar is 785 px. The top line pads its ends by step 3 and puts the principal after the
  wrapping row, a step-3 gap away, so the row gets 785 − 3 × 12 − 73 = 676 px; the principal is 72.5 px
  (`shell_head_principal`, measured).

- **P2. "System suitability", instrumented** (P1's `MorphText` probe, results at 1000 px):

  ```
  KPROBE text "System suitability"  lines 1 width 100.96875 ... implicitWidth 100.96875 label.implicitWidth 99.96875 advance 99.96875
  KPROBE text "System suitability"  lines 2 width 100.96875 ... implicitWidth 100.96875 label.implicitWidth 101.890625 advance 99.96875
  ```

  The text was sized from an advance of 99.97 px. Its label then changed font and needs 101.89 px, and the text's
  width did not follow.

- **P3. Text widths in stock Qt** (`lg-kd-probe-text.qml`, `qml6` offscreen): bold 13 px "System suitability" has
  an advance of 110.4 px and a label width of 111.4; regular has 99.97 for both. A wrapping label given its advance
  plus 1 px keeps one line, in a `RowLayout` too (`lg-kd-probe-text2.qml`). So the 1 px slack is right when the
  advance is read in the label's own font.

- **P4. The Basic check box** (`lg-kd-probe-check.qml`): `leftPadding 6 padding 6 spacing 6 indicator.x 6`. In the
  receipt grab at 1000 px, "Condition on receipt" starts at x = 245, the first box's fill at x = 250 (to 266), and its
  label at x = 272.

- **P5. Colours in the grabs.** Review at 1000 px: the selected queue entry's WS id draws (23,120,74), the accent;
  its product line (77,84,97), muted, as the unselected entries' do; an unselected WS id draws (28,30,33). Samples at
  1000 px: the selected sidebar entry's label draws (201,204,214) (`#C9CCD6`), the same as every other entry's; the
  design's `.nav.on` is `#fff` against `#C9CFD8` at rest.

- **P6. Where a grab's warnings go.** Run as `visual-review.md` writes it, the instrumented client wrote nothing to
  stderr (0 lines, the probe included). With `QT_FORCE_STDERR_LOGGING=1` the same run wrote 33 probe lines. Qt sends
  its messages to the journal when stderr is not a console, so **every `.err` file of the batch J grabs is empty
  whatever happened**, and the comparison's "no overflow report" is unverified, as reviewer A suspected. One data
  point with the variable set: review at 1000 px reports nothing.

## The items

| Item | Verdict | Where it lands | Lane |
|---|---|---|---|
| K1 a selected item's content in the accent | **Measured** (P5), **read** (`MorphButton.contentScheme`) | a colour role `color.selectionText`, following the surface's text; neither look recolours | K-C |
| K2 the head wraps | **Measured** (P1). The principal's 85 px causes both wraps; J5's minimum causes neither on its own | the principal joins the trail; samples and plate narrow their trails (K-S) | K-B |
| K3 the principal off the actions' line | **Read**, consistent with P1: the principal is centred on the whole top line | the same change: the principal sits in the trail, centred on the actions | K-B |
| K4 a switch case ignores its column's gap | **Read** (`lowerSwitch`, `holding`) | a case's and a default's holder take the gap of the column they sit in | K-A |
| K5 "System suitability" wraps | **Measured** (P2, P3). A library defect: a text's width does not follow its font | the width binding depends on the font | K-A |
| K6 the dialog's two rules | **Read** (`MorphDialog`, `MorphDrawer`) | both rules draw the card's `divider` | K-C |
| K7 check boxes inset from their label | **Measured** (P4) | the box sets its own sides and spacing | K-A |
| K8 cards draw a shadow | **Read**: `elevation.card` already exists, and dialogs read it too | the gallery sets `elevation.card` flat; dialogs and drawers read `elevation.dialog` | K-C |
| K9 the date input's calendar icon | **Read**: the button is the calendar's only way in | keep it | none |

## K1: a selected list item keeps its text colour

**Problem.**

- **Measured (P5).** Under the `accent` look, the selected review entry's WS id draws in the accent. The design's
  `.qi.sel` (09) is `background: #EAF5EE; box-shadow: inset 3px 0 0 #17784A`, with no `color`. Console 390's
  compact list is the same: the mockup's `.sel td` sets a background and a bar only.
- **Read.** `MorphButton.contentScheme` sets `text: root.accentSelection ? root.scheme.accent : root.scheme.text`;
  the item's own label and icon do the same. The muted line beside the WS id stays muted, because only the `text`
  role is swapped.
- **The `fill` look misses too (P5).** The design's sidebar (10) draws `.nav` in `#C9CFD8` and `.nav.on` in `#fff`.
  Under `fill` our label keeps the surface's `color.text`, `#C9CCD6`, so the selected entry's label does not
  brighten.
- So neither design recolours a selected item to its accent: one keeps the text and the other lightens it. What
  changes is a text colour of the selection's own, which no token states today.

**Decision.**

- **What a look marks.** `list.selection` keeps its two values, and each now names only the mark:
  - `accent`: the selection colour and a 3 px accent bar at the leading edge;
  - `fill`: the selection colour alone.
- **What a selected item's text draws in.** A new colour role, `color.selectionText`, read per surface as every
  colour role is. A selected item-style button draws its label, its icon and its content's `Normal`-role text in it,
  under either look. A surface that does not set it, in a theme that does not set it either, takes that surface's
  `color.text`, so the default is the text colour unchanged.
  - Muted, tones and every other role of the content stay the item's.
  - The terminal marks a selected item by inverting it and reads no colour token, so nothing changes there.
- **How "follows the text" is drawn.** `MorphScheme` gains `property color selectionText` and `property bool
  hasSelectionText`, and `readonly property color selectedText: hasSelectionText ? selectionText : text`.
  - `theme.cpp` writes both for the theme's scheme and for each surface's: `hasSelectionText` is true only where the
    resolved values hold the token. Writing `false` when it is absent is what lets a bundle without the token restore
    the default.
  - Every built-in scheme in `Theme.qml` leaves `hasSelectionText` false, and `toolbarJoinedScheme` binds both new
    roles to `barScheme`'s, as it binds every role.
  - `kThemeTokens` gives `color.selectionText` an empty default, and `fillValue` leaves an empty-default colour out of
    `ThemeValues::colors` unless the theme sets it. That is the first colour that follows another; the token table's
    comment says so.
- **`MorphButton`.** `accentSelection` becomes `markedSelection` (`style === 1 && selected`), and the label, the icon
  and `contentScheme.text` read `root.scheme.selectedText` while it is true. `contentScheme` copies the two new
  roles. `ButtonImpl::scheme()` keeps returning `contentScheme`, so J10's mechanism is unchanged; only the colour it
  carries changes.
- **Contrast.** `themeContrastProblems` gains one rule: where the theme sets `color.selectionText`, it holds 4.5:1
  on `color.selection`.
- **The gallery.** Its `sidebar` surface sets `color.selectionText: #FFFFFF`. The base leaves it unset, so review's
  queue and console's compact list draw their text as the mockups do.

**Effect on the other example apps.** Every shell app's navigator, and any list item, drew its selected label in the
accent under the default `accent` look. It now draws it in the surface's text colour, with the tint and the bar as
before. An application that wants the old look sets `color.selectionText` to its accent. This is Q1.

**Spec.** `theme.md` (the token in the closed set; `list.selection`'s row, which no longer says "the label in the
accent"; Surfaces); `qt_quick/backend.md` (the item button's selected look).

**Tests.**

- `tests/qt_quick/test_qt_quick_widgets.cpp`:
  - a selected item with content under `accent` on the theme's scheme: the bar shows, and the content's text, the
    label's colour and the icon's colour are the scheme's `text`;
  - on a surface that sets `color.selectionText`, under `fill`: no bar, and the label and content's text are that
    colour; deselecting returns them to `text`, with no re-adoption;
  - **Control:** J10's `contentScheme` (`text` is the accent while selected under `accent`). The first case fails.
- `tests/qt_quick/test_qt_quick_theme.cpp`:
  - a theme whose `sidebar` surface sets the token gives `sidebarScheme.hasSelectionText` true with that colour;
  - the theme's scheme has it false, and `selectedText` equals `text`;
  - a re-apply without the token sets `hasSelectionText` false again.
- `tests/test_ui_theme.cpp`:
  - `themeValues()` of an empty set has no `color.selectionText`; a set that has it carries it;
  - `color.selectionText: #EEEEEE` on `color.selection: #FFFFFF` is a contrast problem.

**Verify.** Re-grab review and console at both sizes. The selected entry's WS id draws (28,30,33). Re-grab any
screen at 1000 px: the selected sidebar label draws (255,255,255), the others (201,204,214).

## K2 and K3: the principal shares the head's line as part of the trail

**Problem (measured, P1).**

- **What wraps.** `_topLine` is `[pad][_topMain][_topPrincipal][pad]`, so the principal and its gap take 85 px from
  `_topMain` on every screen. Without them the row would have 761 px:
  - samples needs 67 + 12 + 664 = 743 px, and fits in 761;
  - plate needs 245.5 + 12 + 461 = 718.5 px, and fits in 761.

  At 676 px both wrap. **The principal alone causes both wraps.**
- **J5's minimum is not the cause on samples.** A stretch child's basis in a wrapping row is its minimum, capped at its
  natural width (`MorphStack.naturalBasisOf`), so "Samples" reserves its own 67 px, not 32 units.
  - On plate the block reserves 245.5 px against a 109.5 px title. That is what J5 decided: the reserve is for the
    inline lead, which then wraps under the title.
  - With the principal gone, plate keeps its trail on the title's line, and its lead (420 px natural) wraps under the
    title inside the 288 px left. That is J5's intended look.
- **Where the principal sits (read).** `_topLine` aligns its items to the centre, while `_topMain` aligns a
  multi-line head's items to their tops. So:
  - in a head with an eyebrow or a lead below, the actions sit at the line's top and the principal at the block's
    middle (instrument: y ≈ 23 against ≈ 38);
  - in a head whose trail wrapped (samples), the principal floats between the two lines (y ≈ 42).

**Decision.** The principal is the trail's last item.

- **The trail.** `_topMain` gets a new `_trail`, a content-sized row (gap step 3, `align: center`, test id
  `shell_head_trail`), in the place `_headSlot` had. It holds `_headSlot`, then `_topPrincipal`.
  - `_topPrincipal` leaves `_topLine`, which becomes `[pad][_topMain][pad]`.
  - `_trail` shows while either child does: `setHeadShown`, `setPrincipalInHead`, `setPrincipal` and `setCompact`
    all end in one `showTrail()`.
- **What that gives.**
  - **K3:** the principal is centred on the actions, on whichever line the trail is.
    - In a multi-line head, `_topMain` aligns tops, so the trail and the principal sit on the first line, level with
      the actions.
    - In a one-line head (`setHeadOneLine(true)`), the trail is centred with the title, as now.
    - A head with no actions holds the principal alone at the trail's place.
  - **K2:** the trail and the principal wrap as one block, so the head never splits them across lines.
    - The row now has 761 px, the 85 px the principal took from it before. The block wraps when the lead block's basis,
      a gap, the trail, a gap and the principal do not fit.
    - Five screens with a trail stay on one line (P1: instrument 572.5, console 638, audit 706, receipt 499, results
      426, all ≤ 761).
    - Samples (828) and plate (803.5) still do not fit. They fit once their trails narrow, which is screen work (below).
- **J5 is unchanged**: the block, its minimum, the title line and `setLeadInline` stay as batch J made them.
- **Not chosen.**
  - **A principal that yields.** It would be hidden or elided while the line is short, so samples and plate fit
    unchanged. No wrapping-row rule can say "drop this child first": a basis-0 child already never starts a line, but
    it takes no gap and shares the spare width with the lead block by weight. And a principal that disappears on two
    screens shows the signed-in name nowhere on them, since the sidebar's foot is hidden while it is in the head. This
    is Q2's alternative.
  - **The lead block's minimum taken from the title.** It would let plate keep its trail without narrowing it. But the
    block's real minimum is its widest unwrappable part, which on plate is the 302 px lead, and no basis rule
    measures "the title alone" on both backends.
  - **A per-screen principal**, shown only where a mockup draws it (console). It is shell chrome, and moving it
    between screens reads as a bug.

**Per backend.** Both frontends lay the frame out from these stacks. On the terminal the principal is drawn on the
trail's row after the last action, and moves with the trail when it wraps.

**Spec.** `shell_frame.md`: the Shape table (`shell_topbar_line`, `shell_topbar_main`, the new `shell_head_trail`
row, `shell_head_principal` now inside it), `setPrincipalInHead`, and the "head wider than its share" row, which no
longer says the principal sits outside the line.

**Tests.**

- `tests/test_ui_shell_frame.cpp`:
  - `shell_head_trail` holds `shell_head` then `shell_head_principal`, and `shell_topbar_line` holds no principal;
  - the trail is hidden while the head is hidden and the principal is not in the head, and shown while either shows.
  - **Control:** the old tree. The first check fails.
- `tests/qt_quick/test_qt_quick_containers.cpp`: an expanded `ShellFrame` with an eyebrow, a title, a lead below, a
  trail of one button and a principal in the head.
  - The principal's vertical centre is the button's (±1 px).
  - The frame sized so that the trail and the principal fit beside the lead block's minimum keeps them on the title's
    line. One unit narrower, both are on the second line.
  - **Control:** the old tree. The principal's centre is the top line's, more than 10 px below the button's.
- `tests/tui/test_tui_shell_frame.cpp`: at 80 columns, the principal is drawn on the trail's row, after the action.
- `tests/client/test_shell.cpp`: the principal-in-head case finds `shell_head_principal` under `shell_head_trail`.

**Verify.** Re-grab instrument at 1000 px: "Dr. A. Meyer" is centred on Log maintenance and Book (y ≈ 23). Re-grab
samples and plate at 1000 px once K-S has narrowed them: one line each, and the head 48 px tall on samples.

## K4: a switch case's nodes take the gap of the column they sit in

**Problem (read).**

- E-signature at 390 px writes `layout::byWidth(width, {signerField(), meaningField(sign)}, …)`, a `switch` whose
  compact case holds two nodes, inside a column whose gap is step 2.
- `ScreenCore::lowerSwitch` lowers each case through `holding(children, inner)`, whose gap defaults to 0, and the
  `default` through `lowerAll(fallback, ctx)`, gap 0 too. So the two fields are a step-0 column inside a step-2 column:
  ~4 px apart, against 16 between the dialog's other fields.
- `document.md` says a node's sizing "means the same inside a `switch` case … as at the root". The spacing between
  a case's nodes does not.
- The reviewer's fix, wrapping the pair in `ui::column(…, {.gap = 2})`, is a screen working around the framework, and
  every multi-node case in a gapped column would need it again.

**Decision.** In a column, a switch case means what its nodes would mean written there.

- `LowerCtx` gains `int listGap = 0`: the gap of the column whose children are being lowered.
  - Only the places that lower a column's children set it: the `column` kind (its `gap`), `lowerArranged` along
    `Axis::Vertical`, and `lowerAll` (its `gap`, the holder being a column). A case's own holder sets it for the
    case's nodes, so a switch in a case inherits it too.
  - Every other place that lowers a child resets it to 0: the `children`/`child` lambdas of `lowerKind` and every
    other caller that builds a `LowerCtx`. So it never leaks to a grandchild.
- `lowerSwitch` passes `ctx.listGap` to `holding(…)` for every case, and to `lowerAll(fallback, ctx, ctx.listGap)`
  for the `default`.
- **A case in a row or a grid is unchanged**: its holder is still a column with gap 0. A row's case content would
  need the holder to become a row and take the row's `align`, and nothing in the examples writes a multi-node case
  in a row. The spec says so.
- **The terminal** gets the same view tree, so its gap is the same step in cells.

**Spec.** `interpreter.md`, the Holders paragraph: a switch case's and a default's holder column takes the gap of the
column the switch sits in. `document.md`, the `switch` row: "several nodes in a case are spaced as the column around
the switch spaces its children".

**Tests.** `tests/test_ui_interpreter_detail.cpp`, on `RecordingBackend`:

- a column with `gap: 2` holding a switch whose matching case has two texts: the case's holder column records
  `gap=2`;
- the same for a `default`;
- a switch inside a panel (whose `lowerAll` gap is the panel's): the holder records the panel's gap;
- a switch inside a case of another switch, in a gapped column: the inner holder records the gap;
- a switch in a row: the holder records `gap=0`;
- a text inside a case's node (a grandchild) does not inherit the gap.
- **Control:** the current `lowerSwitch`. The first three record `gap=0`.

**Verify.** Re-grab e-signature at 390 px. Signer and Meaning are 16 px apart (the column's step 2 plus the field's
own spacing, the same as the other fields). There is no screen change: if K-S has already wrapped the pair in a gapped
column, it removes that once K4 lands.

## K5: a text's width follows its font

**Problem (measured, P2).** The results label "System suitability" is sized at 100.97 px, its regular-weight advance
plus the 1 px slack. Its label then draws in the strong face and needs 101.89 px, so it wraps.

- **Cause (read, consistent with P2).** `MorphText.implicitWidth` is computed by calling `metrics.advanceWidth(…)`. A
  method call registers no dependency on `metrics.font`, so the binding re-evaluates when the text changes but not
  when the font does.
- **Which texts are affected.** Any text whose role is set after its text: the mount binds a node's text before its
  role (`mount.hpp`, the text's `setText` then `setRole`). That covers every section, heading, mono, small, display and overline text a document writes with a fixed
  string, and every text whose theme font changes after mount.
- **Why the shell title is not affected.** It is set after its role, so its advance matches (P1: 328.48 for both).
- **Why batch I had no such wrap.** Its binding read `label.implicitWidth`, which follows the font. The P2 numbers
  also explain the comparison's observation: the slot is 0.92 px narrower than batch I's, so the Pass chips moved by
  less than a pixel.

**Decision.** The width binding depends on the font.

- `MorphText.implicitWidth` reads `metrics.font` before it measures, so a change of role, weight, size or family
  re-measures. `wrapSlack` stays 1, which P3 shows is enough once the advance is read in the label's own font.
- The binding still never reads `label.implicitWidth` for plain text, so the Qt 6.8 binding loop through a required
  text's mark, which reading the label's width closes, stays closed.
- A required text's mark room and a one-line text's elision read the same binding, so they follow too.

**Per backend.** Qt Quick only. The terminal sizes a text in cells and has no font.

**Spec.** `qt_quick/backend.md`, Text: the width is the widest line's advance in the text's current font, plus 1 px.

**Tests.** `tests/qt_quick/test_qt_quick_widgets.cpp`:

- a text given "System suitability" and then the `Section` role, laid out at its implicit width, has one line, and
  its implicit width is at least its label's;
- the same with the `Heading` role, and with the theme's `font.size.body` changed after mount;
- **Control:** the current binding. The section case has two lines.

**Verify.** Re-grab results at 1000 px: "System suitability" is one line, and the first Pass chip moves right by
about 11 px.

## K6: both of a dialog's rules draw the divider

**Problem (read).**

- `MorphDialog` and `MorphDrawer` draw the head rule in `Theme.cardScheme.border` and the footer rule in
  `Theme.divider`. That is the theme-level divider, not even the card's.
- The gallery's grab shows (221,225,230) and (237,240,243). The design draws both rules `#E6E9ED` (`Main.dc.html`,
  `08-Results.dc.html`), and the same colour for the summary box's border.

**Decision.**

- Both rules are rules between sections of a card, which is what `color.divider` names ("a table's row rules and the
  rules of a divided list"; `color.border` is a card's outline). So both draw `Theme.cardScheme.divider`, on the
  dialog, the drawer and the sheet. The comment above the footer rule already says "the divider the cards use
  between sections".
- **In the gallery** both rules become `#EDF0F3`, its divider. That is 7, 7 and 6 levels lighter than the design's
  `#E6E9ED`, and the same as every row rule the design draws in `#EDF0F3`.
- **Not chosen:** a dedicated `color.overlay.rule`. It would be a token for one 1 px line's 7-level difference. This
  is Q4.

**Effect on the other example apps.** A dialog's head rule moves from the default border `#c7c7cc` to the default
divider `#dcdce0`; the footer rule is unchanged unless a surface redefines `card`.

**Per backend.** The terminal draws its rules in its own line character and reads no colour; unchanged.

**Spec.** `qt_quick/backend.md`, Dialog and Drawer: both rules are `cardScheme.divider`.

**Tests.** `tests/qt_quick/test_qt_quick_overlays.cpp`: a dialog with a description and a footer action, under a
theme whose `color.divider` and `color.border` differ: `headRule.color` and `footerRule.color` are both the
divider. The same for a drawer. **Control:** the current files. `headRule` is the border.

**Verify.** Re-grab e-signature at 1000 px. Both rules are (237,240,243).

## K7: a check box sits at its label's edge

**Problem (measured, P4).**

- `MorphCheckbox` sets no horizontal padding, so the Basic style's `padding: 6` puts the indicator 6 px in, and its
  `spacing: 6` sets the label 6 px after it.
- The receipt grab: the group label at x = 245, the box at 250, its text at 272.
- The design's `.ck` is `display: flex; gap: 8px; padding: 3px 0` with `.ck input { margin: 0 }`: the box at the
  group label's edge and the text 8 px after it.
- The table's select box already sets `padding: 0`, so it is aligned.

**Decision.** `MorphCheckbox`'s `CheckBox` sets `leftPadding: 0`, `rightPadding: 0` and
`spacing: Theme.spaceOf(2)`.

- The indicator draws a focused box with a 2 px border inside its own extent and no outer ring, so nothing needs the
  inset.
- The touch-target height on compact is unchanged.

**Effect on the other example apps.** Every check box moves 6 px left, to its column's edge, and its label sits
2 px further from the box.

**Per backend.** The terminal draws `[x] label` from its first cell already; unchanged.

**Spec.** `qt_quick/backend.md`, Checkbox: no side padding; the label a step 2 after the box.

**Tests.** `tests/qt_quick/test_qt_quick_widgets.cpp`: a check box in a column beside a text. The indicator's left
edge in the column's coordinates is the text's (0), and the label starts `indicator.width + Theme.spaceOf(2)` in.
**Control:** the current file. The indicator is at 6.

**Verify.** Re-grab receipt at both sizes. The first box's left edge is the group label's x.

## K8: flat cards, and dialogs that keep their own elevation

**Problem (read).**

- Every card in the design is `.card { background: #fff; border: 1px solid #DDE1E6; border-radius: 6px }`, with no
  shadow.
- The theme already has the token: `elevation.card` draws the shadow under a panel that names no surface
  (`MorphPanel`'s `frameShadow`) and under a `card` surface (`MorphWidget`). So flat cards are a theme setting.
- **But `MorphDialog` and `MorphDrawer` read the card's shadow too.** A theme that makes its cards flat makes its
  dialogs flat, while the design's dialogs keep a shadow (`0 18px 48px rgba(20,24,31,.28)`). So one key means two
  things: the elevation of a card on a page, and that of an overlay above it.

**Decision.**

- **A new shadow token, `elevation.dialog`**: the shadow under a dialog and a drawer (not a sheet, which has none).
  - Its default is `elevation.card`'s default, `0 2 10 #00000014`, so no application's dialogs change.
  - `Theme.qml` gains `dialogShadow*`, which `setShadow(theme, values, "elevation.dialog", "dialog", unset)` writes.
  - `MorphDialog` and `MorphDrawer` read them in place of `cardShadow*`.
- **The gallery's base** sets `elevation.card` to `{ "color": "#00000000", "offsetX": 0, "offsetY": 0, "blur": 0,
  "spread": 0 }`, and leaves `elevation.dialog` at its default. Its cards are flat with their border, and its dialogs
  keep today's shadow.
- **Not chosen:** dialogs following `elevation.popup`. It exists, but it would change every application's dialog
  shadow (from `0 2 10` at 8 % to `0 8 24` at 14 %), and a menu's elevation is not a dialog's.

**Effect on the other example apps.** None: both defaults keep today's look.

**Per backend.** The terminal draws no shadow; unchanged.

**Spec.** `theme.md` (`elevation.dialog` in the closed set, `elevation.card`'s row naming what it is under),
`qt_quick/backend.md` (Dialog, Drawer).

**Tests.**

- `tests/qt_quick/test_qt_quick_theme.cpp`: a theme setting `elevation.dialog` writes `dialogShadowOffsetY`, and
  leaves `cardShadowOffsetY` at its default.
- `tests/qt_quick/test_qt_quick_overlays.cpp`: under a theme whose `elevation.card` is transparent and whose
  `elevation.dialog` is not, the dialog's shadow rectangle has the dialog colour.
  - **Control:** the current `MorphDialog`. The shadow is the card's transparent colour.
- `tests/test_ui_theme.cpp`: `kThemeTokens` holds `elevation.dialog`, with the default above.

**Verify.** Re-grab any screen at 1000 px. Under a card's bottom edge the pixel is the page's background, not a
shade darker. Results at 1000 px: the dialog still has its shadow.

## K9: the date input keeps its calendar button

**Problem (read).**

- `MorphDateTimeInput` draws a `ToolButton` with the framework icon `date.pick` at its trailing edge. The mockups'
  "Received at" (02) and stability's dates (06) are plain text inputs with no icon.
- **But the button is the only way to open the calendar.** The field handles no key that opens it, and clicking the
  field edits its text.
- The mockups are static pictures of a text field. They show no other way to reach a calendar, and do not say there
  is none.

**Decision.** Keep the button. No theme switch and no key.

- **Not a theme switch.** Hiding the button would make the calendar unreachable, against vocal over silent. A theme
  switch that also opened the calendar from the keyboard (Alt+Down, as a combo box does) would be a new interaction,
  designed for one picture's look.
- **Not a node key.** A screen deciding whether its date input has a picker is a screen's choice of looks.
- The difference is an accepted residual, recorded in `visual-review.md` by the coordinator. This is Q3.

## Theme tokens

| Token | Kind | Values or default | Item |
|---|---|---|---|
| `color.selectionText` | colour, read per surface | none: a surface that does not set it, in a theme that does not, takes its `color.text` | K1 |
| `elevation.dialog` | shadow | `0 2 10 #00000014`, `elevation.card`'s default | K8 |

- `kThemeTokens` grows from 70 to 72.
- A bad value of either is refused at `addFile` as any colour or shadow is.

The gallery's bundle (`lims_gallery.cpp`) takes:

- its `sidebar` surface: `color.selectionText: #FFFFFF`;
- its base: `elevation.card` transparent;
- nothing else.

## Questions for the user

| | Question | Recommended |
|---|---|---|
| Q1 | When a theme leaves `color.selectionText` unset, does a selected item's label follow the surface's text (every app's selected nav label loses its accent colour, and keeps its tint and bar) or the accent under `accent` (no app changes; the gallery sets the token on its base)? | **Follow the text.** It is what `accent` was modelled on (09's `.qi.sel`), and one look then means one thing everywhere. An app that wants the accent label sets the token |
| Q2 | At 785 px, samples' and plate's heads cannot hold the title, the trail and the principal on one line. Should those two screens narrow their trails (samples' search, plate's template select), or should the principal give way when the line is short? | **Narrow the screens.** The principal joins the trail and the head never loses the signed-in name. Giving way needs a new wrapping-row rule, and hides the name on exactly those screens |
| Q3 | The date input's calendar button, which the mockups do not draw: keep it, or add a theme switch that hides it and opens the calendar from the keyboard? | **Keep it.** It is the calendar's only way in |
| Q4 | A dialog's two rules: both the card's `divider` (`#EDF0F3` in the gallery, 7 levels from the design's `#E6E9ED`), or a token of their own? | **The divider.** It is the role for a rule between sections |
| Q5 | Flat cards: the gallery sets `elevation.card` transparent, and dialogs and drawers move to a new `elevation.dialog` (default today's card shadow); or dialogs follow `elevation.popup`, which changes every app's dialog shadow? | **`elevation.dialog`**, so no other app changes |

## Defects this batch fixes

These are defects in shipped library behaviour, each closed by the item named. They are this batch's own work, so
they are not filed separately.

- **K1:** under `accent`, a selected item's content draws in the accent, which the look's own design does not; under
  `fill`, no token can lighten the selected label.
- **K3:** the principal in the head is centred on the whole top line, not on the actions' line.
- **K4:** a switch case's or default's nodes ignore the gap of the column the switch sits in.
- **K5:** a text whose role or theme font is set after its text keeps the width of its first font, and wraps.
- **K6:** a dialog's and a drawer's two rules draw two different roles, one of them not the card's.
- **K7:** a check box is inset 6 px from its column's edge.
- **K8:** dialogs and drawers read the card's elevation, so a flat-card theme makes them flat too.

## Not in this batch

- **Grabs that capture warnings (P6).** The grab command in `design/visual-review.md` gains
  `QT_FORCE_STDERR_LOGGING=1`, so a grab's `.err` file holds the overflow report and every Qt warning. That is
  example configuration and is fixed in place by K-B's gallery commit, not filed. Until then, any "no warnings"
  judged from a grab is unverified.
- **A shadow drawn blurred.** `MorphPanel`, `MorphDialog`, `MorphDrawer` and `MorphPopupBackground` draw a shadow
  token as an offset rounded rectangle. They ignore its blur except in the corner radius. So the design's dialog
  shadow, `0 18px 48px` at 28 %, would draw as a hard slab, and the gallery keeps the default instead. Drawing it
  blurred needs `MultiEffect` (available on Qt 6.8) checked on the offscreen grabs. Nobody has reported it yet; it was
  seen while designing K8.
- **A multi-node switch case in a row** (K4): still stacked in a column with gap 0.
- **The terminal's overflow report** (batch J's open item), unchanged.

## Screen work these items need (for lane K-S)

- **Samples, 1000 px** (K2): the head's trail must be at most 597 px for one line with the principal (761 − 67 − 12 −
  85). It is 664 now, so about 67 px off the search box (or the selects).
- **Plate, 1000 px** (K2): the trail must be at most 418 px (761 − 245.5 − 12 − 85). It is 461 now, so about 43 px
  off the template select (`fixed(22)` units today). The lead then wraps under the title, as J5 intends.
- **E-signature, 390 px** (K4): no wrapper column around Signer and Meaning. If K-S has added one, it comes out once
  K-A's interpreter commit lands.
- **Everything else**: none. K1, K6, K7 and K8 change the gallery through the framework and its bundle.

Each width above is from P1 at `300cee8c1`. If K-S changes a head's trail for other reasons first, the bound is what
counts, not the reduction.

## Lanes

Three lanes, each starting from `300cee8c1`. **Framework commits never wait on K-S**: every item has its own tests
and controls. A lane's gallery commit (bundle or screen files) is its last, and is rebased onto K-S's commits as they
land. Only the samples and plate checks of K2's "Verify" need K-S's re-fit.

### K-A: layout and text (K4, K5, K7)

- `include/morph/ui/interpreter.hpp` (`LowerCtx::listGap`; the `column` kind, `lowerArranged`, `lowerAll`,
  `lowerSwitch`; resetting it in `lowerKind`'s lambdas);
- `src/qt_quick/qml/MorphText.qml`, `src/qt_quick/qml/MorphCheckbox.qml`;
- tests: `tests/test_ui_interpreter_detail.cpp`, `tests/qt_quick/test_qt_quick_widgets.cpp` (shared, below);
- spec: `docs/spec/ui/interpreter.md`, `docs/spec/ui/document.md`, `docs/spec/qt_quick/backend.md` (shared,
  below).
- **Gallery commit:** none, unless K-S has already wrapped e-signature's pair; then its removal.

### K-B: shell (K2, K3)

- `include/morph/ui/shell_frame.hpp` (`_trail`, `showTrail()`, `_topLine`'s children);
- tests: `tests/test_ui_shell_frame.cpp`, `tests/qt_quick/test_qt_quick_containers.cpp`,
  `tests/tui/test_tui_shell_frame.cpp`, `tests/client/test_shell.cpp`;
- spec: `docs/spec/ui/shell_frame.md`.
- **Gallery commit:** `design/visual-review.md`'s grab command (`QT_FORCE_STDERR_LOGGING=1`).

### K-C: theme and overlays (K1, K6, K8)

- `include/morph/ui/theme.hpp` (the two tokens; `kThemeTokens` 70 → 72; `fillValue`'s empty-default colour;
  the contrast rule);
- `src/qt_quick/theme.cpp` (`selectionText` and `hasSelectionText` per scheme; `elevation.dialog`);
- `src/qt_quick/qml/`: `Theme.qml`, `MorphScheme.qml`, `MorphButton.qml`, `MorphDialog.qml`, `MorphDrawer.qml`;
- tests: `tests/qt_quick/test_qt_quick_widgets.cpp` (shared, below), `tests/qt_quick/test_qt_quick_theme.cpp`,
  `tests/qt_quick/test_qt_quick_overlays.cpp`, `tests/test_ui_theme.cpp`;
- spec: `docs/spec/ui/theme.md`, `docs/spec/qt_quick/backend.md` (shared, below).
- **Gallery commit:** `examples/lims_gallery/src/lims_gallery.cpp`: the `sidebar` surface's `color.selectionText`,
  and the base's `elevation.card`.

### Shared files: owner and order

The lanes land in the order **K-A, then K-B, then K-C**: K-A and K-B are small and independent, and K-C touches the
theme's closed set, which every theme test reads. A later lane rebases onto the earlier ones before it merges.

| File | Lanes | Owner | What each adds |
|---|---|---|---|
| `tests/qt_quick/test_qt_quick_widgets.cpp` | K-A, K-C | K-C | K-A: the text-width and check-box cases, appended. K-C: J10's selection cases rewritten in place, and its new cases appended after K-A's |
| `docs/spec/qt_quick/backend.md` | K-A, K-C | K-C | K-A: the Text and Checkbox rows. K-C: the item button's selected look, Dialog and Drawer |

No other file is shared. `lims_gallery.cpp` is K-C's alone; `visual-review.md` is K-B's alone, for the grab command.
The coordinator writes the "After batch K" section once, from one set of re-grabs taken with the new command after all
three lanes and K-S have merged.

Each lane re-grabs the screens its items name, at both sizes, with `--user "Dr. A. Meyer"` and
`QT_FORCE_STDERR_LOGGING=1`, and reports what it measured, and every line of each `.err` file, to the coordinator.

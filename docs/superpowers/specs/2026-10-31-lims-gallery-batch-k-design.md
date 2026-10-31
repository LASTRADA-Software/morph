# LIMS gallery batch K: a selection that keeps its text, a principal on the actions' line, gaps through a switch, and the last insets of the controls and the phone's bar

Batch K closes the framework gaps the two comparisons after batch J found (ten screens at 1000×830 and 390×844,
against the mockups and the batch-I grabs). The per-screen fixes those reports mark "screen" are lane K-S's work and
are not designed here. This design takes the nine items that need ui/1 itself to change, or that had to be settled
before a screen can rely on them, and four more (K10 to K13) that K-S's screen work left: a warning on plate and three
phone insets no screen can reach.

The aim is still the review's: ui/1 produces each mockup's look directly, and a screen never works around the
framework. The rules of batches G to J hold:

- a look is a token or a rule, never a screen's choice of pixels;
- a key means the same thing on every kind that takes it;
- a look that changes every application's shell is the theme's to switch;
- the terminal keeps its cells. A purely visual change says what it leaves unchanged there;
- vocal over silent: where the framework cannot do what a document asks, it says so.

Every claim of K1 to K9 was checked against `300cee8c1` (batches A to J), with `build/linux-everything` rebuilt at
that revision; K10 to K13 against `lg-ks` (`300cee8c1` plus lane K-S's screen commits), rebuilt likewise.
Each item says whether its premise was **measured** (run or probed here) or **read** (inferred from the code). The
comparison's grabs are in `lg-cmp/app-j`; probe output is in the session scratchpad (`lg-kd-probe/`,
`lg-kd-ks/`). Neither is in the
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

- **P7. Plate and samples on `lg-ks`** (grabs at 1000×830 with `QT_FORCE_STDERR_LOGGING=1`, `lg-kd-ks/`). Samples:
  one line, the trail beside the title, an empty `.err`. Plate: the trail on the title's line, the subtitle under the
  title on one line running under the template select, and one line on stderr:

  ```
  morph.layout: a column inside `shell_topbar_main` needs 302 px and has 249 px: `shell_title_line` and its siblings neither wrap nor shrink
  ```

- **P8. The phone grabs on `lg-ks`** (390×844, same command; every `.err` empty):
  - instrument: "Current booking" centred at y ≈ 429, its first content line about 43 px lower;
  - console: the dark head spans y 0–59 (60 px); the menu glyph x 21–30, the title from x 57;
  - audit and samples: the primary button spans x 283/302 to 381 and y 8 to 51, so it is 8 px from the top and the
    right edge; the menu glyph x 20–30.

## The items

| Item | Verdict | Where it lands | Lane |
|---|---|---|---|
| K1 a selected item's content in the accent | **Measured** (P5), **read** (`MorphButton.contentScheme`) | a colour role `color.selectionText`, following the surface's text; neither look recolours | K-C |
| K2 the head wraps | **Measured** (P1). The principal's 85 px caused both wraps; J5's minimum caused neither on its own | the principal joins the trail; samples and plate narrowed their trails (K-S, done) | K-B |
| K3 the principal off the actions' line | **Read**, consistent with P1: the principal is centred on the whole top line | the same change: the principal sits in the trail, centred on the actions | K-B |
| K4 a switch case ignores its column's gap | **Read** (`lowerSwitch`, `holding`) | a case's and a default's holder take the gap of the column they sit in | K-A |
| K5 "System suitability" wraps | **Measured** (P2, P3). A library defect: a text's width does not follow its font | the width binding depends on the font | K-A |
| K6 the dialog's two rules | **Read** (`MorphDialog`, `MorphDrawer`) | both rules draw the card's `divider` | K-C |
| K7 check boxes inset from their label | **Measured** (P4) | the box sets its own sides and spacing | K-A |
| K8 cards draw a shadow | **Read**: `elevation.card` already exists, and dialogs read it too | the gallery sets `elevation.card` flat; dialogs and drawers read `elevation.dialog` | K-C |
| K9 the date input's calendar icon | **Read**: the button is the calendar's only way in | keep it | none |
| K10 plate's inline lead runs past its block | **Measured** (P7), **read** (`buildHeadLead`, `MorphStack`) | a wrapping text counts as narrow as its widest word; the inline lead fills its slot | K-A, K-B |
| K11 instrument 390: header to content 43 px | **Measured** (P8), **read** (`MorphPanel`) | an open foldable panel's content starts at its header's foot | K-A |
| K12 console 390: the head is 60 px | **Measured** (P8), **read** (`ShellFrame::setCompact`) | the compact bar is padded by step 1 above and below | K-B |
| K13 audit and samples 390: the button 8 px from the edge | **Measured** (P8); its source half traced | a framed action at the compact trail's end stands step 4 from the edge (Q6) | K-B |

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
  under either look. Where neither the surface nor the theme sets it, it is that surface's `color.text`, so by default
  a selected item's text is its text colour (Q1).
  - Muted, tones and every other role of the content stay the item's.
  - **Its scope is the item-style button**, the kind `list.selection` marks: the navigator's entries, menus drawn as
    lists, a document's list items. A table's selected row is not in it. Its cells keep `color.text` on
    `color.selection`, which the existing contrast rule already holds at 4.5:1, and no mockup draws a selected row's
    text in another colour (05's and 10's `.sel td` set a background and a bar only). `theme.md` says so. Making
    rows follow would need a scheme handed to the cells, as J10 hands one to an item's content; see "Not in this
    batch".
  - The terminal marks a selected item by inverting it and reads no colour token, so nothing changes there.
- **How "follows the text" is drawn.**
  - **The scheme.** `MorphScheme` gains `property color selectionText`, `property bool hasSelectionText` and
    `readonly property color selectedText: hasSelectionText ? selectionText : text`.
  - **The theme's own value.** `Theme.qml` gains `selectionText` and `hasSelectionText`, which `setPlainTokens` writes:
    `hasSelectionText` is true only where the resolved base holds the token, and is written `false` when it does not,
    which is what lets a bundle without the token restore the default.
  - **The built-in schemes that follow the theme** (`themeScheme`, `pageScheme`, `cardScheme`, `toolbarScheme`,
    `barScheme`) bind both new roles to those properties, as they bind every role. So a token set on the base reaches
    them, as `list.selection` on the base does.
  - **The built-in sidebar** keeps `hasSelectionText` false. Its text and selection colours are its own palette, so
    a base-level selection text meant for light surfaces does not reach it; a theme that declares a `sidebar`
    surface sets it there. `theme.md`'s sidebar row says so. `toolbarJoinedScheme` binds both to `barScheme`'s.
  - **Declared surfaces.** `setSurfaces` writes both for each surface the index declares, from that surface's
    resolved values (the base's layer included), `false` where they hold no token.
  - **The token's default.** `kThemeTokens` gives `color.selectionText` an empty default, and `fillValue` leaves an
    empty-default colour out of `ThemeValues::colors` unless the theme sets it. That is the first colour that follows
    another; the token table's comment says so.
- **`MorphButton`.** `accentSelection` becomes `itemSelected` (`style === 1 && selected`), and the label, the icon
  and `contentScheme.text` read `root.scheme.selectedText` while it is true; `MorphButtonBackground` reads
  `listSelectionFill` for the bar as now. `contentScheme` copies the three new members. `ButtonImpl::scheme()` keeps
  returning `contentScheme`, so J10's mechanism is unchanged; only the colour it carries changes.
- **Contrast.** `themeContrastProblems` gains one rule: where the theme sets `color.selectionText`, it holds 4.5:1
  on `color.selection`.
- **The gallery.** Its `sidebar` surface sets `color.selectionText: #FFFFFF`. The base leaves it unset, so review's
  queue and console's compact list draw their text as the mockups do.

**Effect on the other example apps.** Every shell app's navigator, and any list item, drew its selected label in the
accent under the default `accent` look. It now draws it in the surface's text colour, with the tint and the bar as
before. An application that wants the old look sets `color.selectionText` to its accent.

**Spec.** `theme.md` (the token in the closed set, with its scope; `list.selection`'s row, which no longer says "the
label in the accent"; Surfaces, the sidebar row); `qt_quick/backend.md` (the item button's selected look).

**Tests.**

- `tests/qt_quick/test_qt_quick_widgets.cpp`:
  - a selected item with content under `accent` on the theme's scheme: the bar shows, and the content's text, the
    label's colour and the icon's colour are the scheme's `text`;
  - on a surface that sets `color.selectionText`, under `fill`: no bar, and the label and content's text are that
    colour; deselecting returns them to `text`, with no re-adoption;
  - **Control:** J10's `contentScheme` (`text` is the accent while selected under `accent`). The first case fails.
- `tests/qt_quick/test_qt_quick_theme.cpp`:
  - a theme whose `sidebar` surface sets the token gives `sidebarScheme.hasSelectionText` true with that colour;
  - a theme that sets it on the base only gives `cardScheme.selectedText` that colour, and leaves the built-in
    `sidebarScheme.hasSelectionText` false;
  - a theme that sets it nowhere: `themeScheme.selectedText` equals `themeScheme.text`;
  - a re-apply of an index that **still declares the same `sidebar` surface**, now without the token, sets its
    `hasSelectionText` false again (`setSurfaces` writes only declared surfaces, so a re-apply that dropped the
    surface would check nothing). Likewise a re-apply whose base drops the token sets `Theme.hasSelectionText` false.
- `tests/test_ui_theme.cpp`:
  - `themeValues()` of an empty set has no `color.selectionText`; a set that has it carries it;
  - the check that the value maps hold one entry per token (`test_ui_theme.cpp`, around line 204) now expects
    `kThemeTokens.size() - 2`, since this token and K8's `elevation.dialog` are left out while unset;
  - `color.selectionText: #EEEEEE` on `color.selection: #FFFFFF` is a contrast problem.

**Verify.** Re-grab review and console at both sizes. The selected entry's WS id draws (28,30,33). Re-grab any
screen at 1000 px: the selected sidebar label draws (255,255,255), the others (201,204,214).

## K2 and K3: the principal shares the head's line as part of the trail

**Problem (measured, P1).**

- **What wraps.** `_topLine` is `[pad][_topMain][_topPrincipal][pad]`, so the principal and its gap take 85 px from
  `_topMain` on every screen. Without them the row would have 761 px. At `300cee8c1`:
  - samples needs 67 + 12 + 664 = 743 px, and fits in 761;
  - plate needs 245.5 + 12 + 461 = 718.5 px, and fits in 761.

  At 676 px both wrapped. The principal alone caused both wraps. Lane K-S has since narrowed both trails (Q2), and
  both heads now hold their trail on the title's line (P7).
- **J5's minimum is not the cause on samples.** A stretch child's basis in a wrapping row is its minimum, capped at its
  natural width (`MorphStack.naturalBasisOf`), so "Samples" reserves its own 67 px, not 32 units. On plate the block
  reserves 245.5 px against a 109.5 px title, for the inline lead to wrap under the title. Plate's remaining
  warning is that lead, which is K10's.
- **Where the principal sits (read).** `_topLine` aligns its items to the centre, while `_topMain` aligns a
  multi-line head's items to their tops. So:
  - in a head with an eyebrow or a lead below, the actions sit at the line's top and the principal at the block's
    middle (instrument: y ≈ 23 against ≈ 38);
  - in a head whose trail wrapped, the principal floats between the two lines (samples at `300cee8c1`: y ≈ 42).
- **Its visibility goes stale (read).** `_topPrincipal`'s visibility is computed only in `showChrome()`, and
  `setPrincipal` does not call it. The client signs in through `showSignedIn(true)`, which runs `showChrome()` with
  the old name, and then `setPrincipal(name)` (`shell.cpp`, the sign-in path). So after a sign-out, a sign-in leaves
  the head's principal hidden.

**Decision.** The principal is the trail's last item.

- **The trail.** `_topMain` gets a new `_trail` in the place `_headSlot` had: a content-sized **wrapping** row (made
  by `wrappingRow`, gap step 3, `align: center`, test id `shell_head_trail`). It holds `_headSlot`, then
  `_topPrincipal`.
  - It wraps because a wrapping row gives no gap to a child with no basis (`MorphStack.place`; the terminal's
    `planWrap` alike), while a non-wrapping one gives a gap to every shown child. `setHeadShown(true)` shows
    `_headSlot` whenever the head has an eyebrow or a lead, even with no trail content, and an empty shown slot in a
    non-wrapping row would put 12 px before the principal (reproduced by the review: 78 px against 66). In a
    wrapping row the empty slot takes no room and no gap.
  - As a content-sized child of `_topMain` it is laid out at its natural width, so it wraps inside itself only when
    it is alone on a line narrower than itself. The principal then goes under the actions rather than past the edge.
  - `_topPrincipal` leaves `_topLine`, which becomes `[pad][_topMain][pad]`.
- **One place decides what shows.** `showTrail()` computes `_topPrincipal`'s visibility
  (`_principalInHead && !_compact && !_principalName.empty()`) and `_trail`'s (either child shown).
  - `setHeadShown`, `setPrincipalInHead`, `setPrincipal`, `setCompact` and `showChrome()` all end in it, so the
    stale visibility above is gone.
  - `showChrome()` no longer sets `_topPrincipal` itself.
- **What that gives.**
  - **K3:** the principal is centred on the actions, on whichever line the trail is.
    - In a multi-line head, `_topMain` aligns tops, so the trail and the principal sit on the first line, level with
      the actions.
    - In a one-line head (`setHeadOneLine(true)`), the trail is centred with the title, as now.
    - A head with no actions holds the principal alone at the trail's place.
  - **K2:** the trail and the principal wrap as one block, so the head never splits them across lines.
    - The 85 px the principal took from the row now count inside the trail, so whether a head wraps is unchanged on
      every screen. The one difference is the empty-slot gap that is gone. At `300cee8c1` five screens with a trail
      fit (P1: instrument 572.5, console 638, audit 706, receipt 499, results 426, all ≤ 761).
    - Samples and plate fit once their trails are at most 597 and 418.5 px; K-S has narrowed them (P7).
- **J5 is unchanged**: the block, its minimum, the title line and `setLeadInline` stay as batch J made them.
- **Not chosen.**
  - **A principal that yields** (Q2: the user keeps the principal). No wrapping-row rule can say "drop this child
    first", and a principal that disappeared on two screens would show the signed-in name nowhere on them.
  - **The lead block's minimum taken from the title.** The block's real minimum is its widest unwrappable part. On
    plate that was the 302 px lead, until K10 lets the lead wrap.
  - **A per-screen principal**, shown only where a mockup draws it (console). It is shell chrome, and moving it
    between screens reads as a bug.
  - **The client telling the frame whether the trail has content**, the review's other route to the empty-slot gap.
    It needs a new frame call and a workspace signal, while a wrapping row already gives that rule on both backends.

**Per backend.** Both frontends lay the frame out from these stacks. On the terminal the principal is drawn on the
trail's row after the last action, with no gap from an empty slot, and moves with the trail when it wraps.

**Spec.** `shell_frame.md`: the Shape table (`shell_topbar_line`, `shell_topbar_main`, the new `shell_head_trail`
row, `shell_head_principal` now inside it), `setPrincipalInHead`, `setPrincipal`, and the "head wider than its share"
row, which no longer says the principal sits outside the line.

**Tests.**

- `tests/test_ui_shell_frame.cpp`:
  - `shell_head_trail` is recorded `wrap`, and holds `shell_head` then `shell_head_principal`; `shell_topbar_line`
    holds no principal;
  - the trail is hidden while the head is hidden and the principal is not in the head, and shown while either shows;
  - `setPrincipalInHead(true)`, then `showSignedIn`-style `showChrome` with an empty name, then `setPrincipal("x")`:
    `shell_head_principal` is visible;
  - **Control:** the old tree, and the old `setPrincipal`. The first and third checks fail.
- `tests/qt_quick/test_qt_quick_containers.cpp`: an expanded `ShellFrame` with an eyebrow, a title, a lead below, a
  trail of one button and a principal in the head.
  - The principal's vertical centre is the button's (±1 px).
  - The frame sized so that the trail and the principal fit beside the lead block's minimum keeps them on the title's
    line. One unit narrower, both are on the second line.
  - With the head shown but its slot empty, the principal's left edge is the trail's (no gap).
  - **Control:** the old tree. The principal's centre is the top line's, more than 10 px below the button's. With a
    non-wrapping `_trail`, the empty-slot case is 12 px off.
- `tests/tui/test_tui_shell_frame.cpp`: at 80 columns, the principal is drawn on the trail's row, after the action;
  with an empty slot, it starts at the trail's first cell.
- `tests/client/test_shell.cpp`: the principal-in-head case finds `shell_head_principal` under `shell_head_trail`,
  and after a sign-out and a sign-in it is visible again.

**Verify.** Re-grab instrument at 1000 px: "Dr. A. Meyer" is centred on Log maintenance and Book (y ≈ 23). Re-grab
samples and plate at 1000 px: one line for the trail, each beside its title.

## K4: a switch case's nodes take the gap of the list they sit in

**Problem (read).**

- E-signature at 390 px writes `layout::byWidth(width, {signerField(), meaningField(sign)}, …)`, a `switch` whose
  compact case holds two nodes.
- The 390 grab shows the signature dialog drawn as a sheet, so that switch is a child of the **dialog's body**.
  `lowerDialog` lowers the body with a fresh `innerCtx` and holds it in `holding(…, bodyGap)`, `kDialogGap` = 3.
  (The `called` case's `ui::column(…, {.gap = 2})` is the other place `signatureFields` is used.)
- `ScreenCore::lowerSwitch` lowers each case through `holding(children, inner)`, whose gap defaults to 0, and the
  `default` through `lowerAll(fallback, ctx)`, gap 0 too. So the two fields are a step-0 column inside a step-3
  body: ~4 px apart, against 12 px between the body's other fields.
- `document.md` says a node's sizing "means the same inside a `switch` case … as at the root". The spacing between
  a case's nodes does not.
- The reviewer's fix, wrapping the pair in `ui::column(…, {.gap = 2})`, is a screen working around the framework.
  Every multi-node case in a gapped list would need it again. K-S has removed it.

**Decision.** In a vertical list, a switch case means what its nodes would mean written there.

- `LowerCtx` gains `int listGap = 0`: the gap of the vertical list whose node is being lowered.
- **Leak-proof by construction.** `ScreenCore::lower()` zeroes `listGap` in the context it passes on for every kind
  except `switch`. So no kind's children ever see their grandparent's gap: a grid's cells, a row's children,
  `lowerArranged`'s horizontal branch and every lambda of `lowerKind` included. Only the holders of a vertical list
  set it, for the nodes they hold:
  - the `column` kind, for its children (its `gap`; the `children` lambda takes the gap for this);
  - `lowerArranged` along `Axis::Vertical` (its `gap`);
  - `lowerAll` (its `gap`, the holder being a column): a panel's, a collapsible's, a scroll's and a tab's content;
  - the dialog's and the drawer's body (`innerCtx.listGap = bodyGap`);
  - a switch case's holder and its `default`, for their own nodes (the gap they took), so a switch inside a case
    keeps it.
- `lowerSwitch` passes `ctx.listGap` to `holding(…)` for every case, and to `lowerAll(fallback, ctx, ctx.listGap)`
  for the `default`.
- **A case in a row or a grid cell is unchanged**: its holder is still a column with gap 0, since `lower()` zeroed the
  gap. A row's case content would need the holder to become a row and take the row's `align`, and nothing in the
  examples writes a multi-node case in a row. The spec says so.
- **The terminal** gets the same view tree, so its gap is the same step in cells.

**Spec.** `interpreter.md`, the Holders paragraph: a switch case's and a default's holder column takes the gap of the
vertical list the switch sits in (a column, a panel's or a tab's content, an overlay's body). `document.md`, the
`switch` row: "several nodes in a case are spaced as the list around the switch spaces its nodes".

**Tests.** `tests/test_ui_interpreter_detail.cpp`, on `RecordingBackend`:

- a column with `gap: 2` holding a switch whose matching case has two texts: the case's holder column records
  `gap=2`;
- the same for a `default`;
- a switch inside a panel (whose `lowerAll` gap is the panel's): the holder records the panel's gap;
- **a switch in a dialog's body** (default gap 3) and in a drawer's: the holder records `gap=3`;
- a switch inside a case of another switch, in a gapped column: the inner holder records the gap;
- a switch in a row, and one in a grid cell inside a gapped column: the holder records `gap=0`;
- a switch inside a column inside a case: it takes that column's gap, not the case's;
- **Control:** the current `lowerSwitch`. The first four record `gap=0`.

**Verify.** Re-grab e-signature at 390 px. Signer and Meaning are as far apart as the sheet's other fields, the
body's step 3 (12 px between the fields' boxes).

## K5: a text's width and its elision follow its font

**Problem (measured, P2).** The results label "System suitability" is sized at 100.97 px, its regular-weight advance
plus the 1 px slack. Its label then draws in the strong face and needs 101.89 px, so it wraps.

- **Cause (read, consistent with P2; reproduced by the review on Qt 6.12).** `MorphText.implicitWidth` is computed by
  calling `metrics.advanceWidth(…)`. A method call registers no dependency on `metrics.font`, so the binding
  re-evaluates when the text changes but not when the font does.
- **The elision has the same fault (reproduced by the review).** `fitted` is a separate binding,
  `metrics.elidedText(…)`, and does not follow the font either. In the review's probe it stayed "System suitability"
  after the font change, against "System suitabi…" for the same call gated on the font. So a mono or `singleLine`
  text whose role is set after its text, in a box of fixed width, draws past its width.
- **Which texts are affected.** Any text whose role is set after its text: the mount binds a node's text before its
  role (`mount.hpp`, the text's `setText` then `setRole`). That covers every section, heading, mono, small, display and
  overline text a document writes with a fixed string, and every text whose theme font changes after mount.
- **Why the shell title is not affected.** It is set after its role, so its advance matches (P1: 328.48 for both).
- **Why batch I had no such wrap.** Its binding read `label.implicitWidth`, which follows the font. The P2 numbers
  also explain the comparison's observation: the slot is 0.92 px narrower than batch I's, so the Pass chips moved by
  less than a pixel.

**Decision.** Every measuring binding depends on the font.

- `implicitWidth` and `fitted` both read a member of `metrics` that always holds before they measure:
  `metrics.height >= 0 ? <measure> : <same measure>`.
  - **Not `pixelSize`**, as `Theme.monoAdvance` does. An app without `font.family` draws `strongFont` as
    `boldOf(platformMetrics.font)`, which keeps a point size, so its `pixelSize` is −1. A guard on `pixelSize > 0`
    would measure every Section, Overline and caps text as 0. `height` changes with any font change and is never
    negative.
- `wrapSlack` stays 1, which P3 and the review's probe (label 111.375 against advance 110.375) show is enough once the
  advance is read in the label's own font.
- The bindings still never read `label.implicitWidth` for plain text, so the Qt 6.8 binding loop through a required
  text's mark, which reading the label's width closes, stays closed. `label.font` depends on the role and the theme
  only, never on a width, so the new read adds no loop.
- A required text's mark room follows `implicitWidth`, and `elided` and the tooltip follow `fitted`.

**Per backend.** Qt Quick only. The terminal sizes a text in cells and has no font.

**Spec.** `qt_quick/backend.md`, Text: the width is the widest line's advance in the text's current font, plus 1 px;
a one-line text elides by its current font.

**Tests.** `tests/qt_quick/test_qt_quick_widgets.cpp`:

- a text given "System suitability" and then the `Section` role, laid out at its implicit width, has one line, and
  its implicit width is at least its label's;
- the same with the `Heading` role, and with the theme's `font.size.body` changed after mount;
- a `singleLine` text in a parent of fixed width, given its text and then the `Section` role, elides: `elided` is
  true and `fitted` ends in "…";
- the theme with no `font.family` (the platform's strong font): a Section text's implicit width is greater than 0;
- **Control:** the current bindings. The section case has two lines, the `singleLine` case is not elided. A guard on
  `pixelSize > 0` makes the no-family case 0.

**Verify.** Re-grab results at 1000 px: "System suitability" is one line, and the first Pass chip moves right by
about 1 px (the label's 101.89 against 100.97).

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
- **Not chosen:** a dedicated `color.overlay.rule`. It would be a token for one 1 px line's 7-level difference. The
  user decided for the divider (Q4).

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

**Decision (Q5).**

- **A new shadow token, `elevation.dialog`**: the shadow under a dialog and a drawer (not a sheet, which has none).
- **While unset, it follows `elevation.card`**, the way `color.selectionText` follows `color.text`. So no
  application changes, including a theme that set `elevation.card` to restyle its dialogs as well as its cards.
  - `kThemeTokens` gives it an empty default (no shadow spec), and `fillValue` leaves an empty-default shadow out of
    `ThemeValues::shadows` unless the theme sets it.
  - `Theme.qml` gains `dialogShadow*` and `hasDialogShadow`, written by
    `setShadow(theme, values, "elevation.dialog", "dialog", unset)`. `hasDialogShadow` is written `false` when the
    token is absent.
  - `dialogShadowShown*` read `hasDialogShadow ? dialogShadow* : cardShadow*`. `MorphDialog` and `MorphDrawer` read
    those in place of `cardShadow*`.
  - Their shadow rectangles get an `objectName`, `dialogShadow`, for the tests to find.
- **The gallery's base** sets `elevation.card` to `{ "color": "#00000000", "offsetX": 0, "offsetY": 0, "blur": 0,
  "spread": 0 }`. It sets `elevation.dialog` to the framework's card default, `0 2 10 #00000014`, so its cards are
  flat with their border and its dialogs keep today's shadow.
- **Not chosen:** dialogs following `elevation.popup`. It exists, but it would change every application's dialog
  shadow (from `0 2 10` at 8 % to `0 8 24` at 14 %), and a menu's elevation is not a dialog's.

**Effect on the other example apps.** None: an unset `elevation.dialog` draws exactly what `elevation.card` draws.

**Per backend.** The terminal draws no shadow; unchanged.

**Spec.** `theme.md`: `elevation.dialog` in the closed set, following `elevation.card` while unset, and
`elevation.card`'s row naming what it is under. `qt_quick/backend.md`: Dialog, Drawer.

**Tests.**

- `tests/qt_quick/test_qt_quick_theme.cpp`:
  - a theme setting `elevation.dialog` writes `dialogShadowOffsetY` and `hasDialogShadow` true, and leaves
    `cardShadowOffsetY` at its default;
  - a theme setting only `elevation.card` gives `dialogShadowShownOffsetY` the card's value;
  - a re-apply whose base drops `elevation.dialog` writes `hasDialogShadow` false.
- `tests/qt_quick/test_qt_quick_overlays.cpp`:
  - under a theme whose `elevation.card` is transparent and whose `elevation.dialog` is not, the dialog's
    `dialogShadow` has the dialog colour; the same for a drawer;
  - **Control:** the current `MorphDialog`. The shadow is the card's transparent colour.
- `tests/test_ui_theme.cpp`: `kThemeTokens` holds `elevation.dialog`, and `themeValues()` of an empty set has no
  shadow at that path (the per-token count is K1's `size() - 2`).

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
- The difference is an accepted residual, recorded in `visual-review.md` by the coordinator. The user decided to keep
  the button (Q3).

## K10: an inline lead that is wider than its block wraps its own text

**Problem (measured, P7).**

- Plate at 1000 px, on `lg-ks`, reports once:

  ```
  morph.layout: a column inside `shell_topbar_main` needs 302 px and has 249 px: `shell_title_line` and its siblings neither wrap nor shrink
  ```

- In the grab, "ELISA, host cell protein · worksheet WS-2026-0415" sits under the title on one line, 302 px long. It
  runs under the template select, which draws over its end.
- **K2 does not close it.** The trail is now 415 px (249 = 676 − 12 − 415). With the principal in the trail, the
  lead block gets 761 − 12 − (415 + 12 + 73) = 249 px, the same. The principal's 85 px only move from the line into
  the trail.
- **Cause (read).**
  - The inline copy of the head's lead (`Workspace::buildHeadLead`, `head_lead_inline`) is a content-sized wrapping
    row inside `_headInline`, a content-sized non-wrapping stack. In the title line, `MorphStack.basisOf` caps
    `_headInline` at the line's 249 px. But its child keeps its natural 302 px, because a wrapping row's minimum (J3)
    is its widest child's basis, and a text's basis is its whole line. So the child is laid out past its box, though
    the subtitle is rich text that could wrap.
  - The report is the same rule seen from above: the lead block's content minimum is the title line's widest basis,
    302. The report says "neither wrap nor shrink" of a text that can wrap.
- J5 promised that the lead "wraps to a line of its own" under the title. It does, but it does not wrap its own words
  when that line is still too narrow.

**Decision.** A text that wraps counts as narrow as its widest word, and the inline lead fills its slot.

- **A wrapping text's minimum.**
  - `MorphText` gains `readonly property real wrapMinimumWidth`. For a text that wraps (not `oneLine`), it is the
    widest word's advance in the label's font, plus `wrapSlack`, and for runs, each run's words in that run's face
    (mono or body). For a one-line text it is its implicit width. It reads `metrics.height` as K5's bindings do, so
    it follows the font.
  - It is not the text's `Layout.minimumWidth`, so no non-wrapping layout changes.
- **A wrapping row's content minimum** (`MorphStack`, the J3 measure behind both the propagated minimum and the
  report):
  - each child counts as the lesser of its basis and its own minimum, where it declares one: a text's
    `wrapMinimumWidth`, a stack's content minimum;
  - a rigid child (a button, a fixed-width input, a non-wrapping row of rigid children) declares a minimum equal to
    its natural width, so nothing that cannot shrink is hidden from the report.
- **The inline copy fills its slot.** `buildHeadLead` gives the `Inline` copy `width: stretch` (no `minWidth`).
  - `_headInline` stays content-sized, so the frame's slot, not the lead, still holds the spare width, as the
    builder's comment says.
  - When the title line gives `_headInline` less than its natural width, the lead's row takes that width and its
    subtitle wraps inside it.
- **What plate does then.** Its lead block (249 px) holds the title, and under it the subtitle on two lines. The
  trail stays on the title's line, and there is no report, since the lead block's content minimum is now the widest
  of the title and "WS-2026-0415", which is under 249.
- **Effect elsewhere.** A wrapping row's minimum can only go down, so the propagated minimum shrinks and the
  overflow report fires less. It fires no less where a child truly cannot shrink. The lane re-runs batch J's P8
  measure: the six example render smokes at 1024×768 and 390×844, with `QT_FORCE_STDERR_LOGGING=1`. It reports
  every `morph.layout` line that appears or disappears.

**Per backend.** The terminal has no report. Its wrapping row lays the inline copy out in the cells the title line
gives it, and with `stretch` the subtitle wraps at a cell inside them.

**Spec.** `qt_quick/backend.md`: Text, the wrap minimum; Layout, a wrapping row's content minimum. `workspace.md`:
the inline lead fills its slot. `shell_frame.md`: the inline lead wraps under the title, and inside its line when
that is narrower still.

**Tests.**

- `tests/qt_quick/test_qt_quick_containers.cpp`: a wrapping row 150 px wide holding a 300 px plain text and a
  60 px button:
  - its content minimum is the text's widest word, not 300;
  - the text, alone on its line, is 150 px wide with two or more lines;
  - no `morph.layout` warning is written (a message handler counts them);
  - **Control:** the current measure. The minimum is 300 and one warning is written. A row of rigid children only
    still reports.
- The same file: an expanded `ShellFrame` whose inline lead is wider than the lead block beside its trail. The lead's
  right edge is at most the block's, and the trail's top equals the title's.
  - **Control:** a content-sized inline copy. The lead runs past the block.
- `tests/qt_quick/test_qt_quick_widgets.cpp`: `wrapMinimumWidth` of "ELISA, host cell protein" is the advance of
  "protein" plus 1. Set the `Section` role after the text, and it changes.
- `tests/test_ui_workspace.cpp`: the `Inline` lead copy records `layout=stretch(1)`; the `Below` copy is unchanged.

**Verify.** Re-grab plate at 1000 px: the trail on the title's line, the subtitle on two lines under the title,
nothing under the template select, and an empty `.err`.

## K11: a foldable panel's content starts at its header's foot

**Problem (measured, P8; read).**

- Instrument at 390 px: "Current booking" (the header's text, centred at y ≈ 429) to the first content line is about
  43 px. The mockup's `details` has `padding: 0 12px 8px`, a `summary` with `min-height: 44px` and its title centred,
  and the content right after it (`margin: 0`): about 30 px.
- `MorphPanel` with a header and `collapsible`:
  - the header is flush with the frame's top, at least a touch target tall (44 on compact, `MorphSectionHeader`);
  - then the inner column's spacing, 5 px;
  - then the content column's `Layout.topMargin`, the panel's padding (step 2, 8 px).
- So the content starts 13 px below the header's foot. The header row already holds its own space around its title:
  that is why it is a touch target. The spacing and the top padding add a second gap.

**Decision.** In an open foldable panel with a header, the content starts at the header's foot.

- The content column's top margin is 0 there, and the spacing between the header and the content is 0.
- The padding stays at the sides and the bottom.
- A panel that does not fold keeps its padding above the content and the 5 px spacing under its title, since its
  title row is only a line of text tall.

**Effect on the other example apps.** Every open collapsible panel's content moves up 13 px (with padding 2; in
general the padding plus 5 px). A folded panel is unchanged.

**Per backend.** The terminal draws a panel's title on its border row and its content on the next. Unchanged.

**Spec.** `qt_quick/backend.md`, Panel: a foldable panel's content follows its header.

**Tests.** `tests/qt_quick/test_qt_quick_containers.cpp`: an open collapsible panel, padding 2, with a title and a
text. The text's top in the panel's coordinates is the header's bottom (±0.5). A non-collapsible panel keeps its
padding plus spacing. **Control:** the current `MorphPanel`. The text is 13 px lower.

**Verify.** Re-grab instrument at 390 px. "Current booking" to its first line is about 30 px.

## K12: the compact top bar is one touch target tall plus step 1

**Problem (measured, P8; read).**

- Console at 390 px: the dark head is 60 px (y 0–59). Mockup 10's compact header is `height: 52px; padding: 0 4px`,
  with 44 px buttons: 4 px above and below.
- `ShellFrame`'s top bar pads its line with zero-height spacers a bar gap apart. `_topBar->setGap(2)` is 8 px, and
  `setCompact` tightens only the line's ends (`_topLine` gap 1) and its items (`_topMain` gap 2). So the bar is
  44 + 2 × 8 = 60 px.
- Mockup 10 is the only mockup that draws the shell's compact bar. Its sides are already step 1 (the menu glyph
  measures x 20–30, as the mockup's 4 + (44 − 18) / 2 ≈ 17 px glyph would sit from a 4 px end).

**Decision.** `setCompact(true)` sets `_topBar`'s gap to step 1, and `setCompact(false)` back to step 2. The compact
bar is the touch target plus 4 px above and below: 52 px.

**Effect on the other example apps.** Every compact shell head is 8 px shorter. The expanded head is unchanged.

**Per backend.** The terminal's bar pads by cells. Read, not run: its spacers between the bar's rows map a step to
cells, so the lane checks in `tests/tui/test_tui_shell_frame.cpp` whether the compact bar's row count changes, and
says so in the spec either way.

**Spec.** `shell_frame.md`, The compact form: the bar's padding above and below the line is step 1.

**Tests.** `tests/test_ui_shell_frame.cpp`: `shell_topbar` records `gap=1` compact and `gap=2` expanded.
`tests/qt_quick/test_qt_quick_containers.cpp`: a compact frame with a menu and one ghost icon button; the bar's height
is `Theme.touchHeight + 2 * Theme.spaceOf(1)`. **Control:** the current `setCompact`. The bar is 16 px taller than
the target.

**Verify.** Re-grab console at 390 px: the dark head is 52 px.

## K13: a framed action at the compact bar's end stands step 4 from the edge

**Problem (measured, P8).**

- Audit and samples at 390 px: Export PDF and Register (filled, primary) span y 8–51, and end at x 381, 8 px from the
  right edge. Samples' Register is no longer flush, as K-S reported; it measures as audit does.
- Mockups 04 and 05 draw the phone head as `padding: 12px 16px`, with the filled button 16 px from the edge.
- **Where the 8 px come from.**
  - Read: the line's end pad is step 1 (4 px), the left side (the menu at 4 px) agrees.
  - Inferred, not traced: the other 4 px on the right are a gap before an empty shown item in the trail, the kind
    K2's review reproduced.
- **K12 moves the bar to step 1 above and below**, as mockup 10 draws it. That makes the top inset 4 px against 04's
  and 05's 12. The two kinds of mockup disagree:
  - 10 draws the shell's bar, with ghost icon buttons whose glyphs land about 17 px from the edge;
  - 04 and 05 draw a page head with no shell, with a framed button whose frame is 16 px from the edge.

  In both the visible mark is about 16 px in.

**Decision (proposed; Q6).** The compact bar keeps step 1 at its ends, and a **framed** action at the trail's end
stands step 4 from the edge.

- **The rule.** A ghost or icon-only button's frame is invisible, so its glyph already sits inside its 44 px box. A
  framed button (normal, primary, danger, outlined) shows its frame, so the bar adds step 3 after it, making step 4.
- **Who knows.** The workspace knows the head's actions, their emphasis and whether each is icon-only. It
  computes `headTrailEndsFramed(tab)`, a `Prop<bool>`: whether the last *visible* head action draws a frame. The
  client passes it to `ShellFrame::setTrailEndInset(bool)`, which shows a fixed spacer of step 3 at the end of
  `_trail`.
  - This is compact only. Expanded, the line's step-3 ends and the gaps already match the mockups.
- **The phantom 4 px.** The lane traces it with batch K's P1 probe before K13. If it is an empty shown item in the
  trail, K2's wrapping `_trail` removes it, or the item is hidden while empty.
- **Vertically** the framed button stays 4 px from the bar's top (K12), against 04's and 05's 12. One bar cannot be
  52 px for 10 and 68 px for 04, and 10 is the shell's own drawing. This is the residual Q6 asks about.

**Per backend.** The terminal draws a button's frame as brackets on its row. What the step-3 spacer becomes in cells is not
verified here; the lane checks it in `tests/tui/test_tui_shell_frame.cpp` and that the 80-column trail still fits.

**Spec.** `shell_frame.md`, The compact form: a framed action at the trail's end stands step 4 from the edge;
`workspace.md`, the head's trail.

**Tests.**

- `tests/test_ui_workspace.cpp`: `headTrailEndsFramed` is true for a trail ending in a primary action and false for
  one ending in a ghost icon-only action. It follows the last action's visibility.
- `tests/qt_quick/test_qt_quick_containers.cpp`: a compact frame whose trail ends in a primary button. The button's
  right edge is `Theme.spaceOf(4)` from the bar's. With a ghost icon button, its box is `Theme.spaceOf(1)` from it.
  - **Control:** no end inset. The primary button is `Theme.spaceOf(1)` (plus any phantom gap) from the edge.

**Verify.** Re-grab audit and samples at 390 px: the filled button ends 16 px from the right edge. Console at 390 px:
the scan button's glyph is unchanged.

## Theme tokens

| Token | Kind | Values or default | Item |
|---|---|---|---|
| `color.selectionText` | colour, read per surface; item-style buttons only | none: where neither the surface nor the theme sets it, the surface's `color.text`. The built-in sidebar takes it only from a declared `sidebar` surface | K1 |
| `elevation.dialog` | shadow | none: while unset, `elevation.card`'s value | K8 |

- `kThemeTokens` grows from 70 to 72; both new tokens have empty defaults and are absent from `ThemeValues` until set.
- A bad value of either is refused at `addFile` as any colour or shadow is.

The gallery's bundle (`lims_gallery.cpp`) takes:

- its `sidebar` surface: `color.selectionText: #FFFFFF`;
- its base: `elevation.card` transparent, and `elevation.dialog` `0 2 10 #00000014`.

## Decisions

The user decided the five questions the first draft raised:

| | Question | Decision |
|---|---|---|
| Q1 | A selected item's text while `color.selectionText` is unset | The surface's text colour (K1) |
| Q2 | Samples' and plate's heads at 785 px | Narrow their actions (done by K-S); keep the principal, in the trail (K2) |
| Q3 | The date input's calendar button | Keep it (K9) |
| Q4 | A dialog's two rules | Both in the card's divider colour (K6) |
| Q5 | Dialogs under flat cards | A new `elevation.dialog`, following `elevation.card` while unset, so no app changes (K8) |

## Question for the user

| | Question | Recommended |
|---|---|---|
| Q6 | The phone's top bar comes from two kinds of mockup: 10 draws the shell's bar 52 px tall, with ghost icon buttons 4 px from its ends; 04 and 05 draw a page head with a filled button 16 px from the side and 12 px from the top. Should a framed action at the bar's end stand step 4 from the side (K13), leaving it 4 px from the top, or should the bar follow 10 alone (Export PDF and Register 4 px from the edge)? | **K13.** The visible mark then sits about 16 px in on every phone screen, and the bar keeps the shell's 52 px. The 4 px against 12 at the top is the residual |

## Review response

The design was reviewed at `15cd3a55b` (2 blockers, 4 should-fixes, nits). Every finding is taken; none is pushed
back.

| Finding | Where it is taken |
|---|---|
| B1 K4 misses the dialog body the e-signature grab shows | K4: the overlay body sets `listGap = bodyGap`; a dialog and a drawer test; "Verify" now expects the body's step 3 |
| B2 K5 leaves `fitted` stale | K5: `fitted` reads the font too; a `singleLine` elision test |
| S1 the font read must not be gated on `pixelSize` | K5: gated on `metrics.height >= 0`; a no-`font.family` test |
| S2 a base-level `color.selectionText` never reaches the built-in surfaces | K1: `Theme.selectionText`/`hasSelectionText` written by `setPlainTokens`, bound by the built-in schemes; the built-in sidebar left out, and said so; a base-only test |
| S3 a selected table row does not follow the token | K1: the token is scoped to item-style buttons in `theme.md`; rows are under "Not in this batch" |
| S4 the principal's stale visibility; an empty slot's gap | K2: `showTrail()` owns the principal's visibility; `_trail` is a wrapping row; tests for both |
| K4 wording and leaks | K4: `lower()` zeroes the gap for every kind but `switch`; the holders set it; grid and row tests; a `forEach` row's holder under "Not in this batch" |
| K1 test count | K1 and K8: the per-token count becomes `size() - 2` |
| K8 a theme that set `elevation.card` for its dialogs | K8: `elevation.dialog` follows `elevation.card` while unset (Q5) |
| K8 the shadow has no `objectName` | K8: `dialogShadow` |
| K1 naming | `itemSelected` |
| K1 re-apply test | K1: the re-apply keeps the surface declared |
| Lanes | K-A no longer edits e-signature (K-S removed the wrapper); S3 adds no files; S4 stays in `shell_frame.hpp` |

## Defects this batch fixes

These are defects in shipped library behaviour, each closed by the item named. They are this batch's own work, so
they are not filed separately.

- **K1:** under `accent`, a selected item's content draws in the accent, which the look's own design does not; under
  `fill`, no token can lighten the selected label.
- **K2/K3:** the principal in the head is centred on the whole top line, not on the actions' line; after a sign-out
  and a sign-in it stays hidden.
- **K4:** a switch case's or default's nodes ignore the gap of the list the switch sits in, an overlay's body
  included.
- **K5:** a text whose role or theme font is set after its text keeps the width and the elision of its first font.
- **K6:** a dialog's and a drawer's two rules draw two different roles, one of them not the card's.
- **K7:** a check box is inset 6 px from its column's edge.
- **K8:** dialogs and drawers read the card's elevation, so a flat-card theme makes them flat too.
- **K10:** a wrapping row treats a text that wraps as rigid, so the inline lead runs past its block and the overflow
  report blames text that could wrap.
- **K11:** an open foldable panel puts its padding and a spacing between its touch-sized header and its content.
- **K12:** the compact top bar is padded by the expanded bar's step above and below.

K13 is a look the two kinds of mockup draw differently, not a defect, and waits on Q6.

## Not in this batch

- **Grabs that capture warnings (P6).** The grab command in `design/visual-review.md` gains
  `QT_FORCE_STDERR_LOGGING=1`, so a grab's `.err` file holds the overflow report and every Qt warning. That is
  example configuration and is fixed in place by K-B's gallery commit, not filed. Until then, any "no warnings"
  judged from a grab is unverified. P7 and P8 were taken with it.
- **A shadow drawn blurred.** `MorphPanel`, `MorphDialog`, `MorphDrawer` and `MorphPopupBackground` draw a shadow
  token as an offset rounded rectangle. They ignore its blur except in the corner radius. So the design's dialog
  shadow, `0 18px 48px` at 28 %, would draw as a hard slab, and the gallery keeps the card default instead. Drawing it
  blurred needs `MultiEffect` (available on Qt 6.8) checked on the offscreen grabs. Nobody has reported it yet; it was
  seen while designing K8.
- **A selected table row's text** (K1): it keeps `color.text`. Following `color.selectionText` would need the row to
  hand its cells a scheme, as J10 does for an item's content.
- **A multi-node switch case in a row** (K4): still stacked in a column with gap 0.
- **A `forEach` row's holder** (K4): a row whose template holds several nodes is a gap-0 column, as a switch case was.
  No example writes one where the gap shows. The same `listGap` would carry the `forEach`'s own `gap` there.
- **The terminal's overflow report** (batch J's open item), unchanged.

## Screen work these items need

Lane K-S has finished, on `lg-ks`:

- samples' and plate's head controls are narrower, and both trails hold the title's line at 1000 px (P7);
- e-signature's gapped column is gone, which K4 makes right.

These items need no further screen change. K1, K6, K7, K8 and K10–K13 change the gallery through the framework and
its bundle. If a later screen change widens samples' or plate's trail, the bounds are 597 and 418.5 px at 1000 px
(P1).

## Lanes

Three lanes, each starting from `lg-ks` (`300cee8c1` plus K-S's commits). The CI-fix lane's commits on `300cee8c1`
are merged at the fold, not by these lanes. Every item has its own tests and controls. A lane's gallery commit
(bundle or grab command) is its last.

### K-A: layout and text (K4, K5, K7, K10's measure, K11)

- `include/morph/ui/interpreter.hpp` (`LowerCtx::listGap`; `lower()` zeroing it; the `column` kind and its
  `children` lambda, `lowerArranged`, `lowerAll`, the overlay body, `lowerSwitch`);
- `src/qt_quick/qml/`: `MorphText.qml` (K5's font reads, K10's `wrapMinimumWidth`), `MorphStack.qml` (K10's
  wrapping minimum), `MorphCheckbox.qml`, `MorphPanel.qml` (K11);
- tests: `tests/test_ui_interpreter_detail.cpp`, `tests/qt_quick/test_qt_quick_widgets.cpp` and
  `tests/qt_quick/test_qt_quick_containers.cpp` (both shared, below);
- spec: `docs/spec/ui/interpreter.md`, `docs/spec/ui/document.md`, `docs/spec/qt_quick/backend.md` (shared, below);
- the P8 re-measure of the six example smokes for K10, reported to the coordinator.
- **Gallery commit:** none.

### K-B: shell (K2, K3, K10's inline lead, K12, K13)

- `include/morph/ui/shell_frame.hpp` (`_trail`, `showTrail()`, `_topLine`'s children, the compact bar gap,
  `setTrailEndInset`);
- `include/morph/ui/workspace.hpp` (the `Inline` lead copy's stretch; `headTrailEndsFramed`), `src/client/shell.cpp`
  (passing it on);
- tests: `tests/test_ui_shell_frame.cpp`, `tests/test_ui_workspace.cpp`, `tests/qt_quick/test_qt_quick_containers.cpp`
  (shared, below), `tests/tui/test_tui_shell_frame.cpp`, `tests/client/test_shell.cpp`;
- spec: `docs/spec/ui/shell_frame.md`, `docs/spec/ui/workspace.md`.
- K13 waits on Q6; if the answer is "follow 10 alone", K13 is dropped and only its phantom-gap trace stays.
- **Gallery commit:** `design/visual-review.md`'s grab command (`QT_FORCE_STDERR_LOGGING=1`).

### K-C: theme and overlays (K1, K6, K8)

- `include/morph/ui/theme.hpp` (the two tokens; `kThemeTokens` 70 → 72; `fillValue`'s empty-default colour and
  shadow; the contrast rule);
- `src/qt_quick/theme.cpp` (`selectionText`/`hasSelectionText` on the theme and per surface; `elevation.dialog`,
  `hasDialogShadow`);
- `src/qt_quick/qml/`: `Theme.qml`, `MorphScheme.qml`, `MorphButton.qml`, `MorphDialog.qml`, `MorphDrawer.qml`;
- tests: `tests/qt_quick/test_qt_quick_widgets.cpp` (shared, below), `tests/qt_quick/test_qt_quick_theme.cpp`,
  `tests/qt_quick/test_qt_quick_overlays.cpp`, `tests/test_ui_theme.cpp`;
- spec: `docs/spec/ui/theme.md`, `docs/spec/qt_quick/backend.md` (shared, below).
- **Gallery commit:** `examples/lims_gallery/src/lims_gallery.cpp`: the `sidebar` surface's `color.selectionText`,
  and the base's `elevation.card` and `elevation.dialog`.

### Shared files: owner and order

The lanes land in the order **K-A, then K-B, then K-C**. K-B's K10 inline-lead commit relies on K-A's wrapping
minimum for its "no report" check, and K-C touches the theme's closed set, which every theme test reads. A later
lane rebases onto the earlier ones before it merges.

| File | Lanes | Owner | What each adds |
|---|---|---|---|
| `tests/qt_quick/test_qt_quick_widgets.cpp` | K-A, K-C | K-C | K-A: the text-width, elision, wrap-minimum and check-box cases, appended. K-C: J10's selection cases rewritten in place, and its new cases appended after K-A's |
| `tests/qt_quick/test_qt_quick_containers.cpp` | K-A, K-B | K-B | K-A: K10's wrapping-row and K11's panel cases, appended. K-B: K2, K10's frame case, K12 and K13, appended after |
| `docs/spec/qt_quick/backend.md` | K-A, K-C | K-C | K-A: Text, Checkbox, Panel, Layout (the wrapping minimum). K-C: the item button's selected look, Dialog and Drawer |

No other file is shared. `lims_gallery.cpp` is K-C's alone; `visual-review.md` is K-B's alone, for the grab command.
The coordinator writes the "After batch K" section once, from one set of re-grabs taken with the new command after all
three lanes have merged.

Each lane re-grabs the screens its items name, at both sizes, with `--user "Dr. A. Meyer"` and
`QT_FORCE_STDERR_LOGGING=1`, and reports what it measured, and every line of each `.err` file, to the coordinator.

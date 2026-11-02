# Review of batch S design (`lg-s-design.md`)

I checked the design against `lg-fold-r` (`0a9a0bf9c`) in `/home/yaraslau/repo/morph-lims-e2`, reading only, with
`git show lg-fold-r:<path>`. I built nothing and edited nothing in either repository, and I did not open
`morph-lims-e1`. Paths below are relative to that worktree unless they start with `$S/`.
- **Measured** pixel numbers are PIL reads of `$S/lg-cmp/app-r/` and `$S/lg-cmp/mock/`. The scripts and crops are in
  `$S/lgs-rev-px/` (`col.py` and `row.py` scan one column or row; `head-*.png` are the head bands).
- **Measured** probe: one plain-QML file on Qt 6.12.0 offscreen, `$S/lgs-rev-qml/gap.qml`, output in `gap.out`.
- Everything else is **read** from code, or **inferred** where I say so. Nothing ran on Qt 6.8.1: no 6.8 build is on
  this machine.

**Verdict.**
- **S1** is the right vocabulary. The trail is a row of actions, and `actions.gap` is the right token for it. It
  leaves one row of the same actions outside the rule: an overlay screen's footer (S1). The new `wrap` means nothing
  where an action bar most often lives, an overlay's footer (S2). A computed `axis` gets past the `gap` refusal (S3).
  Its consequences are understated: samples' head grows the most and is not in U4 (S5).
- **S2**'s premise is right: the bands are the same colour, and the mockups close the header with a `#DDE1E6` rule.
  A 1 px rule inside the header is the right vocabulary. As written, though, the colour never reaches a table drawn
  in a card, and the tests would not notice (B1).
- **U1 to U6, A15 and A16** are right, with one more 4 px row of actions that nothing covers (S6).

**Counts: 1 blocker, 6 should-fixes** (S1 to S6).

---

## Premises I verified

| Design claim | Status | Evidence |
|---|---|---|
| #2: the head's tools and actions are 4 px apart | measured, holds | plate 1215 at y 36: select 700–884, Auto-fill 889–1015, Save 1020–1118. Samples 1000: 329–505, 510–640, 645–767, 772–903. Both are 4 px gaps |
| #2: `buildHeadTrail` is a wrapping `Row` of gap 1 with three `forEach`es of gap 1 | read, holds | `workspace.hpp:1058-1135` |
| #2: the mockups space head items 8 px apart | read, holds | `07-Plate.dc.html:47` and `10-Console.dc.html:65` use `gap: 8px`. Instrument's actions div (`03-Instrument.dc.html`) uses `gap: 8px` |
| S1: `MorphStack` spaces an action row by `stepLength` in both layouts | read, holds | `MorphStack.qml:40`, `:224`, `:354`, `:433`. Wrapped lines are also `space` apart (`:521`), so a trail that wraps puts 8 px between its lines, not 4. CSS flex-wrap with `gap: 8px` does the same |
| S1: a wrapping row gives no gap to a child with no width | read, holds | `place()` and `wrappedNaturalWidth` skip a gap for `basis == 0`. **Measured** (`gap.out`): a `RowLayout` does give one: `implicitWidth` 126 = 50 + 8 + 60 + 8 + 0 |
| S1: the terminal is unchanged | read, holds | `layout::stepCells` gives 1 column and 0 rows for both steps 1 and 2 (`layout.cpp:520-525`), and the terminal's `setActionRow` does nothing (`container_widgets.hpp:47`) |
| S1: the trail is never mounted inside an overlay's content | read, holds | `shell.cpp:506` mounts it as its own `Mounted` into `headSlot()`, and `workspace.hpp:823` mounts it into `page->slot`. A new `Mounted` starts with `_overlay` null |
| S1: refusing `gap` breaks no existing valid document | read, holds | No `actionBar` in `examples/`, `tests/` (the corpus included) or `docs/` has a `forEach` child. `gallery.cpp:145`, the lims screens and `test_ui_interpreter_commands.cpp:717,721` hold buttons only. `stepperParts`' `extra` is a button in its only user (stability) |
| #3: the two header bands are the same colour; the difference is a rule | measured, holds | App, stability 1215 at x 250: `#FAFBFC` 305–332, then the first row with its `#EDF0F3` rule at 362. Mock at x 250: `#FAFBFC` 393–418, `#DDE1E6` at 419, the row rule at 448. Mock header is 26 + 1 = 27 px, ours is 28 |
| #3: every mockup `th` has `border-bottom:1px solid #DDE1E6` | read, holds | 02, 04, 05, 06, 08, 09 and 10 all do. 03 and 07 have no `th` |
| S2: the terminal reads no table colours and sets the header apart by its role | read, holds | `TableImpl::paintHeader` (`table_widget.cpp:422-435`) draws the labels in `theme.textBold` and draws no rule |
| S2: the gallery's `divider` is `#EDF0F3`, lighter than the header rule | read, holds | `lims_gallery.cpp` `kThemeBase`. `#DDE1E6` is the gallery's `color.border` |
| #4: nothing between plate's run line and the page clips | read, holds for plate. **The general sentence is false**: `MorphSlot` clips while it scrolls, and so do `MorphDialog`, `MorphDrawer`, `MorphTable` (interactive or virtualized), `MorphSelect`, `MorphProgress` and an eliding `MorphBadge`. The run lines sit in a `switchOn` slot (not scrollable, so it does not clip) inside a collapsible, which has no `clip` |
| #5: Sample `fixed(11)` is 96 px and Outcome takes the rest | measured, holds | receipt 1215: Time from 900, Sample 970–1066, badges end at 1126, the pane at 1198 |
| U2: Sample becomes about 126 px and the badges end 33–40 px from the edge | inferred, holds | 236 px for the two columns, with naturals of about 96 and 77 px. Each gets about 31 px of the spare, so Sample is about 127 px and the badges end about 40 px from the edge |

---

## Blockers

### B1. S2: the header rule's colour never reaches a table in a card, and the tests would pass anyway

**Read.**
- The design maps the token in "both of `theme.cpp`'s scheme maps". Only `kSchemeProperties` is a scheme map, and it
  feeds the surfaces a theme declares (`setSurfaces`, `theme.cpp:309-343`). `kColorProperties` sets a property on
  `Theme`, so `Theme.qml` needs `property color tableHeaderRule`. Without it, `setProperty` fails.
- `Theme.qml` builds seven built-in schemes, and each one copies `tableHeader` from `Theme`:
  - `themeScheme` (`:416`), `pageScheme` (`:461`), `cardScheme` (`:506`), `sidebarScheme` (`:546`),
    `toolbarScheme` (`:591`) and `barScheme` (`:635`);
  - `toolbarJoinedScheme` (`:682`), which copies `barScheme`.

  `MorphButton.qml:154` copies a scheme role by role too. `Theme.qml` is not in S-F's file list.
- **A table in a panel draws with `cardScheme`** (inferred; read: `MorphPanel.qml:29`, and a nested panel tests
  `inheritedScheme !== Theme.cardScheme`). That covers every gallery table, stability's included. Unless
  `cardScheme` binds the new role, it keeps `MorphScheme`'s transparent default, and U5 draws nothing.
- **The tests miss it.** The Qt case puts a table under the theme's own scheme, so binding `themeScheme` alone passes
  it.
- **The third mutation names a case that does not exist.** No dark scheme goes through `kSchemeProperties`:
  - a theme index's `dark` variant resolves like `base`, through `kColorProperties`;
  - the gallery has no dark variant at all (`kThemeIndex` names `base` and surfaces only).

**Fix:**
- **Files.** Add `Theme.qml` to S-F's list: the property, plus one binding in each of the seven schemes. In
  `toolbarJoinedScheme`, take the value from `barScheme`, as `tableHeader` does. Decide what `sidebarScheme` takes;
  `theme.tableHeaderRule` is the obvious choice. Add `MorphButton.qml:154`. The rule for the lane: grep `tableHeader`
  in `src/qt_quick/qml` and add the new role beside every hit.
- **Spec.** `theme.md`'s row says the token maps to `Theme.tableHeaderRule`, and from there to every built-in scheme
  and every declared surface.
- **Qt tests, all discriminating:**
  - a table in a panel with no surface (`cardScheme`). **Control:** drop the card binding, and it is transparent;
  - a table on a surface the theme declares, which sets `color.table.headerRule` itself (the `kSchemeProperties`
    path). **Control:** drop that entry.
- **Mutations.** Replace "the dark-scheme case" with these two.

---

## Should-fixes

### S1. S1 leaves an overlay screen's footer outside the row of actions

**Read.**
- The design rejects a separate token because "a head's trail holds the screen's own actions, the same ones an
  overlay's footer shows (`overlayActions`)", and "a second token would let two rows of the same actions disagree".
- Today, `overlayActions` (`workspace.hpp:642-660`) is a `Row` of gap 2 holding a `forEach` of gap 2. It is mounted
  as its own `Mounted` into the footer (`:599`).
- `MorphFooter`'s `RowLayout` spaces its own children by `Theme.actionSpacing`. The footer has one child, though:
  that `Row`. So the buttons are `spaceOf(2)` apart.
- With `actions.gap: 12`, a screen's head trail and a dialog's `actions` go to 12 px. The same screen opened as an
  overlay keeps 8 px. E-signature is that case: its Cancel and Sign actions show only when it is called as an overlay
  (`esignature.cpp:225-231`).

**Fix:**
- `overlayActions` returns `ui::actionBar` around the same `forEach`. It mounts in place, because its `Mounted` has no
  overlay, and S1's direct-child rule marks the `forEach`. Name it in the S1 commit and in `workspace.md`.
- **Qt test:** with `actions.gap: 12`, an overlay screen's two footer actions are 12 px apart. **Control:** today's
  `Row`.

### S2. `wrap` does nothing on an action bar in an overlay's footer

**Read, then inferred.**
- Inside an overlay's content, an action bar mounts into the footer (`mount.hpp:1951-1954`).
- `MorphFooter`'s `RowLayout` is anchored only on the right, unless the footer stretches, so its width is its implicit
  width (`MorphFooter.qml:25-33`).
- A wrapping `MorphStack`'s implicit width is its one-line natural width (`wrappedNaturalWidth`), so it is never given
  less and never wraps. In a dialog narrower than its actions, the row runs out past the left edge.
- In a sheet's stretching footer, `share()` sets `sharesWidth` on the bar. How that combines with `place()` is not
  defined.
- R3 refused `wrap` everywhere, so this case could not arise. S1 accepts `wrap` everywhere and makes it reachable.

**Fix (vocal over silent):**
- The mount refuses a wrapping `ActionBar` whose footer is non-null, with a message such as "an overlay's footer is
  one row of actions; `wrap` is for an action bar in place".
- Alternatively, make it work: the footer gives a wrapping child its full width. The design would then have to say
  how that combines with stretching.
- **Mount test:** `ActionBar{.wrap = true}` inside a dialog's content throws. **Control:** the same bar outside the
  dialog mounts.

### S3. A computed `axis` gets past the `gap` refusal

**Read.**
- `checkForEachWrap` reads only a literal `axis` (`document.hpp:4541-4557`).
- The interpreter reports `wrap` and `divided` against a computed axis at lowering time (`interpreter.hpp:9121-9133`).
- The design refuses `gap` on "a horizontal `forEach`" directly in an `actionBar`. A computed axis that evaluates to
  horizontal passes the load. The mount then ignores its `gap` without a word.

**Fix:**
- At load, refuse a literal horizontal `axis` together with `gap`.
- At lowering, report `gap` when the computed axis is horizontal and the parent is an `actionBar`, as `wrap` is
  reported. Lowering a `forEach` needs to know its parent's kind for this. Pass it, or check from the `actionBar`'s
  lowering over its children.
- `view_tree.md` says that a C++ `ForEach`'s `gap` is not read while the `ForEach` is a horizontal direct child of an
  `ActionBar`.
- **Document test:** a computed-axis `forEach` with `gap` in an `actionBar` reports at `/…/gap`.

### S4. S1's tests and mutations: four corrections

- **The direct-child rule needs a depth control.**
  - "The same `ForEach` inside a plain `Row` records neither" does not catch a member test of the form "any
    `ActionBar` is mounting", rather than "my parent is that `ActionBar`".
  - Add a `ForEach` inside a `Row` inside an `ActionBar`, which records neither.
  - Add the mutation "any depth".
- **The second mutation's stated failure.** The Qt case has one tool, so there is no tool-to-tool gap to fail. It
  fails on the action-to-action gap. Correct the attribution, or give the case two tools, which plate's and samples'
  heads both have.
- **The empty-parts assertion must be exact.**
  - **Measured** (`gap.out`): a non-wrapping `RowLayout` gives an empty, visible child a gap. The no-`setWrap`
    mutation therefore leaves the last action 8 px short of the trail's end.
  - The existing head test checks `actionRight >= slotRight - gap - 0.5` (`test_qt_quick_containers.cpp`, the
    trailing-edge case near `:1725`), so it passes either way.
  - "Where it is today" is not an assertion. Write it as two:
    - the last action's right edge equals `head_trail`'s right edge within 0.5 px;
    - with no tools, the first action's left edge equals the trail's left edge within 0.5 px.
- **The mount test's own gap.** Give the `ForEach` gap 0. Its "gap of step 2 whatever its own gap" then shows on the
  terminal as well, where step 0 is 0 columns and step 2 is 1.

### S5. The consequences name two heads; five grow, and samples grows most

**Read, then measured.** Trail gaps per expanded head, each growing 4 px. Visibility is read from each screen's
`declareHead`:

| Screen | Trail | Gaps | Growth |
|---|---|---|---|
| samples | search, status, priority / Register | 3 | +12 |
| plate | template / Auto-fill, Save | 2 | +8 |
| console | lab, find / Connected (end lead) | 2 | +8 |
| audit | search / CSV, PDF | 2 | +8 |
| instrument | Log maintenance, Book / ··· | 2 | +8 |
| e-signature, receipt, results, review | one item, or actions shown only in an overlay | 0 | 0 |

**Room at 1000, measured ink against the trail, less `_topMain`'s step-3 gap (inferred 12 px):**
- **samples:** the title's ink ends at 291 and the trail starts at 329, so about 24 px are spare. After S1 about 12
  are left. It is the tightest head.
- **instrument:** the tags end at 631 and the trail starts at 672, so about 29 px are spare, and about 21 after S1.
- **audit and console:** more than 60 px are spare.

**The plate subtitle risk is low** (inferred). At 1215 the subtitle's ink ends at 644 and the select starts at 700.
About 44 px are spare after the gap, and about 36 after S1.

**Fix:**
- List the five heads in Consequences.
- Add samples, audit and instrument at 1000 to U4's measurements.
- Give U4 a rule for samples like plate's: if its head wraps, narrow the search (23 units) by the fewest units that
  bring it back to one line.

### S6. Samples' expanded bulk bar is a fifth row of actions 4 px apart, and nothing covers it

**Measured.** At samples 1000 (y 78), Assign to worksheet… is 527–687, Change priority… 692–825 and Print labels
830–926: 4 px apart. The mockup's bar is `display:flex; gap:10px` (`05-Samples.dc.html:50-52`).

**Read.** `bulkBar` (`samples.cpp:341-366`) is a `ui::row(…, {.gap = 1})`.
- U1 converts only `compactBulkBar`, so the same screen would draw its phone bar at 8 px and its desktop bar at 4.
- A4 ("the bulk bar at 70 against 54") is about the bar's position, not its gap.
- Neither review flagged it: reviewer A's samples 1000 and 1215 grabs were identical to Q.

**Fix:**
- Add `bulkBar` to U1. Keep the text, Select all, the stretch spacer and Clear as children: the stepper footer already
  holds a spacer and non-action content.
- Measure 8 px between the three framed buttons. The mockup's 10 is the theme's gap, not this screen's to choose.
- Alternatively, record it as A17 with that reason.

---

## The screen items and the accepted items

| Item | Assessment |
|---|---|
| U1 | Right (read: all four sites are `ui::row(…, {.gap = 1})` in a `pinned` bottom, not in an overlay, so the bars mount in place). The review note's room at 1000 drops from about 160 to 148 px, and "Approving needs your" is about 117 px, so it stays on two lines (inferred). Add `bulkBar` (S6) |
| U2 | Right (inferred arithmetic above). On the terminal the column becomes stretch too. Run the gallery's terminal receipt tests and report whether any column position is pinned |
| U3 | Right for plate (read: no clip on the path). Correct the sentence "only `MorphScroll` sets `clip`" (table above). The mockup puts the markers flush with the rule's start too (`mock-plate-phone.png`), so both sides should go to 0, as U3 says. The selected-ring grab is the right control |
| U4 | Widen it to samples, audit and instrument at 1000, and add samples' fallback (S5) |
| U5 | Drop "in the dark scheme …": the gallery has no dark variant (B1). Measure a table in a card (stability) and one on a declared surface, if the gallery has one, to show B1's fix holds |
| U6 | Fine. "Every head trail gap" should list the five heads in S5's table |
| A15 | Agree. I did not re-measure; reviewer A's 322 against 318 px stands. It is the same trailing-inset mechanism as A14 |
| A16 | Agree: batch K kept the picker so a date can be picked without typing |

## Answers to the focus questions

| Question | Answer |
|---|---|
| S1: effect on every head in every example | Only `lims_gallery` declares head tools, actions or menus (read: grep of `examples/`). Five expanded heads grow by 8–12 px (S5). Compact heads are unchanged: each shows at most one trail item. The terminal is unchanged (read: `stepCells` gives 1 column for steps 1 and 2, and 0 rows for both) |
| S1: does `wrap` keep R3's rules? | The theme's gap: yes. The footer placement: yes, but there `wrap` is a silent no-op (S2). `percent` children are refused in a wrapping bar, as in a wrapping row. That matches the loader, whose `takesPercent` already excludes `actionBar`. The R3 test that refuses `wrap` (`test_ui_document.cpp:278-296`) changes, which the design's mutation already covers |
| S1: does refusing `gap` break a valid document? | No (read; table above). A computed `axis` slips past the refusal (S3) |
| S1: the plate subtitle risk | Low: about 36 px of room are left at 1215 (inferred from measured ink). Samples 1000 is the tighter case (S5) |
| S2: is the premise right? | Yes, re-measured (table above). The mockup's band is 26 px plus a 1 px rule. Ours is 28 px with no rule |
| S2: is a 1 px rule in the header the right vocabulary? | Yes. It sits inside the header's height, as CSS's `border-bottom` sits inside a `th`'s box, so no row moves. It is a colour token at the same fixed 1 px as the row rule. It is transparent by default, so the stock look is kept. It has its own colour, because the mockups' header rule is darker than their `divider`. Its flow through the schemes is wrong as written (B1) |
| S2: on the terminal | Unchanged and correct. The header is bold, and a rule would cost a row. The terminal reads no theme tokens (`tui/frontend.md:80`) |
| Mutation controls | They fail as stated except: S2's third mutation names a case that does not exist (B1); S1's second fails on a different assertion than stated; S1's third passes the existing loose test, so it needs the exact assertion; the direct-child rule needs a depth control (S4) |
| Qt 6.8.1 | Nothing needs an API newer than 6.8 (read). S1 changes no QML. S2 adds a `Rectangle` anchored at the bottom, with an `objectName`. The tests use `findChild` and geometry. The probe ran on 6.12 only. The design does not mention 6.8; add one line |

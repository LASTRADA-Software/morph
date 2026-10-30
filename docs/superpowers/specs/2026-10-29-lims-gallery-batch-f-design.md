# LIMS gallery batch F: layout, theme and containers

Batch F of the LIMS gallery visual review (`examples/lims_gallery/design/visual-review.md`, items F1–F14): the
differences between the running screens and their mockups that come from how ui/1 lays a screen out, what a theme can
say, and how a container draws. Every item is a change to the vocabulary, the interpreter, a frontend or the theme
set, so that ui/1 itself produces the mockup's look; a screen changes only to use what the item adds, or to drop a
workaround the item makes unnecessary, in the same commit.

Batch G (component looks and text) is designed in parallel. Where the two meet, F defines the shared token or
vocabulary and G cites it; [Shared with batch G](#shared-with-batch-g) lists each one.

Every claim below was checked against `d5292bc0a` (`lg-ci2-int`, batches A–E landed), and every line cited is at that
revision. Each item says whether its premise was **reproduced** (run on this revision) or **read** (inferred from the
code). The review's evidence (grabs at 1000×830, 390×844 and the sizes its working notes list, pixel reads, a minimal
popup probe) is taken as measured where the item says so. Three throwaway probes were run for this design, with
`qml6` 6.12.0 on the offscreen platform, and kept out of the tree:

- `lg-dF-panel.qml`, `lg-dF-panel2.qml`: `MorphPanel.qml`'s layout (an inner `ColumnLayout` filling the frame, a
  header row, a divider, the content `ColumnLayout` with `Layout.fillWidth` only) in a 400 px tall panel holding
  65 px of content, today and with three candidate fixes;
- `lg-dF-popup.qml`, `lg-dF-popup3.qml`: a `Popup` under a trigger 8 px from the right edge of a 400 px window, with
  `margins` and with an end-alignment rule written as a binding and as an `onAboutToShow` assignment.

Their output is quoted where an item rests on it. The design was reviewed once (`lg-rF-review.md`); [Review
response](#review-response) says what each finding changed.

| Item | Where it lands | Verdict on the review |
|---|---|---|
| F1 | interpreter: the column that holds a node list, and the one-row `forEach` (`lazy`) a case or an overlay body is mounted through | Right, and wider: the default case's column (`lowerAll`, more than one node) and a panel's multi-node content lose a child's stretch the same way; and the slot sees the `lazy` wrapper, not the column, so both must carry it (read) |
| F2 | Qt Quick `MorphPanel`, `MorphCollapsible` | Right (reproduced). **The fix the issue and the review suggest does not work:** `Layout.alignment: Qt.AlignTop` on the content column still leaves the content 95 px down a 400 px panel |
| F3 | Qt Quick `MorphMenu`, `MorphSelect`'s dropdown, `MorphDateTimeInput`, a shared `MorphPopupBackground`; terminal `MenuButtonImpl`, `DropdownSelectImpl` | Right (reproduced). `margins` alone moves the popup to the window's edge, not to the trigger's end; the trigger's position must be read when the popup opens |
| F4 | `row` and a horizontal `forEach` gain `wrap`; both frontends | Right |
| F5 | `ShellFrame`, `Workspace`'s head, the client shell | Right; needs F4. The sign-out button and the principal leave the top line, which is what frees it |
| F6 | no new vocabulary; a layout case, F1, F2 and the two screens | **Mostly expressible already** (read): a stretching splitter fills the page, a pane takes a `surface`, and `pinned` can be a pane. What stops the screens is that they wrap the splitter in `pinned`, F2's centring, and F1 for review's compact case |
| F7 | `gap` on `panel`, `dialog`, `drawer`, `collapsible`; `divided` on `column` and a vertical `forEach`; table cell padding tokens | Right |
| F8 | the token reader (`$root`, refusing a token with members), an unread-token report (#1070) | Right (measured by the review). The DTCG 2025.10 format itself says a tool **must** report an object holding both `$value` and child tokens |
| F9 | `color.hover`, `color.track`; `color.neutral` becomes the neutral tone's colour; a contrast report | Right. The cause is a token that means two things, which the gallery could not avoid misreading |
| F10 | a panel draws on the `card` surface; controls fill with `color.controlSurface` | Right |
| F11 | `color.controlBorder` | Right |
| F12 | `font.size.small`, `.caption`, `.mono`, `.display`, `font.weight.strong`; text roles `display` and `small` | Right |
| F13 | ghost text in the accent; the active tab label in `text`; a selected item in `color.selection` | Right |
| F14 | `color.scrim` | Right; the drawer too |

## The shape of the batch

Four rules hold throughout.

- **A token means one thing.** Where one token serves two purposes (`color.neutral` as a tone and as a fill,
  `color.border` as a divider and as a control outline), it is split, and each component reads the one it means.
  Every new token has a fixed default in `kThemeTokens`, as every token has today: no token falls back to another,
  so what a theme does not set never depends on what else it sets. That has a cost, stated where it lands: the
  neutral tone no longer follows a theme's `muted` (F9), and a control's outline no longer follows its `border`
  (F11). Each built-in surface scheme in `Theme.qml` gets its own value for every new role
  ([Built-in schemes](#built-in-schemes)), so the dark sidebar is not drawn with the light defaults. The only in-tree theme is the gallery's
  (`grep -rln 'ui-theme/1' examples src include` finds only `lims_gallery.cpp` outside the framework), so no other
  application changes look by a token's change of meaning.
- **A theme that says something the client does not read hears about it.** A token with members is refused, as DTCG
  requires; a token outside the set that nothing reads, and a combination of colours a reader cannot read, are
  reported. Nothing a bundle writes is dropped in silence.
- **A node list is transparent to sizing.** Wherever the interpreter wraps several nodes, or one case's nodes, in a
  column of its own, that column stretches on an axis when one of its children does, so a document's `sizing` means
  the same inside a case, a dialog or a panel as at the root.
- **Visual tokens are Qt Quick's; structure is everyone's.** A colour, a radius, a font size or a shadow is drawn by
  Qt Quick; the terminal has no palette mapping (`theme.md`, Not yet) and keeps its look. A change that moves where
  things are (wrapping, gaps, a panel's content, a popup's place, the top bar) is drawn by both, with a layout
  conformance case wherever geometry is observable. Each item says what the terminal does.

## Lanes

Three lanes run in parallel. Each lane owns its files. Where a lane writes into a file that another lane owns, that
hunk is named in the table, and it sits at a distinct place in the file.

| Lane | Items | Owns | Named hunks in another lane's file |
|---|---|---|---|
| **F-L, layout vocabulary** | F1, F4, F7 (gap, `divided`) | `interpreter.hpp` (`lazy`, `holding`, `lowerAll`, the switch, overlay and container lowerings), `document.hpp` (node shapes), `view.hpp` (`Row`, `Column`, `ForEach`), `author.hpp` (options), `mount.hpp`, `backend.hpp` (`StackWidget`), `recording_backend.hpp`, `src/qt_quick/containers.{hpp,cpp}` (Qt `StackImpl`: `setWrap`, `setDivided`, and re-running `place()` from `adopted()` as the grid does at `containers.cpp:52-67`), `MorphStack.qml`, `src/tui/container_widgets.*`, `src/tui/layout.*`, `docs/spec/ui/{document,view_tree,interpreter,author,backend_contract}.md` | the gallery screens that drop a hand-rolled gap or wrap |
| **F-T, theme** | F8, F9, F10 (controls), F11, F12, F13, F14, F7 (table cell padding) | `theme.hpp`, `service.hpp` (the report at `build`), `src/qt_quick/theme.cpp`, `Theme.qml`, `MorphScheme.qml`, `MorphWidget.qml` (the tone functions), `MorphButton*.qml`, `MorphInputBackground.qml`, `MorphCheckbox.qml`, `MorphProgress.qml`, `MorphTabs.qml`, `MorphTable*.qml`, `MorphField.qml`, `MorphText.qml`, `MorphDialog.qml`, `MorphDrawer.qml`, `docs/spec/ui/theme.md`, `docs/spec/qt_quick/backend.md`, `lims_gallery.cpp`'s theme index and token files, `tests/qt_quick/test_qt_quick_theme.cpp` | `view.hpp` (`TextRole`, two enumerators); `document.hpp` (role names) and `document.md` (the role list); `interpreter.hpp` (`roleOf`); `recording_backend.hpp` (its `TextRole` switch, `:315-317`); `src/tui/leaf_widgets.cpp` (the two roles); `backend_contract.md` (Button emphasis and style, `DialogWidget`'s sheet and scrim sentences); `MorphSelect.qml` (track and hover lines only) |
| **F-S, shell and containers** | F2, F3, F5, F6, F10 (panels) | `MorphPanel.qml`, `MorphCollapsible.qml`, `MorphMenu.qml`, new `MorphPopupBackground.qml`, `MorphDateTimeInput.qml` (the popup), `MorphSelect.qml` (the dropdown popup's placement and background), `MorphSlot.qml` (F6's minimum-height scroll), `src/qt_quick/` panel widget (its `scheme()`, F10), `src/tui/list_widgets.cpp` (popup placement), `workspace.hpp` (the head), `shell_frame.hpp`, `src/client/shell.cpp`, `docs/spec/ui/{workspace,shell_frame}.md`, `docs/spec/client/client.md`, `review.cpp`, `console.cpp`, the shell tests listed under F5 | `backend_contract.md` (the cross-axis paragraph for F2; `MenuWidget`, `SelectWidget`, `DateTimeInputWidget` for F3; `SlotWidget` for F6); `src/tui/container_widgets.cpp` (`SlotImpl::scrolls`, F6); `document.md` (F6's one sentence); `lims_gallery.cpp`'s theme index (the `pane` surface, F6) |

**Order and waits.**

- **F-L:** F1, F4, F7. F7's `divided` rules draw in `Theme.divider`, so F7 **waits for F-T's F11**. The rest of F-L
  waits for nothing.
- **F-T:** F8, F9, F11, F10, F12, F13, F14, then the table cell padding. F8 comes first because every later token must
  be loadable from the gallery's bundle. F9 and F11 come early because F-S and F-L wait on them.
- **F-S:** F2, F6, then F3, then F5.
  - F3 **waits for F-T's F9**, so F-S writes `scheme.hover` and `Theme.popupShadow*` itself, and neither lane edits
    `MorphMenu.qml` twice.
  - The panel half of F10 has a test that reads `Theme.controlSurface`, so it **waits for F-T's F10**.
  - F5 **waits for F-L's F4**, because the head is two wrapping rows.

**Shared append points, known conflicts.** Three places conflict when the lanes are folded. Resolve them as `next.md`
says.

- The `std::array<ConformanceCase, 16>` size line in `layoutConformanceCases`, which F-L, F-S and batch G all append to.
- The case list in `test_ui_conformance.cpp`.
- The numbered case lists in `backend_contract.md`.

**Conformance numbering.** At this revision `conformanceCases()` has 54 cases and `layoutConformanceCases()` has 16.
This batch adds no behaviour case and six layout cases:

- three in F-L: one for F1 and two for F4;
- three in F-S: one for F2 and two for F6.

F3's popup is not a widget that the probe can read, so each frontend's own test checks its geometry. Each case is
named below by its description. Its number is its position once the lanes and batch G are folded.

## F1: a case's, an overlay's and a node list's holder passes on its children's stretch (#1069)

**Problem (the review measured it; the cause was read).**

- The review measured the cut at the same y at 390×500, 390×844 and 390×1400, on stability, samples, receipt and
  results.
- A switch's non-default case is lowered through `lazy(…)` (`interpreter.hpp:8295`; `lazy` is defined at
  `:6665-6671`). `lazy` is a one-row `forEach` with a default `Common`, so its sizing is `content` on both axes. The
  mount makes a `StackWidget` for it and applies that empty common (`mount.hpp:1257-1263`).
- Inside the lazy row, the case's nodes are held by `holding()` (defined at `:6679-6685`, called at `:8308`). That
  column has no sizing either.
- The slot reads only its **direct** child's height sizing: `MorphSlot.qml:28-35` (`stretchesDown`) and
  `SlotImpl::scrolls` (`container_widgets.cpp:124`). That direct child is the lazy `forEach`'s stack.
- So a `pinned` page inside a case gets no height to fill. Its `scroll` falls back to its implicit height, which is
  at most 20 units (`MorphScroll.qml:42-43`).
- The default case escapes only when it is one node, which `lowerAll` passes through (`:2655-2658`, `:8322`).

A dialog's and a drawer's body take the same path: `lazy` (`:8211`), then `holding` (`:8253`). `lowerAll` with more
than one node builds the same plain column (`:2659-2663`). It is used for:

- a `default` of several nodes;
- a panel's, a scroll's or a collapsible's content written as several nodes;
- a table cell of several nodes (`:8593`);
- a table's `empty` content (`:8776`);
- a group heading (`:8795`);
- an error boundary's fallback (`:9194`);
- a forEach row (`holding`, `:8465`).

**Decision: every holder a node list is mounted through has its content's sizing.**

- **The column.** `detail::listColumn(std::vector<Node> children)` returns the column. On each axis its
  `common.layout` is `Sizing::stretch(w)` when at least one child's sizing on that axis is a stretch, where `w` is the
  largest weight among those children. Otherwise it is `content`. Bounds and `alignSelf` stay unset, because they
  belong to a child, and the child keeps them inside the column. A child's layout is read from its `NodeData` with
  `std::visit` over the kinds' `common` (every kind has one). `holding()` and `lowerAll` build their column through
  `listColumn`, and `holding` then adds its `visible` and `keys` as now.
- **The lazy row.** `lazy(build, LayoutHints hints)` gives the one-row `forEach` those hints as its `common.layout`.
  `lowerSwitch` and `lowerDialog` work out the hints from the case's or the body's document nodes before they build
  the lazy node, by the same max-weight rule. Each child's `sizing` is taken as follows:
  - **A literal sizing** is read as it is written. Every gallery screen writes a literal.
  - **A computed sizing** is evaluated once, with `once()`, in the enclosing scope, when every name it reads is
    visible there. The loader knows the case's own declarations.
  - **A computed sizing that reads a name the case itself declares** cannot be evaluated before the case mounts. The
    holder then stays `content` on that axis, and the interpreter reports it at lowering, at the sizing's path: "this
    sizing reads `x`, which the case declares, so the case is held at its content size". It is never silently
    dropped.

  The mount and the backends are unchanged. A `forEach` still takes its own `common`, and a list that is not a
  holder is untouched. This is why the other option, a mount rule that "a one-row `forEach` takes its row's sizing",
  was not taken: it would change what every `forEach` means.

So a node list means what its nodes would mean if they were written inline. A stretching child of a case stretches
the case's slot. A `forEach` row in a vertical list whose content stretches in height now takes a share of the list's
spare height, as the same content written directly in a column would.

**Spec.**

- `interpreter.md`, Lowering, a paragraph "Holders". The interpreter mounts the following through a holder:
  - a case;
  - an overlay's body (a one-row `forEach`, so that the content's scope lives only while it is mounted);
  - several nodes;
  - a row's content (a column).

  A holder's sizing on each axis is a stretch, by the largest weight, when a child's is, and its content otherwise. A
  single node of a list is lowered unwrapped. A computed sizing that reads a case's own names is reported.
- `document.md`, the `sizing` paragraph: a node's sizing is the same inside a case, an overlay or a row as at the
  root.

**Tests.**

- **Layout case** "a slot showing a one-row ForEach whose holder stretches fills the slot". It is built from
  view-tree nodes, the way `lazy` builds them:
  - Setup: a column of fixed height 20, holding a `Switch` whose case is a one-row `ForEach` sized
    `content × stretch`. Its row is a column of a stretching scroll of 40 one-line texts above a text `bar`.
  - Check: `bar`'s bottom is the column's bottom, on Qt Quick and the terminal.
  - **Control:** give the `ForEach` today's default `Common`. The slot then shows its natural height and scrolls it.
    On Qt Quick `bar` sits under a 20-unit scroll, at most 21 units down, so it lands below the column's bottom. On
    the terminal it lands at row 41.
- `tests/test_ui_interpreter_detail.cpp` (RecordingBackend). Each check reads the record of **the node the `Switch`
  mounts**, the slot's direct child, and not the inner column:
  - A `switch` whose non-default case holds a `column` with `sizing: {height: stretch}` records that node as
    `content/stretch(1)`.
  - A case holding two texts records `content/content`.
  - A `default` of two nodes, one with `width: {stretch: 2}`, records `stretch(2)/content`.
  - A dialog's body holding a stretching text input records the lazy node as `stretch(1)` across.
  - A case whose child's sizing is `cond` over a name the case declares records `content/content`, and the report
    names the path.
  - **Control 1:** build `lazy` without hints. The first, fourth and fifth checks fail, while the inner column
    still records `stretch`. This is the incomplete fix the test must reject.
  - **Control 2:** keep the hints but build the column without `listColumn`. The third check fails.
- `lims_gallery_render_smoke`, compact stability at 390×844:
  - Check: the footer's bottom is the window's bottom less the page padding, and "Continue" is inside the window.
  - **Control:** Control 1 puts the footer at y ≈ 440.

**Re-grab.** stability, samples, receipt, results and review at 390×500, 390×844 and 390×1400. In each, the bottom
bar sits at the window's bottom and the content scrolls under it.

**Risks.** Two changes in layout are expected:

- A content-height `forEach` row whose content stretches was laid out at its natural height. It now takes spare
  height.
- The `lowerAll` users listed above (cells, empty states, group headings, error-boundary fallbacks) now stretch
  whenever one of their nodes does. That is the meaning they claim, but it is a change in layout.

The lane runs the GUI and unit tests of every example (bank, bookmarks, lims, gallery, kanban, lims_gallery) and the
gallery's grabs.

## F2: a panel's content starts at its top (#1072)

**Problem (reproduced).** `MorphPanel.qml:33-83` stacks three cells in an inner `ColumnLayout` that fills the frame:

- a header row;
- a divider;
- a content `ColumnLayout` that has `Layout.fillWidth` only.

When a Qt Quick layout has more height than it needs, it spreads the spare height over all its cells.

`lg-dF-panel.qml` is a 400 px panel with two 30 px children:

```
F2 today          content y 215 height 65
F2 alignTop       content y 95  height 65
F2 stretchChild   content y 28  height 372
```

`lg-dF-panel2.qml` makes the content column fill the height:

```
F2 fill           header y 32 content y 215
F2 fill+alignTop  header y 32 content y 215
F2 fill+filler    header y 0  content y 28   child0 y 0 child1 y 35
```

The reviewer re-ran the probe independently and got the same pattern (today 220, `AlignTop` 105, filler 31).

What the probes show:

- **`Layout.fillHeight` alone changes nothing.** A `ColumnLayout` whose children don't fill has a maximum height
  equal to its children's, so `Layout.fillHeight` on the content column has no effect.
- **The suggested fix does not work.** `Layout.alignment: Qt.AlignTop` is what the issue and the review suggest.
  With it, the header still sits 32 px down and the content 95 px down.
- **Only a cell that fills takes the spare height.** The filler variant is the only one that puts the content at the
  top.

`MorphCollapsible.qml` (lines 21-51) is built the same way.

**Decision.** The fix applies to both the panel and the collapsible.

- **No shown content child stretches down:** the inner layout ends with a filler, an `Item` with
  `Layout.fillHeight: true`.
- **A shown content child stretches down:** the content column gets `Layout.fillHeight: true`, and the filler hides.

This is `MorphStack`'s `stretchesAlong` rule (`MorphStack.qml:27-35`), applied to the content column's children.
The header stays at the top and the content follows it. A stretching child takes the panel's spare height.

The terminal: `PanelImpl` lays its content out with the stack solver, which packs at the start. The layout case below
holds it there on both frontends.

**Spec.** `backend_contract.md`, the cross-axis paragraph: "a panel's or a collapsible's content starts under its
header whatever the panel's height; only a stretching child takes the spare height".

**Tests.**

- **Layout case: "a panel taller than its content keeps the content under its header".**
  - A panel of fixed height 20, titled, holding one text: the text's top is at most 3 units below the panel's top,
    counting the header and padding, on Qt Quick and the terminal.
  - A second panel holding a stretching column: the column's bottom is the panel's bottom less its padding.
  - **Control:** remove the filler, and the first text is centred, about 8 units down. Remove the `fillHeight`
    switch, and the stretching column keeps its natural height.
- **`tests/qt_quick/test_qt_quick_containers.cpp`:** a collapsible of fixed height 20 holding one text gets the same
  check. **Control:** the same mutations as the layout case.

**Re-grab.** console at 1000×1200 and 1000×830: the worklist's tab bar sits at its panel's top. Tab switches cannot be
grabbed, so the probe's geometry and the layout case stand in for them.

## F3: popups stay inside the window and end-align to their trigger (#1071)

**Problem (reproduced).**

- `MorphMenu.qml:181-184`: the `Popup` sets `y` under the trigger, but neither `x` nor `margins`. So it opens
  rightwards from the trigger's start, and with `margins` at Qt's −1 it is never pushed back inside the window. The
  review's probe measured it at 312 → 440.6 in a 400 px window.
- This design's probe adds the following:

  ```
  margins: 4, no x            popup 267.4 to 396      (pushed to the window's edge, not to the trigger's end at 392)
  x bound to mapToItem(...)   popup 267.4 to 396      (the binding read the trigger's position once, before layout)
  x set in onAboutToShow      popup 263.4 to 392      (end-aligned to the trigger)
  left-edge trigger           popup   8   to 136.6    (start-aligned, unchanged)
  ```

- A menu **bar** has a single `Popup` for every top entry, at `y: bar.height` with no `x`. Nothing records which entry
  opened it.
- The date picker's popup (`MorphDateTimeInput.qml:120-131`) is end-aligned to its field and has no `margins`, so a
  field at the window's left edge opens past it.
- The select's dropdown is a `ComboBox` popup (`MorphSelect.qml:54-120`) with the Basic style's placement and frame.
- The menu's popup has no `background`, so it draws the Basic style's frame.

The last three findings were read, not reproduced.

**Decision.**

- **Placement.** One rule covers every popup that opens from a control: a menu's level, a menu bar's level, a
  select's dropdown and the date picker. When the popup opens:
  1. It goes under its trigger, at the trigger's start, if it fits there inside the window.
  2. If not, its end goes at the trigger's end.
  3. `margins` is `Theme.spaceOf(1)`, so Qt then keeps it inside the window whatever the sizes.

  The position is computed in `onAboutToShow` from `mapToItem(null, …)`, never bound, because the probe shows a
  binding reads the position too early. In a right-to-left layout the two ends swap (`LayoutMirroring`).
- **The menu bar's trigger.** A menu bar records the entry that opened its popup in `openFrom: Item`, set in the
  entry's `onClicked` before `open()`, and the rule reads that item.
- **A shared surface, `MorphPopupBackground.qml`.** This is a new `MorphUi` type and the one popup frame. It is a
  `Rectangle` with:
  - the card scheme's `surface`;
  - a 1 px `border` in the card scheme's `border`;
  - `radius: Theme.radiusControl`;
  - a shadow drawn below it from `Theme.popupShadow*`, which is `elevation.popup`.

  It takes a `scheme` property, which defaults to `Theme.cardScheme`. These popups use it as their `background`: the
  menu's, the menu bar's, the date picker's, and the select's `ComboBox.popup`. A batch G component that opens a
  popup uses the same type; that includes G12's select dropdown.
- **The highlight.** An entry's highlight is a `color.hover` fill (`scheme.hover`), not the Basic style's. F-S writes
  it after F-T's F9 has landed `color.hover` and `elevation.popup`.
- **The terminal.** `MenuButtonImpl::placement` and `DropdownSelectImpl::placement` (`list_widgets.cpp:586-603`,
  `:1137-1150`) today keep the popup inside the viewport by shifting it left. They take the same rule as Qt Quick:
  start-aligned when it fits, otherwise end-aligned to the trigger, then clamped. A select is in the rule on both
  frontends, so the contract below covers it too.

**Spec.**

- `backend_contract.md`, the `MenuWidget`, `SelectWidget` (dropdown) and `DateTimeInputWidget` rows: "a popup opens
  under its control, at the control's start when it fits in the window, else ending at the control's end, and always
  inside the window".
- `qt_quick/backend.md`: `MorphPopupBackground`.

**Tests.**

- `tests/qt_quick/test_qt_quick_overlays.cpp`:
  - A button-style menu sits 8 px from the right edge of a 400 px window. It opens with its right edge at the
    trigger's right edge, within 1 px, and inside the window.
  - A menu at the left edge opens with its left edge at the trigger's left edge.
  - A menu bar's second entry opens its popup under that entry, not under the first.
  - A dropdown select at the right edge, and a date input at the left edge, open inside the window.
  - Each of these popups' `background` is a `MorphPopupBackground`.
  - **Control 1:** remove the `onAboutToShow` placement, and the first popup ends at 440.
  - **Control 2:** keep the placement but make it a binding, and the first popup ends at the window's edge less the
    margin, 4 px off the trigger's end.
  - **Control 3:** without `openFrom`, the menu bar's popup sits under the bar's start.
- `tests/tui/test_tui_lists.cpp`: on a 40-column screen, with a 12-wide popup:
  - A trigger at columns 30–35 opens the popup at column 24, so it ends at 36.
  - A trigger at columns 2–7 opens it at column 2.
  - The same holds for a dropdown select.
  - **Control:** today's shift puts the first popup at column 28.

**Re-grab.** None, because a static grab cannot open a popup. The Qt test is the evidence, and the user report in
the review is re-checked by hand.

## F4: a row can wrap

**Problem (measured by the review).**

- `Row` has no wrap (`view.hpp:718-738`), so the head row is one line that cannot shrink. The head row is built in
  `workspace.hpp:775-870` and holds the subtitle, a horizontal badge `forEach`, a spacer, the tools, the actions and
  the menu.
  - At 700 px, the instrument head loses "Book" and "⋯".
  - At 390 px, its third badge is cut.
- The mockups' own CSS wraps the tags: `display: flex; gap: 6px; flex-wrap: wrap` in `03-Instrument.dc.html`.
- The shell frame's design table rejected a single line for the head because "one line when it fits needs a flow
  container that measures its children, which ui/1 has none of" (`shell_frame.md`, Design decisions). This item is
  that container.

**Decision: `wrap` on `row` and on a horizontal `forEach`.**

- **Document.**
  - `row` and `forEach` take `wrap`, a literal boolean, default `false`.
  - The loader refuses, with the path:
    - a `wrap` that is not a literal boolean;
    - `wrap: true` on a `forEach` whose `axis` is a literal other than `horizontal`.
  - A computed `axis` is read at mount, as a computed `sizing` is. If it is not horizontal there, the mount reports
    it, and the `forEach` is laid out without wrapping.
  - A column has no `wrap` key, so the existing refusal of unknown keys covers it.
- **View tree.**
  - `Row::wrap` and `ForEach::wrap` are a `bool`, set once.
  - The mount calls `StackWidget::setWrap(true)` once, right after `createStack` and before the first child, and
    only for a wrapping stack.
  - A stack made for a row that does not wrap records nothing new.
  - A C++ owner that builds a wrapping stack itself, such as the shell frame (F5), calls `setWrap(true)` before it
    creates the stack's first child.
- **Author.** `ContainerOptions::wrap` (`std::optional<bool>`), used by `row`, and `ForEachOptions::wrap`.
- **What wrapping means.** It is the flex-wrap rule:
  1. **Basis.** Each shown child has a basis along the row:
     - a `content` child: its natural width;
     - a `fixed(n)` child: `n` units;
     - a `stretch` child: its `minWidth` when it has one, and its natural width otherwise.

     Each basis is clamped into the child's bounds, and is at most the row's width.
  2. **Lines.** Children fill lines in order. A child starts a new line when its basis, plus one gap, does not fit
     after the children already on the line.
     - A line always holds at least one child.
     - A child whose basis is 0 (a zero-width spacer) takes no gap, and never starts a line.
  3. **Spare width.** On each line, the `stretch` children share the line's spare width by weight. Each stops at its
     maximum, the way `distribute` shares a stack's spare length. A line with no `stretch` child places its spare
     width by the row's `justify`, per line.
  4. **Line height.**
     - A line is as tall as its tallest child.
     - The row's `align` places each child down its line.
     - Lines are `gap` apart, the same step as between children. One `gap` serves both axes, as CSS's `gap` does.
  5. **Overwide child.** A child wider than the row sits alone on its line, at the row's width, so a text wraps
     inside it.

  The row's natural width is its children's width on one line. Its natural height is the height of its lines at the
  width it is given.
- **Qt Quick.**
  - With `wrap` set, `MorphStack` lays its children out in a plain `Item` with a `place()` function, as `MorphGrid`
    does for tracks (`MorphGrid.qml:56-…`).
  - `place()` runs the rule above from each child's natural width (the grid's `naturalWidth`), and sets each child's
    `x`, `y` and `width`. It then reads each child's `implicitHeight` at that width to get the line height, and sets
    its own `implicitHeight`.
  - The Qt `StackImpl` (`src/qt_quick/containers.cpp`) calls `place()` again whenever any of these changes: the
    stack's width, a child's implicit size, a child's visibility, or the gap. It connects this in `adopted()`, as the
    grid's `place` is connected (`containers.cpp:52-67`).
- **Terminal.**
  - `layout::planWrap(std::span<Item const> items, Track width, int rowGap)` returns each item's line, x and width,
    by the rule.
  - `StackImpl` with wrap uses it in `childAreas`, and in `naturalSize` to get rows from the lines' natural heights.
  - A gap of step 1 is one column between children and no row between lines (`layout::stepCells`), as a vertical
    stack's is today.
- **RecordingBackend** records `wrap=true`.

**Spec.**

- `document.md`, the `column, row` row and the `forEach` row: `wrap`.
- `view_tree.md`: `Row::wrap`, `ForEach::wrap`.
- `backend_contract.md`, Containers: the `StackWidget` row gains "a wrapping row (`setWrap(true)`, before its first
  child) fills lines in order …", with the five points. The interfaces table gains `setWrap`.
- `author.md`: the options.

**Tests.**

- **Layout case: "a wrapping row starts a line only when out of room".**
  - Setup: a wrapping row with gap 1 and three `fixed(12)` texts.
  - At a width of 3 × 12 plus two step-1 distances, all three share a top.
  - At a width of 30 units, the third text's x is the row's x, and its top is below the first text's bottom. The
    row's height is that of the two lines.
  - The widths are stated in units, plus the backend's own step-1 distance (`Theme.spaceOf(1)` px on Qt Quick, one
    column on the terminal), as the other cases do.
  - **Control:** a backend that ignores `setWrap` puts the third text past the row's right edge at width 30.
- **Layout case: "a stretch child of a wrapping row takes its line's spare width".**
  - Setup: a `stretch` text of 30 characters with `minWidth: 10`, and a `fixed(12)` text.
  - At width 40, the stretch child is 40 − 12 − the backend's step-1 distance wide, on the same line as the fixed
    child.
  - At width 20, the stretch child is 20 wide on line 1, and the fixed child is on line 2.
  - **Control:** take the basis from the natural width (30) instead of `minWidth`. At width 40 that puts the fixed
    child on line 2 (30 + gap + 12 > 40), and the first check fails.
- `tests/tui/test_tui_layout.cpp` runs `planWrap` on the same inputs, and also checks:
  - a child wider than the track sits alone on its line, at the track's width;
  - a zero-basis child takes no gap.

  **Control:** as above.
- `tests/test_ui_document.cpp`: the two refusals, and `wrap: false` is accepted.
- `tests/test_ui_mount.cpp`:
  - a wrapping row records `wrap=true` before its first child;
  - a plain row records nothing;
  - a computed non-horizontal `axis` with `wrap` is reported.

**Gallery.** Every hand-rolled tag row takes `wrap: true`: the `layout::tag` rows in instrument, samples and receipt,
and console's compact cards. F5 covers the head's use.

## F5: the head shares the top line, and wraps only when out of room

**Problem (measured by the review).**

- Today's top bar has two lines (`shell_frame.hpp:96-140`):
  - the first holds the navigation menu, the title, the principal and "Sign out";
  - the second, shown whenever the head has content, holds the head row in a scroll that moves sideways.
- So every screen with a head loses a line, 30–50 px, even at 1000 px with room to spare (samples, plate).
- On compact, the title wraps, because ☰ and "Sign out" take about 100 px of the line (I6).
- The mockup's head is one band:
  - the breadcrumb, the title, and the tags wrapping under them on the left;
  - the actions at the top right;
  - on a phone, the actions drop to a row of their own.

**Decision.**

- **The head is two parts.** `Workspace` builds a screen's head from the same bindings `buildHeadRow` uses today:
  - The **lead** holds the subtitle, muted, then the badges, in one row with `wrap: true` and gap 1. The badge
    `forEach` inside it is itself `wrap: true` (F4), so the badges wrap under one another whatever the lead's width.
  - The **trail** holds the tools, the actions and the menu, in a row with gap 1.

  `headPartsForActive()` returns both parts as `Node`s, replacing `headRowForActive()`. With no tab open, each is a
  defined, invisible row. `activeHeadShown()` is unchanged.
- **In a page** (`HeadPlacement::InPage`), the head row is a wrapping row with gap 2, `align: start` and
  `justify: end`.
  - It holds the lead, sized `stretch` with `minWidth: kHeadLeadMinimum` (32 units), then the trail.
  - Where the trail fits beside 32 units of lead, the lead takes the rest of the width and its badges wrap.
  - Where it does not fit, the trail drops to its own line, at the trailing edge (`justify: end` on that line).
  - The spacer goes.
- **In the shell frame**, `_topLine` stays a non-wrapping row that holds its two zero-width pad spacers (`_topStart`,
  `_topEnd`). Between them is a new wrapping row, `shell_topbar_main`, sized `stretch`, with gap 3, `align: start` and
  `justify: end`.
  - The frame creates `shell_topbar_main` through a small helper in its member-initialiser list. The helper creates
    the stack and calls `setWrap(true)` before returning, so the stack's children, created after it, are
    children of a wrapping stack from the start (F4's ordering rule).
  - Because the pad spacers sit outside the wrapping row, every line of the wrapping row is padded at both ends.
  - The wrapping row holds, in order:
    1. the navigation menu (compact only);
    2. a **lead block**, a column sized `stretch` with `minWidth: kHeadLeadMinimum`, holding the title
       (`shell_title`, a heading) and the lead slot (`shell_head_lead`);
    3. the trail slot (`shell_head`).
  - The client mounts the two head parts into the two slots.
  - The second line goes, and its scroll with it (`shell_head_line`, `shell_head_scroll`).
  - `setHeadShown(shown)` now shows or hides the two slots.
- **The principal and "Sign out" leave the top line** (Decisions 1).
  - **Expanded:** they sit at the bottom of the sidebar, under the navigator, which takes the sidebar's spare height.
    The principal (`shell_principal`) is muted; below it, "Sign out" (`shell_signOut`) is a ghost button. The
    sidebar's scheme gives both readable colours ([Built-in schemes](#built-in-schemes)).
  - **Compact:** the navigation menu ends with a separator and a "Sign out" entry, test id `shell_nav_signOut`,
    whose description is the principal. Choosing it calls `onSignOut`. The frame appends the entry to
    `Navigator::menuEntries()`, routes the last index to sign-out, and routes every other path to
    `Navigator::choose`.
  - **Without `onSignOut`:** neither is shown, as now.

  Moving them is what lets the head share the line. Left on it, two owners' trailing widgets would compete for one
  edge: the screen's actions and the client's sign-out. Wrapping would then move one of them away from the other
  unpredictably.

What the instrument screen then looks like:

- **1000×830:** one line. On the left, the title and the lead (subtitle, three badges, wrapping). On the right,
  "Log maintenance", "Book", "⋯".
- **390 px:** the title has the line to itself. The badges wrap under it, and the actions take the next line, as the
  mockup draws them.

**Spec.**

- `shell_frame.md`:
  - Shape: the top-bar rows of the table, the sidebar's foot, `shell_head_lead`, `shell_nav_signOut`.
  - The compact form: the sign-out entry.
  - Design decisions: the row for a head wider than its share of the bar is replaced by "One wrapping line: the title
    and the lead stretch from 32 units, the trail wraps under them only when it does not fit; the principal and
    sign-out sit in the sidebar or the navigation menu, so the line has one owner's controls at its end".
- `workspace.md`, The head row: the lead and the trail, and `headPartsForActive()`.
- `client/client.md`, The shell: where the parts mount.

**Tests.** First, the existing tests that read today's top bar, and what each becomes:

| Test | Reads today | Becomes |
|---|---|---|
| `tests/test_ui_shell_frame.cpp:51-61` | `shell_principal`, the `shell_head` row, a `shell_signOut` click | the principal and sign-out in the sidebar; `shell_head` is the trail slot; the click unchanged (expanded) |
| `tests/test_ui_shell_frame.cpp:124-160` | `shell_head_line` and `shell_head_scroll` shown and hidden, sign-out beside the head | `setHeadShown` shows and hides `shell_head_lead` and `shell_head`; the line and the scroll no longer exist (the test asserts that) |
| `tests/client/test_shell.cpp:211` | `shell_head_line` shown | `shell_head` shown |
| `tests/client/test_sign_in.cpp:323` | `shell_head` as a `Row` | `shell_head` as a `Row`, now the trail slot (unchanged find) |
| `tests/client/test_sources.cpp:549-635` | `shell_principal` text, `shell_signOut` click | unchanged ids, now in the sidebar; an added compact case clicks `shell_nav_signOut` |
| `tests/qt_quick/test_qt_quick_containers.cpp:648-649` | principal and sign-out level on one line | principal above sign-out in the sidebar's foot |
| `tests/qt_quick/test_qt_quick_containers.cpp:860-890` | head actions against `shell_signOut` at 1000 and 400 px, `shell_head_scroll` | the one-line and the wrapped checks below |
| `examples/bank/screens/tests/test_bank_screens.cpp:138` | clicks `shell_signOut` | unchanged: the harness window is expanded; a compact run would choose `shell_nav_signOut` |
| `tests/tui/test_tui_shell_frame.cpp` | the two-line bar | the wrapping line at 80 and 40 columns |

The new checks:

- `tests/test_ui_shell_frame.cpp` (RecordingBackend):
  - the wrapping row records `wrap=true` before its first child, and holds the lead block and the trail slot;
  - `shell_head_line` does not exist;
  - the sidebar holds `shell_principal` and `shell_signOut`;
  - on compact, the navigation menu's last entry is "Sign out"; choosing it calls `onSignOut` once and opens no
    screen.
  - **Control:** route the last index to `Navigator::choose`, and sign-out is not called.
- `tests/test_ui_workspace.cpp`:
  - the head row in a page records `wrap=true`;
  - its lead records `stretch(1) min=32`;
  - the badge `forEach` records `wrap=true`;
  - `headPartsForActive()` gives the same parts.
  - **Control:** drop `minWidth`, and the record changes.
- `tests/qt_quick/test_qt_quick_containers.cpp`, with a title, two badges and two actions:
  - at 1000×600, the actions' top equals the title's top (one line);
  - at 390×600, the actions' top is below the badges' bottom.
  - **Control:** the old two-line frame puts the actions below the title at 1000.
- `tests/tui/test_tui_shell_frame.cpp`: at 80 columns, the bar is one line; at 40, two.

**Re-grab.** Every screen at 1000×830, 800×700, 700×700, 600×800 and 390×844. No head is cut and no action
disappears. At 1000, the head shares the title's line wherever the mockup draws it so: samples, plate, audit and
receipt.

## F6: a splitter that fills the page, with a surface and a pinned bar per pane

**Problem (read; the review made the measurement).**

Review's expanded layout is `pinned({splitter(...)})` (`review.cpp:737`). The splitter is the scroll's content, so it
is only as tall as its content, and the action row follows the content at y ≈ 495. Console puts the splitter last in
a column that is only as tall as its content (`console.cpp:755-767`).

The vocabulary can already say what the mockup draws:

- a screen root that stretches in height fills the page (`workspace.md`, Shape);
- a `splitter` sized `{height: stretch}` takes the root column's spare height;
- a splitter gives its panes its whole height across: `SplitView` resizes its children across, and the terminal's
  `SplitterImpl` gives each pane the full extent across (read);
- every node takes a `surface` (`Common::surface`), and `MorphWidget` draws a surface's background
  (`MorphWidget.qml:300`). A theme surface draws square, so a flush white pane is a theme surface, as the gallery's
  `selected` bar is;
- `pinned` is a column like any other, so it can be a pane.

Three things stopped the screens: the outer `pinned`, F2's centring of console's panels in the panes, and F1 for
review's compact case.

The review found one real gap. A screen root that stretches is never scrolled. A scrollable page slot scrolls only a
child that does not stretch (`SlotImpl::scrolls`, `container_widgets.cpp:124`; `MorphSlot.qml`, `stretchesDown`).
Console's stretching root holds the alert, the KPIs, a row of chart panels, and then the splitter. On a window
shorter than those, the splitter gets nothing, and the rest is cut at the window's edge.

**Decision.**

- **No new vocabulary.** A pane whose content is a `pinned` keeps its bar at the pane's bottom.
- **A stretching child of a scrollable slot is at least its `minHeight` tall.** While the slot is shorter than that,
  the slot scrolls the child at its `minHeight`, as it scrolls a child that is only as tall as its content. A child
  without a `minHeight` fills the slot as it does today. This floor is the bound the child declares; the child's
  natural height is not used. With natural height, every stretching table and every `pinned` page would scroll the
  page instead of filling it, and a stretching table is sized not to.
- **Where the code changes.**
  - `SlotImpl::scrolls` becomes "scrollable, and no shown child stretches down, unless that child's `minHeight` is
    more than the slot's height". This is a named hunk in F-L's `container_widgets.cpp`.
  - On Qt Quick, `MorphSlot` does the same with the child's `minHeightUnits`.
- **The screens use the bound.** Console's expanded root column takes `minHeight` equal to its KPIs and charts plus
  24 units of splitter. Review's root does not need one.

**Spec.**

- `document.md`, "A bar that stays in view", one sentence: "a splitter pane may be pinned: a stretching splitter
  gives each pane its height, and a pinned pane keeps its bars at the pane's ends".
- `backend_contract.md`, the `SlotWidget` row: "a stretching child fills a scrollable slot, but is never shorter than
  its `minHeight`; while the slot is shorter, the slot scrolls it".
- `workspace.md`, Shape: the same rule for a page.

**Tests.**

- **Layout case: "a stretching splitter's panes fill it, and a pinned pane keeps its bar at its bottom".**
  - Setup: a column of fixed height 20 holding a splitter sized `stretch × stretch` with two panes. The first pane
    is a column with a text. The second is a `pinned` of 40 texts, with a text `bar` at the bottom.
  - Check: each pane is 20 tall, and `bar`'s bottom is at 20.
  - **Control:** size the splitter `content` in height. The second pane is then as tall as its content (on Qt Quick
    20 lines, capped; on the terminal 41), with `bar` below the column.
- **Layout case: "a stretching child of a scrollable slot below its minimum height scrolls".**
  - Setup: a scrollable slot of height 20 holding a column sized `stretch` with `minHeight: 30`. The column holds
    a first and a last button.
  - Check: the column is 30 tall. The last button lies outside the slot until it is focused, and inside it after.
  - With `minHeight: 10`, the column is exactly 20 tall, as L15's stretching case requires.
  - **Control:** today's `scrolls()`. The 30-unit column is clipped, and the focused last button stays outside.
- **Panels in panes.** No separate case: F2's case already covers a panel stretched taller than its content, and a
  pane is one way to stretch it.

**Gallery.**

- **Review, expanded.**
  - The root is a column, `stretch × stretch`, holding the splitter (`stretch × stretch`) and the dialog.
  - The queue pane is a column with the gallery's new `pane` surface: white and square, `color.surface` `#FFFFFF`.
    This is a named hunk in F-T's theme index.
  - The worksheet pane is `pinned({…}, {.bottom = actions})`, with the action row moved out of the content.
- **Console, expanded.** The root column stretches in height with the `minHeight` above. The splitter takes
  `stretch` height, and its panes keep their panels.

**Re-grab.**

- review and console at 1000×830 and 1400×900: the panes reach the page's bottom, review's actions sit at the
  worksheet pane's bottom, and console's worklist tab bar stays at its pane's top.
- console at 1000×700: the page scrolls, and nothing is cut at the window's edge.

## F7: gaps inside containers, dividers, and table cell padding

**Problem (measured by the review).**

- **Dialogs.** A dialog's content is one column with no gap (`holding`, `:8253`). On the e-signature screen the
  summary ends at y = 337 and the banner starts at 338.
- **Panels.** A panel's content is `lowerAll`'s column, with gap 0. On receipt, the note input touches the buttons
  below it (497 / 498).
- **Workaround.** Screens wrap their content in a `column` with a gap (`esignature.cpp:236`). The same repair recurs
  in many screens, which AGENTS.md calls a framework seam.
- **Lists.** Lists have no rule between rows: instrument's events, audit's compact entries, review's queue, plate's
  runs.
- **Table cells.** A table cell has horizontal padding (`MorphTableRow.qml:60`, `space.2`) but no vertical padding,
  so a two-line cell touches the row rules (audit, A3).

**Decision.**

- **`gap` on `panel`, `dialog`, `drawer` and `collapsible`.** This is the spacing step between the node's children,
  as on a `column` (Decisions 3).
  - Defaults: 2 for `panel` and `collapsible`, and 3 for `dialog` and `drawer`. These are the mockups' 8 and 12 px at
    the default scale.
  - The interpreter passes the gap to `listColumn` (F1), so a single child is unaffected. The view tree is
    unchanged, because the column carries the gap.
  - The gap is read once, untracked, as `gap` is everywhere.
  - **This is a document default, not a view-tree one.** A `ui::Panel` built in C++ holds a single `child`, and its
    author already picks that child's column gap. For example, the forms engine builds its groups as
    `ui::Panel{.child = ui::column(…)}` (`form_view.hpp:351`, `:375`, `:563`, `:571`). Giving `Panel` a gap would add
    a second way to say the same thing to every C++ author, so document panels and code panels may differ. The forms
    engine keeps its own gaps; no forms test changes.
- **`divided` on `column` and on a vertical `forEach`.** This is a literal boolean, default `false`.
  - Between each two shown children, a 1 px rule in `color.divider` (F11) is centred in the gap. The rule takes 1 px
    of its own, so `gap: 0` still separates the children.
  - Refused on a horizontal `forEach` and on a `row`.
  - The mount calls `StackWidget::setDivided(true)` once, before the first child, and only when `divided` is set.
  - Qt Quick: `MorphStack` draws the rules in an overlay at the middle of each gap, from the laid-out children.
  - **The terminal draws no rule.** On the terminal a rule would take a row of its own, which would double a list's
    height, and a list's lines are already apart.
  - RecordingBackend records `divided=true`.
- **Table cell padding (F-T).** Two dimension tokens: `table.cellPaddingX` (default 8 px, which is today's
  `space.2`) and `table.cellPaddingY` (default 4 px).
  - Each cell is inset by them.
  - A row is at least as tall as its tallest cell plus twice the vertical padding, and at least
    `Theme.targetHeight − 8`, as now.
  - The header labels take the same padding.
  - The terminal keeps its one-column gap between cells and no vertical padding.

**Other users.** These are the users of document panels, dialogs, drawers and collapsibles with several children, and
each lane runs its GUI tests:

- `examples/bank/screens/bank_screens.cpp`;
- `examples/bookmarks/screens/bookmarks_screens.cpp`;
- `examples/lims/screens/lims_screens.cpp`;
- `examples/gallery/src/gallery.cpp`;
- the generic client's own dialogs in `src/client/shell.cpp`;
- the lims gallery.

Their dialogs gain 12 px between fields, and their multi-node panels 8 px. A geometry assertion that pinned the old
spacing is updated in the same commit, named in its message.

**Spec.**

- `document.md`: the four kinds' `gap` with its default, and `column`'s and `forEach`'s `divided`.
- `backend_contract.md`, Containers: `setDivided`, and "the terminal draws no rule".
- `theme.md`: the two padding tokens.

**Tests.** There is no layout conformance case for the gap. A conformance case mounts view-tree nodes, so "a panel's
children are its gap apart" would mount a `Column` with gap 2, which existing cases already cover. Its control would
mutate the test, not the feature. The gap is the interpreter's, and its tests are there:

- `tests/test_ui_interpreter_detail.cpp`:
  - A `panel` of two nodes records a column `gap=2`.
  - A dialog records `gap=3`.
  - `gap: 0` records none.
  - A `column` with `divided: true` records `divided=true`.
  - **Control:** drop the default, and the record changes.
- `tests/qt_quick/test_qt_quick_containers.cpp`:
  - A divided column of three texts with gap 2 has two rules. Each is 1 px tall, centred on the middle of its gap,
    and coloured `Theme.divider`.
  - **Control:** make `setDivided` a no-op, and no item named `divider` exists.
- `tests/qt_quick/test_qt_quick_table.cpp`:
  - With `table.cellPaddingY` at 6, a cell's top is the row's top plus 6.
  - **Control:** ignore the token, and the offset is 0.
- `tests/test_ui_document.cpp`: refuses `divided` where it is not allowed, and a non-integer `gap` on the four kinds.

**Gallery.**

- E-signature, receipt and results drop their wrapping columns.
- `divided: true` with `gap: 0`: instrument's events and qualification, audit's compact entries, review's queue, and
  plate's compact runs.
- The gallery's theme sets `table.cellPaddingY` to 6.

**Re-grab.**

| Screen | Size | Expect |
|---|---|---|
| esignature | both sizes | 12 px between the summary, the banner and the fields |
| receipt | 1000×830 | the note input and the buttons apart |
| audit | 1000×830 | cell text clear of the row rules |
| instrument | 1000×830 | rules between the events |

## F8: a token's parts load from a bundle, and an unread token is reported (#1070)

**Problem (measured by the review; cause read).** `readGroup` (`theme.hpp:539-571`) reads any object with `$value`
as a token through `readToken` (`:495-524`), which never looks at its other members. The gallery writes the tone
parts as the spec names them:

```json
"ok": { "$value": "#17602F", "bg": { "$value": "#E2F4E8" }, "border": { "$value": "#BCDFC9" }, "dot": { … } }
```

and `color.ok.bg`, `.border` and `.dot` never load. The review measured the warn badge at `#FFF4E5` and the error
banner at `#FDECEA`, the `Theme.qml` defaults. The same reader skips every `$`-prefixed member of a group (`:555`), so
DTCG 2025.10's own answer, a group's `$root` token, is dropped too. Two closed-set names were chosen to dodge it:
`control.touchHeight` and `font.mono` are siblings "since a token's object is never read for tokens inside it"
(`theme.md`, the closed set; `kThemeTokens`'s comments).

DTCG 2025.10, §6.1: "If an object contains both `$value` and child tokens/groups, this creates an invalid structure"
and "Tools MUST report this as an error"; §6.2: "Groups support root tokens using the reserved name `$root`", and an
alias names it `{color.accent.$root}`.

**Decision.**

- **A token with a member is refused.** `readToken` refuses an object with `$value` and any member not starting with
  `$`: "the token `color.ok` holds `bg`: a token holds no tokens; write `color.ok` as a group whose `$root` is the
  token". The bundle is then refused at `addFile`, as any malformed token file is.
- **`$root` is read.** In a group, a member named `$root` is a token at the **group's own path**: `color.ok.$root` is
  `color.ok`. So `{"ok": {"$root": {"$value": …}, "bg": {"$value": …}}}` gives `color.ok` and `color.ok.bg`, and a
  file that writes `"ok": {"$value": …}` gives the same `color.ok`. An alias may name either spelling:
  `{color.ok.$root}` is read as `{color.ok}`. A group whose path is a token in the same file twice (a `$root` and a
  sibling token of the same name, which JSON nesting cannot write) cannot occur; across layers the later file wins,
  as today.
- **Unknown types are kept, not dropped**: a token whose `$type` ui-theme/1 does not read is parsed into the set with
  its type, so the report below names it. `themeValues` already ignores a path whose type is not the set's.
- **A token nothing reads is reported.** `unreadThemeTokens(ThemeIndex const&, TokenFileLookup const&)` returns the
  paths, over every file the index names, that are neither a `kThemeTokens` path nor reachable from one through
  aliases (a plain alias and an alias inside a composite value, followed transitively, over the layered union). A
  palette token that a set token aliases is read; `color.sucess` is not. Each is reported once, with its file:
  - by `UiRegistry::build` at warn: `[ui-serve] theme <index>: <file>: the token <path> is not a ui-theme/1 token and
    no token of the set refers to it, so nothing draws it` (or `… is of type <t>, which ui-theme/1 does not read`);
  - by `applyBundleTheme` in its problems, which the Qt Quick frontend logs (`frontend.cpp:194`).
  It is a report, not a refusal: a design tool's export carries tokens for other platforms, and the application must
  still start.

**Spec.** `theme.md`, Token files: `$root`, the refusal, unknown types kept; a new section "What the client reads",
the report; the closed-set rows of `control.touchHeight` and `font.mono` lose their "never read" clause, which no
longer holds (their paths stay). `kThemeTokens`'s two comments say the same, in the present tense.

**Tests.**

- `tests/test_ui_theme.cpp`:
  - `{"color": {"$type": "color", "ok": {"$root": {"$value": "#17602F"}, "bg": {"$value": "#E2F4E8"}}}}` parses to
    `color.ok` and `color.ok.bg`; `{color.ok.$root}` and `{color.ok}` both resolve to `#17602F`.
  - `"ok": {"$value": "#17602F", "bg": {…}}` is refused, naming `color.ok` and `bg`. **Control:** today's reader
    accepts it with `color.ok.bg` missing; the test asserts the refusal, so it fails on today's code.
  - A token of `$type: "gradient"` and a `color.sucess` beside a `color.accent` aliasing `{palette.green}`:
    `unreadThemeTokens` names `color.sucess` and the gradient, not `palette.green`. **Control:** make the walk ignore
    aliases, and `palette.green` is named.
- `tests/test_ui_service.cpp`: a bundle with an unread token builds, and the log holds the warning once.
- `tests/qt_quick/test_qt_quick_theme.cpp`: a bundle theme written with `$root` and parts sets `Theme.warnBg` to the
  bundle's value. **Control:** skip `$root` in `readGroup`, and `warnBg` keeps its default.

**Gallery.** The base layer writes each tone as a group with `$root` and its parts; the theme then loads
`#FFF1D6` for the warn fill and `#FDE7E7` for the err fill.

**Re-grab.** esignature at 1000×830: the error banner's fill reads `#FDE7E7`; receipt at 1000×830: the warn
banner's fill reads `#FFF1D6`, the bundle's `color.warn.bg` (the mockup's banner uses a paler `#FFF6E5` than its tag;
that difference is G4's); instrument: the warn badge `#FFF1D6`.

## F9: a hover fill and a track separate from the neutral tone

**Problem (measured by the review).** `color.neutral` means two things.

- **As a fill.** The spec reads it as a fill: a progress track ("its fill is `color.accent` on a `color.neutral`
  track", `theme.md`). The QML paints it in four places:
  - under a hovered item and under a ghost button (`MorphButtonBackground.qml:35-36`);
  - as the segmented select's track (`MorphSelect.qml:198`);
  - as the progress track (`MorphProgress.qml:31`).
- **As a tone colour.** `color.ok`, `color.warn`, `color.err` and `color.info` are each their tone's text colour. By
  that analogy the gallery set `color.neutral` to the neutral tag's text, `#363C46` (`lims_gallery.cpp:42`).

The measured result:

| Place | Contrast on `#363C46` |
|---|---|
| A hovered queue entry, its text | 1.50:1 |
| A hovered queue entry, its muted text | 1.46:1 |
| "All 7" in the segmented control | 1.46:1 |
| The progress track | dark |

The neutral tone itself has no parts. Today it *is* `scheme.muted`: `toneOf`, `toneBorderOf` and `toneDotOf` return
muted, and `toneBgOf` returns muted at 15 % (`MorphWidget.qml:80-140`). So the mockup's neutral tag (`#EEF0F3` fill,
`#363C46` text) cannot be written at all. The trap is in the name, not in the gallery.

**Decision (Decisions 2).**

- **`color.neutral` is the neutral tone's colour**, as the other four tones' are. It gains `color.neutral.bg`,
  `.border` and `.dot`. `toneOf`, `toneBgOf`, `toneBorderOf` and `toneDotOf` read them for `Neutral`.
  - The theme-level defaults equal today's default look: `#6e6e73` (today's default `muted`), `#6e6e7326` (muted at
    15 %), `#6e6e73` and `#6e6e73`.
  - **What changes:** today the neutral tone follows each scheme's `muted`. After F9 it follows `color.neutral*`
    instead. A theme or surface that sets `muted` but not `neutral` no longer moves its neutral badges with it. This
    is the stated cost of "no token falls back to another" (The shape of the batch).
  - **What it moves in-tree:**
    - The gallery sets `neutral` itself.
    - The built-in dark sidebar has its own muted, `#7d8396`. It keeps it: the sidebar binds its `neutral*` roles to
      its own muted ([Built-in schemes](#built-in-schemes)), so its look does not change.
    - No other in-tree scheme sets `muted`.
- **`color.hover`** is the fill under a hovered or pressed list item, ghost button, segmented option and menu entry.
  - The default is `#1d1d1f14`, the default text colour at 8 %.
  - It is translucent, so it darkens whatever light surface it lies on by the same step, and keeps the text on that
    surface readable: 8 % over white is `#ededed`, where text measures 14.4:1 and muted 4.33:1.
  - A dark scheme sets its own (the sidebar's is `#ffffff14`).
- **`color.track`** is the unfilled part of a progress bar and the segmented select's track. The default is
  `#e5e5ea`, today's `color.neutral` default, so the track looks the same.
- **A theme whose text cannot be read on these fills is reported (Decisions 4).**
  `themeContrastProblems(ThemeValues const&)` checks two colours on four fills:

  | Text | Minimum |
  |---|---|
  | `color.text` | 4.5:1 |
  | `color.muted` | 3:1 |

  The fills are `color.hover`, `color.selection`, `color.track` and `color.controlSurface`.
  - A translucent fill is composited over `color.surface` first.
  - The check runs for the base values and for each theme surface's values.
  - It is reported through the same two channels as F8's unread tokens, for example `color.hover: color.text is
    1.50:1 on it, below 4.5:1`.
  - Built-in schemes are not `ThemeValues`, so the Qt test in [Built-in schemes](#built-in-schemes) checks them.
  - Muted gets 3:1 rather than 4.5:1 because the framework's own defaults hold 3:1 but not 4.5:1. Muted on
    `#e5e5ea` is 4.04:1, and on `#cfe3ff` it is 3.88:1. The check exists to catch a fill that makes text
    disappear, as 1.46:1 does, not to restyle the defaults.
- **`elevation.popup`** joins the set here (F3 uses it). Its default is `#00000024`, offsets 0 and 8, blur 24,
  spread 0.

**Spec.**

- `theme.md`:
  - The closed set gains these rows: neutral with its parts, hover, track, and `elevation.popup`.
  - The paragraph "A progress bar reads two of them" now names `color.track`.
  - The Qt Quick mapping table gains `neutral`, `neutralBg`, `neutralBorder`, `neutralDot`, `hover`, `track` and
    `popupShadow*`.
  - "What the client reads" gains the contrast check.
- `backend_contract.md`, Button emphasis: "a faint tint on hover" names `color.hover`.

**Tests.**

- `tests/test_ui_theme_values.cpp`:
  - Checks the defaults of the new paths.
  - `themeContrastProblems` on the gallery's old `neutral` used as `color.hover` names `color.text` at 1.50:1.
  - A `color.controlSurface` of `#ffffff` with a text colour of `#c9ccd6` is reported at 1.6:1.
  - The defaults give no problem.
  - **Control:** drop the fill's alpha before comparing. The translucent default `#1d1d1f14` is then read as opaque
    near-black, and text on it is reported.
- `tests/qt_quick/test_qt_quick_theme.cpp`:
  - Applying `color.neutral` `#363C46` sets `Theme.neutral`, and leaves `Theme.hover` and `Theme.track` at their
    defaults.
  - A hovered item's background (`MorphButtonBackground`'s `color`) is `Theme.hover`.
  - A progress track is `Theme.track`.
  - **Control:** keep `scheme.neutral` in `MorphButtonBackground`, and the item reads `#363C46`.
- The existing test "a progress bar fills with the accent on the neutral track"
  (`test_qt_quick_theme.cpp:625-630`) sets `color.neutral` as a track. It moves to `color.track`, and its name and
  mutation note follow.

**Gallery.**

| Token | Value | Source |
|---|---|---|
| `color.neutral` | `#363C46` (unchanged) | |
| `color.neutral.bg` | `#EEF0F3` | |
| `color.neutral.border` | `#C3C9D2` | |
| `color.neutral.dot` | `#7A828F` | |
| `color.hover` | `#F0F3F7` | the mockups' `.mi.hl` |
| `color.track` | `#E7EAEE` | the mockups' `.seg` |
| `elevation.popup` | `0 8px 24px rgba(20,24,31,.14)` | the mockup |

The review's screen fix ("the gallery theme's `neutral` is a fill") is no longer needed: the gallery's reading of the
name becomes the right one.

**Re-grab.**

| Screen | Size | Expect |
|---|---|---|
| review | 1000×830 and 390×844 | the "Show" track `#E7EAEE`, and "All 7" readable |
| instrument | 1000×830 | the booking track `#E7EAEE` |

Hover cannot be grabbed, so the Qt test stands in for it.

## F10: cards and controls are white on the page

**Problem (measured by the review).**

- **Panels.** A panel fills with the surface of the scheme it inherits (`MorphPanel.qml:29`). A screen sits on the
  `page` scheme, whose surface is `color.background` (`Theme.qml`, `pageScheme`). So every panel, and every input
  inside it, is `#F6F7F9`, the page's own colour, and shows only by its 1 px border. Seen on stability, samples,
  plate and receipt.
- **Controls.** Inputs and normal buttons fill with their scheme's surface (`MorphInputBackground.qml:31`;
  `MorphButtonBackground.qml`, `fill: scheme.surface`). On a tinted bar (the `selected` surface, `#EAF5EE`) they take
  the tint.
- **Workaround.** Review's screen adds `.surface("card")` to each panel (`review.cpp:537`).

**Decision (Decisions 3).**

### A panel draws on the `card` surface unless it names a surface (F-S)

1. **Where the default lives.** The panel's Qt widget owns it: `PanelImpl` in `src/qt_quick/`, which is a
   `ContainerCore`. Neither the interpreter nor the mount sets `surface: "card"`, so the document, the view tree and
   the RecordingBackend record stay unchanged. Changing those would also change what a terminal or a host component
   reads as the panel's surface.
2. **How the content inherits it.** While the panel's surface is empty, `PanelImpl::scheme()` returns
   `Theme.cardScheme`. `ContainerCore::adopt` and `passSchemeDown` (`widget.cpp:310-345`) then offer the card scheme
   to the panel's children. With any surface set, the panel takes that surface, as today.
3. **Drawing without a second frame.**
   - `MorphWidget` draws a surface's background and the card's shadow only when `surface` is set
     (`MorphWidget.qml:286-305`). A panel whose surface is empty therefore draws its frame alone: fill from
     `Theme.cardScheme.surface`, plus border, `radius.card`, and a shadow `Rectangle` under it from
     `Theme.cardShadow*`, with the same geometry as `MorphWidget`'s.
   - A panel with `surface: card`, or another surface that `Theme.draws`, leaves its own frame unfilled and drawn only
     as a border, so the frame is not drawn twice. That double frame exists today.
4. **An empty or unset surface means the default.** A `surface` that is bound and evaluates to `""` (as at
   `samples.cpp:601`) means the default, which is `card`.
5. **A panel inside a card is flat.** A panel whose inherited scheme is already the card scheme (a panel in a panel,
   a dialog or a drawer) draws its border and fill but no shadow. A card on a card has no elevation.
6. **A flat panel** says `surface: page`.

A collapsible is not a card. Batch G's framed collapsible (G5) decides its own surface.

### Controls fill with `color.controlSurface` (F-T)

- The token fills a text input, a text area, a select's field, a date input, a check box's box, and a normal or
  danger-outline button.
- Its default is `#ffffff`, the same as `color.surface`'s default.
- It is a scheme role, so a theme surface can set it, and each built-in scheme has its own value
  ([Built-in schemes](#built-in-schemes)).
- A surface that sets only `color.surface`, such as the gallery's `selected` bar, leaves its controls on
  `controlSurface`, which is white.
- A read-only input keeps its own fill.

### Other users

Every document panel turns into a card:

- `examples/bank`, `bookmarks`, `lims` and `gallery`;
- the client's own panels;
- every forms-engine group: `ui::Panel`, `form_view.hpp:351` and `:563`, are mounted by the same `PanelImpl`.

Forms groups become cards on purpose, as dialogs already are. A nested group (an object inside an object, or an
array row inside a group) is a panel in a card, so it is flat by rule 5 and draws no card-in-card shadow. Each lane
runs those examples' GUI tests and the forms tests (`test_qt_quick_*`, `test_forms_*`). It updates in the same commit
any colour assertion that read a panel's old fill.

### The plate's wells

Wells are push buttons on the `standard`, `control`, `blank`, `sample` and `empty` surfaces (`plate.cpp:345-370`).
Their token files set `color.surface` and `color.border` (`lims_gallery.cpp:56-76`). Under the change, a button fills
with `controlSurface` and is outlined in `controlBorder` (F11), so every well would turn white with a grey outline.
To keep the wells' colours, the gallery's well surface files also set `color.controlSurface` and
`color.controlBorder` to their fill and outline. The plate is re-grabbed.

**Spec.**

- `theme.md`, Surfaces:
  - "a panel draws on `card` unless it names a surface; an empty surface is the default; a panel in a card draws no
    shadow";
  - the closed set and the mapping table gain `color.controlSurface`.
- `backend_contract.md`, Button emphasis: "a normal button (the control surface's fill …)".

**Tests.**

- `tests/qt_quick/test_qt_quick_containers.cpp`:
  - A panel on the page scheme fills with `Theme.surface`, and a text input inside it with
    `Theme.controlSurface`.
  - A panel with `surface: page` fills with `Theme.background`.
  - A panel inside a panel draws no shadow item.
  - A panel with `surface: card` has exactly one filled frame.
  - **Control:** fill with the inherited scheme, and the first check reads `background`.
- `tests/qt_quick/test_qt_quick_theme.cpp`:
  - A button inside a row on the `selected` surface fills with `controlSurface`, not with the surface's tint.
  - **Control:** keep `fill: scheme.surface`.

**Gallery.**

- Review drops `.surface("card")` from its panels.
- The theme sets `color.controlSurface` `#FFFFFF`.
- The five well surfaces set their control fill and outline.

**Re-grab.**

| Screen | Sizes | What the grab must show |
|---|---|---|
| samples, stability, plate, receipt | 1000×830 and 390×844 | panels and inputs `#FFFFFF` on `#F6F7F9` |
| samples | the same | the bulk bar's buttons white on `#EAF5EE` |
| plate | the same | the selection bar's buttons white on `#EAF5EE`; wells still blue `#E6ECFF`, amber `#FFF1D6`, grey `#EEF0F3` |

## F11: a control's outline apart from a divider

**Problem (measured by the review).** The check box outlines its box in `scheme.border` (`MorphCheckbox.qml:47-49`).
The gallery sets that role to the hairline it uses for cards and dividers, `#DDE1E6`. An unchecked box on `#F6F7F9`
then almost disappears (receipt, audit, stability). The mockups outline every control in `#C3C9D2`, and draw
dividers in `#DDE1E6` or `#EDF0F3`.

**Decision.** F11 adds two colour tokens.

- **`color.controlBorder`** outlines these controls: a text input, a text area, a select's field, a date input, a
  check box's box, a radio and a normal button.
  - Its default is `#c7c7cc`, today's `color.border` default. A theme that sets neither token looks as it does
    today.
  - A theme that sets `border` alone no longer moves its control outlines with it. This is the same stated cost as
    in F9.
  - The only in-tree scheme with its own border is the sidebar, and it gets its own `controlBorder`
    ([Built-in schemes](#built-in-schemes)).
- **`color.divider`** draws the rules F7's `divided` adds, and a table's row rules. Its default is `#dcdce0`.
  - **This changes the default look of tables.** Today a table's row rules are full `scheme.border`
    (`MorphTableRowBackground.qml:79`), `#c7c7cc` by default. They become the lighter `#dcdce0`.
  - The panel's title rule stays as it is. Batch G (G5) removes it, so F neither recolours it nor tests it
    (Decisions 6).

`color.border` stays the outline of a card, a panel, a popup and a table. A focused control's outline stays
`color.focus`, and a toned one stays its tone's border.

**Spec.** `theme.md`, the closed set and the mapping table gain `controlBorder` and `divider`.
`backend_contract.md`, Table cells: "row rules in `color.divider`".

**Tests.**

- `tests/qt_quick/test_qt_quick_theme.cpp`:
  - an unchecked check box's outline is `Theme.controlBorder`;
  - a text input's resting outline is `controlBorder`;
  - a table's row rule is `Theme.divider`.
  - **Control:** keep `scheme.border` on the check box, and on the row rule.
- F7's divided-column test checks the rules' colour.

**Gallery.** `color.controlBorder` `#C3C9D2`, `color.divider` `#EDF0F3`.

**Re-grab.** receipt, audit and stability at 1000×830: unchecked boxes are outlined in `#C3C9D2` (pixel read), and
the table rules read `#EDF0F3`.

## Built-in schemes

`Theme.qml` keeps five built-in schemes: `themeScheme`, `pageScheme`, `cardScheme`, `sidebarScheme` and
`toolbarScheme`. Batch G adds a sixth, `bar`. `MorphScheme.qml` declares each new role with its `ui-theme/1`
default. Each built-in scheme then sets every new role, either bound to the theme's own value or as a literal of its
own palette. The dark sidebar holds the navigator's search field, its pin buttons and, after F5, the ghost sign-out
button. If the sidebar took the light defaults, its text would read 1.6:1 on a white field (`#c9ccd6` on `#ffffff`),
its controls would get a light outline, and its hover would be invisible.

| Role | theme, page, card, toolbar (and G's `bar`) | sidebar | sidebar contrast (measured) |
|---|---|---|---|
| `controlSurface` | `theme.controlSurface` | `#2a3040` | text `#c9ccd6` 8.21:1, muted `#7d8396` 3.48:1 |
| `controlBorder` | `theme.controlBorder` | `#3a4152` | 1.52:1 on `#1f2430`, against the light default's 1.67:1 on white |
| `hover` | `theme.hover` | `#ffffff14` (white at 8 %, `#313641` over `#1f2430`) | text 7.55:1, muted 3.2:1 |
| `track` | `theme.track` | `#2f3646` (its `selection`) | — |
| `divider` | `theme.divider` | `#2f3646` (its `border`) | — |
| `neutral`, `neutralBorder`, `neutralDot` | `theme.neutral…` | its `muted`, `#7d8396` | — |
| `neutralBg` | `theme.neutralBg` | `#7d839626` (its `muted` at 15 %) | — |
| `scrim`, `popupShadow*` | theme-level only; not scheme roles | — | — |

The contrasts were computed with the WCAG 2 formula, using the same calculation that measured the review's 1.50:1. A
theme surface named `sidebar` replaces the whole built-in scheme as it does today: its layer is resolved over the
theme, so a role the layer does not set takes the theme's value, not these literals.

**Tests.** These go in `tests/qt_quick/test_qt_quick_theme.cpp`, with no theme applied.

- The navigator's search field, `navigator_search` inside `shell_sidebar`, fills with a colour that gives the
  sidebar's text at least 4.5:1.
- A hovered pin button in the sidebar fills with `sidebarScheme.hover`.
- Every built-in scheme has a value for every role that `MorphScheme` declares. The test walks each scheme's
  properties and compares them against `MorphScheme`'s.

**Control:** remove the sidebar's `controlSurface` literal. The field then reads `#ffffff`, at 1.6:1.

## F12: a small type step and a display size

**Problem (measured by the review).** Every `MorphUi` text draws at `Theme.bodyPixels` or at the heading size
(`theme.md`, On Qt Quick), and strong text uses `Font.Bold`. The mockups use five type styles:

| Use | Mockup style |
|---|---|
| body text | 13 px |
| tags and table headers | 11 px, weight 600 |
| field captions | 12 px |
| monospace | 12 px |
| KPI numbers | 22 px, weight 700 |

Because every screen draws all of these at body size, it looks larger and heavier than its mockup. Table headers,
for example, use `Theme.strongFont` (`MorphTable.qml:389`).

**Decision.** Five tokens. For a size, `0` means "derived from the body size", as for the two sizes that exist
today. No size derives from another new token, so rule 1 holds.

| Token | Default | Read by |
|---|---|---|
| `font.size.small` | body × 0.85, rounded | table header labels; the `small` text role; badges (batch G, G1) |
| `font.size.caption` | body × 0.92, rounded (12 at a 13 px body) | a field's caption above its control (`MorphField`) |
| `font.size.mono` | the body size | the `mono` role and mono spans (`Theme.monoFont`) |
| `font.size.display` | body × 1.7, rounded | the `display` text role |
| `font.weight.strong` | 700 (`fontWeight`) | every strong font: `strongFont`, `headingFont`, the table header, a section |

`font.weight.strong` is the first `fontWeight` token in the set. It gets its own kind, `ThemeTokenKind::FontWeight`,
and is read into `ThemeValues::numbers` as its numeric weight. A DTCG weight name already parses
(`checkFontWeight`).

`Theme.qml` gains the properties `smallPixels`, `captionPixels`, `monoPixels`, `displayPixels` and `strongWeight`,
and the fonts `smallFont`, `smallStrongFont`, `captionFont` and `displayFont`. Each font is built with `fontOf`.

**Two text roles.** Both are appended after `Mono`, so the wire indexes stay the same.

- **`display`**: the display size, the strong weight, the text colour. For a KPI's number.
- **`small`**: the small size, regular weight, the text colour. For a footnote, or a timestamp beside a heading.

The loader's role names gain both (`document.hpp:3930`), as do the interpreter's `roleOf`, `TextRole`, and
RecordingBackend's role switch (`recording_backend.hpp:315-317`). The terminal draws `display` in bold, as it draws
`heading`, and draws `small` as `normal`, because a terminal cell has one size.

**Spec.**

- `theme.md`:
  - the closed set gains the five tokens;
  - the "On Qt Quick" paragraphs on fonts gain the five tokens and "every strong font draws at
    `font.weight.strong`";
  - the sentence "There is no `font.size.mono`" is replaced by that token's row.
- `document.md` and `view_tree.md`: the two roles.

**Tests.**

- `tests/test_ui_theme_values.cpp`:
  - the defaults of the five tokens;
  - `font.weight.strong` set to `"semi-bold"` reads as 600.
- `tests/qt_quick/test_qt_quick_theme.cpp`, with body 13 and small 11:
  - a table header label's `font.pixelSize` is 11, and its weight is `strongWeight`;
  - a field caption is `captionPixels`;
  - a `display` text is `displayPixels`.
  - **Control:** keep `Theme.strongFont` on the header, and it reads 13.
- `tests/test_ui_document.cpp`: `role: "display"` and `role: "small"` load; `"tiny"` is still refused.
- `tests/tui/test_tui_widgets.cpp`: a `display` text is bold.

**Gallery.** The theme sets these values (Decisions 6):

| Token | Value |
|---|---|
| `font.size.body` | 13 |
| `font.size.small` | 11 |
| `font.size.caption` | 12 |
| `font.size.mono` | 12 |
| `font.size.display` | 22 |
| `font.size.heading` | 16 |
| `font.weight.strong` | 700 |

The strong weight stays at 700 because on the reference machine fontconfig resolves 600 to an italic face, so the
mockups' 600 cannot be drawn there yet. Console's KPI values take `role: "display"`. Badges belong to batch G (G1).

**Re-grab.** samples, audit and results at 1000×830: header labels are 11 px (read from the glyph height). console:
the KPI values are 22 px.

## F13: the accent where the design uses it

**Problem (measured by the review).**

- **Ghost buttons.** A ghost button's label is `scheme.text` (`MorphButton.qml:102-103`). So "Clear all",
  "Select all 312", "Save draft", "Apply ICH Q1A template" and review's back link are black, where the mockups draw
  them in the accent.
- **The active tab.** Its label is in the accent (`MorphTabs.qml:83`). The mockups draw it in the text colour, over an
  accent underline.
- **A selected list item.** It fills with `Qt.alpha(accent, 0.12)` (`MorphButtonBackground.qml:35`), not with
  `color.selection`.
- **The sheet's dismiss control.** It is a flat button in the text colour (`MorphDialog.qml:136-141`), where the
  mockup's "Cancel" is in the accent.

**Decision.**

- **Ghost buttons.** A ghost button's label is `scheme.accent`, and so is a ghost-emphasis menu trigger's. Its hover
  is `color.hover` (F9). A ghost is the vocabulary's link-like action, which is what the mockups' `.btn.ghost` is.
- **Tabs.** The active tab's label is `scheme.text`, and an inactive tab's is `scheme.muted`. The 2 px underline
  stays in the accent.
- **A selected list item** (`ButtonStyle::Item`) fills with `scheme.selection`. The item's leading bar stays in the
  accent.
  - **This changes the default look.** Today a selected item is the accent at 12 %, about `#e2ecf7` over white. The
    default `color.selection` is `#cfe3ff`, which is darker.
  - **A selected push button keeps today's accent tint and ring.** This matches batch G, which owns the selected
    well's look (Decisions 6). F does not change `restFill`.
- **The sheet's dismiss control** is drawn as a ghost button.
- **The terminal** is unchanged, because it has no palette. A ghost keeps `Normal`'s style.

**Spec.**

- `backend_contract.md`:
  - Button emphasis: "a ghost button … its label in the accent".
  - Button style and selection: "tints the current item in `color.selection`; a selected push button keeps the
    accent tint".
  - `DialogWidget`'s sheet row: "a ghost dismiss control".
- `qt_quick/backend.md`: the tabs row.

**Tests.**

- `tests/qt_quick/test_qt_quick_widgets.cpp`:
  - A ghost button's label colour is `Theme.accent`.
  - A selected item's background is `Theme.selection`.
  - A selected push button's rest fill is still the accent tint.
- `tests/qt_quick/test_qt_quick_containers.cpp`: the checked tab's label is `Theme.text`.
- **Control:** revert each changed line, and its check fails.

**Gallery.** No screen change. The theme's `color.selection` becomes `#EAF5EE`, the mockups' selected fill.

**Re-grab.** samples, stability, plate and audit at 1000×830: ghost labels are `#17784A`. Every tab bar: the active
label is `#1C1E21`.

## F14: a modal scrim

**Problem (measured by the review).** `MorphDialog.qml:60-69` is `modal: true` with no `Overlay.modal`, so the Basic
style's dim is drawn: `#E5E6E7` at (100, 400) over a `#F6F7F9` page, where the mockup dims to about `#8F9399`
(`rgba(20,24,31,.35)` in `08-Results.dc.html`'s notes). The drawer (`MorphDrawer.qml`) is the same.

**Decision.** **`color.scrim`**, default `#0000004d` (black at 30 %). The dialog's and the drawer's `Overlay.modal` is a
`Rectangle` in `Theme.scrim`. A sheet fills the window and draws no scrim. The terminal draws none: a dialog is a
framed box over the content, as now.

**Spec.** `theme.md`, the closed set and mapping table: `scrim`. `backend_contract.md`, `DialogWidget`: "a modal
dialog dims the window behind it with `color.scrim` (Qt Quick)".

**Tests.** `tests/qt_quick/test_qt_quick_overlays.cpp`: an open dialog's `Overlay.modal` item's colour is
`Theme.scrim`; after `applyTheme` with `color.scrim` `#14181F59`, it follows. **Control:** remove `Overlay.modal`,
and no such item exists.

**Gallery.** `color.scrim` `#14181F59`.

**Re-grab.** esignature (the dialog) at 1000×830: the page behind reads about `#8F9399` at (100, 400).

## The closed set after F

`kThemeTokens` goes from 46 to 63 paths; `kColorProperties` and `kSchemeProperties` (`theme.cpp:34-95`) gain every new
colour, `kDimensionProperties` the sizes and paddings, and `MorphScheme.qml` and each built-in scheme in `Theme.qml`
the colour roles (bound to the theme's, as every role is).

| Path | Kind | Default | Item |
|---|---|---|---|
| `color.neutral` (meaning changed), `color.neutral.bg`, `.border`, `.dot` | colour | `#6e6e73`, `#6e6e7326`, `#6e6e73`, `#6e6e73` | F9 |
| `color.hover` | colour | `#1d1d1f14` | F9 |
| `color.track` | colour | `#e5e5ea` | F9 |
| `color.controlSurface` | colour | `#ffffff` | F10 |
| `color.controlBorder` | colour | `#c7c7cc` | F11 |
| `color.divider` | colour | `#dcdce0` | F11, F7 |
| `color.scrim` | colour | `#0000004d` | F14 |
| `font.size.small`, `.caption`, `.mono`, `.display` | dimension | 0 (derived) | F12 |
| `font.weight.strong` | font weight | 700 | F12 |
| `table.cellPaddingX`, `table.cellPaddingY` | dimension | 8, 4 | F7 |
| `elevation.popup` | shadow | `#00000024`, 0, 8, 24, 0 | F3, F9 |

`font.size.caption` derives from the body size (× 0.92), not from `font.size.small`, so no token falls back to another.

Every name is a sibling where the set already uses siblings (`color.controlBorder` beside `color.border`, as
`control.touchHeight` beside `control.height`): with `$root` (F8) a child would now be readable, but the set keeps
one spelling per path, and `controlBorder` is not a part of `border` the way `bg` is a part of a tone.

## Shared with batch G

| F defines | G uses it for |
|---|---|
| `font.size.small`, `font.weight.strong` (F12) | G1's tag: 11 px at the strong weight (700 here, Decisions 6) |
| `color.neutral` with `.bg`, `.border`, `.dot` (F9) | G1's neutral tag `#EEF0F3` / `#363C46`; G1's chip outline (`color.controlBorder`) |
| `color.controlBorder`, `color.controlSurface` (F10, F11) | G1's chip, G6's themed check box in a table, G11's inputs |
| a panel on `card` (F10), `color.divider` (F11) | G5's framed collapsible; G3's pinned bar separator |
| `divided` (F7) | G5's 44 px rows inside a framed section; G6 does not need it (tables draw their own rules) |
| the `display` role (F12) | G9's KPI "Past due 7": `display` with a tone, which G9 adds |
| `wrap` (F4) | G10's tab overflow does not use it; G8's plate legend does |
| `color.hover` (F9) | G2's stepper and G10's "More" menu entries |
| `MorphPopupBackground` and the placement rule (F3) | G12's select dropdown, and any popup G opens |
| the built-in schemes' new roles ([Built-in schemes](#built-in-schemes)) | G's `bar` scheme binds them to the theme's, as the light schemes do |
| a selected push button keeps the accent tint (F13) | G8's selected well |

## Verification summary

Each item's test fails on today's code or under its stated mutation. After the three lanes are folded, every screen is
re-grabbed on Qt Quick at 1000×830 and 390×844, and at the sizes named per item, with
`morph_examples_client --app lims-gallery --screen lims-gallery.<id> --grab-size WxH --grab f.png`, and compared with
the mockups' PNGs in the review's scratchpad (`lg-cmp/mock/`). The comparison is by pixel reads at the points the
review measured, not by eye, and the result table goes into `visual-review.md`'s batch F section, item by item:
fixed, or what remains.

## Not in this batch

- The head's subtitle above the title (an eyebrow) and a head subtitle with spans: batch G (G9).
- The tabs joining the head's band (I7): the frame has no way to lift a screen's node into the top bar; F5 brings the
  head and the title together, and the tabs stay in the page.
- A white page on compact (I20, S7-7): the page is `color.background` at every width class; a theme variant for
  compact is a density layer, which `theme.md` lists as not yet chosen.
- Dialog title size (E3) and footer rule (E5), and the panel's title rule: batch G's (Decisions 6).
- Uppercase, letter-spaced captions (A8): `font.size.small` gives the size; text transform is not a token.

## Open questions

Questions 1–6 of the first draft are settled in [Decisions](#decisions). Two remain open:

1. **The floor of a scrollable slot** (F6). The floor is a stretching child's declared `minHeight`, not its natural
   height, so a screen has to opt in to page scrolling below its minimum. **Default:** the declared bound. Using the
   natural height would scroll every stretching table and every `pinned` page.
2. **A case sizing that reads the case's own names** (F1). It is reported, and the case is held at its content size.
   **Default:** report it. No screen writes one today.

## Decisions

Settled by the user on 2026-10-29:

1. Sign-out and the principal leave the top bar on every app on the generic client: the sidebar's foot, or the ☰ menu on compact.
2. `color.neutral` becomes the neutral tone only, with `.bg`, `.border` and `.dot`; `color.hover` and `color.track` are new; no alias for the old meaning.
3. Panels and collapsibles default to a gap of 2, dialogs and drawers to 3, and a panel draws on the `card` surface unless it names another.
4. The contrast check reports text below 4.5:1 and muted text below 3:1 as a theme problem; it does not refuse the theme.
5. Tokens of an unknown type stay in the parsed set so the report can name them.
6. Reconciled with batch G: `font.weight.strong` stays 700 in the gallery theme (600 resolves to an italic face on the reference machine); G removes the panel title rule, so F does not recolour it; G owns the dialog title size and footer rule.
7. A stretching child of a scrollable slot scrolls below its declared `minHeight`, not below its natural height: the bound is the author's, and a natural height would make every stretching page scroll as soon as its content grows.
8. A case or dialog body whose sizing reads names the case itself declares is reported and stays content-sized, rather than guessed.

## Review response

The design had one review, `lg-rF-review.md`. Each finding was checked against the code at `d5292bc0a` before it
was taken.

- **B1, taken.**
  - **Finding:** `lazy` (`interpreter.hpp:6665-6671`) is the slot's direct child, and the slot reads only that
    child's sizing (`MorphSlot.qml:28-35`, `container_widgets.cpp:124`). Making the column stretch alone would have
    passed the old test and left the cut.
  - **Change:** F1 now gives `lazy` the case's or the body's hints. That is the review's option (a), plus a report
    for a computed sizing that reads the case's own names.
  - **Tests:** the interpreter test now asserts on the slot's direct child, with that incomplete fix as its control.
    A layout case puts a one-row `ForEach` in a slot.
- **B2, taken.**
  - **Change:** a new section, [Built-in schemes](#built-in-schemes), gives every built-in scheme a value for every
    new role, with the sidebar's contrasts measured. A Qt test covers the navigator's search field, and
    `controlSurface` joins the contrast check.
- **S1, taken.** F9 and F11 now state that the neutral tone no longer follows `muted`, and control outlines no longer
  follow `border`. The sidebar binds its `neutral*` roles to its own muted.
- **S2, taken.** The lane table now covers:
  - `containers.{hpp,cpp}`, `recording_backend.hpp`'s role switch, `backend_contract.md` and `document.md`;
  - the `pane` surface and `MorphSlot.qml`;
  - three explicit waits: F7 on F11, F-S's F10 on F-T's F10, and F3 on F9, with F3 writing its own `MorphMenu` lines;
  - the shared append points, named as known conflicts.
- **S3, taken.**
  - **Change:** the wrapping row sits inside a plain `_topLine` between the pad spacers. A helper creates it and calls
    `setWrap` before its children exist.
  - **F4 rule:** zero-basis items take no gap. The lead's badge `forEach` wraps.
- **S4, taken.** F5 lists each of the nine tests that read today's top bar, and what each becomes.
- **S5, taken.**
  - **Change:** F7 and F10 list the other users.
  - **Decision:** forms groups become cards, and a nested panel draws no shadow. The well surfaces set their control
    tokens.
  - **Argued:** view-tree panels do not get F7's gap, because their author already sets the child column's gap.
- **S6, taken.** The second F4 case uses a 30-character stretch child with `minWidth: 10`, and widths are expressed
  in each backend's step distance.
- **S7, taken as settled by Decisions 6.**
  - F12's gallery weight is 700.
  - F11 no longer touches the panel's title rule.
  - F13 keeps the accent tint on a selected push button.
- **S8, taken, with a wider fix.**
  - **Change:** a stretching child of a scrollable slot is never shorter than its `minHeight`, and the slot scrolls
    it below that. The new layout case and the console's `minHeight` use it.
  - **Rejected:** a `minHeight` on the splitter alone. That would have cut the page, not scrolled it.
- **S9, taken.** F10 now says:
  - the default lives in `PanelImpl::scheme()`;
  - the children inherit it through `passSchemeDown`;
  - how the frame and the shadow avoid being drawn twice;
  - that `""` means the default.
- **S10, taken.**
  - **Change:** the select's dropdown joins the placement rule and the contract on both frontends. The menu bar
    records `openFrom`.
  - **Coordinator's note:** `MorphPopupBackground.qml` is one shared item, which a `ComboBox` popup and G12 use too.
- **Nits, all taken.**
  - Line citations corrected.
  - The `lowerAll` users listed.
  - F7's layout case dropped, with the reason.
  - Real test file names used.
  - The progress-track test that set `color.neutral` moves to `color.track`.
  - F8's re-grab expects `#FFF1D6`.
  - F9's control is now "ignore the alpha".
  - `font.size.caption` derives from the body size.
  - F4 reports a computed axis at mount.
  - The default-look changes of F11's row rules and F13's selection fill are stated.


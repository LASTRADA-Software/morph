# LIMS gallery batch D: layout, overlays and navigation

Batch D of the LIMS gallery initiative (`examples/lims_gallery/design/batches.md`): how screens compose and move. A
bar that stays in view, an overlay that becomes a sheet on a phone, an overflow menu, a search and filters in the
screen's head, removable filter chips, tabs a command can choose and whose labels follow the data, grid columns of
different widths, a surface that follows the data, a wizard whose footer can reach its step's fields, a screen opened
as a dialog that answers its opener, and the application's navigation with counts and a compact form. It lands on
`feat/lims-gallery` as one commit per item, in four parts (below); the gallery drops each workaround in the same
commit as the feature that makes it unnecessary.

Every claim below was checked against the code at `1e4c9cf34` (batch A landed), and every line cited is at that
revision. The commits after it (the B and C designs, and B part 1's monospace role) move lines of `interpreter.hpp`
and `view.hpp` by a few places. Each item says whether its premise was **reproduced** (run on this revision) or
**read** (inferred from the code). Where the gap log was wrong or incomplete, the item says so. Three throwaway probes
were built and run, then kept out of the tree:

- a Qt Quick probe (Qt 6.12.0, offscreen, Basic style, software scene graph), compiled with the flags of
  `tests/qt_quick/test_qt_quick_containers.cpp` and linked against the build's `libmorph_qt_quick.a`;
- a terminal probe, compiled with the flags of `tests/tui/test_tui_backend.cpp` and linked against `libmorph_tui.a`;
- an interpreter probe on `RecordingBackend`, through the corpus harness (`tests/ui_interpreter_support.hpp`).

They were compiled against the worktree's headers, which at the time held another lane's uncommitted text-role edits
(`document.hpp`, `interpreter.hpp` lines 5571–5600 and 7185–7200, `view.hpp`, the theme). Those touch none of the code
the probes exercise. Their output is quoted where an item rests on it.

| Item | Where it lands | Verdict on the gap log |
|---|---|---|
| G51 | interpreter, `Tab`, mount, both frontends | Right, reproduced |
| G20 | loader, interpreter, author, both frontends' tab bars | Right that a command cannot choose a tab. The overflow was not seen; reproduced now: the terminal never shows a chosen tab past the bar's end, and Qt Quick does not when the tab is chosen before the bar's first layout |
| G32 | loader, interpreter, `Common`, mount | Right. The dashed outline of an empty well stays out of ui/1 |
| G35 | loader, interpreter, mount | Right, and worse: `focus` already picks the wrong widget when a row scope reuses an id (reproduced) |
| G16 | specs, author, one layout case | **Wrong.** ui/1 already expresses it; reproduced on both frontends. What the gallery lacks is the idiom, and every screen is clipped on a short window today, which the workspace's page scrolling fixes |
| G38 | author only | Right that `wizard` is model-backed. A wizard over screen state needs G35 and a helper, not a node |
| G23 | loader, interpreter, `Grid`, both frontends | Right |
| G18 | `menu` node, view tree, the four backends; the head | Right; the same hand-rolled overflow is in two screens, and the console uses the trick for its navigation |
| G27 | `badge`, the four backends | Right; one design, the first item to drop |
| G26 | the head (`ScreenHead`), `Workspace`, interpreter | Right; the same in-page toolbar is in four screens |
| G4 | `dialog`, the four backends | Right; the terminal also ignores a dialog's sizing |
| G50 | `Workspace`, the client shell, `DocumentScreenHost`, `host.md` | Right; and a screen that `return`s from a tab leaves an empty tab behind (read), and a call to a screen whose tab is already open is never answered (read) |
| G56 | `Navigator`, `ShellFrame`, the app document, the client shell | **Mis-framed.** The console's sidebar is the application's navigation, not the screen's own. The gaps are counts on the navigator's entries and a shell that has no compact form (reproduced: the sidebar takes half of a compact terminal). The counts need the client to mount the app document, which it never does (read) |

## The shape of the batch

Four rules hold throughout, so an author learns them once.

- **A document chooses its layout by the width class; a frontend never does.** Every responsive item builds on batch
  A's `{"env": "widthClass"}`: a dialog becomes a sheet where its `sheet` condition says so, and a menu entry, a head
  action or a head tool shows where its `visible` says so. The one exception is the host's own chrome, which no
  document draws: the shell frame's compact form and a screen the host opens as a dialog follow the backend's
  `widthClass()` directly, as batch A's touch-sized controls do.
- **A bound key is controlled, and the user's change comes back as an event.** This is batch B's rule for a table's
  `selection`, `sort` and `page`, and a select's `selected`. Tabs take `selected` and `onChoose` the same way (G20).
- **The head stays data.** A screen's head crosses `ScreenHandle`, which a host screen implements too
  (`docs/spec/ui/host.md`, Host screens). Its menu (G18) and its tools (G26) are parts the screen evaluates and the
  workspace draws, like its badges and actions; no node of one screen is mounted inside another's widgets.
- **Navigation stays in `host.md`'s model.** A `navigate` with `as: dialog` is still a call that the next mount of
  that screen adopts, answered once by `return`, `close: self` or any other close (G50). The batch adds a host that
  shows it, not a second protocol.

Design 01's sheet, written with the batch:

```json
{ "kind": "dialog", "id": "signature", "title": "Sign to approve results",
  "description": "Your signature is stored with the record and cannot be removed.",
  "sheet": { "fn": "eq", "args": [ { "env": "widthClass" }, "compact" ] },
  "dismissLabel": "Cancel",
  "open": { "ref": "open" }, "cancel": { "set": "open", "value": false },
  "actions": [
    { "id": "cancel", "label": "Cancel", "onClick": { "set": "open", "value": false },
      "visible": { "fn": "ne", "args": [ { "env": "widthClass" }, "compact" ] } },
    { "id": "sign", "label": "Sign and approve", "emphasis": "primary", "onClick": [ … ] } ],
  "children": [ … ] }
```

(`description` is batch C's.)

## Dependencies on batches B and C

- **B.** G23's track `align` is B's column `align` (`start`, `center`, `end`), with B's meaning for a text and a
  label-width cell, and it reuses the `textAlign` plumbing B adds to `MorphText`. G35's "next row" example computes the
  next key with B's `slice`. Neither part lands before B's part 1 (`align`) and part 2 (`slice`).
- **C.** G27's chip is a badge with C's `style: outline`. G4's sheet head draws C's dialog `description`, and G50
  shows a screen's subtitle as that description. A menu entry's muted hint (G18) is named `description`, C's word
  for a line of help under a thing. G26's head select may take C's `segmented` style; the batch defines no segmented
  control of its own.
- **C's G52** lets an item button hold display-only content, `badge` included, and refuses every kind whose
  interaction is a part. A badge with `onRemove` (G27) is such a kind: the loader refuses it inside an item's
  content, with C's message. Whichever of G27 and G52 lands second adds the rule.
- If C has not landed, G4 and G50 draw no description line and G27 draws a filled chip. Nothing else waits.

## Parts and order of landing

Thirteen items and one prerequisite fix, three conformance cases, five layout cases and four backends are more than
one lane carries. The batch lands as four pull requests on `feat/lims-gallery`, each green on its own:

| Part | Items | Conformance | Layout cases | Needs |
|---|---|---|---|---|
| D1, following the data | G51, G20, G32, G35, G16, G38 | 51 | L11, L15 | B part 2 for G35's row example only |
| D2, layout and controls | G23, G18 (the `menu` node), G27 | 52, 53 | L12, L13 | B part 1 (G23); C only for the gallery's outlined chips |
| D3, the head and overlays | G18 (the head's menu), G26, G4 | — | L14 | D2's button-style menu; C for G4's description line only |
| D4, navigation | the app document's mount, G50, G56 | — | — | D2's menu, D3's sheet and head |

Within a part the order is the table's. D1 is mostly interpreter work, with G20's tab bars on both frontends; D2 is
backend work on all four backends; D3 changes `ScreenHead`, `Workspace` and `DialogWidget`; D4 changes
`DocumentScreenHost`, the client shell and the frame. D4's first item is a defect fix that stands alone and may land
before the rest of the batch.

---

## G51: a tab label follows the data

**Problem (reproduced).** The interpreter probe mounts tabs whose second label is
`format("Comments · {0}", n)` and a button that adds one to `n`:

```
D-PROBE G51 before tabs=[Overview,Comments · 0]
D-PROBE G51 after  tabs=[Overview,Comments · 0]
```

- The lowering evaluates each label once: `textOf(once(ctx, label), format)`
  (`include/morph/ui/interpreter.hpp:6331-6336`).
- `Tab::label` is a plain string, "set once" (`include/morph/ui/view.hpp:674-680`).
- The mount sends the labels once (`include/morph/ui/mount.hpp:926-934`).

The review screen mounts its tabs under a `switch` on the worksheet, one case per worksheet, so another worksheet
mounts new tabs with new labels (`examples/lims_gallery/src/screens/review.cpp:590-608`). That also throws away the
reviewer's tab pick.

**Decision.** A tab's label is bound like every other text. No vocabulary changes: `label` is already an expression,
and the lowering stops reading it once.

**Lowering.** Each `Tab::label` is `bindProp<std::string>` of the part's `label` in the tabs' enclosing scope, as a
panel's `title` is (`textProp`).

**View tree and mount.**

- `Tab::label` becomes `Prop<std::string>`. `view_tree.md:116` drops "set once" for the labels; the list of tabs
  stays set once.
- The mount builds one `Prop<std::vector<std::string>>` over every label, as `mountKind(Menu)` does for its entries
  (`mount.hpp:802-822`), and calls `setTabs` again on each change.

**Contract.** `TabsWidget::setTabs` is already called again: the workspace re-sends its captions on every change,
then the highlight (`workspace.hpp`, `showTabs`). What is new is that a call with the same number of labels replaces
the captions and keeps the highlight, the shown page and the keyboard focus, so no caller has to re-send them.

- **Qt Quick.** Today the `Repeater`'s model is the `labels` list, so a new list destroys and rebuilds every
  `TabButton`: `bar.currentIndex` falls back to 0 and a focused button is gone. Measured in `qml6` with Qt 6.12's
  Basic style, highlighting B, then relabelling `[A, B]` to `[A, B · 3]`:

  ```
  qml: A(model=list): currentIndex 0 sameItem false text B · 3
  qml: B(model=count): currentIndex 1 sameItem true text B · 3
  ```

  So `MorphTabs`'s `Repeater` takes the label count as its model, and each `TabButton` reads its caption from
  `root.labels[index]`. A relabel with the same count keeps every button, the highlight and the focus; a new count
  rebuilds them, as now, and the caller sends the highlight after it, as the workspace does.
- **Terminal.** `TabsImpl::setTabs` (`src/tui/list_widgets.cpp:697-700`) keeps `_selected` already. Read.
- **`RecordingBackend`** records the new `tabs`.

**Spec.** `view_tree.md:116`; `backend_contract.md:221` (`setTabs` with the same count keeps the highlight);
`document.md:176`, the `tabs` row: a tab's `label` follows its expression.

**Tests.**

- **Conformance case 51**, "relabelled tabs keep the highlighted tab and its page". `setTabs({"A","B"})`, the user
  picks B, `setTabs({"A","B · 3"})`: `textOf(tabs)` reads `B · 3`, and B's page slot is the one visible.
  `ConformanceProbe::textOf` gains "the label of the tab a Tabs highlights"; the terminal's `probeText` already is
  that (`list_widgets.cpp:735`), and the Qt Quick and recording probes read it the same way. **Control:** give
  `MorphTabs`'s `Repeater` the list as its model again, and Qt Quick reads `A`; a backend that ignores a second
  `setTabs` reads `B`.
- `tests/test_ui_interpreter_control.cpp`: the probe's document records `[Overview,Comments · 1]` after the click.
  **Control:** restore `once`, and it records `· 0`.

**Gallery.** Review drops the per-worksheet `switch` around its tabs. The tabs keep the reviewer's pick across
worksheets, which the design does not contradict.

**Risks.** A label that reads a query re-sends every label when it changes. Tab lists are short.

## G20: choosing a tab from a command, and a tab bar too narrow for its tabs

**Problem.**

- **Choosing (read).** `tabs` takes parts `{label, children}` (`document.hpp:785`, `document.md:176`), and the
  lowering keeps the pick in a signal of its own (`interpreter.hpp:6339-6341`). The view tree already has a bound,
  controlled `Tabs::selected` (`view.hpp:682-691`, `mount.hpp:943-960`); the document cannot reach it.
- **Overflow (reproduced; the gap log had not seen it).** Seven tabs with the last one chosen:
  - Qt Quick, 360 px wide, read 1.5 s after the choice. Chosen before the first layout, `Training` stays at
    x = 533, outside the window, though the bar highlights it. Chosen after it, Basic's `TabBar` scrolls it fully into
    view; read before its scroll animation ends, it is still on its way (x = 320).

    ```
    D-REV G20 before=1 tabsRootWidth=360.0
    D-REV G20 currentIndex=6 count=7
    D-PROBE G20 tab 'Training' x=533.2 w=74.2 visible=1      (chosen before layout)
    D-REV G20 before=0 tabsRootWidth=360.0
    D-PROBE G20 tab 'Training' x=285.8 w=74.2 visible=1      (chosen after layout)
    ```
  - Terminal, 40 columns. The bar draws from the first label and the canvas cuts it; the chosen `[Training]` is not
    on screen (`TabsImpl::paint`, `list_widgets.cpp:745-757`):

    ```
    D-PROBE-TUI G20 | Overview  Calibration  Maintenance  Boo
    ```

The instrument's Book opens a booking dialog instead of the Bookings tab (`instrument.cpp`, `declareHead`).

**Decision.**

- **Choosing.** `tabs` takes `selected` and `onChoose`, the pair a `select` has (`document.md:168`), with batch B's
  controlled rule. A tab is chosen by an `id` of its own, not by its index, so a tab added to the document later never
  shifts what a command meant. A tab part takes no `visible`: the list of tabs stays set once.
- **Overflow.** The bar keeps the chosen tab wholly in view and scrolls to it, on both frontends. A "More ▾" tab that
  collects what does not fit is **not in ui/1**: it needs a measuring pass that decides which tabs move, per backend,
  the same reason batch A gave the head a line of its own instead of an overflow. A scrolling bar reaches every tab
  with the widgets each frontend has. A frontend may later draw the overflow as a menu without a vocabulary change.

**Vocabulary.**

| Key | Where | Shape | Meaning |
|---|---|---|---|
| `id` | tab part | name | The tab's key; unique among the tabs |
| `selected` | `tabs` | expression, a tab id | Bound: the tab shown, controlled. A literal `null` is no binding |
| `onChoose` | `tabs` | commands | The user picked a tab; `event` is its id |

- Every tab has an `id` when the node gives `selected` or `onChoose`; the loader refuses one without, and a
  duplicate.
- A `tabs` node with an `id` has the element value `selected`: the shown tab's id, bound or the node's own.
- `set` never names a tabs node, as it never names a table (batch B). A command chooses a tab by setting the state
  `selected` reads.
- Author: `Tab::id`, and `ui::tabs(tabs, TabsOptions{.selected, .onChoose})`.

```json
{ "kind": "tabs", "id": "record", "selected": { "ref": "tab" },
  "onChoose": { "set": "tab", "value": { "ref": "event" } },
  "tabs": [ { "id": "overview", "label": "Overview", "children": … },
            { "id": "bookings", "label": "Bookings", "children": … } ] }
```

and Book's `onClick` is `{"set": "tab", "value": "bookings"}`.

**Lowering.** The ids map to indices once. A literal `selected` that names no tab is refused at load. A bound
`selected` becomes `Tabs::selected` through that map; a value that names no tab is reported once at `/selected` and
shows the first tab. `onSelect(index)` runs `onChoose` with the id as a user
event; without `selected`, the node also moves its own pick, as now.

**Per backend (overflow).**

- **Qt Quick.** `MorphTabs.select()` calls `bar.contentItem.positionViewAtIndex(index, ListView.Contain)` after the
  bar is polished, and again when the bar's width changes. That places the chosen tab inside the bar however it got
  chosen. Measured: the same call made after the first layout moves the before-layout `Training` from x = 533 to
  x = 285.8, its right edge on the window's.
- **Terminal.** `TabsImpl` keeps a first visible label. Painting moves it so the chosen label fits, and draws `‹` at
  the start and `›` at the end while labels are cut there. A click on a label maps through that offset; a click on
  `‹` or `›` chooses the previous or next tab, as Left and Right do, so a pointer reaches every tab. Left and Right
  are unchanged.
- **`RecordingBackend`.** Nothing; it lays nothing out.

**Spec.** `document.md:176` and the load-time rules (tab ids); `interpreter.md`, Relations (the tabs' controlled
`selected`); `qt_quick/backend.md:155` and `tui/frontend.md:91` (the chosen tab is kept in view; the terminal's
markers).

**Tests.**

- `tests/test_ui_interpreter_control.cpp`: tabs bound to `tab`; a button sets `tab` to `bookings`, and the Tabs
  record `selected=1` with page 1 visible. A user's pick of `overview` runs `onChoose` with `"overview"` and, while
  `tab` is not written, the highlight returns to `bookings`. **Control:** lower `selected` from the own signal again,
  and the first check fails.
- Corpus: a tab without an `id` under `selected` is refused; two tabs with one id are refused; a literal `selected`
  that names no tab is refused.
- `tests/qt_quick/test_qt_quick_containers.cpp`: the probe's seven tabs at 360 px, chosen before and after the first
  layout; once the bar's view has stopped moving, the chosen button lies within the bar both times. A read taken
  during the bar's scroll animation finds the tab on its way and proves nothing. **Control:** drop the
  `positionViewAtIndex` call, and the first reads x = 533.
- `tests/tui/test_tui_containers.cpp`: the same at 40 columns draws `[Training]` and a `‹`. **Control:** paint from the
  first label again.

**Gallery.** Instrument: Book sets the tab to `bookings` and opens the booking dialog there.

**Risks.** A bar that used to be cut now scrolls on the terminal; the cells it shows change.

## G32: `surface` follows the data

**Problem (read).** `surface` is a name on every node (`document.md:156`), read into a plain string
(`interpreter.hpp:5832`, `Common::surface`, `view.hpp:372`) and applied once (`mount.hpp:566`). The samples screen draws
each compact card twice, one on `outOfSpec` and one plain, and shows one by `visible`
(`samples.cpp:487-491`); the plate gives template wells literal surfaces and tells a sample well from an empty one by
its label only.

Both backends already handle a changed surface (read): Qt Quick's `ItemBase::showSurface` writes the property and
passes the scheme down again (`src/qt_quick/widget.cpp:237-242`); the terminal stores it and reads it at paint
(`src/tui/widget.hpp:92`).

**Decision.** `surface` is an expression on every node. A plain string is still a literal expression, so every
document that loads today loads the same.

**Vocabulary.** `surface`'s shape changes from a plain string (`Shape::Text`) to an expression of text. A value that
names no surface draws as no surface does, as an unknown literal does today (`theme.md`, Surfaces); a value that is
not a string is reported once at the node's `surface` path and treated as empty. Author: `Node::surface` gains an
`AnyHandle` overload beside the `std::string` one.

**View tree, mount and contract.** `Common::surface` becomes `Prop<std::string>`. The mount calls `setSurface` when
it is bound or not empty, and again on each change. `backend_contract.md`'s `Widget` row: `setSurface` may be called
again, and restyles the widget and everything inside it. `view_tree.md:147` drops "set once".

**What stays out.** A dashed outline for an empty plate well is a shape style no surface carries, as batch B leaves
the well's circle out. An empty well takes a surface of its own (`empty`, muted) instead.

**Spec.** `document.md:156` (every node); `view_tree.md:147`; `backend_contract.md` Interfaces.

**Tests.**

- `tests/test_ui_interpreter_control.cpp`: a panel with `surface: cond(flag, "outOfSpec", "")` records
  `surface=outOfSpec`, and after the flag clears, `surface=`. **Control:** apply the surface once in the mount, and the
  second record is missing.
- `tests/qt_quick/test_qt_quick_theme.cpp`: under `brandTheme()`, a panel's background colour follows a surface
  changed after mount, and so does a text inside it. **Control:** skip `passSchemeDown` in `showSurface`, and the
  text's colour stays.

**Gallery.** Samples draws one card per row with a computed surface. The plate's sample wells take `sample` or `empty`.

**Risks.** A document whose `surface` was a string that parses as an expression form cannot exist: surface names are
plain words.

## G35: a command names an element inside a `switch` case, or in another row

**Problem.**

- **Reproduced by the gap log:** `` `perPullInput` is not an element of this scope `` when the wizard's footer
  focuses a field in the step's case. The loader requires a `focus` target among the elements of the command's scope
  chain (`document.hpp:3323-3325`).
- **Reproduced here, a defect the gap log did not see.** At run time `focus` goes by the bare id: the lowering gives
  every element `focusId = node.id` (`interpreter.hpp:5831`), and `Mounter::focus` takes the first widget inserted
  under that name (`mount.hpp:258-270`). A screen with a text input `name` after a `forEach` whose rows also declare
  `name`, and a button that focuses `name`:

  ```
  D-PROBE G35 focused=4 screenName=7 rowName(first)=4
  ```

  The screen's own field (widget 7) is the one the loader resolved; the first row's field (4) takes the focus, and no
  diagnostic is reported. The same rule sends a focus from inside row 2 to row 1's field.
- **The "next row" case (read).** 08's "Enter moves to the next result" needs a row's command to focus the same
  field in another row; `focus` takes a name, not a row.

**Decision: a focus target resolves to declarations, and the widget is found by declaration.**

- **Focus keys.** The lowering gives an element (a node with an `id`) the focus key of its declaration: the node's
  JSON Pointer in the document, followed by `@<canonical row key>` for each row scope it sits in, outermost first.
  A node in a component's fallback is keyed under the key of the `custom` node that uses it, since one fallback is
  lowered once per use. A key is therefore unique per declaration, row and use, and the same on every re-render: a
  switch case mounted again, or a row that moves, keeps its keys. Nodes without an `id` get none, as now.
- **Lexical first.** A `focus` target that names an element of the command's scope chain resolves to that
  declaration, as now. Inside a row, that is the current row's element.
- **Into switch cases.** Otherwise the loader looks through the switch cases reachable from the command's scope
  chain: down through `switch` cases only, at any depth, never into a row, a dialog, a drawer or a component. The
  target resolves to the declarations of that name there.
  - Declarations in **sibling cases of one switch** are allowed: at most one case is mounted, so at most one is
    focusable. That is the wizard, where two steps may each have a field `notes`.
  - Declarations in **two different switches** are refused (both may be mounted), naming both paths.
  - At run time the command tries each candidate key; the mounted one takes the focus. None mounted is reported as
    today (`` `x` is not mounted ``).
- **Another row.** `focus` takes an optional `row`, an expression of a row key, for a target declared in a row scope
  of a `forEach` or a table: `{"focus": "result", "row": e}`.
  - The loader resolves the target among the elements of the row scopes reachable from the command's scope chain:
    the command's own row scope, and the row scopes of the `forEach`es and tables below its scope chain, through
    switch cases as above. Two such row scopes declaring the name are refused, naming both paths.
  - The key is the target's pointer with the command's own enclosing row keys, the innermost replaced by `e`; a
    target in a row scope below the command's takes `e` as its first row key.
  - `row` on a target that is not in a row scope is refused at load. A `row` that evaluates to `null` focuses nothing
    and reports "not mounted", as any target that is not mounted does.
- **What stays the same.** Only `focus` reaches into cases. Reading an element's values from outside its case stays
  refused: an unmounted case has no value to read, and a step's validity belongs in screen state (G38).

**Lowering and runtime.** `commonOf` sets `focusId` from the node's path, the enclosing component uses' keys and the
enclosing rows' keys. The
command carries its candidate pointers, resolved at load, and its `row` expression. `Mounted::focus` keeps its
signature: the interpreter passes keys, and an application that calls it with its own names still finds what it named.

**Spec.** `document.md:265-269` (Targets: `focus` an element, or one in a switch case below, with `row` for a row's);
`interpreter.md`, Commands (resolution and the keys); `view_tree.md`'s `focusId` row (a key the lowering makes, unique
per declaration, row and component use).

**Tests** (`tests/test_ui_interpreter_commands.cpp`).

- The probe's document: the screen's field takes the focus. **Control:** go back to `focusId = node.id`, and the row's
  field does.
- A footer button focuses `perPull`, declared in case 1 of a switch; with the switch on case 1 it takes the focus, and
  on case 0 the command reports "not mounted".
- Sibling cases 1 and 2 both declare `notes`; the focus follows the case shown.
- Two switches each declaring `x`: the document is refused with both paths. **Control:** drop the rule, and it loads.
- Inside row 2, `focus: name` focuses row 2's field. `{"focus": "result", "row": 3}` focuses row 3's.
- A `forEach` inside a `forEach`: a command in outer row 2's inner row 1 with `row: 3` focuses outer row 2's inner
  row 3, not outer row 1's. **Control:** key the element by its innermost row key only, and the first inserted, outer
  row 1's, takes the focus.
- A component fallback used twice, each declaring `name`: a focus from inside the second use focuses the second's.
  **Control:** key the element by its pointer alone, and the first use's field takes it.
- Corpus: `row` on a screen-level target is refused; two `forEach`es below the command declaring the `row` target are
  refused.

**Gallery.**

- Stability's Continue focuses the blocking field itself; the banner's "Go to the field" stays as a second way.
- Results: Enter in a result field focuses the next row's, with the next key written as
  `first(slice(keys, indexOf(keys, row.id) + 1, 1))` over a `let` `keys` of the rows' ids (B's `slice`), under an
  `if` that skips the last row; and the help line gets its sentence back. Slicing the rows and reading `.id` of the
  result would be refused: a field is not read below a computed value (G36).

**Risks.** A document that relied on `focus` reaching the first of several same-named widgets changes. No screen in the
tree does: `grep -rn '"focus"\|ui::focus(' examples` finds the stability banner and screen-level fields only.

## G16: a bar that stays in view

**Problem (the gap log's conclusion is wrong).** The gap log is right that there is no screen-level footer, and wrong
that one is needed: ui/1 already says this. A column that stretches in height, holding a `scroll` that stretches above
the bar, keeps the bar on screen while the content scrolls. The workspace makes every page stretch for exactly this
("so a screen whose root stretches fills the page", `docs/spec/ui/workspace.md`, Shape). Reproduced on both frontends,
with a page of 40 lines and a button, inside `ShellFrame` and a hidden-bar `TabsWidget` as the workspace builds them:

```
Qt Quick, 480 × 420:
D-PROBE G16 scroll=1 window=480x420 barTop=376.0 barBottom=420.0 lastLineBottom=875.0
D-PROBE G16 scrollTop=39.0 scrollHeight=329.0
D-PROBE G16 scroll=0 window=480x420 barTop=883.0 barBottom=927.0 lastLineBottom=875.0

Terminal, 60 × 20:
D-PROBE-TUI 18|                              line 16
D-PROBE-TUI 19|                              [ Receive, next ]      (with the scroll)
D-PROBE-TUI 19|                              line 17                (without: the button is below the screen)
```

The second line of each shows the real finding: **a screen that does not scroll is cut at the window's edge**, and no
gallery screen scrolls (`grep -rn 'scroll' examples/lims_gallery/src/screens` finds nothing). Every compact layout,
and the long expanded ones, lose their bottom on a short window. That is the same example defect in every screen, so
by `AGENTS.md`'s bar it is a framework seam, and the seam is this idiom: the framework has the mechanism and does not
say how to use it.

**Decision.**

- No new node. A screen-level footer would be a second way to say what a column and a scroll say, and the column
  also gives a bar at the top (G26's in-page form) and both at once.
- The idiom is written down where an author looks, and the author API gets a helper:
  `ui::pinned(Nodes content, PinnedOptions{.top, .bottom})` returns a column sized `{width: stretch, height: stretch}`
  of `top`, a vertical `scroll` sized to stretch holding `content`, and `bottom`.
- A layout conformance case keeps the idiom true on both frontends.
- **The workspace scrolls a root that does not stretch in height**, so no screen is ever cut off at the window's
  edge. A root that stretches keeps its own layout, as now: it fills the page, and a stretching table, splitter or the
  pinned idiom inside it gets the page's height. Scrolling every page would leave a stretching child no height to
  stretch to.
  - `SlotWidget::setScrollable(bool)`, pure, default `false`: a scrollable slot shows a child whose height does not
    stretch at its natural height inside a vertical viewport of the slot's own height, and lays out a stretching
    child as a slot does now. The slot reads its shown child's height sizing itself, so a remount or a switch of
    root follows without a call. The workspace calls `setScrollable(true)` on every page slot it makes.
  - **Qt Quick.** `MorphSlot` gains `scrollable`. While it is set and no shown child stretches down (its existing
    `stretchesDown`), the child sits in a `Flickable` with `MorphScrollBar`, as `MorphScroll` holds its content, its
    `contentHeight` the child's implicit height; the focused item is kept in view as in a `scroll`. Otherwise the
    slot is laid out as now.
  - **Terminal.** `SlotImpl` with `scrollable` and a child that does not stretch down gives the child its natural
    height in a viewport that follows the focus and takes the arrows, PageUp/PageDown, Home/End and the wheel, as
    `Scroll` does; a stretching child is laid out as now.
  - **`RecordingBackend`** records `scrollable=true`.

**Spec.** `document.md`, a short "A bar that stays in view" paragraph under View nodes; `author.md`, `pinned`;
`backend_contract.md`, the cross-axis paragraph gains "a `stretch` child of a column takes the column's spare height,
so a stretching scroll above a bar keeps the bar at the column's end", and the `SlotWidget` rows gain
`setScrollable`; `workspace.md`, Shape (every page scrolls a root that does not stretch in height);
`qt_quick/backend.md` and `tui/frontend.md`, the Slot rows.

**Tests.**

- **Layout case L11**, "a stretching scroll above a bar keeps the bar at its column's end": a column of fixed height 20
  holding a stretching scroll of 40 one-line texts and a text `bar`. `bar`'s bottom is the column's bottom, and the
  scroll is less than 20 tall. **Control:** a backend that sizes the scroll to its content, ignoring its `stretch`,
  puts `bar` below the column; the case itself with the scroll taken out does the same.
- **Layout case L15**, "a scrollable slot scrolls a content-height child and fills with a stretching one": a slot of
  fixed height 20 with `setScrollable(true)`. Holding a column of 40 one-line texts, the slot stays 20 tall and the
  last text is outside its bounds until the probe focuses it, and then inside them. Holding a column that stretches
  in height, the column is exactly the slot's 20 and nothing scrolls. **Control:** a backend that ignores
  `setScrollable` draws the 40 lines past the slot's bottom, and the focused last line stays outside its bounds.
- `tests/test_ui_workspace.cpp`: every page slot records `scrollable=true`. **Control:** drop the call, and the record
  is missing.
- `tests/test_ui_author.cpp`: `pinned` writes the column, scroll and sizes.

**Gallery.** Receipt, samples, stability, results and review go through `pinned` and put their compact action rows in
`bottom`; the batch A width class decides which layout carries a bar. Every other screen's root keeps its content
height and is scrolled by the workspace.

**Risks.** A content-height root that was cut at the window's edge now scrolls in every application, which changes
what a long page shows on a short window; nothing past the edge was reachable before. A root that stretches is laid
out as before.

## G38: a wizard over screen state

**Problem (read).** `wizard` is the forms engine's (`model` and `action` required, `document.md:196`). Stability
builds its own from `steps`, a `switch` on a `step` state and hand-made Back and Continue (`stability.cpp:551-688`).
The one thing it could not do was reach the blocking field from Continue, which is G35.

**Decision: an author helper, not a node.** With G35, every part of the wizard is a document node that already
exists. A `stepper` node would only rename them, would sit beside a `wizard` of a different meaning, and would need a
lowering, view-tree mapping and spec of its own for one design. The helper writes the same JSON the gallery writes by
hand.

**Author API.** `ui::stepper(StepperOptions)`:

| Field | Meaning |
|---|---|
| `step` | The `int` state holding the current step |
| `steps` | `{label, children, canContinue, blocking, onBlocked}` per step: a condition (default `true`), an element Continue focuses while the step cannot continue, and commands run then |
| `backLabel`, `continueLabel`, `finishLabel` | Texts; English defaults |
| `onFinish` | Commands Continue runs on the last step |
| `extra` | Footer nodes between Back and Continue (Save draft) |
| `labelled` | Whether the steps indicator shows its labels |

It returns `column[steps(items from labels and step), switchOn(step, one case per step), row[Back, spacer, extra…,
Continue]]`. Continue runs `if canContinue(step) then (last ? onFinish : set step+1) else [onBlocked, focus
blocking]`.

**Spec.** `author.md`, `stepper`.

**Tests.** `tests/test_ui_author.cpp`: the JSON for two steps; the document loads; on `RecordingBackend`, Continue with a
blocked step focuses its field (needs G35). **Control:** omit the `focus`, and the field is not focused.

**Gallery.** Stability's footer, indicator and switch become one `stepper`; `next`, `backButton` and
`continueLabel` go.

## G23: grid columns of different widths

**Problem (read; the gap log's grab stands).** `grid` takes a column count (`document.hpp:761-765`), and its columns
share the width (`backend_contract.md:143`, `MorphGrid.qml`, the terminal's "equal columns", `tui/frontend.md:87`).
Instrument draws each event as a `row` of a `fixed(18)` time and a stretched text (`instrument.cpp`, `eventList`);
the plate is a 13-column grid whose letter column is as wide as a well and whose numbers sit at the start of their
cells (`plate.cpp:387`). Two screens, two hand-rolled tracks.

**Decision.** A grid takes column tracks, each a sizing and an alignment. They are a table column's two layout keys
(B), so a grid track and a table column read the same.

**Vocabulary.** `grid` takes `tracks`, parts `{width, align}`:

- `width`: a sizing form (`content`, `{"fixed": n}`, `{"stretch": w}`), as a column part's; default `stretch(1)`.
- `align`: B's `start`, `center` or `end`, placing each cell's content across its track, with B's meaning: a text's
  text and a label-width kind's box move; a container fills the track. The grid's own `align` keeps its meaning, the
  cell's place down its row, so a track's `align` is horizontal and the grid's vertical, as a table column's `align`
  and B's `valign` are.
- `columns` and `tracks` are exclusive; with `tracks`, the column count is their number. `span` covers tracks.

```json
{ "kind": "grid", "gap": 1, "tracks": [ { "width": { "lit": { "fixed": 3 } } },
  { "width": { "lit": { "fixed": 6 } }, "align": "center" }, … ] }
```

Author: `ui::grid(children, {.tracks = {{.width = ui::fixed(3)}, {.width = ui::fixed(6), .align = Align::Center}}})`.

**View tree and contract.** `Grid` gains `std::vector<GridTrack> tracks` (`GridTrack{Sizing width; Align align}`),
set once. `GridWidget::setTracks(std::vector<GridTrack>)` is pure; the mount calls it instead of `setColumns` when
tracks are given. Tracks are sized as a row sizes its children: fixed tracks first, content tracks to their widest
cell, stretch tracks share the rest by weight; a spanned cell does not size a track.

**Per backend.**

- **Qt Quick.** `MorphGrid` takes `tracks`. Each child's `Layout.preferredWidth`/`Layout.fillWidth` and
  `Layout.horizontalStretchFactor` come from its column's track (spans sum theirs). A track's `align` is a second,
  horizontal alignment on the child, combined into `Layout.alignment` with the vertical one the grid's `align` writes
  through `crossAlign` today (`MorphWidget.qml`, `Layout.alignment`, which in a grid sets only the vertical bits);
  a text takes it through B's `textAlign`.
- **Terminal.** `GridImpl::setTracks` computes column widths with `layout::distribute`, the table's solver, and places
  each cell by its track's `align`.
- **`RecordingBackend`** records `tracks=[fixed(3),fixed(6);align=Center]`.

**Spec.** `document.md:171`; `view_tree.md:111`; `backend_contract.md:143,217` and the Alignment paragraph;
`qt_quick/backend.md:149`; `tui/frontend.md:87`.

**Tests.**

- **Layout case L12**, "grid tracks of fixed and stretch width": tracks `fixed(10)` and `stretch` in a 40-unit grid
  with gap 0 give cells 10 and 30 wide. **Control:** ignore `setTracks`, and both are 20.
- **Layout case L13**, "a centred track centres a label-width cell": a badge in a `fixed(12)`, `center` track has
  equal space either side, within a unit. **Control:** ignore `align`.
- Corpus: `columns` with `tracks` is refused; `align: stretch` on a track is refused.

**Gallery.** Instrument's events become a two-track grid (`fixed(18)`, `stretch`); the plate's grid is `fixed(3)` and
twelve `fixed(6)`, `center` tracks, and its headers centre over the wells.

**Risks.** None outside grids that give `tracks`.

## G18: an overflow menu behind an icon button

**Problem (verified by trying, gap log).** `menu` is a menu bar: each top-level entry is a button drawn by its label
(`MorphMenu.qml`, `qt_quick/backend.md:147`; the terminal's list, `tui/frontend.md:85`). An entry part is
`{label, icon, keys, checked, enabled, onClick, items}` (`document.hpp:750`): no separator, one string, no
`visible`. The head's `actions` are buttons only (`document.md:91-100`). Two screens hand-roll the same overflow,
a one-entry menu bar standing in for a button: instrument's "More actions" (`instrument.cpp:104-117`) and samples'
bulk "⋯" (`samples.cpp:511`). The console's "☰ Menu" (`console.cpp:475`) is the same trick for its navigation.

**Decision, part 1 (D2): a menu drawn as one button.**

| Key | Where | Shape | Meaning |
|---|---|---|---|
| `style` | `menu` | literal `bar` (default) or `button` | `button`: one button that opens the entries as a popup |
| `label`, `icon`, `emphasis` | `menu` | as a `button`'s | The trigger, for `style: button` |
| `separator` | entry part | literal `true` | A line between entries; the part takes no other key |
| `description` | entry part | expression, text | A muted line with the entry: "needs reason" |
| `visible` | entry part | condition | Hidden entries are left out |

- A button-style menu has a `label` or an `a11y.name`; an icon-only trigger without either is refused, since it
  would have no accessible name. A label given with an icon is the accessible name and the tooltip when the theme
  draws the icon alone.
- `more` joins the framework icon names (`theme.md`, Icons; `kFrameworkIcons`): drawn from the theme's set when it
  names it, and as `⋯` otherwise, on both frontends. An icon-only trigger with any other icon the set does not name
  draws its accessible name as its label, so a trigger is never blank.
- `description` is C's word for a line of help under a thing.
- Separators the hidden entries leave at the start or end of a level, or next to another separator, are not sent.
- Author: `ui::menuButton(MenuButtonOptions{.label, .icon, .emphasis}, items)`; `MenuItem` gains `separator`,
  `description` and `visible`.

**View tree and contract.**

- `Menu` gains `MenuStyle style` (`Bar`, `Button`), `Prop<std::string> label`, `std::string icon`,
  `ButtonEmphasis emphasis`. `MenuItem` gains `bool separator`, `Prop<std::string> description`, `Prop<bool>
  visible`. `MenuEntry` gains `separator` and `description`.
- `IViewBackend::createMenu(parent, MenuStyle)`; `MenuWidget::setTrigger(std::string_view label, std::string_view
  icon, ButtonEmphasis)`, called for a button-style menu at mount and when the label changes.
- The mount sends only visible entries, and maps an activated path back through the visible ones.
- A separator is never chosen and never opens anything. `setOnActivate`'s paths count separators, so a path names the
  same entry whichever backend reports it.

**Per backend.**

- **Qt Quick.** `MorphMenu` with `buttonStyle` draws one `MorphButton`-styled button (its emphasis, icon and label) and
  opens the popup at the root level. A separator is a `MenuSeparator`; a description is a second, muted line in
  `Theme.bodyFont`. The popup's entries are `Accessible.MenuItem`.
- **Terminal.** A button-style menu draws `[label ▾]`, or `[⋯]` for an icon-only one. Enter, Space or a click opens the
  list overlay the dropdown select uses, holding the menu's own list, so submenus open and close in it as they do in
  a bar (Right, Enter or Space opens one, Left or Backspace goes up). Esc or a choice closes the overlay and hands the
  focus back to the trigger. A separator is a `─` row the cursor skips; a description follows the label, muted.
- **`RecordingBackend`** records `style=Button`, the trigger, and `;separator` and `;description=…` on entries;
  `chooseMenuEntry` refuses a separator.

**Decision, part 2 (D3): the head's own menu.** The screen head gains `menu`, parts as a menu's entries, each entry
that is chosen (not a submenu, not a separator) with a required `id`, unique across the head's menu, as a head
action has. The workspace draws them as a button-style menu after the actions, icon `more`, accessible name
`menuLabel` (an expression, default `"More actions"`). Like the actions, the head menu is data:

- `ScreenHead::menu()` is a nullable `reactive::Computed<std::vector<HeadMenuEntry>> const*`: the visible entries,
  each a `MenuEntry` with its `id`.
- `ScreenHead::choose(entryId)` runs the entry's `onClick` as a user event in the screen scope, in one batch, and
  reports an unknown, hidden or disabled id as `run` does (`head menu entry <id> is unknown`, `… is hidden`,
  `… is disabled`). The workspace maps the path the widget reports to the id through the entries it last sent, so an
  entry hidden between drawing and choosing is refused by name, never mistaken for its neighbour.
- A host screen's head returns null and shows no menu. The app document refuses `menu` and `menuLabel`, as it
  refuses the other head keys.
- Author: `screen.menu(MenuItem)` appends an entry, as `screen.action` appends an action.

Batch A's note that the overflow "can later collapse the head's actions into it on `compact`" is met by `visible` on
actions and entries; the workspace collapses nothing by itself (open question 5).

**Spec.** `document.md:169` and The head; `view_tree.md:109`; `backend_contract.md:77` (Menus) and the
`MenuWidget` row; `host.md`, `ScreenHead`; `workspace.md`, the head row; `qt_quick/backend.md:147`;
`tui/frontend.md:85`.

**Tests.**

- **Conformance case 52**, "a button-style menu reports the entry past a separator, and never the separator": entries
  `[A, separator, B]`, the trigger labelled `More`; `textOf(menu)` reads `More`, `chooseMenuEntry([2])` reports
  `[2]`, `chooseMenuEntry([1])` reports nothing. `chooseMenuEntry` opens a button-style menu through its trigger
  first; `textOf` gains "a button-style menu's trigger label". **Control:** a backend that treats the separator as an
  entry reports `[1]`; one that ignores `MenuStyle::Button` and draws a bar reads an empty text.
- `tests/test_ui_mount.cpp`: a hidden entry is not sent, and activating the shown entry after it runs the right
  handler; a separator left first by a hidden entry is not sent. **Control:** map the path without skipping hidden
  entries.
- `tests/test_ui_workspace.cpp`: a head with `menu` shows `head_menu`; choosing an entry runs it; an entry hidden after
  the menu was drawn is refused with "is hidden", and its neighbour does not run; `activeHeadShown()` is true for a
  head with a menu only.
- Corpus: an icon-only button-style menu without `label` or `a11y` is refused; `separator` with a `label` is refused.

**Gallery.** Instrument's "More actions" becomes the head's menu, with a separator before "Take out of service…" and
`description: "needs reason"`; samples' bulk "⋯" becomes a button-style menu; the console's menu goes with G56.

**Risks.** `createMenu` gains a parameter; four implementations and the echoing test backend change.

## G27: removable filter chips

**Problem (read).** `badge` has `tone`, `text` and `icon`, no action (`document.hpp:870`). The audit screen draws a
chip as a row of a text and a ghost "×" on the `card` surface (`audit.cpp:215-222`): two widgets, larger than the
design's 26 px chip, and not one control to assistive technology. One design asks for it.

**Decision.** A badge takes `onRemove`, and then draws a remove control after its text. A chip is a badge: an
outlined one is C's `style: outline`. No `chip` kind.

**Vocabulary.** `badge` gains `onRemove` (commands) and `removeLabel` (expression, the control's accessible name;
default `"Remove " + text`, English like `selectAllLabel`). Author: `ui::badge(…).onRemove(commands)` or
`BadgeOptions::onRemove`.

**View tree and contract.** `Badge` gains `Action onRemove` and `Prop<std::string> removeLabel`.
`BadgeWidget::setOnRemove(Action, std::string_view label)` is pure; the mount calls it only when `onRemove` is given.
A removable badge takes focus; Delete, Backspace, Enter or Space on it, or a click on its control, runs `onRemove`.
`ConformanceProbe::dismiss` covers it: dismissing a removable badge is the user removing it. The control's icon is
the framework name `close`, the one a dismissible banner draws (`×`). A badge without `onRemove` takes no focus, as
now.

A chip removed while it had the focus is destroyed with it, and the frontend puts the focus where it puts it after any
destroyed widget. A document that wants it on the next chip says so in `onRemove`, with `focus` and G35's `row`.

**Per backend.**

- **Qt Quick.** `MorphBadge` gains a small `ToolButton` drawing `close`, `Accessible.name` the remove label, at
  `Theme.spaceOf(1)` after the text; the badge's focus target is the button.
- **Terminal.** `[Result ×]`, or `(Result ×)` for C's outlined badge; the `×` cell is the click target; the badge is
  focusable.
- **`RecordingBackend`** records `removable=Remove Result`; `dismiss(badge)` runs the handler.

**Spec.** `document.md:185`; `view_tree.md:125`; `backend_contract.md:230`, and `dismiss` in the probe section;
`qt_quick/backend.md` and `tui/frontend.md:100`.

**Tests.**

- **Conformance case 53**, "a removable badge reports its removal once": `dismiss(badge)` runs `onRemove` once; a
  badge without it takes no dismissal. **Control:** a backend that never calls the handler.
- `tests/qt_quick/test_qt_quick_widgets.cpp`: the remove button's accessible name is `Remove Result`; the chip with
  C's outline is no taller than `Theme.targetHeight − 8` (with or after C).
- Corpus: a badge with `onRemove` inside an item button's content is refused (with or after C's G52).

**Gallery.** Audit's chips become outlined, removable badges; `chip` goes.

**Risks.** None outside badges that give `onRemove`. The first item to drop if the review wants a smaller batch.

## G26: a search and filters in the screen's head

**Problem (read).** The head is `subtitle`, `status` and `actions` (`document.md:63,91-100`), evaluated by the screen
and drawn by the workspace's head row (`include/morph/ui/workspace.hpp`, `buildHeadRow`). No input can sit there.
Four screens put the same row of inputs under the head instead: audit (`audit.cpp:298-308`), samples
(`samples.cpp:457`), plate's template select (`plate.cpp:488`) and the console's lab select and sample search
(`console.cpp:489-505`).

**Decision: head tools, as data.** The head gains `tools`, parts of two closed kinds: a search field and a select.
Each is evaluated in the screen scope and drawn by the workspace, as an action is. A node of the screen mounted in
the shell's widgets would break the head's contract: it crosses `ScreenHandle`, which a host screen implements, and
the head row is mounted by the shell and rebuilt on a tab switch, outliving no screen.

**Vocabulary.** The head gains `tools`, parts `headTool`:

| Key | Kinds | Shape |
|---|---|---|
| `id` | both | name, required, unique among the tools |
| `kind` | both | `search` or `select`, required |
| `label` | both | expression, required: the accessible name |
| `value`, `placeholder`, `onChange`, `onSubmit` | search | expression; expression; commands (`event` the text); commands |
| `options`, `selected`, `style`, `onChoose` | select | as a `select`'s; `style` `dropdown` or C's `segmented` |
| `width` | both | a literal integer of units; default 30 for a search, content for a select |
| `visible`, `enabled` | both | conditions |

Values are controlled: a search shows `value` and reports each edit through `onChange`, as a bound text input does.
The app document refuses `tools`, as it refuses the other head keys. Author: `screen.tool(HeadSearchOptions)` and
`screen.tool(HeadSelectOptions)`, appending as `screen.action` does; `filter` is already an expression form's name.

**Host and workspace.**

- `HeadTool{id, kind, label, value, placeholder, options, selected, style, width, enabled}`, beside `HeadAction` in
  `interpreter.hpp`.
- `ScreenHead::tools()`, a nullable `reactive::Computed<std::vector<HeadTool>> const*`, and
  `ScreenHead::edit(toolId, Value)` (a search's text, a select's key) and `submit(toolId)`, which run the commands as a
  user event in one batch and report an unknown, hidden or disabled id as `run` does.
- `buildHeadRow` draws, in order: the subtitle, the badges, the spacer, the tools, the actions, the menu (G18). Each
  tool is a `TextInput` or `Select` keyed by id (`head_tool_<id>`), its value bound to the tool's, its caption hidden
  and its `label` its accessible name.
- A head has content, for the row's own visibility and for `activeHeadShown()`, when it has a subtitle, a badge, an
  action, a tool or a menu entry.
- The head line still scrolls sideways when it is too wide (batch A).

**Spec.** `document.md`, The head; `host.md`, `ScreenHead`; `workspace.md`, the head row; `interpreter.md`, the
head's tools are controlled.

**Tests.**

- `tests/test_ui_workspace.cpp`: a head with a search bound to `q` shows `head_tool_q`; typing `HPLC` runs `onChange`
  with it and the field shows the state's value; a select's choice runs `onChoose`; a hidden tool is not drawn.
  **Control:** drop the tools from `buildHeadRow`, and the lookup fails.
- `tests/test_ui_host.cpp`: `edit` of an unknown id answers false with "head tool x is unknown".
- `tests/client/`: through `mountShell`, the tool sits in `shell_head`, and `activeHeadShown()` is true for a head with
  tools only.

**Gallery.** Audit, samples, plate and the console move their search and filters into the head on their expanded
layouts, and the in-page rows go there. On compact the designs draw them in the page, so the head tools there are
hidden by `visible` and the in-page row stays.

**Risks.** A head with tools takes more of the top bar's second line; it scrolls when it must. Two closed kinds may be
too few (a date range); open question 6.

## G4: an overlay that becomes a sheet on compact

**Problem (verified by trying, gap log).** A `dialog` takes no size bounds (`document.md:244-247`). On Qt Quick a
literal `sizing: fixed(n)` sets its width (`MorphDialog.qml:57-62`), and it is a centred popup at any width. The
terminal ignores the sizing: its box is the content's natural size, centred (`OverlayCore::frameSize`,
`src/tui/container_widgets.cpp:637-645`; read). The e-signature screen draws its compact sheet in the page under a
`switch` on the open state, with its own Cancel row (`esignature.cpp:183-201`), and so loses the dialog's `ready` and
`submitted`.

**Decision.** A dialog takes `sheet`, a condition: while true it is drawn as a sheet that fills the window, with a head
of its own holding a dismiss control and the title, and its footer's buttons across the width. The document chooses
by the width class; the frontend does not decide.

- A sheet needs its own dismiss control: a phone has no Escape and no outside to press.
- `dismissLabel` (expression, default `"Close"`) labels it. It dismisses exactly as Escape does: the dialog closes,
  then `onDismiss` and `cancel` run.
- A drawer takes no `sheet`: on compact it already fills the height, and its width is the frontend's.

**Vocabulary.** `dialog` gains `sheet` (condition) and `dismissLabel` (expression). Author: `DialogOptions::sheet` and
`dismissLabel`.

**View tree and contract.** `Dialog` gains `Prop<bool> sheet` and `Prop<std::string> dismissLabel`.
`DialogWidget::setSheet(bool)` and `setDismissLabel(std::string_view)` are pure; the mount calls them when bound or
not default. A sheet keeps the dialog's every rule: the focus trap, the footer, the dismissal; changing `sheet` while
open keeps the content mounted and its state.

**Per backend.**

- **Qt Quick.** `MorphDialog` with `sheet`: the `Popup` anchors fill `Overlay.overlay` with no margin and radius 0;
  `closePolicy` is `CloseOnEscape` only. A head row holds a ghost button (`dismissLabel`) and the title, with C's
  description under it. The footer's buttons fill the width equally. The content scrolls between head and footer,
  as now.
- **Terminal.** The box fills the viewport. Its first row draws `[dismissLabel]` before the title; a click on it
  dismisses. The footer's buttons share the bottom row.
- **`RecordingBackend`** records `sheet=true` and `dismissLabel`.

**Spec.** `document.md:177` and the load-time rule on overlay bounds (a sheet is the window's size);
`view_tree.md:117`; `backend_contract.md:148,222`; `qt_quick/backend.md:156`; `tui/frontend.md:92`.

**Tests.**

- **Layout case L14**, "a sheet fills the surface": an open dialog with `setSheet(true)` holding a stretching text is at
  least as wide as a stretching root beside it, less the padding. **Control:** ignore `setSheet`, and the text is its
  natural width.
- `tests/qt_quick/test_qt_quick_overlays.cpp`: a sheet's popup is the window's size; clicking its dismiss button
  closes it and reports one dismissal; a press outside is not possible and Escape still dismisses. Turning `sheet`
  off while open keeps a typed text.
- `tests/tui/test_tui_dialog.cpp`: a sheet covers the screen and `[Cancel]` dismisses on click.
- `tests/test_ui_interpreter_control.cpp`: `sheet` bound to `eq(env widthClass, "compact")` records `sheet=true` after
  `setWidthClass("compact")`.

**Gallery.** E-signature has one dialog for both layouts, a sheet on compact; `compactSheet` and the `switch` go.

**Risks.** A restyled `MorphDialog` that does not declare `sheet` stays a centred popup, and a sheet then has no head
dismiss control; the restyle check reports an undeclared property, as for every other.

## The client mounts the app document (#1063)

D4's first item, a defect fix that G56's counts depend on. It stands alone and may land before the rest of the batch.

**Problem (read, not run).** `interpreter.md` (The app root scope) has `Interpreter::mountApp(doc)` mount the app
document for the session: while the `AppRoot` lives, `{"ref": "app.name"}` in any screen reads its state, and its
queries run once. The generic client loads the app document (`src/client/shell.cpp:441-453`, `loadDocuments`) and
keeps it for the explorer only (`shell.cpp:644-645`). Nothing outside the tests calls `mountApp`:

```
$ git grep -n 'mountApp(' 1e4c9cf34 -- src include examples
1e4c9cf34:include/morph/ui/interpreter.hpp:3044:    [[nodiscard]] std::expected<std::unique_ptr<AppRoot>, Refusal> mountApp(std::shared_ptr<Document const> doc);
1e4c9cf34:include/morph/ui/interpreter.hpp:7838:inline std::expected<std::unique_ptr<AppRoot>, Refusal> Interpreter::mountApp(std::shared_ptr<Document const> doc) {
```

So under `morph::client` no app state, `let`, query or `onMount` is ever set up, and `app.*` reads nothing. A client
cannot mount it either: the interpreter a `DocumentScreenHost` mounts through is private to it (`host.hpp`,
`_interpreter`).

**Decision.**

- `DocumentScreenHost::mountApp(std::shared_ptr<Document const>) -> std::expected<std::unique_ptr<AppRoot>,
  MountError>` mounts the app document through the host's own interpreter, so its commands reach the same
  `InterpreterHost` (its `navigate` is the host's) and its diagnostics the host's `diagnostics` callback. The
  `AppRoot` is destroyed before its host.
- The client mounts it once the frame and its workspace are built and before the first tab, each time it builds
  them, and destroys it after the screens and before the session changes hands, as `interpreter.md` orders:
  - `buildFrame` destroys the old root after it destroyed the old workspace's tabs, and mounts the current app
    document after;
  - sign-out destroys it after `forceCloseAll`, before the source signs out;
  - the shell's destructor destroys it after the workspace and before the host.
- A refused app document is reported through `onProblem`, and the screens mount without it, as a screen whose
  document does not load is left out.
- A development reload of the app document is reported and takes effect at the next frame build; the root is not
  remounted in place.

**Spec.** `host.md`, `DocumentScreenHost` (`mountApp`); `client/client.md`, The shell (the app document's mount and
its order).

**Tests.** `tests/client/`: an app document declaring a state `x` with an init of 7, and a screen whose text reads
`app.x`, served through `mountShell`: the text reads `7`; after a sign-out and sign-in it reads `7` again from the new
root, and the old root's `onUnmount` ran once. **Control:** drop the `mountApp` call, and the text is empty.

## G50: `navigate` as a dialog that answers its opener

**Problem.**

- **Reproduced by the gap log** (test `review: approve and sign opens the e-signature screen as a tab`): the generic
  client opens every `navigate` as a tab. Its host overrides only `onNavigate(screenId, params)`
  (`src/client/shell.cpp:50-64`), so `NavigateRequest::as` (`interpreter.hpp:186-195`) is dropped, and no report is
  made.
- **Read, not in the gap log.** The call protocol already works through a tab: `DocumentScreenHost::navigate` files
  the call and the next mount adopts it (`host.hpp:955-970`, `mountAt` → `adoptCall`, `:550-563`). But after the
  callee's `return`, the handle destroys the screen and calls `onScreenClosed` (`host.hpp:972-986`, `finish`,
  `:888-905`), which the client's host does not override. So the tab stays, its page empty, titled with the screen's
  last title.
- Design 01 never `return`s, so no `onResult` could fire (gallery).

**Decision.** The library's workspace shows a navigated screen as a dialog or a drawer, and drops a tab or overlay
whose screen closed itself. Everything stays inside `host.md`'s protocol:

- **The call.** `navigate` files the call as now. The overlay mounts the callee with `mountInto`, so the mount adopts
  the call. An overlay always mounts a new instance: it never focuses an open tab by identity, so a call is never left
  unclaimed. A catalog `singleton` limits tabs; an overlay is not a tab, so a singleton screen open in a tab may also
  be open in an overlay.
- **Documents only.** An overlay mounts a document screen. A catalog entry that names a `host` screen asked for as a
  dialog or drawer opens on the tab path, and the substitution is reported once per screen; a host screen adopts no
  call, so its call is settled as below.
- **The overlay.** `Workspace::openOverlay(NavigateRequest)` makes a `DialogWidget` (or a `DrawerWidget` on the end
  side) under the workspace's parent, titled by the handle's `title()`, with the screen's subtitle as C's description,
  and mounts the screen into it. The callee's head actions become the overlay's footer buttons, built from
  `head()->actions()` as the head row builds them. Its badges are not shown. Its head menu and tools (D3) are not
  shown either, and the workspace reports once per overlay that it left them out, naming the screen. A dialog is a
  sheet while the backend's `widthClass()` is `compact` (G4), and follows the class while it is open.
- **Closing.** A user's dismissal closes the dialog first (`DialogWidget`'s contract). The workspace opens it again at
  once, inside the dismissal, so the callee stays reachable, and calls `requestClose`; the overlay goes only when that
  answers `true`. So "a host never destroys a handle whose close is pending" holds, and a dirty callee without a
  guard stays. A close the callee refuses leaves the overlay as it was, with no step closed in between.
- **Answers.** `return` and `close: self` reach `onScreenClosed(handle)`; the client's host forwards it to
  `Workspace::screenClosed(handle)`, which removes the overlay, or the tab whose screen it was.
- **Lifetimes.** An overlay's handle is destroyed before its `DialogWidget` or `DrawerWidget`, as `mountInto` requires
  of any container. The workspace destroys its overlays before its tabs, newest first, in its destructor and in
  `forceCloseAll`; so a sign-out, a rebuild of the frame and the shell's own destruction close a callee before the
  opener its call answers, and the opener's `onCancel` runs into a live scope, as closing tabs newest first already
  does. `closeAll`, `closeOthers` and `closeSaved` walk tabs only: a modal overlay is open over them, so no user
  reaches those walks while one is.
- **`as: window`** opens a tab in the generic client, which has one window; `client.md` says so, and the client
  reports the substitution once per session at `info`.
- **Routes and reload.** An overlay adds no history entry and is not `onMounted`'s screen; it is not the shown page. A
  development reload remounts tabs; an overlay keeps its document until it closes.
- **Nesting.** A callee may navigate as a dialog again; overlays stack, and each closes on its own.

**API.** `Workspace::openOverlay(NavigateRequest const&) -> std::expected<void, MountError>`,
`Workspace::screenClosed(ScreenHandle&)`, `Workspace::overlays()` (for tests: each overlay's screen id),
`DocumentScreenHost::settleCall(screenId, params)`. The client's `Host` overrides `onNavigate(NavigateRequest)`
(dialog and drawer to the workspace, the rest to the tab path, posted as now) and `onScreenClosed`.

**Spec.** `host.md` (the `navigate` and Call and return bullets name the workspace as such a host); `workspace.md`,
a new "Overlays" section; `client/client.md:76-77` ("opens it as a tab" becomes "as a tab, or as a dialog or drawer
when `as` asks; `window` as a tab"; a screen that closes itself loses its tab).

**Tests.**

- `tests/test_ui_workspace.cpp` (RecordingBackend):
  - `openOverlay` with `as: dialog` records a `Dialog`, open, titled by the callee, holding its root; the tab bar is
    unchanged;
  - the callee's `return 42` runs the opener's `onResult` with 42 and removes the dialog;
  - a dismissal of a clean callee runs `onCancel`; of a dirty one keeps it open and records `open=true` again;
  - `setWidthClass("compact")` before opening records `sheet=true`;
  - `forceCloseAll` with an overlay open runs the opener's `onCancel`, then closes the tabs; the overlay's handle is
    gone before its dialog. **Control:** close the tabs first, and `onCancel` finds no opener and runs nothing;
  - a callee whose head has a menu and a tool reports once that they were left out.
  - **Control:** route `as` to the tab path, and the first check fails.
- `tests/client/`: through `mountShell`, the review's approve opens a dialog, not a tab; a screen opened as a tab that
  `return`s loses its tab. **Control:** drop the `onScreenClosed` override, and the tab stays.
- `tests/test_ui_host.cpp` keeps its call-and-return cases unchanged.

**Gallery.** E-signature takes a `worksheet` param and `return`s the signature's meaning on sign; review's approve
navigates as a dialog with `onResult` that marks the worksheet signed. Expanded, e-signature still shows its own
dialog when opened as a screen of its own.

**Risks.** An application whose documents used `as: dialog` and relied on getting a tab now gets a dialog in the
generic client. That is what the document said.

### A call whose navigation mounted nothing (#1064)

**Problem (read, not reproduced).** A call waits in `DocumentScreenHost::_unclaimed` until a mount of that screen
with equal params adopts it (`host.hpp:989-1000`). The workspace's `open` focuses an open tab instead of mounting when
the screen is a `singleton` or an open tab has the same identity (`workspace_model.hpp`, `open`). The call then stays
unclaimed, so neither `onResult` nor `onCancel` runs, against `host.md`'s "answered exactly once". Read further: the
stale call stays in the list, and the next mount of that screen with those params, however it is opened, adopts it.
That screen then answers an opener that did not open it, and its own `close: self` closes it as a callee would, where
it would otherwise go to the application's host. A refused mount, and a navigation the shell drops while a sign-in
reloads, leave a call the same way.

**Decision.** A navigation that mounts nothing answers its call with a cancel.

- `DocumentScreenHost::settleCall(screenId, params)`, protected, cancels the oldest unclaimed call of that screen and
  params, the one the mount would have adopted: `onCancel` runs, and the cancel is reported at `info` ("navigate to
  <screen> mounted no screen; its call was cancelled").
- `NavigateRequest` gains `call`, true when the `navigate` filed one. A host calls `settleCall` for each such
  navigation it handled that mounted no document screen, and for no other. It is one call per navigation, oldest
  first as adoption is, so a second navigation to the same screen, filed before the first was handled, keeps its own
  call for its own mount.
- `Workspace::open` and `WorkspaceModel::open` say whether they mounted a tab or focused one (a `mounted` flag beside
  the tab). The client settles the call when they focused one, when the mount was refused, when the screen is a host
  screen (which adopts no call), and for a navigation it drops while a sign-in reloads. `forceCloseAll` cancels every
  unclaimed call, so none outlives the session's screens.
- The focused tab never adopts the call: a screen the user opened for their own purpose would otherwise answer, and
  close itself, for an opener it never saw (open question 10).

**Tests.** First a `tests/test_ui_workspace.cpp` case that reproduces it: a singleton screen open in a tab, a
`navigate` to it with `onResult` and `onCancel` from another tab, then the singleton's `return`. Today neither runs.
With the change `onCancel` runs once and the singleton's `return` goes to the application's host. **Control:** drop
the `settleCall` call, and neither runs. If the reproduction shows the call already answered, this sub-item is
dropped and the issue closed as invalid.

## G56: the application's navigation, with counts and a compact form

**Problem.**

- **The gap log's framing is wrong (read).** The console's sidebar (`console.cpp:440-475`) is not a navigation of the
  screen's own: its entries open the other gallery screens, and design 10 draws the whole application frame, a `<nav
  aria-label="Main">` with the logo, then the page with its header (`10-Console.dc.html`). It is the shell's navigator,
  redrawn because the navigator lacks two things.
- **Counts (read).** `Navigator` groups catalog entries by module and group with headings
  (`include/morph/ui/navigator.hpp`); an entry is an item button with no count. "Review 4" and "Instruments 1" are
  live data.
- **No compact form (reproduced).** `ShellFrame`'s sidebar is always `kShellSidebarWidth` (28) units
  (`shell_frame.hpp:23,82`), and nothing in the frame or the client reads the width class beyond writing the env. In the
  terminal probe at 60 columns, a `compact` window, the page starts at column 30: the sidebar takes half the screen,
  and the "compact" layout gets about 30 units.
- **The app document is never mounted (read; a prerequisite).** Live counts belong to the app document, whose queries
  run once per session (`interpreter.md`, The app root scope). The generic client never mounts it, so `app.*` reads
  nothing there today; D4's first item fixes that.
- A screen's own sub-navigation (sections of one screen) is expressible today: item-style buttons and a `switch`, or
  `tabs`. Nothing is added for it.

**Decision.**

- **Counts.** The app document gains `navigation`, parts `{screen, text, tone, visible}`: `screen` a screen id
  literal, the rest expressions in the app scope. `AppRoot::navigationBadges()` is a `Computed` of the visible ones.
  `Navigator::setBadge(screenId, std::optional<NavigatorBadge{text, tone}>)` draws a badge at the end of the entry's
  line (the entry becomes a row of the item and the badge). A screen the navigator does not list is reported once.
  The entry stays one button to assistive technology, and its accessible name carries the count: "Review, 4".
- **The client copies the counts** with an effect over `navigationBadges()`, made after the app document is mounted
  (D4's first item) and the frame built, and destroyed before either goes, so it never writes into a navigator that
  is gone or reads a root that is.
- **The compact shell.** `ShellFrame::setCompact(bool)`: the sidebar and the principal hide, and the top bar's first
  line starts with a button-style menu (G18) with the icon `nav.menu`, a new framework name drawn as `☰` when the
  theme's set does not name it, and the accessible name "Navigation". It holds the navigator's groups as submenus, its
  pins first, each count after its label ("Review · 4"), and the current screen's entry checked.
  `Navigator::menuEntries()` and `Navigator::choose(path)` give the frame that menu. The client calls
  `setCompact(widthClass != "expanded")` from the listener it already has (`followWidthClass`, `shell.cpp:230-237`):
  the frame takes its compact form on `compact` and `medium`.
- **Which width the class measures.** The class stays the window's, as batch A defined it. Below `expanded` the
  sidebar is gone, so the page is nearly the window, and a document's class describes the space it gets. With the
  sidebar on `medium`, an 80-unit window, an 80-column terminal among them, would give the page 52 units, narrower
  than any `compact` window. On `expanded` the page is the window less 28 units, at least 132.

**Spec.** `document.md`, the app document's `navigation`; `interpreter.md`, The app root scope;
`workspace.md`, Navigator and pins (badges, `menuEntries`); `shell_frame.md` (the compact form, a Shape row and a
Design decisions row); `client/client.md` (mounts the app document; the frame's compact form).

**Tests.**

- `tests/test_ui_workspace_start.cpp` (where the navigator is tested): `setBadge("lims.review", {"4", Info})` records
  a badge `4` after `nav_lims.review`; `nullopt` removes it; the entry's accessible name is "Review, 4";
  `menuEntries()` lists groups with "Review · 4".
- `tests/test_ui_shell_frame.cpp`: `setCompact(true)` hides `shell_sidebar` and shows `shell_nav_menu`; choosing an
  entry calls `onOpen`. **Control:** a no-op `setCompact`, and the sidebar stays visible.
- `tests/client/`: through `mountShell`, `setWidthClass("medium")` hides the sidebar and `"expanded"` shows it.
  **Control:** compact the frame on `"compact"` only, and the medium window keeps its sidebar.
- `tests/client/`: an app document whose `navigation` counts a state shows the badge, and it follows the state.
  **Control:** drop the effect, and no badge is drawn.
- `tests/tui/test_tui_frontend.cpp`: at 60 and at 80 columns the page starts at column 2, not 30; at 160 columns at
  column 30.

**Gallery.** The gallery gains an app document with the console's counts. The console drops its sidebar and its
"☰ Menu"; its lab select and sample search move to the head (G26); its "Connected" tag stays a head badge.

**Risks.** Mounting the app document runs its `onMount` and queries in every client session that has one; that is
what the spec says the session does, and no example in the tree ships an app document with queries. A compact window
loses the navigator's search; the menu lists every entry.

---

## Totals

- **Conformance.** `conformanceCases()` goes from 50 (after B and C) to 53: case 51 (G51), 52 (G18), 53 (G27).
  `test_ui_conformance.cpp`'s count becomes 53. Run on all three backends.
- **Layout cases.** `layoutConformanceCases()` goes from 10 (after B) to 15: L11 and L15 (G16), L12 and L13 (G23),
  L14 (G4).
  Run on Qt Quick and the terminal.
- **`ConformanceProbe`.** No new operation. `textOf` reads a Tabs' highlighted label and a button-style menu's trigger
  label; `dismiss` covers a removable badge; `chooseMenuEntry` opens a button-style menu through its trigger.
- **New pure virtuals**, four implementations each (Qt Quick, terminal, `RecordingBackend`, the test-only
  `EchoingBackend`): `SlotWidget::setScrollable`, `MenuWidget::setTrigger`, `GridWidget::setTracks`,
  `BadgeWidget::setOnRemove`,
  `DialogWidget::setSheet` and `setDismissLabel`; `createMenu` gains `MenuStyle`.
- **`ScreenHead`** gains nullable `menu()`, `tools()` and the `choose`, `edit`, `submit` members, with defaults, so a
  host screen's head compiles unchanged.
- **Framework icon names** (`kFrameworkIcons`, `theme.md`, Icons) gain `more` (`⋯`) and `nav.menu` (`☰`), with their
  terminal glyphs; the removable badge draws the existing `close`.
- **`DocumentScreenHost`** gains `mountApp` and the protected `settleCall`; `NavigateRequest` gains `call`;
  `Workspace::open` reports whether it mounted.

## Not in this batch

- **A "More ▾" tab** (G20): the bar scrolls to the chosen tab instead. A frontend may draw a menu later without a
  vocabulary change.
- **Shapes** (G32's dashed outline, B's round well): no node carries a shape.
- **A `stepper` node** (G38): an author helper over existing nodes.
- **A screen-level footer node** (G16): a column and a scroll already say it.
- **A screen's own navigation component** (G56): item buttons, a `switch` and `tabs` express it.

## Open questions

1. **Sheets by default on compact.** Should every dialog be a sheet on `compact` unless it says otherwise?
   **Default:** no; `sheet` is the document's choice, and only the host's navigated overlays follow the class
   themselves. A small confirm dialog reads better centred on a phone.
2. **`dismissLabel`'s default.** `"Close"` is English text in the library, as `selectAllLabel` is in B. **Default:**
   keep it, and document `t` for translation.
3. **Decided: the workspace scrolls a content-height page.** It scrolls a screen's root that does not stretch in
   height, so no screen is ever cut off; a root that stretches keeps its own layout, the pinned-bar idiom among them
   (G16, `SlotWidget::setScrollable`, L15).
4. **Focus into dialogs.** G35 reaches into switch cases only. A focus into a closed dialog would always report "not
   mounted". **Default:** leave dialogs out.
5. **Collapsing head actions on compact.** Should the workspace move a head's non-primary actions into its menu while
   the class is `compact`? **Default:** no; the author's `visible` decides, as the gallery's designs differ per screen.
6. **More head tool kinds.** A date range or a toggle? **Default:** search and select only, until a design needs more.
7. **Decided: `as: window` in the generic client** opens a tab, and the client reports the substitution once per
   session at `info`, as it reports a host screen asked for as a dialog. A substitution the document cannot see is
   still one its author should hear about.
8. **Decided: the frame is compact below `expanded`.** Its sidebar collapses into the navigation menu on `compact`
   and `medium`. With the sidebar, an 80-unit `medium` window would leave the page 52 units, narrower than any
   `compact` window; without it the page is nearly the window. On `expanded` the page is the window less 28 units, at
   least 132 (G56).
9. **The order of D's parts against B and C.** D1 needs nothing of B or C except G35's row example. **Default:** D1 may
   land beside B and C; D2 after B part 1, and before or after C; D4's first item at any time.
10. **Decided: a call to a screen whose tab is already open is cancelled.** The navigation mounted nothing, so its
    call, the oldest unclaimed one of that screen and params, is answered with `onCancel`, exactly once (G50). A
    screen the user opened for their own purpose never starts answering, or closing itself, for an opener it never
    saw.

## Defects this batch fixes

- **The generic client never mounts the app document.** Read, not run. D4's first item, filed on its own since it
  affects every screen that reads `app.*`.
- **A screen that `return`s from a tab leaves an empty tab** (`src/client/shell.cpp:50-64`, `host.hpp:972-986`). Read,
  not run. G50 fixes it, so it belongs to that change.
- **`focus` takes the first widget of a name across scopes** (`interpreter.hpp:5831`, `mount.hpp:258-270`).
  Reproduced (`focused=4` for the screen's field 7). G35 fixes it, so it belongs to that change.
- **A `navigate` with a call to a screen whose tab is already open is never answered**, and its call is adopted by a
  later, unrelated mount. Read, not reproduced. G50's sub-item reproduces it first, then settles it.


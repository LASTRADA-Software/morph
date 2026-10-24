# LIMS gallery batch A: defects and responsive layout

Batch A of the LIMS gallery initiative (`examples/lims_gallery/design/batches.md`): the defects the gallery found
in the declarative UI, and a width class that is actually set. It lands on `feat/lims-gallery` as one commit per
item. The order is below; the gallery drops each workaround in the same commit as the fix that makes it unnecessary.

Every claim below was re-checked against the code at `612f99899` (master `842701f0d` plus the gallery). Each item
says whether its cause was **reproduced** (run on this revision) or **read** (inferred from the code). Where the gap
log was wrong, the item says so.

| Item | Where the fix lands | Verdict on the gap log |
|---|---|---|
| G22 | interpreter | Right, and the same rounding also hits `intOf` and `componentNumber` |
| G25 | `view.hpp`, interpreter | Right |
| G49 | interpreter | Right |
| G12 | interpreter | Right; the two specs disagree, and the fix keeps both meanings by scope |
| G7 | `MorphDialog.qml`, `MorphDrawer.qml` | Right; the drawer has the same defect |
| G21 | `MorphProgress.qml` | Right |
| G34 | `MorphKeyValue.qml` | The symptom is right; the cause the log inferred is wrong |
| G48 | `MorphInputBackground.qml`, terminal field | Right; the specs say nothing either way, so this adds the rule |
| G42 | Qt Quick, terminal, `RecordingBackend` | Reproduced. Both real backends click, and `RecordingBackend` alone runs the chord |
| G15, G40, G54 | `MorphTableRow.qml`, `MorphTable.qml` | Right |
| G1 | `IViewBackend`, the client shell, the three backends, the testkit | Right; `locale`, `platform` and `online` are never set either |
| G5 | `Theme`, the Qt Quick controls | Right |
| G19 | `ShellFrame` | Right |

Order of landing: G22, G25, G49, G12 (interpreter), then G7, G21, G34, G48 (QML only), then G42, the table items, G1,
G5 (which needs G1's width class), and G19.

## G22: `progress` rounds an exact quotient to its own places (#1051)

**Cause (read; the gap log's reproduction stands, not re-run).** `ScreenCore::lowerNode`'s `progress` branch
(`include/morph/ui/interpreter.hpp:6309-6324`) reads a decimal through `number->value.toDouble()`.
`Rational::toDouble()` (`include/morph/util/rational.hpp:478`, body at `:1435-1443`) rounds to the value's own
`decimalPlaces`. `29 / 100` is the exact 29/100 at 0 places, because `div` keeps the operands' places
(`docs/spec/ui/expressions.md:71-74`). So the bar gets `round(0.29) = 0`.

The same reading appears in two more places in the interpreter:

- `componentNumber` (`interpreter.hpp:5481-5492`), which hands a custom component's `number` prop over. Here
  `29 / 100` also crosses as 0, for a decimal and for a quantity.
- `intOf` (`interpreter.hpp:5526-5534`), documented as "a decimal truncated toward zero". It casts `toDouble()`, so
  29/10 at 0 places becomes `round(2.9) = 3`, not 2. `intOf` reads a grid's `columns`, `gap`, `span` and similar
  properties. Read only, not run.

**Fix.** The fix belongs in the interpreter, not in `Rational`. `docs/spec/util/rational.md:326` says `toDouble` is
a display reading and never an exact path, and that holds.

- One helper in `interpreter.hpp`'s `detail`, `realOf(Rational const&)`, returns
  `static_cast<double>(numerator) / static_cast<double>(denominator)`, unrounded.
- `progress`, and the decimal and quantity cases of `componentNumber`, read through `realOf`.
- `intOf` truncates exactly: `numerator / denominator` in integer arithmetic, which truncates toward zero as its
  documentation says.

**Spec.** `docs/spec/ui/view_tree.md:126` (`Progress::value`, 0 to 1) is unchanged. `docs/spec/ui/document.md`'s
`progress` row gains a sentence: a decimal value is read exactly, at whatever places it carries.
`docs/spec/ui/interpreter.md`'s Lowering section states the rule once: wherever the lowering turns a decimal into a
number for the view tree, it uses the exact value, and only display rounds to places.

**Tests** (`tests/test_ui_interpreter_calculation.cpp`).

- A `progress` whose value is `{"fn": "div", "args": [29, 100]}`, mounted on `RecordingBackend`, records `value`
  `0.29`.
- A grid whose `columns` is `{"fn": "div", "args": [29, 10]}` has 2 columns.
- A custom component's `number` prop given 29/100 receives 0.29.
- **Control:** put `toDouble()` back in each of the three readers, and each test fails: 0, 3 and 0.

**Risks.** None outside these readers. A document that worked around the rounding with explicit places
(`decimal(x, 2)`) reads the same value as before.

## G25: `forEach` drops its common properties (#1052)

**Cause (read; the gap log's `testId` reproduction stands).**

- The interpreter lowers `forEach` in `lowerRows` (`interpreter.hpp:7066-7086`). It returns
  `ui::forEach<Value>(rows, keyFn, rowView, axis, gap)` and never calls `commonOf`. `table`, in the same function,
  does call it.
- `ui::forEach` (`include/morph/ui/view.hpp:1395-1421`) has no parameter for a `Common`, although
  `ForEach::common` exists (`view.hpp:871-872`). The mount already applies it: `mountKind(ForEach)` calls
  `applyCommon` (`include/morph/ui/mount.hpp:1124-1130`).

So every common key on a `forEach` (`testId`, `visible`, `enabled`, sizing and bounds, `alignSelf`, `a11y`,
`tooltip`, `surface`, `keys`, `drag`, `drop`, `autofocus`) is loaded, type-checked and then dropped without a
report.

**Fix.**

- Both `ui::forEach` overloads take a trailing `Common common = {}`, which goes into `ForEach::common`. Their
  Doxygen gains the `@param`.
- `lowerRows` builds the node with `commonOf(node, ctx, element, common)`.
- The workspace's head row calls `ui::forEach` positionally and keeps the default.

**Spec.** `docs/spec/ui/view_tree.md`'s `ForEach` row names `common`. `document.md:150-157` is already right: every
node takes the common properties. Nothing else changes.

**Tests** (`tests/test_ui_interpreter_control.cpp`).

- A `forEach` with `testId: "rows"` and `visible: {"ref": "shown"}`. The `Column` with test id `rows` exists, and
  toggling `shown` hides it while its rows stay mounted.
- A `forEach` with `sizing: {"fixed": 20}` records that layout.
- **Control:** drop the `commonOf` call, and the lookup by test id fails.

**Risks.**

- A screen that set a common key on a `forEach` and relied on it being ignored now gets it. The gallery's five
  wrapper columns become redundant and are removed in the same commit.
- A `surface` on a `forEach` now draws a background behind its rows.

## G49: a `splitter`'s `sizes` is dropped (#1055)

**Cause (read; the gap log's reproduction stands).** The splitter branch (`interpreter.hpp:6379-6387`) sets
`children` and `orientation` only. The loader accepts `sizes` as an expression (`include/morph/ui/document.hpp:883`).
`Splitter::sizes` (`view.hpp:1102-1104`), the mount and `MorphSplitter.qml:19-35` all apply sizes. So nothing
between the document and the view reads the key.

**Fix.**

- The branch binds `spec.sizes = bindProp<std::vector<int>>(ctx, node.prop("sizes"), {}, …)`, mapping each list
  item through `intOf`, which reads a decimal exactly after G22.
- A value that is not a list of numbers is reported once for its path and lowers to empty, the same rule the
  lowering applies to a computed `sizing`.
- The document keeps no `onResize`: a user's drag stays the splitter's own (`MorphSplitter.qml`'s `userSized`).

**Spec.**

- `document.md:190`: `sizes` is a list of integers in units, one per pane, and a list of another length shares the
  space equally, as `view_tree.md:132` already says.
- `interpreter.md`: the lowering binds `sizes`.

**Tests.**

- `tests/test_ui_interpreter_control.cpp`: a splitter with `sizes: {"lit": [37, 96]}` records `sizes=[37,96]`.
- The gallery's test `review: the splitter's sizes are dropped by the lowering` is turned around to expect
  `[37,96]`.
- **Control:** remove the binding, and both fail with `sizes=[]`.

**Risks.**

- On Qt Quick, a splitter whose `sizes` add up to more than its width overflows the same way an explicit `fixed`
  pane would. That is the author's choice to make.
- The terminal's splitter (`src/tui/region_widgets.cpp`) already reads `setSizes`. Not run here.

## G12: `close: self` in a dialog asks the host to close the screen (#1048)

**Cause (read; the gap log's reproduction stands).** `ScreenCore::runOne` (`interpreter.hpp:4645-4651`) sends every
`{"close": "self"}` to `InterpreterHost::closeSelf(_doc->id, nullopt)`. The command carries no notion of where it
runs. Two specs give `self` two meanings:

- the author API and `document.md:268-269` say `self` is "the dialog or drawer the command list runs in"
  (`include/morph/ui/author.hpp:1291-1295`, `:2786-2789`);
- `docs/spec/ui/host.md:100-101` and `interpreter.md:36` make `close: self` end a screen that `navigate` opened
  (`tests/test_ui_host.cpp:107`).

The corpus uses the dialog meaning (`tests/ui/corpus/documents.json:158`, a dialog's `default` ending with
`close: self`). It passes only because the corpus never runs it on a host.

**Fix: `self` is resolved by scope, so both meanings hold.**

- A dialog's or drawer's content already runs in a scope whose `owner` is that element (`lowerDialog`,
  `interpreter.hpp:6929`; `RtScope::owner`, `:1706-1707`, documented as "the dialog or drawer whose content it is").
  The children, the `default` and `cancel` keys, and each action's `onClick` all bind in that scope.
- `close: self` walks `ctx.scope` and its `parent`s to the first scope with an `owner`, and closes that element
  exactly as `close: <id>` does. That includes refusing and reporting one whose `open` is an expression.
- Outside every dialog and drawer, `close: self` asks the host as before, so a screen opened as a dialog still ends
  itself.
- `onDismiss` runs in the enclosing scope (`interpreter.hpp:6971-6977`), where the overlay is already closed, so it
  is unaffected.
- A mutation's `onSuccess` runs in the scope that declares the mutation. Declared in a dialog's `scope`, its
  `close: self` closes that dialog. Declared at the screen, it ends the screen. That is the same lexical rule.

**Spec.**

- `document.md:269`: "`close` also `self`: the innermost dialog or drawer whose content holds the command, or the
  screen when there is none".
- `interpreter.md`'s Commands section gets the same rule.
- `host.md:100-101` qualifies `close: self` as "outside any dialog or drawer".
- `author.hpp`'s `Self` and `close(Self)` docs say both cases.

**Tests** (`tests/test_ui_interpreter_commands.cpp`).

- A dialog whose action's `onClick` is `[{"close": "self"}]`. Open it, click the action: the dialog's `open` is
  `false`, and the host's `closeSelf` was not called. The test uses a host that counts calls.
- The same with the dialog's `default`, via `press(…, "Enter")`.
- The same inside a `switch` case inside the dialog, to test the parent walk.
- The existing screen-level case (`testId: "self"` in `kHostDoc`) still reaches `closeSelf`.
- **Control:** restore the unconditional `host->closeSelf`, and the dialog cases report "`close: self` is not
  supported by the host".

**Risks.** A document that already put `close: self` inside a dialog to close a screen opened as a dialog would now
close only the inner dialog. No such document exists in the tree (`grep -rnE 'close\((ui::)?self\)|"close": *"self"'`
finds only the cases above). The gallery's `ui::close(element)` workaround goes back to `ui::close(ui::self)`.

## G7: the Qt Quick dialog draws the style's grey popup (#1047)

**Cause (reproduced).** `MorphDialog.qml`'s `Popup` sets no `background`, so the Basic style's own background
(`palette.window`) draws. `morph_examples_client --app lims-gallery --ui=qt --screen lims-gallery.esignature --grab`
on this revision reads `#efefef` at (600, 262). The drawer (`MorphDrawer.qml`) sets none either, so it is the same
defect; read only, not grabbed.

The dialog's content inherits the scheme of the node that declares the dialog (`MorphWidget.inheritedScheme`). A
dialog declared inside a `sidebar` surface therefore draws the sidebar's light text on whatever the popup is.

**Fix.**

- **Background.** The dialog's `Popup` and the drawer's `Drawer` get a `background`. It is the card:
  `Theme.cardScheme.surface`, border `Theme.cardScheme.border`, and `Theme.radiusCard` on the dialog (the drawer is
  square against its edge), with the card's shadow (`elevation.card`).
- **Padding.** Their padding is `Theme.spaceOf(4)` (16 px), the page padding the shell frame uses.
- **Content scheme.** The content draws with the card scheme whatever the declaring node's surface:
  - `DialogImpl` and the drawer's impl return `Theme.cardScheme` from `scheme()`, which their children inherit,
    instead of the placeholder root's;
  - the title `Label`s colour themselves `scheme.text`.

**Spec.**

- `docs/spec/qt_quick/backend.md` adds a dialog-and-drawer row: drawn as the theme's `card` surface, whatever
  surface encloses the node.
- `docs/spec/ui/theme.md`'s Surfaces table notes that a dialog and a drawer draw on `card`.

**Tests** (`tests/qt_quick/test_qt_quick_theme.cpp`, under `brandTheme()`).

- An open dialog's background item colour is the brand theme's `color.surface`, and its radius is `radius.card`.
- A text inside a dialog declared under a `sidebar` row reads `Theme.cardScheme.text`.
- The same two for a drawer.
- **Control:** remove the `background`, and the colour reads the Basic `palette.window`.

**Risks.** A restyled dialog (`qt_quick::Components` restyle of `MorphDialog`) is unaffected. The default dialog
gets a little more padding, so its content shifts 12 px inward. The terminal is unchanged: its dialog is a box in the
screen's own colours.

## G21: the Qt Quick progress bar ignores the accent (#1050)

**Cause (reproduced).** `MorphProgress.qml` uses the Basic `ProgressBar` with neither `background` nor
`contentItem`. The instrument grab on this revision reads `#3599d9` at (970, 360), while the bundle's `color.accent`
is `#17784A`.

**Fix.** The bar gets its own `background` and `contentItem`:

- the track is `root.scheme.neutral`, at `Theme.spaceOf(1)` (4 px) tall, with fully rounded ends;
- the fill is `root.scheme.accent`, as wide as `visualPosition` of the track;
- while `indeterminate`, the fill is a segment a third of the track wide that slides across the track on a loop.
  Replacing `contentItem` loses Basic's own animation, so this one replaces it. The animation runs only while the
  item is visible, so a hidden bar costs nothing.

**Spec.** `docs/spec/qt_quick/backend.md`: a progress bar is the accent on the `neutral` track. `theme.md`'s
`color.accent` and `color.neutral` rows list it as a reader.

**Tests** (`tests/qt_quick/test_qt_quick_theme.cpp`).

- Under `brandTheme()`, the fill item's colour is the brand accent and the track's is the brand neutral.
- An indeterminate bar's segment `x` changes between two frames.
- **Control:** delete the `contentItem`, and the colour assertion fails.

**Risks.** The bar's implicit height drops from Basic's (about 6 px) to 4 px, so the layouts around it lose 2 px.

## G34: Qt Quick `keyValue` polish loop (#1054)

**Cause (reproduced; the gap log's explanation is wrong).** A throwaway Qt Quick test, run and then deleted, made a
bare `KeyValue` on `qt_quick::Backend` (`Scene`, 800 × 600, offscreen, Qt 6.12.0) and counted the `polish()` warnings
for each pair:

```
G34 Containers=needed root w=800.000 iw=114.344 polish=10
G34 Containers=180 needed root w=800.000 iw=137.344 polish=10
G34 Containers=x root w=800.000 iw=79.344 polish=0
G34 Container=180 needed root w=800.000 iw=131.344 polish=0
G34 Containers=xxxxxxxxxx root w=800.000 iw=133.344 polish=10
```

The widget is 800 px wide, far wider than its content. So the gap log's explanation is wrong. It said a content-sized
widget makes the value cap equal to the value's own width, and that a fractional pixel then wraps it. That cannot
happen here.

What does cause it is the cap itself. In the same throwaway test, removing only `Layout.maximumWidth` from the value
labels (`MorphKeyValue.qml:31-48`) gave `polish=0` for all five pairs. The cap reads `grid.width` and
`labelColumnWidth`, and `labelColumnWidth` is written from a label's `onImplicitWidthChanged`, which runs during the
grid's own polish. That is a binding cycle through the layout, and which text widths close it is incidental. Why
these exact widths trigger it was not traced.

**Fix: the grid sizes from its columns' own rules, with no cap.**

- Captions don't wrap (`Text.NoWrap`), don't fill, and keep `Layout.minimumWidth: implicitWidth`, so a caption is
  never cut or wrapped.
- Values wrap (`Text.Wrap`) and fill (`Layout.fillWidth: true`), so a value narrower than its unwrapped width wraps
  inside the width the grid is given.
- `valueMaxWidth` and `labelColumnWidth` are deleted.

The throwaway test ran this exact change: `polish=0` for all five pairs. The existing test `qt_quick widgets:
key-value wraps a long value instead of overflowing a fixed width` still passed, and a `fixed(20)` keyValue with
caption `Serial number` and a UUID value grew in height (it wrapped) instead of in width.

**Spec.** `docs/spec/qt_quick/backend.md` adds: captions keep their width, and values wrap in what is left.

**Tests** (`tests/qt_quick/test_qt_quick_widgets.cpp`).

- A message handler counts warnings containing `polish`. Each pair in the issue's table mounts once with no such
  warning.
- The existing wrap test stays.
- **Control:** restore the cap, and the `Containers` = `needed` case counts 10.

**Risks.** A caption longer than the widget's width now overflows instead of wrapping. Captions are short labels, and
the gallery's longest is "Last pull". A keyValue in a container narrower than its captions is a layout mistake the
author can see.

## G48: a read-only field looks editable on Qt Quick

**Cause (read).** `MorphTextInput.qml:37-41` passes `readOnly` to the `TextField`. Its `MorphInputBackground`
(`src/qt_quick/qml/MorphInputBackground.qml`) is given only `scheme` and `focused`, and draws `scheme.surface` with
the border. The select, the date input, the text area and the file picker use the same background. The contract
(`docs/spec/ui/backend_contract.md:56-58`) defines what a read-only field does, not how it looks, so neither backend
is wrong against a spec. This item adds the rule.

**Fix.** A read-only input is drawn so that it does not invite typing:

- **Qt Quick.** `MorphInputBackground` gains `readOnly`, which every user passes. Read-only draws the page's
  background colour (`Theme.background`, `color.background`) instead of `scheme.surface`, and keeps the border and
  the focus ring, because a read-only field still takes focus for selection.
- **Terminal.** `paintField` (`src/tui/field_widgets.cpp:149-160`) draws a read-only field with no input background
  (`InputFieldStyles::background = nullopt`), in the normal text colour. It reads as text in place and stays distinct
  from a disabled field's muted text.

**Spec.** `backend_contract.md`'s **Read-only** bullet adds: "and is drawn so that it does not look editable: on Qt
Quick on the page's background colour, on the terminal without the input fill". `tui/frontend.md`'s Field state
section says the same.

**Tests.**

- `tests/qt_quick/test_qt_quick_theme.cpp`: under `brandTheme()`, a read-only text input's background colour is the
  brand `color.background`, and an editable one's is `color.surface`.
- `tests/tui/test_tui_fields.cpp`: the read-only field's cells carry no input background style.
- **Control:** ignore `readOnly` in the background, and both fail.

**Risks.** None outside inputs. A restyled input background that does not declare `readOnly` keeps its look, since
an undeclared property is skipped (`src/qt_quick/widget.hpp:60`).

## G42: Enter on a focused button never reaches the button's own Enter chord

**Reproduced on all three backends.** A throwaway conformance case, added to `conformanceCases()` and deleted again,
mounted a button with `onClick` and its own `keys` entry `Enter` inside two columns, and pressed Enter with focus on
it through each backend's probe. The Qt Quick probe sends a real `QKeyEvent` to the focused item, and the terminal
probe sends a real key event to the screen.

| Backend | What Enter ran |
|---|---|
| `RecordingBackend` (`morph_tests`) | `G42 OBSERVED: Enter ran [chord;]` |
| Qt Quick (`morph_qt_quick_tests`, offscreen) | `G42 OBSERVED: Enter ran [click;]` |
| Terminal (`morph_tui_tests`) | `G42 OBSERVED: Enter ran [click;]` |

The gap log said the two backends disagree. In fact `RecordingBackend` is the odd one out, and the gallery's plate
test passes only because it runs there.

**Cause.**

- **Qt Quick.** The chord handler is `Keys.onPressed` on the widget's root (`MorphWidget.qml:187-190`). Focus is on
  the inner `Button`, whose `Keys.onReturnPressed` and `Keys.onEnterPressed` fire the click
  (`MorphButton.qml:95-96`). A specific `Keys.on<Key>Pressed` handler accepts the event, so it never propagates to
  the root.
- **Terminal.** `WidgetBase::dispatch` (`src/tui/widget.cpp:344-351`) runs the widget's own `key()` first and only
  an ignored key reaches `chord()`. `tui/frontend.md` ("Keys and chords") specifies that order.
- **`RecordingBackend`.** `press` (`include/morph/ui/testing/recording_backend.hpp:1799-1812`) models chords only,
  never a widget's own keys.
- **The contract** (`backend_contract.md:52-55`, `view_tree.md:148`) says a chord goes to the innermost declaring
  widget, and says nothing about the widget's own key handling.

**Fix: a chord a widget declares itself comes before that widget's own handling of the key. A chord an ancestor
declares gets only the keys the focused widget leaves.**

Why this rule:

- the author declared the binding on that very node, which is the only way to say "Enter here means something
  else";
- a dialog's `default` (an Enter chord on the dialog's content) must still not take Enter from a focused button
  inside it, so ancestors keep coming after the widget's own handling.

The rule per backend:

- **Terminal.** `WidgetBase::dispatch` tries `chord()` before `key()`. The same handler-may-destroy-the-widget care
  applies: return as soon as either one handles the key.
- **Qt Quick.** `MorphWidget` gains `takeOwnChord(event): bool`, the existing `question("chord", …)` call. Every
  control whose focus target handles keys itself asks it first:
  - `MorphButton`'s Return and Enter handlers become `event => { if (!root.takeOwnChord(event)) root.fire("click") }`;
  - the focus targets of the text input, text area, check box, select, slider, date input and table get
    `Keys.priority: Keys.BeforeItem` and a `Keys.onPressed` that accepts the event when `takeOwnChord` took it.
- **`RecordingBackend::press`.** When the focused widget does not declare the chord itself and its kind has a
  built-in key, the built-in action runs and `press` returns `true` without walking outwards. The built-in keys:
  - a `Button`'s Enter and Space click;
  - a single-line `TextInput`'s Enter submits;
  - a `Checkbox`'s Space toggles;
  - a `Table`'s Enter activates the current row.

  This is what makes `RecordingBackend` agree with the other two for an ancestor's chord too.

**Spec.**

- `backend_contract.md`'s **Chords** bullet and `view_tree.md:148` state the rule.
- `tui/frontend.md`'s "Keys and chords" changes "A widget's own key handling comes first" to "A chord the widget
  declares comes first, then the widget's own key handling, then its ancestors' chords".
- The `RecordingBackend` section lists the built-in keys `press` honours.

**Tests.** Two conformance cases in `include/morph/ui/testing/backend_conformance.hpp`, run on all three backends:

1. Enter on a focused button that declares an Enter chord runs the chord and not the click.
2. Enter on a focused button inside a column that declares an Enter chord clicks the button, and the column's chord
   does not run.

Then:

- `test_ui_conformance.cpp`'s count goes from 34 to 36;
- the gallery's plate test keeps `press(well, "Enter")` and now means it on every backend.

**Control:**

- restore the terminal's `key()`-first order, and case 1 fails on the terminal;
- drop `MorphButton`'s check, and it fails on Qt Quick;
- drop the built-in keys from `RecordingBackend::press`, and case 2 fails there.

**Risks.**

- A test that pressed Enter on a button to reach a dialog's `default` now clicks the button instead, as a user
  would. Each such test is wrong today and is fixed where it fails.
- A text input that declares a chord such as `Ctrl+A` now loses that key's built-in meaning. This is the rule doing
  what it says.

## G15, G40, G54: table cells on Qt Quick

**Causes (read; the gap log's grabs stand).**

- **G15 (a tag stretched across its column).** `MorphTableRow.qml`'s `place()` (`:37-55`) sets every cell's `width`
  to its column's width less the insets. A badge, a button or a check box draws its frame over that width. In a stack
  the same kinds keep their natural width through `MorphWidget.fillsAcross` (`MorphWidget.qml:37,154`; `false` on
  `MorphBadge`, `MorphCheckbox` and a non-item `MorphButton`), which `place()` ignores.
- **G40 (cells at the top of a taller row).** The same function sets every cell's `height` to the tallest cell's and
  leaves `y` at 0. A `Text` has top vertical alignment, so a text cell draws at the top beside a taller select or
  check box.
- **G54 (an overfull table overlaps its own cells).** `MorphTable.qml`'s `columnWidths` (`:47-68`) gives a stretch
  column `max(0, total − used)`. When the fixed columns use more than the table's width, that column is 0 wide: its
  header goes, and a neighbour's unclipped text draws over its cells. The table's `Flickable` scrolls vertically only
  (`:227-245`) and clips only when virtualized, so the columns past the edge cannot be reached.

On the terminal, `TableImpl::columnWidths` (`src/tui/table_widget.cpp:127-134`) uses the shared `layout::distribute`,
and each cell paints in its own subcanvas. So cells cannot draw over each other there (read, not run).

**Fix (Qt Quick).**

- **G15.** In `place()`, a cell whose `fillsAcross` is `false` is `min(implicitWidth, column − 2·inset)` wide at the
  column's start. Every other cell keeps the column's width. This is the cross-axis rule stacks already follow
  (`backend_contract.md:156-162`: "a kind whose width is its label … draws at its natural width at the line's
  start"), now applied to cells as well.
- **G40.** Each cell keeps `min(implicitHeight, tallest)` and is centred vertically in the row, at
  `y = (tallest − height) / 2`.
- **G54.**
  - A stretch column is never narrower than a content column's floor: its header's width plus the padding, and at
    least 8 units, the floor `columnWidths` already gives a content column.
  - The header and the body sit in one horizontally flickable area, clipped, with the horizontal `MorphScrollBar`,
    whose content width is the sum of the column widths. When that sum fits, the area does not scroll and nothing
    changes.

**Spec.** `backend_contract.md:156-162`'s last sentence ("a table's cells are sized by the table, not by this rule")
becomes:

- a table's cell fills its column's width, except a kind whose width is its label, which draws at its natural width
  at the column's start;
- every cell keeps its own height and is centred in its row;
- a stretch column is at least as wide as its header;
- a table whose columns are wider than the table scrolls horizontally on Qt Quick and cuts at its edge on the
  terminal.

`document.md:252-254` is unchanged: a cell still takes no `alignSelf`.

**Tests** (`tests/qt_quick/test_qt_quick_table.cpp`).

- A badge cell in a 30-unit content column is narrower than the column, and starts at the inset.
- A text cell beside a select cell has equal space above and below, within one pixel.
- A table 60 units wide with fixed columns of 40 and 40 and a stretch column:
  - the stretch column is at least its header's width;
  - no two cells' x-ranges overlap;
  - the horizontal flick's `contentWidth` exceeds its width.
- **Control:** revert `place()`, and the first two fail. Revert `columnWidths`, and the overlap check fails.

**Risks.**

- A table of multi-line text cells beside single-line ones now centres the single-line ones. Some designs (an audit
  trail) prefer the top. If one needs that, it is a column option for batch B, not a reason to keep the defect.
- The gallery's `layout::cellTag` row wrapper goes.
- A virtualized table's vertical flick keeps its own clip. The horizontal area wraps both the header and that flick,
  so virtualization's `contentY` arithmetic is untouched. This needs checking against
  `test_qt_quick_table.cpp`'s virtualization cases.

## G1: `{"env": "widthClass"}` is never set (#1046)

**Cause (read, re-checked by grep).**

- `Session::setEnvironment` (`interpreter.hpp:960-962`) is the only writer of `_env` besides `backendChanged`
  (`:3046-3066`), which inserts `backend` after a committed switch. Nothing calls it: `grep -rn setEnvironment
  include src examples` finds only its definition, and two tests.
- The Qt Quick backend computes the class from its host item's width (`widthClassOf`, `src/qt_quick/context.cpp:22-30`;
  `Context::widthChanged`, `:98-108`) and hands it to `BackendOptions::onWidthClass`. `FrontendConfig::onWidthClass`
  (`include/morph/qt_quick/frontend.hpp:50-52`) passes that through, and its doc says "The shell sets it". No shell
  does, and the generic client passes `{}` (`examples/client/main.cpp:234`).
- The order makes the callback unusable for that anyway: `Frontend::run` builds the backend, and so computes the
  first class, before `mountShell` makes the shell and the `Session` (`src/qt_quick/frontend.cpp:166-206`).
- `ShellOptions` (`include/morph/client/shell.hpp:131-157`) has no way to reach the session.

So under `morph::client`, `env.widthClass` reads `null` on every frontend.

The same is true of `locale`, `platform` and `online`, which `expressions.md:42` and `author.hpp:1206` also list:
nothing sets them, and `backend` is set only after a `switchBackend`. That is outside this batch, and filed
as #1056.

**Where the class is set: the shell, from the backend.**

- The value belongs to the `Session`, since every screen of a client shares it, and a `Session` belongs to the shell
  (`src/client/shell.cpp:156`).
- What knows the width is the view backend. The shell already holds it (`Shell::_backend`), and so does every test
  harness.
- So the seam is on `IViewBackend`, and the shell copies its value into the session. The `Interpreter` is per screen
  and is the wrong owner.

**Fix.**

- **`IViewBackend`** (`include/morph/ui/backend.hpp`) gains two pure virtuals, documented in full:
  - `std::string widthClass() const`: `compact`, `medium` or `expanded`, the class of the surface the backend draws
    on;
  - `void setOnWidthClass(std::function<void(std::string const&)>)`: one listener, called on the owner each time the
    class changes, outside any layout pass. Empty clears it. A backend reports to one listener, the shell mounted on
    it.

  They are pure, not defaulted, so a backend cannot silently report nothing. There are three implementations.
- **`Session::setEnvironmentValue(std::string name, Value value)`** sets one `env` value and keeps the others.
  `setEnvironment` replaces the whole record, which would drop `backend`, and `backendChanged` already merges this
  way.
- **The client shell.** `Shell::start` writes `widthClass` from `_backend->widthClass()` before the first mount, then
  registers the listener, which writes each change. `~Shell` clears it. A class change re-evaluates every
  `{"env": "widthClass"}` in every open tab, through the session's signal.
- **Qt Quick.**
  - `Backend::widthClass()` is the class `Context::widthChanged` last computed. Its listener is called from
    `widthChanged`, which runs from the host item's `widthChanged` signal on the GUI thread, never inside polish.
  - `BackendOptions::onWidthClass` and `FrontendConfig::onWidthClass` are removed: two ways to learn one fact, one of
    them documented to do what nothing does. Every caller passes `{}` except one test, which moves to
    `setOnWidthClass`.
  - `widthClassOf` and its thresholds are unchanged.
- **Terminal: a class computed from the columns.**
  - `compact` below 80 columns, `medium` below 160, `expanded` from 160.
  - The terminal's unit is a column: `fixed(n)` is `n` columns, and `n` average character widths on Qt Quick, about
    7 to 8 px for a 13–14 px body font. So 600 px and 1200 px are about 80 and 160 units, and the same document lays
    out alike on both frontends. A fixed class would make every terminal render one layout, whatever its size.
  - `tui::Backend::widthClass()` reads `screen.viewportArea().width` when asked.
  - The terminal `Session` (`src/tui/session.cpp:159-173`) calls a new `Backend::refreshWidthClass()` after
    dispatching a `ResizeEvent`, outside the draw, so a change re-renders on the next turn. `fit()`, which runs inside
    the draw, never calls the listener.
- **`RecordingBackend`.** It reports `expanded` until `setWidthClass(name)` changes it. The setter also calls the
  listener, so a test can drive a resize. A recording backend has no width, and `expanded` is the class every screen
  had before this change.

**The testkit and the gallery.**

- `ScreenHarness` (`examples/common/testkit/screen_harness.hpp:126`) takes a trailing `std::string widthClass =
  "expanded"` and sets it on its `RecordingBackend` before `mountShell`. It also exposes `setWidthClass` for resize
  tests.
- `GalleryScreen` passes its `width` there, not as a param.
- The gallery's render smoke resizes the real window between mounts, 400 × 900 for `compact` and 1280 × 900 for
  `expanded`, through the `QQuickWindow` its `onEngine` hook receives, and asserts the class each mount saw. So the
  smoke drives the same path the client does.
- The screens drop `kWidthParam`. `layout::widthClass` becomes `coalesce(env widthClass, "expanded")`. The `width`
  params that `index`, `review` and `console` forward through `navigate` go.

**Spec.**

- `expressions.md:42`: `widthClass` is one of `compact`, `medium`, `expanded`, set by the client from its view
  backend and updated on a resize.
- `backend_contract.md` Interfaces: the two `IViewBackend` members.
- `backend_contract.md` RecordingBackend: `setWidthClass` and the `expanded` default.
- `interpreter.md:37`: `setEnvironmentValue`.
- `client/client.md` "The shell": the shell sets `env.widthClass`.
- `qt_quick/backend.md:43-45` and `qt_quick/frontend.md:35-38`: `onWidthClass` replaced by `widthClass()`.
- `tui/frontend.md`: the column thresholds and the reasoning above.

**Tests.**

- `tests/test_ui_interpreter_permissions.cpp`, beside the existing `setEnvironment` case: `setEnvironmentValue`
  keeps `backend` after a switch, and a switch keeps `widthClass`.
- `tests/client/` (a new case beside `test_sign_in.cpp`'s shells): a screen showing `{"env": "widthClass"}` through
  `mountShell` on a `RecordingBackend` set to `compact` shows `compact`. `setWidthClass("medium")` updates it.
- `tests/qt_quick/test_qt_quick_frontend.cpp`: a `qt_quick::Frontend` whose `onEngine` resizes the window to 400 px
  shows `compact`, and after a resize to 1300 px shows `expanded`. This is #1046's own closing condition.
- `tests/tui/test_tui_frontend.cpp`: a 70-column screen shows `compact`, and a resize event to 120 shows `medium`.
- Gallery: every `GalleryScreen` test and the render smoke now select their layout through the class.
- **Control:**
  - remove the shell's `setEnvironmentValue` call, and the client, Qt Quick and terminal tests read an empty text;
  - make the gallery's `layout::widthClass` constant `expanded`, and the compact gallery tests fail. This shows they
    depend on the class.

**Risks.**

- **State loss.** A screen that switches layout on the class unmounts the other case's subtree when the class
  changes, and with it the state declared in that case's scope. A window dragged across 600 px loses what was typed
  in a field whose state lives inside a `switchOn` case. That is how `switch` works. Screens keep their state at
  screen level, as the gallery's do.
- **Removed public members.** `onWidthClass` goes from two public option structs. Every in-tree caller is updated.
- **Several backends on one window.** An application that builds several `qt_quick::Backend`s and its own `Session`
  writes the class itself. The library has no rule for which backend's class wins.

## G5: touch-sized controls on compact

**Cause (read).** `Theme.controlHeight` (`src/qt_quick/qml/Theme.qml:82`, token `control.height`) is one height.
These read it:

- `MorphButtonBackground`;
- `MorphInputBackground`, used by the text input, select, date input and file picker;
- `MorphTextArea` (×2);
- `MorphTableRowBackground` (−8).

The check box and the tabs don't read it. `theme.md:221` lists a runtime density choice under "Not yet", and no
node or class can ask for a larger control.

**Decision: a theme token the Qt Quick frontend applies while the width class is `compact`. It is not a density
layer.**

- The theme index's `density` layers (`theme.md`, "The theme index") are a named, author- or user-chosen variant,
  and the spec reserves a `{"pref": "density"}` to choose one. A width class is a fact about the window, not a
  preference. Selecting a layer by it would need a reserved density name in every bundle, and the whole theme
  re-resolved and re-applied on each class change.
- One more token in the closed set does it without either: `control.height.touch`, a dimension, default 44 px (the
  common minimum touch target).
- A frontend rule with no token would hard-code 44 for every application. The token keeps it a theme value, and
  `theme.md` already says `control.*` joins the set "with the components that read them".

**Fix (Qt Quick only).**

- `kThemeTokens` gains `control.height.touch` (44). `applyTheme` maps it to a new `Theme.touchHeight`.
- `Theme` gains `touchTargets: bool`, written by the backend's `Context::widthChanged` with the class
  (`compact` → `true`), and a read-only `targetHeight`: `touchTargets ? Math.max(controlHeight, touchHeight) :
  controlHeight`.
- Every component that reads `Theme.controlHeight` reads `Theme.targetHeight`. The check box and the tab button take
  `targetHeight` as their minimum implicit height, so their touch target grows while the indicator keeps its size.
- The terminal is unchanged: a cell is the target.

**Spec.** `theme.md`:

- "The closed set" gains the `control.height.touch` row;
- "On Qt Quick" maps it and states the rule (controls are at least `control.height.touch` tall while the window is
  `compact`);
- "Not yet" keeps the runtime density choice.

`qt_quick/backend.md` notes that the class also sets `Theme.touchTargets`.

**Tests** (`tests/qt_quick/test_qt_quick_theme.cpp`).

- At 400 px a button's and a text input's heights are 44, and a check box's is at least 44.
- At 1000 px a button is `control.height`.
- A theme setting `control.height.touch` to 48 gives 48 at 400 px.
- **Control:** have `Context::widthChanged` not write `touchTargets`, and the compact case reads 32.

**Risks.**

- `Theme` is one per engine (`qt_quick/backend.md`, "Several mounts"), so two backends of different widths in one
  engine share whichever class was written last. The generic client has one backend per window.
- Compact layouts grow taller. The gallery's compact designs are drawn at 44.
- A narrow desktop window gets touch-sized controls too. The width class is the only signal morph has. Following the
  pointer type is an open question below.

## G19: the screen's head overflows the shell's top bar (#1049)

**Cause (reproduced).** The instrument grab on this revision, at 1280 × 900, shows the defect. The title wraps to
two lines, and "Sign out" is drawn over the head's "Book" action.

- `ShellFrame` (`include/morph/ui/shell_frame.hpp:59-111`) puts the title (stretch), the head slot (stretch), the
  principal and the sign-out button in one row.
- The head row (`Workspace::buildHeadRow`, `include/morph/ui/workspace.hpp:363-428`) is the subtitle, the badges, a
  stretch spacer and the actions. Its natural width (here a breadcrumb, three tags and two buttons, about 860 px)
  exceeds the slot's share.
- A Qt Quick `RowLayout` cannot shrink a child below its minimum, so the row overflows its slot, and the sign-out
  button, laid out later, is drawn on top.
- The terminal's solver takes overflow "from the last child first" and then clips (`tui/frontend.md:120-125`). There
  the bar would cut the sign-out button or the actions instead. Read, not run.

**Decision: the head gets its own line in the top bar, and that line scrolls horizontally when even it is too
narrow.**

- **Wrap**, chosen: the head drops to a second line of the bar. The design draws this record's head in two lines,
  and every backend can do it with the widgets it has.
- **Elide** loses the badges and the actions, which are the reason the head exists. Only text can elide.
- **An overflow menu** needs a menu node (G18, batch D) and a measuring pass to decide which actions move into it.
  That is a backend-specific layout feature the vocabulary does not have. Batch D's overflow menu can later collapse
  the head's actions into it on `compact`. The second line stays correct either way.
- A *conditional* wrap (one line when it fits) needs a flow container that measures its children. ui/1 has none,
  and a width-class rule cannot decide it: the defect reproduces at 1280 px, an `expanded` window.

**Fix** (`ShellFrame`, backend-neutral, so both frontends get it).

- The top bar becomes a column on the `toolbar` surface with two rows.
  - Line 1: the spacer, the title (stretch), the principal, the sign-out button and the spacer, as today.
  - Line 2: a horizontal `scroll` (`createScroll(…, Axis::Horizontal)`) holding the head slot.
- Line 2 is hidden while the active tab's head has no subtitle, badge or action, which is the condition
  `buildHeadRow` already binds to its row's `visible`. A screen without a head keeps a one-line bar.
- The title has the first line to itself, beside two small widgets, so it no longer wraps at any width the gallery
  uses.
- In the scroll, the head row fills the viewport when narrower, so its actions stay at the trailing edge. When wider,
  it scrolls.
- `headSlot()` keeps its name and meaning, and is now line 2's content.

**Spec.** `shell_frame.md`'s Shape table:

- the `shell_topbar` row becomes a column, with `shell_topbar_main` (line 1) and `shell_head_line` (the scroll,
  line 2);
- the `headSlot()` bullet says the slot is on its own line, shown only while the head has content, and scrolls
  sideways when wider than the bar;
- the Design decisions table gains the wrap, elide or menu row and its reasoning.

**Tests.**

- `tests/test_ui_shell_frame.cpp` (`RecordingBackend`): the head slot is under `shell_head_line`, not beside
  `shell_signOut`, and `shell_head_line` is hidden while the head row is.
- `tests/qt_quick/test_qt_quick_containers.cpp`: a `ShellFrame` at 1000 px with `onSignOut` set and a head row of a
  subtitle, three badges and two actions mounted into `headSlot()`.
  - Neither action's scene rectangle intersects `shell_signOut`'s, and both lie inside the window.
  - At 400 px the second action lies inside the scroll's content, and the content is wider than the viewport.
- **Control:** revert `ShellFrame` to the one-row bar, and the 1000 px intersection check fails, as the grab does.

**Risks.**

- A screen with a head spends one more bar line, about 40 px on Qt Quick and one row on the terminal.
- The existing test `qt_quick containers: the shell's principal sits level with its sign-out button` reads line 1
  and still holds.
- Whether the horizontal scroll lets the row's stretch spacer reach the viewport's width depends on how the scroll
  content fills its width (`backend_contract.md:158`). This needs checking on both frontends in the commit.

## Open questions

1. **G5's signal.** The width class stands in for "touch input". Should Qt Quick instead follow whether a touch
   screen is present (`QInputDevice::devices()`), or both? The design follows the class, because that is what the
   gallery's designs are drawn for.
2. **G1's terminal thresholds.** 80 and 160 columns keep a unit a unit on both frontends. The consequence is that a
   standard 80-column terminal is `medium`, not `compact`. Is that the intended reading?
3. **G15/G40's default.** Cells are centred vertically by default. Should batch B's table work add a per-column
   vertical alignment, or is centred enough?
4. **G42's `RecordingBackend` built-in keys.** The list (Button Enter and Space, single-line TextInput Enter,
   Checkbox Space, Table Enter) is what the two real backends do today. Should the conformance suite gain a case per
   kind, or are the two cases above enough?

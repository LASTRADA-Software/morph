# LIMS gallery gap log: what the ten designs need that ui/1 cannot express, and the defects found building them

### G1: `{"env": "widthClass"}` is never set (defect)
- **Design(s):** 01, 02, 03, 04, 05, 06, 07
- **Needs:** every design switches between an expanded (≥ 1200 px) and a compact (< 600 px) layout on the window's
  width class.
- **ui/1 today:** the expression exists (`docs/spec/ui/expressions.md:42`) and `Session::setEnvironment` holds it
  (`include/morph/ui/interpreter.hpp:962`), but nothing calls `setEnvironment`: `grep -rn setEnvironment include src`
  finds only that definition. The Qt Quick backend computes the class and hands it to `FrontendConfig::onWidthClass`,
  whose doc says "The shell sets it as `{"env": "widthClass"}`" (`include/morph/qt_quick/frontend.hpp:50-52`,
  `docs/spec/qt_quick/backend.md:44-45`); the generic client passes `.onWidthClass = {}`
  (`examples/client/main.cpp:234`), and `morph::client::ShellOptions` (`include/morph/client/shell.hpp:131-157`) has
  no way to reach the mounted session's environment. The testkit's `ScreenHarness` and render smoke have none either.
  Verified by grep; that the value reads `null` at run time is inferred from it, not observed.
- **Workaround used:** every screen declares an optional `width` param and lays out for
  `coalesce(param width, env widthClass, "expanded")` (`examples/lims_gallery/src/screens/layout.hpp`); tests and the
  render smoke pass the param. In the real client every screen therefore shows its expanded layout at any window
  width.
- **Weight:** High

### G2: Monospace text for identifiers
- **Design(s):** 01, 02, 03, 04, 05, 07
- **Needs:** record, lot, sample, equipment and investigation ids (`WS-2026-0412`, `L-26-0381`, `OOS-2026-031`) set
  in a monospace face, the LIMS convention that makes ids scannable and unambiguous (0/O, 1/l).
- **ui/1 today:** a `text` node's `role` is `normal`, `muted`, `heading`, `section`, `error` or `success`
  (`docs/spec/ui/document.md:164`); the theme has one `font.family` (`docs/spec/ui/theme.md:130`). No mono role, no
  per-node font. Inferred from reading.
- **Workaround used:** ids are plain text.
- **Weight:** Medium

### G3: Inline emphasis inside one text
- **Design(s):** 01
- **Needs:** a bold run inside a sentence: "**Password incorrect.** 3 of 5 attempts left…", "Signing at … as
  **Approved**."
- **ui/1 today:** `text`'s `text` and `banner`'s `text` are single strings (`docs/spec/ui/document.md:164,184`);
  there are no spans or markup. Inferred from reading.
- **Workaround used:** the whole sentence in one weight.
- **Weight:** Low

### G4: An overlay that becomes a full-screen sheet on compact
- **Design(s):** 01
- **Needs:** the same signature dialog as a centred 480 px dialog when expanded and as a full-screen sheet, with its
  own top bar (Cancel, title) and a full-width bottom button, when compact.
- **ui/1 today:** a `dialog` takes no size bounds (`docs/spec/ui/document.md:244-247`); a literal `sizing: fixed(n)`
  sets its width on Qt Quick (`src/qt_quick/qml/MorphDialog.qml:57-62`, used here for the 480 px), but there is no
  full-screen or sheet presentation and no way to choose one per width class. Verified by trying.
- **Workaround used:** compact renders the dialog's fields in the page under a `switch` on the `open` state, with an
  in-page Cancel row and a stretched primary button; the expanded layout keeps the real `dialog`. The two layouts
  share the field builders but not the overlay's `ready`/`submitted` semantics.
- **Weight:** Medium

### G5: Touch-sized controls on compact
- **Design(s):** 01, 02, 03, 04, 05, 06, 07
- **Needs:** 44 px controls and touch targets in the compact layout (30 px when expanded).
- **ui/1 today:** one global `control.height` token (`docs/spec/ui/theme.md:128`); a runtime density choice is listed
  under "Not yet" (`docs/spec/ui/theme.md:221`), and no node or width class can ask for a larger control. Inferred
  from reading.
- **Workaround used:** none; compact controls keep the theme's height.
- **Weight:** Medium

### G6: A dialog's description line
- **Design(s):** 01
- **Needs:** a muted line under the dialog's title ("Your signature is stored with the record and cannot be
  removed."), part of the dialog's head, divided from the body.
- **ui/1 today:** `dialog` has `title` and no subtitle or description (`docs/spec/ui/document.md:177`), though a
  screen's head has `subtitle` (`docs/spec/ui/document.md:63`). Inferred from reading.
- **Workaround used:** a muted `text` as the dialog's first child.
- **Weight:** Low

### G7: The Qt Quick dialog ignores the theme's surface (defect)
- **Design(s):** 01, 02, 03
- **Needs:** the dialog drawn as the theme's card: `color.surface` (#FFFFFF here), the card radius, padded content.
- **ui/1 today:** `MorphDialog.qml`'s `Popup` sets no `background` (`grep -n background
  src/qt_quick/qml/MorphDialog.qml` finds none), so the Basic style's popup background shows. Reproduced on revision
  842701f0d plus this branch: `QT_QPA_PLATFORM=offscreen morph_examples_client --app lims-gallery --ui=qt --screen
  lims-gallery.esignature --grab out.png`; the dialog body's pixel at (600, 262) is `#efefef` while the bundle's theme
  sets `color.surface` to `#FFFFFF`, and the content sits about 4 px from the dialog's edge. Not checked on the
  terminal frontend.
- **Workaround used:** none.
- **Weight:** Medium

### G8: The required mark sits beside the control, not the caption
- **Design(s):** 01, 02
- **Needs:** "Meaning \*": the mark after the field's caption.
- **ui/1 today:** `required` draws a mark the frontend places (`docs/spec/ui/view_tree.md:158`); on Qt Quick it is a
  red asterisk in a gutter right of the control, below the caption's line (seen in the grab above). No placement
  choice. Verified by trying.
- **Workaround used:** none; the frontend's placement.
- **Weight:** Low

### G9: A unit beside a free input
- **Design(s):** 02, 06
- **Needs:** "Temperature on arrival" with `°C` drawn inside the field's trailing edge.
- **ui/1 today:** the view tree's `TextInput` has `unit` and `decimals` (`docs/spec/ui/view_tree.md:106`), but only
  the forms engine sets them; the document's `textInput` keys are `value`, `placeholder`, `mode`, `icon`, `commitOn`,
  `label`, `showLabel` and its events, with no `unit` (`docs/spec/ui/document.md:166`), and `author::TextInputOptions`
  has none (`include/morph/ui/author.hpp:3185`). Inferred from reading.
- **Workaround used:** the unit in the caption: "Temperature on arrival, °C".
- **Weight:** Medium

### G10: Help text under an input
- **Design(s):** 02
- **Needs:** a muted line tied to a field ("Required range 2–8 °C"; "The field keeps focus after every scan. Press
  Enter to look up a typed ID."), announced with it (`aria-describedby`).
- **ui/1 today:** inputs carry `label`, `placeholder` and `errors` (`docs/spec/ui/document.md:159-166`); there is no
  hint or description key, and `a11y` has only `name` and `role`. Inferred from reading.
- **Workaround used:** a muted `text` under the input, in a column with it; not associated for assistive technology.
- **Weight:** Medium

### G11: A scanner field that stands out
- **Design(s):** 02
- **Needs:** the scan field as the screen's focal control: larger monospace text, an accent outline and a soft glow.
- **ui/1 today:** a `textInput` has no size or emphasis (`docs/spec/ui/document.md:166`); its leading `icon` works,
  served from the bundle's icon set (`docs/spec/ui/theme.md:183-202`, verified: the barcode glyph draws).
- **Workaround used:** an ordinary input with the barcode icon, `autofocus`, and a `focus` command after each scan.
- **Weight:** Low

### G12: `close: self` in an inline dialog asks the host to close the screen, not the dialog (defect)
- **Design(s):** 02
- **Needs:** a dialog's footer button that closes the dialog it is in.
- **ui/1 today:** the author API documents `self` as "The dialog or drawer a command list runs in" and
  `close(self)` as "`close` of the dialog or drawer the command list runs in"
  (`include/morph/ui/author.hpp:1291-1295`, `include/morph/ui/author.hpp:2786-2789`), and `document.md:268` lets
  `close` name "a dialog or drawer (`close` also `self`)". The interpreter sends every `close: self` to
  `InterpreterHost::closeSelf` for the screen (`include/morph/ui/interpreter.hpp:4647-4650`). Reproduced: a reject
  dialog whose confirm action ended with `ui::close(ui::self)`, mounted through the testkit's `ScreenHarness`,
  reported this problem (verbatim): document lims-gallery.receipt at /view/default/0/children/1/actions/1/onClick/3:
  `close: self` is not supported by the host
- **Workaround used:** the dialog's actions close it by its element, `ui::close(element)`.
- **Weight:** Medium

### G13: A date and time display format
- **Design(s):** 02, 04, 06
- **Needs:** "24-Oct-2026 09:41": the unambiguous day-month-name-year form regulated labs use.
- **ui/1 today:** `dateTimeInput` takes `value`, `mode`, `label`, `showLabel` and events, no format
  (`docs/spec/ui/document.md:181`); on Qt Quick it shows `2026-10-24 09:41` (seen in the grab). The `date` and
  `dateTime` functions have only `short`, `medium` and `long` styles (`docs/spec/ui/expressions.md:141-143`).
  Verified by trying.
- **Workaround used:** none; the frontend's format.
- **Weight:** Low

### G14: A secondary destructive button
- **Design(s):** 02
- **Needs:** "Reject sample…" as an outlined button with red text, so the filled primary "Receive and scan next"
  stays the one action that stands out.
- **ui/1 today:** `emphasis` is `normal`, `primary`, `danger` or `ghost` (`docs/spec/ui/document.md:165`); `danger`
  draws a filled red button on Qt Quick (seen in the grab), as loud as the primary. Verified by trying.
- **Workaround used:** `danger` as it draws.
- **Weight:** Low

### G15: A status tag in a table cell is stretched across the column
- **Design(s):** 02
- **Needs:** an outcome tag ("Received", "Rejected") at its label's width in a table column.
- **ui/1 today:** a table sizes its cells itself (`docs/spec/ui/backend_contract.md:156-162` exempts its cells from
  the cross-axis rule), and a bare `badge` cell is drawn the full width of its column on Qt Quick (seen in the grab).
  `alignSelf` cannot help: it is allowed only on a child of a row, column, grid or `forEach`
  (`docs/spec/ui/document.md:252-254`). Verified by trying.
- **Workaround used:** the badge wrapped in a `row` (`layout::cellTag`), inside which it keeps its width.
- **Weight:** Low

### G16: A bottom action bar that stays in view on compact
- **Design(s):** 02, 05, 06
- **Needs:** "Reject…" and "Receive, next" pinned to the bottom of the phone screen while the form above scrolls.
- **ui/1 today:** no screen-level footer or sticky region; the only footer is a dialog's or drawer's (`actions`,
  `docs/spec/ui/document.md:177`; `ActionBar` mounts only inside one, `docs/spec/ui/view_tree.md:131`). Inferred
  from reading; not tried on a short window.
- **Workaround used:** the buttons as the last row of the screen's column, scrolling with it.
- **Weight:** Medium

### G17: A group of checkboxes with a legend and a required mark
- **Design(s):** 02
- **Needs:** "Condition on receipt \*" as the legend of four checkboxes (a `fieldset`), required as a group.
- **ui/1 today:** no group node; `required` on a `checkbox` marks that one box, and an element's `Required` error
  fires when its value is not engaged (`docs/spec/ui/interpreter.md:225-228`). Inferred from reading.
- **Workaround used:** a plain text "Condition on receipt \*" above the boxes; the receive button is enabled only
  while the three mandatory boxes are checked.
- **Weight:** Low

### G18: An overflow menu behind an icon button
- **Design(s):** 03, 05
- **Needs:** a "⋯" button beside the record's actions (accessible name "More actions") that opens a menu of further
  actions, with a separator before the destructive one and a muted hint on it ("Take out of service… (needs
  reason)").
- **ui/1 today:** `menu` is a menu bar whose entries are parts `{label, icon, keys, checked, enabled, onClick, items}`
  (`docs/spec/ui/document.md:169`): a top-level entry is drawn by its label, there is no separator part, and an
  entry's text is one string. A screen head's `actions` are buttons only (`docs/spec/ui/document.md:91-100`), so the
  overflow cannot sit with them. Verified by trying.
- **Workaround used:** a `menu` with one top-level entry labelled "More actions", placed in the page above the tabs
  (expanded) or after the two large buttons (compact); no separator; the hint is part of the entry's label.
- **Weight:** Medium

### G19: The screen head overflows the shell's top bar (defect)
- **Design(s):** 03
- **Needs:** a record's head: title, breadcrumb, three status tags and two actions, as the design draws it in two
  lines at 1000 px.
- **ui/1 today:** the shell mounts the head row in its top bar beside the title (`docs/spec/ui/shell_frame.md:20-31`),
  in one line. Reproduced: `QT_QPA_PLATFORM=offscreen morph_examples_client --app lims-gallery --ui=qt --screen
  lims-gallery.instrument --grab out.png` at the grab's 1280 × 900 window; the title wraps to two lines and the
  top bar's "Sign out" button is drawn over the head's "Book" action, which cannot be clicked. Not tried at other
  widths.
- **Workaround used:** none; the head is the seam the design maps to, so the screen keeps it.
- **Weight:** Medium

### G20: Choosing a tab from a command, and tab overflow on compact
- **Design(s):** 03
- **Needs:** compact shows three tabs and a "More ▾" tab that holds the rest; an action such as Book could open the
  Bookings tab.
- **ui/1 today:** `tabs` takes parts `{label, children}` and keeps the user's pick
  (`docs/spec/ui/document.md:176`); there is no bound selection, no command that picks a tab, and no overflow.
  Inferred from reading; the overflow was not seen, since the grab's window is 1280 px wide.
- **Workaround used:** all five tabs on both layouts; Book opens a booking dialog rather than the Bookings tab.
- **Weight:** Low

### G21: The Qt Quick progress bar ignores the theme's accent (defect)
- **Design(s):** 03
- **Needs:** the booking's elapsed bar in the accent green, thin, on a muted track.
- **ui/1 today:** `MorphProgress.qml` sets no colour (`grep -n color src/qt_quick/qml/MorphProgress.qml` finds
  none), so the Basic style's `ProgressBar` draws. Reproduced with the instrument grab above: the bar's fill at
  (970, 360) is `#3599d9`, while the bundle's theme sets `color.accent` to `#17784A`.
- **Workaround used:** none.
- **Weight:** Low

### G22: `progress` rounds an exact quotient to its places (defect)
- **Design(s):** 03
- **Needs:** a progress value computed as a fraction, such as `29 / 100` for a booking 29 % elapsed.
- **ui/1 today:** `div` keeps the exact quotient, "a decimal at the operands' places, which display rounds"
  (`docs/spec/ui/expressions.md:71-74`), so `29 / 100` is the exact 29/100 at 0 places. `progress` reads a decimal
  through `Rational::toDouble()` (`include/morph/ui/interpreter.hpp:6317-6318`), which rounds to the value's own
  places (`include/morph/util/rational.hpp:478`): the bar gets 0. Reproduced: with
  `ui::progress(ui::Handle<std::int64_t>{29} / 100, …)` the recording backend's `value` is `0` and the Qt Quick bar
  is empty; `format("{0}", 29 / 100)` shows `0` and `decimal(29 / 100, 2)` shows `0.29`.
- **Workaround used:** a literal decimal at two places, `Rational{29, 100, DecimalPlaces{2}}`.
- **Weight:** Medium

### G23: Grid columns of different widths
- **Design(s):** 03, 07
- **Needs:** a two-column list with a fixed label or timestamp column and a filling text column (`130px
  minmax(0, 1fr)`), as the design's details and recent events use; a plate's 22 px row-letter column beside twelve
  44 px well columns (`22px repeat(12, 44px)`), with the column numbers centred over the wells.
- **ui/1 today:** a `grid` takes a column count (`docs/spec/ui/document.md:171`) and its columns share the width
  (`docs/spec/ui/backend_contract.md:174`); there are no column tracks. `keyValue` lays out label and value itself.
  Verified by trying: the events in a two-column grid drew with the text misplaced.
- **Workaround used:** `keyValue` for the details; each event as a `row` of a `fixed(18)` timestamp and a stretched
  text. The plate is a 13-column grid whose letter column is as wide as a well's; its headers sit at the start of
  their cells, as `alignSelf` places a grid cell only across the row (verified in a grab, 1280 × 900).
- **Weight:** Low

### G24: Label-over-value pairs
- **Design(s):** 03
- **Needs:** on compact, the details as a stacked definition list: a small muted label above each value, divided by
  rules.
- **ui/1 today:** `keyValue` takes `items` only (`docs/spec/ui/document.md:188`) and draws label and value side by
  side; no orientation. Inferred from reading.
- **Workaround used:** a column per pair, a muted `text` over a `text`; no dividers.
- **Weight:** Low

### G25: `forEach` drops its own common properties (defect)
- **Design(s):** 04, 05, 06, 07
- **Needs:** a `forEach` with a `testId`, `visible`, `sizing` or `a11y`, as `document.md` says every node takes
  (`docs/spec/ui/document.md:150-157`).
- **ui/1 today:** the interpreter lowers `forEach` without calling `commonOf` (`include/morph/ui/interpreter.hpp:7066-7085`;
  the table beside it does, `interpreter.hpp:7212`), and `ui::forEach` takes no `Common`
  (`include/morph/ui/view.hpp:1395-1405`), though `ForEach` has one (`view.hpp:871-872`). Every common key on a
  `forEach` is loaded and then ignored, with no report. Reproduced: in the audit screen's compact layout,
  `ui::forEach(...).testId("entries")` mounted on `RecordingBackend` leaves no widget of any kind with test id
  `entries` (`screen.find("ForEach"|"Column", "entries")` both empty), while the five row cards are mounted. Not
  checked for `visible`, `drag` or `drop`; inferred from the same code path.
- **Workaround used:** each `forEach` sits alone in a `column` that carries the test id (and, in 05, `visible`).
- **Weight:** Medium

### G26: Toolbar controls in the screen head
- **Design(s):** 04, 05, 07
- **Needs:** the head row's search field ("Search record, field or reason", "Sample, lot or product") and filter
  selects ("Status: open", "Priority: any", "Template: HCP 96, 8 standards") beside the title and its buttons.
- **ui/1 today:** a screen head has `title`, `subtitle`, `status` badges and `actions`, which are buttons
  (`docs/spec/ui/document.md:63`, `91-100`); no input can sit in it. Inferred from reading.
- **Workaround used:** the inputs in a row at the top of the page, right-aligned under the head.
- **Weight:** Medium

### G27: Removable filter chips
- **Design(s):** 04
- **Needs:** each active filter as a small outlined chip with a "×" (accessible name "Remove Result") that removes it.
- **ui/1 today:** `badge` has `tone`, `text` and `icon`, no action (`docs/spec/ui/document.md:186`); there is no chip
  or token node. Inferred from reading.
- **Workaround used:** a `row` on the `card` surface holding a text and a ghost "×" button with the accessible name;
  verified it draws as a bordered box. Larger than the design's 26 px chip.
- **Weight:** Low

### G28: Paging a table over rows
- **Design(s):** 04, 05, 07
- **Needs:** "1–25 of 57" with previous and next page, and a rows-per-page select (05), or "Load 25 more" (compact);
  the first five samples to place and "+ 9 more" (07).
- **ui/1 today:** a `table` over `rows` shows every row; paging belongs to the model-backed table engine (`mode`,
  `docs/spec/ui/document.md:175`), and there is no list slicing function to page by hand
  (`docs/spec/ui/expressions.md:99-107`: `first`, no `take`/`slice`). Inferred from reading.
- **Workaround used:** the design's page text as a literal, with disabled previous and next buttons; the gallery holds
  one page. 07 lists all fourteen samples to place.
- **Weight:** Medium

### G29: A table's selection cannot be bound or set
- **Design(s):** 05
- **Needs:** the sample table opens with three rows selected; "Clear", "Select all 312" and the header's "select all on
  this page" box change the selection; a bulk action clears it when done.
- **ui/1 today:** `table` lists a `selection` key (`docs/spec/ui/document.md:175`, `include/morph/ui/document.hpp:816`)
  and the element exposes `selection` and `selectedCount` for reading (`include/morph/ui/interpreter.hpp:4529-4537`),
  but the lowering binds the view's selection to the element alone (`interpreter.hpp:7182-7199`), never to the key,
  and `set` cannot name a table (`docs/spec/ui/document.md:268`). Reproduced: the table with `selectionMode:
  multiple` and `selection` bound to a state list of three sample ids mounts on `RecordingBackend` with
  `selection=[]` and reports no problem. The key is accepted and ignored.
- **Workaround used:** no table selection. A first column of check boxes bound to a `selected` state list (checked
  while the row's id is in it, toggling adds or removes it), and a "Select all on this page" box above the table in
  place of the header's.
- **Weight:** High

### G30: A control in a column header
- **Design(s):** 05
- **Needs:** a "select all on this page" check box as the selection column's header.
- **ui/1 today:** a column part is `{id, label, width, role, cell}` (`docs/spec/ui/document.md:175`); the header is its
  label. Inferred from reading.
- **Workaround used:** the check box above the table, at the toolbar's start.
- **Weight:** Low

### G31: Sorting a table over rows
- **Design(s):** 05
- **Needs:** "Due ▲" as a sorted, sortable column header (`aria-sort`), and on compact a "Sort: due, earliest first"
  control.
- **ui/1 today:** `sort` is a table key, but "`sort`, `filters` and `hoveredKey` are not lowered"
  (`docs/spec/ui/interpreter.md:320`) for a table over rows; sorting is the model-backed table engine's. The
  expression language has no sort function to order a list by hand (`docs/spec/ui/expressions.md:99-107`). Inferred
  from reading.
- **Workaround used:** the rows are held in due order; the header reads "Due ▲" as a label, and compact's sort button
  is disabled.
- **Weight:** Medium

### G32: `surface` cannot follow the data
- **Design(s):** 05, 07
- **Needs:** a compact sample card tinted while its sample has a result out of specification, the tint the table row
  gets from `rowTone`; a plate well drawn by what it holds, a solid outline while it holds a sample and a dashed one
  while it is empty.
- **ui/1 today:** `surface` is a literal string on every node (`docs/spec/ui/document.md:156`), set once
  (`docs/spec/ui/view_tree.md:147`; `author::Node::surface(std::string const&)`), and there is no tone on a panel or
  card. Custom surfaces do work: the gallery's theme declares `selected` and `outOfSpec` surfaces, and a panel on
  `selected` draws its tint on Qt Quick (verified in a grab of the bulk bar; `outOfSpec` is used only on compact, which the client cannot show, G1). Inferred from reading for the binding.
- **Workaround used:** each card twice, one on the `outOfSpec` surface and one plain, shown by `visible` on the row's
  tone. In 07 the standards, controls and blank are fixed by the template, so their wells take literal `standard`,
  `control` and `blank` surfaces (verified: they draw blue, amber and grey on Qt Quick); a sample well and an empty
  one differ only by their label, `S12` or `·`.
- **Weight:** Low

### G33: An outlined tag
- **Design(s):** 05
- **Needs:** priority tags ("STAT", "Urgent", "Routine") drawn outlined on white, so priority does not read as a status
  tone: a red filled tag means out of specification, a red outlined one means STAT.
- **ui/1 today:** `badge` has `tone`, `text` and `icon` (`docs/spec/ui/document.md:186`), no variant. Inferred from
  reading.
- **Workaround used:** filled tags in the err, warn and neutral tones; STAT and "Out of spec" are told apart by text
  only.
- **Weight:** Low

### G34: Qt Quick `keyValue` reports a polish loop for some caption and value widths (defect)
- **Design(s):** 06
- **Needs:** the stability study's summary as a `keyValue` (Product, Lot, Protocol, Pulls, First pull, Last pull,
  Containers), whose values follow the conditions.
- **ui/1 today:** reproduced on revision 842701f0d plus this branch, Qt 6.12.0 (Arch `qt6-base 6.12.0-3`,
  `qt6-declarative 6.12.0-1`), offscreen, the default font resolving to Nimbus Sans. The smallest document found is a
  screen whose whole view is one `keyValue` with one pair, built as
  `ui::keyValue(ui::list({ui::obj({{"label", "Containers"}, {"value", "needed"}})}))`. Mounted by
  `lims_gallery_render_smoke`, it fails with, verbatim and repeated, `Qt: qrc:/qt/qml/MorphUi/qml/MorphKeyValue.qml:17:5:
  QML GridLayout: possible QQuickItem::polish() loop` and `GridLayout called polish() inside updatePolish() of
  GridLayout`. Whether it loops depends on the text widths alone, not on the container: the same pair loops bare, in
  a column, in a row beside a stretched column, and in a `fixed(33)` or `fixed(60)` panel. Pairs measured, one per
  run:

  | Pair | Loops |
  |---|---|
  | Containers = `needed`, `180 needed`, `180 needed, 150`, `180 needed, 150 in stock`, `xxxxxxxxxx` | yes |
  | Containers = `180`, `a b`, `x` | no |
  | Container, Lot number, Lot numbers, Location = `180 needed` | no |
  | Lot = `180 needed, 150 in stock`; Product = `Paracetamol 500 mg`; First pull = `03-Dec-2026 (40 °C, 1 m)` | no |
  | Product = `Paracetamol 500 mg` then Containers = `x` | yes |

  Inferred from reading `src/qt_quick/qml/MorphKeyValue.qml:31-48`, not verified: the value column's
  `Layout.maximumWidth` is `grid.width - labelColumnWidth - columnSpacing`, and with the widget sized to its content
  that is the widest value's own width, so a fractional pixel of rounding can make the cap a hair narrower than the
  text. The value then wraps, the grid's implicit width changes, and the cap is recomputed inside the polish, for
  ever. That would explain why it follows the widths and not the layout. The instrument screen's `keyValue`
  (design 03) does not loop with its values. Not checked on any other Qt version, font or platform.
- **Workaround used:** design 06 draws its summary, and the product and review panels of its other steps, as rows of
  a muted `fixed(13)` caption beside a stretched `text` (`pair` in `stability.cpp`). Unlike `keyValue`, that row lets
  the containers value take the `error` role (G39).
- **Weight:** Medium

### G35: A command cannot name an element inside a `switch` case from outside it
- **Design(s):** 06
- **Needs:** the wizard footer's Continue, outside the step's content, moving focus to the field that blocks it
  (Containers per pull).
- **ui/1 today:** a switch case is a scope (`docs/spec/ui/document.md:271-273`), and an element belongs to the scope
  it is declared in; the screen's `focus` refused it. Reproduced, verbatim: `document lims-gallery.stability at
  /view/default/0/children/2/children/3/onClick/0/if/1/1: \`perPullInput\` is not an element of this scope`. The
  wizard's page has to be a `switch` on the step, so every field of a step is out of the footer's reach.
- **Workaround used:** Continue only shows the error banner; the banner's own action, inside the case, focuses the
  field.
- **Weight:** Medium

### G36: A field of a computed value cannot be read
- **Design(s):** 06
- **Needs:** "the date of the first pull": the `date` field of the entry a `filter` and `first` pick.
- **ui/1 today:** the loader refuses it, verbatim: `a field is read below a name -- a param, state, let, query, row,
  element or bound name -- and not below a computed value; field \`date\``. Verified by trying.
- **Workaround used:** `map` the field out first, then `first` of that list.
- **Weight:** Low

### G37: Date arithmetic
- **Design(s):** 06
- **Needs:** pull dates as T0 plus a number of months ("First pull 03-Dec-2026 (40 °C, 1 m)", "Last pull
  03-Nov-2029"), following the study start the user picks.
- **ui/1 today:** the functions are logic, arithmetic, strings, lists and display (`docs/spec/ui/expressions.md:99-107`);
  no date addition. Inferred from reading.
- **Workaround used:** a table of each timepoint's date from the design's T0 (03-Nov-2026), looked up by month; the
  dates do not follow a new T0.
- **Weight:** Medium

### G38: A wizard over screen state
- **Design(s):** 06
- **Needs:** a four-step wizard (Product and lot, Conditions and timepoints, Tests per timepoint, Review and sign) with
  Back, Continue blocked by the step's errors, and Save draft.
- **ui/1 today:** `wizard` is model-backed: `model` and `action` are required (`docs/spec/ui/document.md:190`); with no
  backend there is none. `steps` draws the progress (`items`, `current`; verified: `[Product and lot:Done,Conditions
  and timepoints:Active,…]`). Inferred from reading for `wizard`.
- **Workaround used:** `steps` plus a `switch` on a `step` state and hand-made Back and Continue buttons (see G35).
- **Weight:** Medium

### G39: A key-value pair with a tone
- **Design(s):** 06
- **Needs:** the summary's "Containers: 180 needed, 150 in stock" in red, bold, while stock is short.
- **ui/1 today:** `keyValue` takes `items` of `{label, value}` (`docs/spec/ui/document.md:188`); no tone or role per
  value. Inferred from reading.
- **Workaround used:** the summary is not a `keyValue` (G34) but rows of a caption and a `text`, and the containers
  value takes the `error` role while stock is short: red, not bold.
- **Weight:** Low

### G40: Table cells sit at the top of a taller row
- **Design(s):** 06
- **Needs:** a table row's text centred against the controls beside it: the condition's name level with its row of
  check boxes, the test's name level with its "When" select.
- **ui/1 today:** a table sizes and places its cells itself (`docs/spec/ui/backend_contract.md:156-162`), and a
  cell takes no `alignSelf` (`docs/spec/ui/document.md:252-254`, G15). On Qt Quick a `text` cell draws at the top of
  its row while a `checkbox` or `select` cell beside it is taller, so the text sits above the controls' centre line
  (seen in a grab of the conditions matrix and of the tests table, 1280 × 900, offscreen). Verified by trying; not
  checked on the terminal frontend.
- **Workaround used:** none.
- **Weight:** Low

### G41: A selectable cell that keeps its own look
- **Design(s):** 07
- **Needs:** a plate well as a 40 px circle, outlined, that draws a thick accent ring while it is selected; clicking
  it selects it.
- **ui/1 today:** a button's `selected` is a condition only with `style: item` (`docs/spec/ui/document.md:165`),
  which draws an unframed list entry with an accent tint and a bar at its leading edge
  (`src/qt_quick/qml/MorphButtonBackground.qml:4-6`); a push button has no selected or pressed state, `emphasis` is a
  literal in the author API (`author::ButtonOptions::emphasis`), and there is no shape or size per node beyond
  `sizing`. Verified by trying: the sample wells draw unframed, with the tint and bar while selected (grab, 1280 ×
  900).
- **Workaround used:** a sample well is a `style: item` button with `selected` bound to the selection; the template
  wells are push buttons on their surfaces (G32). No circles.
- **Weight:** Medium

### G42: Enter on a focused button may never reach the button's own key binding on Qt Quick (defect, inferred)
- **Design(s):** 07
- **Needs:** Enter on the focused well opens Assign for it, while a click selects the well.
- **ui/1 today:** a node's `keys` are chords handled while focus is inside the node (`docs/spec/ui/view_tree.md:148`).
  The screen binds `Enter` on each sample well, and `RecordingBackend::press` delivers it (the plate test opens the
  dialog with it). On Qt Quick the binding is `Keys.onPressed` on the widget's root (`src/qt_quick/qml/MorphWidget.qml:187-190`),
  but focus is on the `Button` inside it, whose `Keys.onReturnPressed` and `Keys.onEnterPressed` fire the click
  (`src/qt_quick/qml/MorphButton.qml:95-96`) and, by Qt's default, accept the event, so it would not propagate to the
  root. Inferred from reading only: not reproduced, since the offscreen render smoke sends no keys. If it holds,
  Enter selects the well instead of opening Assign, and the two backends disagree.
- **Workaround used:** none; the binding is kept as the design asks.
- **Weight:** Medium

### G43: A click cannot tell its modifiers
- **Design(s):** 07
- **Needs:** Shift-click selects the range of wells from the last one clicked.
- **ui/1 today:** `onClick` carries no `event` payload and no modifiers (`docs/spec/ui/document.md:165`); a chord
  binding takes keys, not clicks (`docs/spec/ui/view_tree.md:148`). Inferred from reading.
- **Workaround used:** a click adds or removes one well; Shift with an arrow key adds the neighbouring well and moves
  focus to it, so a range is built from the keyboard. The page's help line says so in place of the design's "Shift-click
  selects a range".
- **Weight:** Low

### G44: Grouping a list into runs
- **Design(s):** 07
- **Needs:** on compact, consecutive empty wells as one line, "A5–A8 · Empty, 4 wells" with one Assign button, and
  rows D to H folded into one section.
- **ui/1 today:** the list functions are `count`, `sum`, `mean`, `min`, `max`, `any`, `all`, `first`, `includes`,
  `indexOf` and `concatLists` (`docs/spec/ui/expressions.md:107`), with `map` and `filter`; nothing groups adjacent
  elements. Inferred from reading.
- **Workaround used:** one line per well, twelve per row, and one section per row.
- **Weight:** Low

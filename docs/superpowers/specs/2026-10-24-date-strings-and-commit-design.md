# Date strings and input commits: design

Two `ui/1` inputs drop what a document asks of them without saying so:

- **A `dateTimeInput` bound to an ISO string shows an empty field.** Its handler reads the draft with
  `asTimestamp()`, a plain `std::get_if`, so a string reads as nothing. The field then writes a `timestamp` back into
  a value the application keeps as a string. For a `string` state, `writeSlot` refuses that write: "`set` writes a
  value that is not a string".
- **`onCommit` on `dateTimeInput` and `slider` never runs.** The loader accepts the key on both nodes and the spec
  lists it, but only the `textInput` handler wires it. Neither `ui::DateTimeInput` nor `ui::Slider` carries a commit
  callback, and `touched` is set only on a commit, so an error shown in `touched` mode never shows on either.

The layout survey (missing min/max sizes, alignment, form presentation, empty tables, list navigation) needs no code.
Section 3 records the evidence that it is done.

## 1. A date input shows and writes the value's own type

### Which type the field holds

Each `dateTimeInput` holds its value either as a **string** or as a **timestamp**. That choice decides what it writes
back, and it is made in this order:

1. **The static type of `value`, at load.** `checkExpr` already types every expression. A `string` state, a
   `string` parameter or a string literal gives `string`, and a `timestamp` gives `timestamp`. Optional forms count
   the same way. Any other known kind (`bool`, `int`, `decimal`, `quantity`, `record`, `list`) can never be shown, so
   **the loader refuses the document** at the node's `value`: "`dateTimeInput` shows a timestamp or an ISO-8601 date
   or date-time string; `value` is int". `any` and `key` stay undecided.
2. **Undecided at load** (a query field, a reply, an element's value): the bound value's type at the moment of the
   edit. A string gives string, and anything else gives timestamp.
3. **Uncontrolled, or bound to null with an undecided type:** timestamp, as now.

The loader records step 1's outcome for each `dateTimeInput` in a side table that the interpreter reads. The table is
keyed by node, as `_scopeOf` is, and its entries are `string` or `timestamp`; an undecided node has no entry. The
table lives on the loaded `Document` because the checker sees const nodes, and a node can be reused through a
component, but its value's type is fixed by the document.

So the gallery example's `dateTimeInput(when, …)` over `state<std::string>("when")` writes a string from the first
edit, although `when` starts null.

### What it shows

| Value | Shown as |
|---|---|
| `timestamp` | that instant in the display zone (unchanged) |
| string `yyyy-MM-dd` | that calendar date: the instant at midnight of that date in the display zone |
| string that `DateTime::fromIso8601` reads (`yyyy-MM-ddTHH:MM:SS[.fff][Z]`, UTC) | that instant |
| null, or the empty string | an empty field |
| anything else (another string, an int, a record, …) | an empty field, **and a diagnostic** |

The date-only form is a calendar date, not an instant. The forms engine stores a `date` field as the same string and
gives it no zone (`FieldKind::Date` in `include/morph/forms/engine/values.hpp`). Reading it as midnight in the display
zone makes the field show the date that is written, whatever the zone. Reading it as midnight UTC would show the day
before in every zone west of UTC. Both frontends emit local midnight for a date-mode entry and format with the same
offset, so the read and the write-back round-trip exactly. The offset used for reading is the
`format.utcOffsetMinutes` that the handler already passes to the widget as `spec.offsetMinutes`.

The full form is the one `valueFromJson` already reads for a `timestamp` type, and that the `date`/`dateTime`
expression functions already accept from a string (`include/morph/ui/expression.hpp`). It adds no new syntax.

**The diagnostic** goes through `ScreenCore::report` at the node's `value` path, with a fixed reason: "`dateTimeInput`
shows a timestamp or an ISO-8601 date or date-time string". The held value is not part of the reason, because
`report` deduplicates on path and reason. A value that changes on every keystroke elsewhere would otherwise add an
entry each time. Reporting from inside the value slot is safe, since `roleProp` already reports from one. Inspector
evaluation is suppressed by `inspecting`, and neither widget reports a change mid-typing, so a partly typed string
never reaches the draft.

### What it writes back

| The field holds | Written on an edit | Written on clearing |
|---|---|---|
| string | `yyyy-MM-dd` in `date` mode (the picked instant's date in the display zone); `DateTime::toIso8601()` in `date-time` mode | `""` |
| timestamp | the `timestamp` (unchanged) | null (unchanged) |

The string form follows the field's `mode`, not the shape of the string it held. A date-only field always writes a
calendar date, and a date-time field always writes a UTC instant.

The event value of `onChange` and `onCommit` is the value written, so an action bound to the event receives the
application's own type. A `""` draft is not `engaged`, so a `required` date bound to `""` still counts as missing.

### Following the bound value: `sameShown`

After an edit, the input follows its bound value again once the bound value shows the same as the draft. For
`dateTimeInput`, `==` is the wrong test. An application that stores `…T10:16:00Z` gets back a draft of
`…T10:16:00.000Z`, `edited` stays true, and the field stops following its value. `sameShown` gains a
`dateTimeInput` case: both sides are read to an instant with the rule above, and the two instants are compared. Two
values that read to nothing (null, `""`, unreadable) count as the same.

### Why not the other two options

- **Refusing every string at load** would also refuse a string state that holds a date, which is the reported use. The
  refusal above covers only types that can never be shown.
- **Documenting only** leaves the silent empty field in place, which is the defect.

## 2. `onCommit` runs on `dateTimeInput` and `slider`

### `dateTimeInput`

Both backends report a date input's change only when an edit is finished. On Qt Quick that is focus leaving after
typing, Enter, or a day picked in the calendar. On the terminal it is the same, plus each Up/Down/PageUp/PageDown step
(`field_widgets.cpp`). So each change is also the commit. The handler follows `textInput`'s order: it marks the input
`touched`, writes the draft, runs `onChange`, and then runs `onCommit` with the same value. The widget spec needs no
new callback.

### `slider`

A slider reports every step while it is dragged, so a change is not a commit.

- **`ui::Slider`** gains `std::function<void(std::int64_t)> onCommit`, and **`SliderWidget`** gains a pure virtual
  `setOnCommit`. It is pure, not a default no-op, so a backend that leaves it out fails to compile instead of
  silently dropping the event. `mount.hpp` wires it with `event(spec.onCommit)`, as `textInput`'s commit is wired.
  It is not wired as `onChange` is, because that path re-asserts the shown value.
- **Qt Quick** (`MorphSlider.qml`): fire `"commit"` with the value when `pressed` turns false after the value moved
  while pressed. `QQuickSlider` sets `pressed` around a key press as well as a mouse press (`keyPressEvent` and
  `keyReleaseEvent` in Qt 6.8), so one rule covers the mouse and the keyboard. A wheel move happens while not pressed,
  so a move while not pressed also commits. A read-only slider refuses the move and commits nothing.
- **Terminal** (`SliderImpl`): a key step is a finished edit, so it calls `onChange` and then `onCommit`. A pointer
  drag (`pressAt`, `dragTo`) calls `onChange` per cell and commits on `pressEnded()` when the value moved. A handler
  may destroy the slider (`moveTo` says so), so both handlers are copied to locals before either runs, and nothing on
  `this` is touched after them.
- **Recording backend** (`FakeSlider`, which `tests/ui_echoing_backend.hpp` reuses): `slide(id, value)` stays a
  finished move and calls both. A new `drag(id, value)` calls only `onChange`, so a test can tell a move in progress
  from a commit.
- **Conformance:** `settersCallNoHandler` adds `.onCommit = note("Slider onCommit")`, so a setter that commits fails
  on every backend.
- **The handler:** `onCommit` marks `touched` and runs the document's `onCommit` with the value.

### The spec

- `docs/spec/ui/view_tree.md` gets the `onCommit` row for `Slider`, and for `DateTimeInput` says that its change is
  the commit and when each one commits.
- `docs/spec/ui/interpreter.md` says the same in its input paragraph, and gives the type rule and `sameShown` from
  section 1.
- `docs/spec/ui/document.md` already lists the key. The `dateTimeInput` row gains the accepted value types and the
  load refusal.

## 3. The layout survey: verdict, no code

| Ask | On master | Covered by |
|---|---|---|
| `minWidth`/`maxWidth`/`minHeight`/`maxHeight` | `LayoutHints` (`view.hpp`), `document.md` | `backend_conformance.hpp`, run by both frontends |
| `align`/`justify` on row, column and grid; `alignSelf` | `document.hpp` keys, `StackWidget::setAlign`/`setJustify` | `backend_conformance.hpp`, run by both frontends |
| `form`/`collection`/`wizard`: `title`, `submitLabel`, `showStatus`, `columns`, `fieldLayout` | `document.md` node table | `test_ui_interpreter_forms.cpp`, `test_forms_engine_view.cpp`, `test_ui_author_forms.cpp` |
| `table`'s `empty` | `document.md` | `test_qt_quick_interpreter.cpp` |
| A list-item navigator with a selected state | `ButtonStyle::Item`, `Navigator` | `test_tui_widgets.cpp`, mount tests |
| A type scale: app name, then section, then entry | `TextRole::Heading` / `Section` / item buttons | `test_qt_quick_containers.cpp`, `test_tui_widgets.cpp` |
| A tab bar hidden with one tab | `Workspace` `setBarShown(count > 1)` | `test_ui_workspace.cpp` |
| A signed-out frame without sidebar and principal; the principal level with *Sign out* | `ShellFrame` | shell frame tests |

Only the layout rows run in the cross-backend suite. The rest are tests of one backend or of the interpreter. The
example screens (`bank_screens.cpp`, `bookmarks_screens.cpp`, `lims_screens.cpp`) already use `maxWidth`, `align`,
`justify`, `submitLabel`, `showStatus` and `fieldLayout`. The survey issue closes with this table. It reopens on a
screen built from a design whose layout still needs a hand-placed spacer or a fixed width.

## Testing

Every test below states its control: what to break, and which assertion must then fail.

- **Load, refused type:** a `dateTimeInput` whose `value` is an `int` state is refused at `…/value`. A `string` state
  and a `timestamp` state load. Control: drop the refusal.
- **Interpreter, string shown** (recording backend): a `dateTimeInput` bound to `"2026-10-23"` with a display offset
  of −300 minutes records the instant `2026-10-23T05:00:00.000Z`. Control: read the string as midnight UTC, and it
  records `2026-10-23T00:00:00.000Z`.
- **Interpreter, string written back:** an edit on a field over a null `string` state, as in the gallery example,
  writes a string in each mode, and the `onChange` event value is that string. Clearing writes `""`. An undecided
  value (a query field) that holds a string writes a string. Control: write a `timestamp`, and the `set` is refused
  and the type assertion fails.
- **Interpreter, timestamp unchanged:** a `timestamp`-typed field still writes a `timestamp`.
- **Interpreter, following the value:** after an edit writes `…T10:16:00.000Z`, a bound value of `…T10:16:00Z`
  clears `edited`, and a later change to the bound value shows. Control: compare with `==`.
- **Interpreter, bad value reported:** an undecided value holding `"23.10.2026"`, then `"24.10.2026"`, leaves the
  field empty and gives exactly one diagnostic at the node's `value` path. Control: drop the report, or put the
  value in the reason, which gives two.
- **Interpreter, `onCommit` on `dateTimeInput`:** an edit runs `onCommit` once, after `onChange`, with the written
  value, and sets `touched`. Control: drop the call.
- **Interpreter, `onCommit` on `slider`:** `drag` runs `onChange` and not `onCommit`; `slide` runs both and sets
  `touched`. Control: commit on every change.
- **Conformance:** `settersCallNoHandler` with `onCommit` passes on all three backends.
- **Qt Quick, slider:** a mouse drag across two steps reports two changes and one commit, on release. A key click
  (press and release) reports one change and one commit. Control: drop the release handler.
- **Terminal, slider:** an arrow key reports one change and one commit. A pointer drag across two cells reports two
  changes and one commit, on release. Control: commit on every drag step.

## Out of scope

- A `dateTimeInput` showing a date string in a zone other than the display zone. Strings have no zone, and the field
  has one.
- Accepting other date string forms (`dd.MM.yyyy`, offsets other than `Z`). Each one is reported, not guessed.

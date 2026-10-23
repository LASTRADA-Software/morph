# Open UI issues #1024, #1027, #1033, #1034

The declarative-UI issues still open in #1020–#1034. They land as one branch and one PR, one commit per change,
cheapest first: #1033, #1034, the date input's re-show fix, #1024, #1027.

## #1033 — a tab's label colour is set on a palette role the style does not read

**Verified.** `frontend.cpp` selects the Basic style. `MorphTabs.qml` sets only `palette.buttonText` on its
`TabButton`, but Basic's `TabButton.qml` colours its label with `checked ? palette.windowText : palette.brightText`.
Fusion reads `buttonText`. So under Basic the scheme's colours never reach the label.

**Fix.** The delegate gets its own `contentItem`: a `Text` with the tab's `font` and `text`, the colour
`checked ? root.scheme.accent : root.scheme.muted`, centred, and elided to the right. This doesn't depend on which
palette role a style reads. The default `contentItem` is an `IconLabel`, which adds only icon support, and morph tabs
have no icons. Width is unchanged, since `width: implicitWidth` follows the `Text`'s implicit width. Accessibility
is unchanged, since the accessible name comes from `TabButton.text`.

**Test** (`tests/qt_quick/test_qt_quick_theme.cpp`): under a branded theme (`brandTheme()`, whose accent and muted
colours differ from Basic's defaults), `labelColor()` of the checked tab is the accent colour and of an unchecked tab
the muted colour. **Control:** revert the delegate, and the test fails.

## #1034 — no logging on the UI bundle serve and fetch path

**Verified.** `service.hpp` logs only document warnings. `service_client.hpp` has no `morph::log` call. The error a
client gets for a file that fails verification is `"<file> does not match its hash"`, which carries neither hash.

**Fix.** Each call is written as `::morph::log::logDebug("[ui-…] … {}", …)` or `logWarn`, using the format overload,
as `remote.hpp` and `io_loop.hpp` do. No document or file content is logged. Hashes are content addresses, not
secrets, so they are logged in full.

- **Registration (`UiRegistry`).**
  - `addScreen`, `addEntry`, `addFile` and `build` each log at their own boundary. On success they log at debug:
    kind, id and byte size; for `build`, the manifest digest and the number of files.
  - On a refusal they log `Refusal::message()` at warn. Refusals come from several internal places
    (`checkNewEntry`, a `loadDocumentText` error, `refuse()`, `checkBuildable()`), so the public method is the one
    point they all pass through. Tag: `[ui-serve]`.
- **Requests (`UiService`).** Each reply logs at debug, giving the request kind, the hash or application, and the
  outcome, including refusals such as `unauthorized`, `not found` and `missing vocabulary`. Tags: `[ui-hello]`,
  `[ui-catalog]`, `[ui-fetch]`. A refused request stays at debug so that a hostile client can't flood a log at
  warn level.
- **Client (`UiClient`).**
  - Opening and polling an application logs, at debug: the application, the known digest, whether the manifest
    changed, and each hash the cache did not hold and so has to fetch.
  - A file or manifest that fails verification now returns an error naming both the expected and the actual hash,
    so callers see them too.
  - Every failure already passes through `fail()`, which logs the error once at warn. That one call covers a
    transport failure, a server refusal and a hash mismatch. Tag: `[ui-fetch]`.
- **Cache (`VerifiedCache::get`).** A stored file that no longer matches its hash is logged at warn as dropped.
  Hits and misses are not logged one file at a time, because the client's fetch line already lists the misses. Tag:
  `[ui-cache]`.

**Tests.**

- `tests/test_ui_service_client.cpp` already has fixtures for a mismatch (around line 453) and a cache drop (around
  line 495). Run them under `morph::log::ScopedLoggerOverride{sink, LogLevel::debug}`, the pattern
  `test_logger.cpp` uses, and assert:
  - the warn line carries both hashes;
  - the `[ui-cache]` drop line appears;
  - a successful open produces its debug lines.
- Update the existing assertions on the error text to the new message, which now names both hashes.
- In the service test, a refused fetch logs its `[ui-fetch]` line at debug.
- **Control:** delete the `fail()` log call, and the warn assertion fails.

## The date input re-shows its value only once

Found while reviewing #1024, inferred from the code and not yet reproduced; the commit adds the test that
reproduces it.

`DateTimeImpl`'s `edit` handler answers unparseable text by re-showing what the field held:
`QString const shown = _shown; _shown.clear(); show(shown);`. After one bad edit `_shown` is empty, so a second bad
edit runs `show("")`. The field goes blank while the value is unchanged and `onChange` never fires.

**Fix.** Re-show without forgetting it: keep `_shown`. The clear is there to defeat `show()`'s no-op when the text
is unchanged, so make that no-op explicit instead of clearing state for it.

**Test** (`tests/qt_quick/test_qt_quick_widgets.cpp`, next to the existing single-bad-edit test): two bad edits in a
row leave the held value's text shown. **Control:** restore the clear, and the second assertion fails.

## #1024 — a date input has no calendar

**Verified.** `MorphDateTimeInput.qml` is a `TextField`; nothing in `src/qt_quick/qml` uses a calendar. A comment
on the issue reports a downstream calendar popup whose `openPicker()` referred to an undeclared id. QML swallowed the
`ReferenceError`, so clicking the button silently did nothing. The tests below therefore drive real clicks.

**Decision: every graphical date input offers a calendar, with no new vocabulary field.** The calendar is
additive: typing works exactly as before, and the calendar is a second way to produce the same text. That is the
difference from `select`'s `style` (`SelectStyle::Dropdown`/`Radio`), which an author does choose, because it
replaces one layout with another. A backend that offers no calendar (the terminal) behaves exactly as today, so there
is nothing for an author to request. `backend_contract.md` states that a graphical backend offers a calendar beside
the typed field and that the terminal offers typed text only.

**Qt Quick.**

- `DateTimeImpl` passes `offsetMinutes` as an initial property next to `withTime`. The QML derives "today" from it
  (UTC now plus the offset) rather than from the system time zone. Otherwise, near midnight, the default month and
  the today outline could be a day away from the date C++ parses.
- A calendar `ToolButton` sits at the field's trailing edge, inside the `rightPadding` it reserves. It has
  `focusPolicy: Qt.NoFocus`, `Accessible.name: "Pick a date"`, and is hidden while the input is read-only.
- Its icon is `Theme.iconSource("date.pick")`. With no icon set, it shows the literal glyph `▦` as text, the way
  `MorphBanner` writes `"×"`.
- `date.pick` joins `kFrameworkIcons`, its doc comment and `theme.md`'s "Framework names" bullet. `iconGlyph` maps it
  to `▦`, so an author-set node icon named `date.pick` still draws on the terminal.
- The button opens a `Popup` with:
  - a month header: previous and next buttons, and the month's name from `Qt.locale()`;
  - a `DayOfWeekRow` and a `MonthGrid`, both in `QtQuick.Controls` from Qt 6.3; morph requires 6.5, CI uses 6.8.1
    and the WASM build 6.10. Every delegate declares its required properties, because `qt_quick_qmllint` fails on any
    warning.
- The popup opens on the month of the field's current date, parsed from its `yyyy-MM-dd` prefix, or on today's
  month. That date is marked with the scheme accent, and today gets an outline.
- Clicking a day builds `yyyy-MM-dd` from the delegate's integer `year`, `month` and `day` roles, not its JS `Date`,
  whose conversion depends on the time zone. A date-time input keeps the `HH:mm` already in the field, or uses
  `00:00`. The click then:
  1. sets the field's text;
  2. sets `root.edited = false`, so a later blur doesn't report the same date again;
  3. calls `root.fire("edit", text)`;
  4. closes the popup.

  `DateTimeImpl` parses that text exactly as it parses typed text, so the zone offset is applied in one place.
- Escape or a click outside closes the popup without a change. Focus returns to the field.

**Tests** (`tests/qt_quick/test_qt_quick_widgets.cpp`):

- An editable input has a visible picker button; a read-only one does not.
- Clicking the button opens the popup, and clicking a day reports the matching instant through `onChange` exactly
  once. Then blur the field and check that `onChange` is still at one call. Covered for a date input, and for a
  date-time input with a nonzero offset, which keeps its time of day.
- With a value two months before today, the popup opens on that value's month.
- **Control:** make the day click fire nothing, and the second test fails.

**Not in scope:** a draft that holds an ISO string instead of a `timestamp` still shows empty. That is filed as
#1039.

## #1027 — a screen's head shows twice when a shell puts it in its top bar

**State.** A document screen's `title`, `subtitle`, `status` and `actions` are its chrome, exposed as the handle's
`ScreenHead`. `Workspace::mountPage` mounts a head row above each page. `morph::client::Shell` also mounts
`Workspace::headRowForActive()` into `ShellFrame::headSlot()`, on the full-width `toolbar` surface. So the head shows
twice.

**Fix.**

- **The option.** A `Workspace` constructor parameter, `HeadPlacement headPlacement = HeadPlacement::InPage`, with
  `enum class HeadPlacement : std::uint8_t { InPage, External }`. It is not a field of `WorkspaceOptions`: that
  struct also configures `WorkspaceModel`, which would silently ignore it.
  - `InPage` is what happens today.
  - `External` makes `mountPage` mount no head row; the host mounts `headRowForActive()` wherever it wants.

  `Shell` constructs its `Workspace` with `External`.
- **Trailing actions.**
  - The row lays out at full width only when a visible child stretches along its axis (`MorphStack.qml`'s
    `stretchesAlong`/`packed`). The bare `ui::spacer()` in `buildHeadRow` sets only `Layout.fillWidth`, so its
    `widthKind` stays 0 and the row stays packed.
  - `buildHeadRow` gives the spacer `common.layout.width = Sizing::stretch()`, and the row the same, so the actions
    sit at the trailing edge in both placements.

**Tests.**

- `tests/test_ui_workspace.cpp`:
  - The existing test that a separately mounted head row gives `rows == 2` stays as the `InPage` regression test.
  - New: with `External`, no page contains a `head` test id, and `headRowForActive()` still shows the active tab's
    subtitle and actions.
- `tests/client/test_sign_in.cpp`: after sign-in, a screen with a head action shows exactly one
  `head_action_<id>`, inside `shell_head`. **Control:** make `External` mount the head anyway, and this test fails.
- Qt Quick: in a wide slot, the last head action's right edge is within one gap of the slot's right edge.
  **Control:** drop the spacer's stretch, and the test fails.

**Spec.** `workspace.md`'s "The head row" section gains the placement parameter and the trailing alignment.
`client.md` says that the shell uses `External`.

**Closing #1027, with what is not covered.** A shell built on `Workspace` gets the head exactly once, at the
position it chooses. A custom shell that mounts screens through `DocumentScreenHost::mountInto` without a
`Workspace` still reads `handle->head()` and builds its own row; no reusable row is offered on that path. Arbitrary
document-authored chrome content (the issue's "second builder") is not provided. Re-open if a screen's chrome needs
more than a title, a subtitle, badges and buttons, or if a second shell outside `Workspace` needs the reusable row.

## Closing

#1033, #1034, #1024 and #1027 close on merge. #1039 stays open.

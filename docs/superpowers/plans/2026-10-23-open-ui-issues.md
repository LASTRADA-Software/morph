# Implementation plan: open UI issues #1033, #1034, date-input re-show, #1024, #1027

The design is `docs/superpowers/specs/2026-10-23-open-ui-issues-design.md`. Read it in full before starting; it
gives the reasoning, and this plan gives the order and the mechanics. Where the two disagree, the design wins: stop
and report the disagreement instead of picking a side.

## Ground rules (apply to every task)

- **Worktree:** `/home/yaraslau/repo/morph-ui-open`, branch `fix/open-ui-issues-1024-1027-1033-1034`. Work only
  here. Do not push, do not open a PR, and do not touch any other checkout.
- **Read first:** `AGENTS.md`, and the spec files each task names under `docs/spec/`. The comment rules in AGENTS.md
  apply:
  - no history, no issue numbers and no commit hashes in comments;
  - every public symbol gets complete Doxygen `@brief`/`@param`/`@tparam`/`@return`;
  - match the density and idiom of the code around you.
- **Builds:** every build, test or clang-format run goes through the shared lock, with at most 10 jobs:
  ```
  L=/tmp/claude-1000/-home-yaraslau-repo-morph/8742d70b-a059-4ed6-9415-5f45a5067fd9/scratchpad/morph-build.lock
  flock $L cmake --build build/linux-everything -j10 --target <targets>
  flock $L timeout 600 ctest --test-dir build/linux-everything -R '<regex>' --output-on-failure
  ```
  - The build tree `build/linux-everything` is already configured. Do not reconfigure it with another compiler
    cache or launcher.
  - Never `pkill -f` anything.
  - Run Qt Quick tests with `QT_QPA_PLATFORM=offscreen`; ctest already sets it for the `qt_quick` label.
- **Targets:** `morph_tests` (core and UI unit tests), `morph_qt_quick_tests`, `morph_client_tests`. The QML lint
  test is `ctest -R qt_quick_qmllint`, and it fails on any qmllint warning.
- **One commit per task,** in the order below. The subject is an imperative sentence about the behaviour (see
  `git log --oneline -20` for the house style). The body explains why. Add `Closes #<n>` to the bodies of the
  #1033, #1034, #1024 and #1027 commits, and end every message with:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01BmVLNhUvbWZjv85Hx2saxn
  ```
  Commit as the repository's configured git user.
- **Every new test needs a control.** After a test passes, break the feature as the design's "Control" line says,
  rebuild, confirm the test **fails**, then restore the feature.
  - Restoring a file from a backup keeps its old modification time, and ninja then keeps the mutant binary. Run
    `touch` on the file after restoring it.
  - Record each control's result (failed as expected, yes or no) in the final report.
- **Before each commit:** run `clang-format -i` on the changed C++ files, then build all three targets and run the
  tests the task names. Keep the tree warning-free.
- **Docs:** update the `docs/spec/` files the task names in the same commit. Doxygen runs with warnings as errors.
- **If a task can't be done as designed** (an assumption in the design is false), stop that task, leave it
  uncommitted, and describe what you found. Do not improvise a different design.

## Task 0 — commit the design and this plan

Commit `docs/superpowers/specs/2026-10-23-open-ui-issues-design.md` and this plan as one commit,
"docs: design and plan for the open UI issues".

## Task 1 — #1033: tab labels take the scheme colour under every style

- **Files:** `src/qt_quick/qml/MorphTabs.qml`, `tests/qt_quick/test_qt_quick_theme.cpp`.
- **Change:** in the `TabButton` delegate, replace `palette.buttonText: …` with
  ```qml
  contentItem: Text {
      text: tab.text
      font: tab.font
      color: tab.checked ? root.scheme.accent : root.scheme.muted
      horizontalAlignment: Text.AlignHCenter
      verticalAlignment: Text.AlignVCenter
      elide: Text.ElideRight
  }
  ```
- **Test:** a new `TEST_CASE` in `test_qt_quick_theme.cpp`, modelled on the existing one that uses `brandTheme()`
  and `labelColor()`. Mount a `ui::tabs` with two pages under `brandTheme()`, then:
  - check that `labelColor(item, "<first tab label>")` is the theme's accent colour;
  - check that the second tab's label is the muted colour. Find how the scheme's accent and muted colours are named
    in `brandTheme()`'s values or in `MorphScheme.qml`, and compare against those.
- **Control:** restore the old `palette.buttonText` line, and the test must fail.
- **Run:** `morph_qt_quick_tests`, plus `ctest -R qt_quick_qmllint`.

## Task 2 — #1034: log the UI bundle's serve and fetch path

- **Files:** `include/morph/ui/service.hpp`, `include/morph/ui/service_client.hpp`,
  `tests/test_ui_service_client.cpp`, `tests/test_ui_service.cpp`.
- **Logging calls:** fully qualified `::morph::log::logDebug(...)` / `::morph::log::logWarn(...)` with the
  `std::format` overload (see `include/morph/core/logger.hpp` and `include/morph/core/remote.hpp` near line 2082 for
  the style). Messages start with the tags from the design: `[ui-serve]`, `[ui-hello]`, `[ui-catalog]`,
  `[ui-fetch]`, `[ui-cache]`.
- **`UiRegistry`:** at the end of `addScreen`, `addEntry`, `addFile` and `build`, log success at debug and a
  refusal at warn, using `Refusal::message()`.
  - Restructure each public method so that every return path goes through that logging. One way: rename the body
    to a private `addScreenImpl`, and have the public method call it and log the result.
- **`UiService`:** in the code that answers `hello`, catalog and fetch requests, log each reply at debug: the
  request kind, the hash or application, and `ok` or the refusal reason. Refusals stay at debug too.
- **`UiClient`:**
  - The mismatch error (around `service_client.hpp:482-485`, `" does not match its hash"`) becomes
    `"<what> does not match its hash (expected <expected>, got <actual>)"`.
  - `fail()` logs the error at warn, once.
  - Opening and polling log at debug: the application, the known digest, whether the manifest changed, and each
    hash that has to be fetched.
- **`VerifiedCache::get`:** when a stored file is dropped for a mismatch, log at warn with the hash.
- **Tests:**
  - Update the assertions at `test_ui_service_client.cpp` ~461 and ~477 to the new error text.
  - In the mismatch fixture (~453-491), install `morph::log::ScopedLoggerOverride` with a capturing sink at
    `LogLevel::debug`, the pattern in `tests/test_logger.cpp`. Assert that one warn line contains both hashes.
  - In the cache-drop fixture (~495-507), assert the `[ui-cache]` line.
  - For a successful open, assert at least one `[ui-fetch]` debug line.
  - In `test_ui_service.cpp`, a refused fetch produces a debug line containing `[ui-fetch]` and the refusal reason.
- **Control:** comment out the log call in `fail()`, and the warn assertion must fail.
- **Run:** `morph_tests` with `-R 'service'` or the tags these files use. Then build `morph_client_tests` too,
  because it includes these headers.

## Task 3 — the date input keeps re-showing its value after repeated bad edits

- **Files:** `src/qt_quick/fields.cpp` (`DateTimeImpl`'s `edit` handler, and `show`), and
  `tests/qt_quick/test_qt_quick_widgets.cpp`.
- **First, reproduce.** Next to the existing test of one unparseable edit (around lines 369-373), add a test with
  two unparseable edits in a row, and confirm it **fails** on the current code before fixing anything. If it passes,
  the bug isn't real: drop this task, don't commit it, and say so in the report.
- **Fix:** keep `_shown` instead of clearing it. To re-show the same text, have `show()` take a `force` flag, or
  write the control's text directly, so it doesn't rely on the cleared state.
- **Control:** the new test failing on the old code is the control. Record it.
- **Commit body:** says it was found in review of the date input. No issue number, since none was filed.

## Task 4 — #1024: a calendar on every graphical date input

- **Files:**
  - `src/qt_quick/qml/MorphDateTimeInput.qml`
  - `src/qt_quick/fields.cpp`, which passes `offsetMinutes` as an initial property next to `withTime`
  - `include/morph/ui/theme.hpp`: `date.pick` in `kFrameworkIcons` (size 11), its doc comment, and `iconGlyph`'s
    table (`▦`, size 10)
  - `docs/spec/ui/theme.md`, the "Framework names" and terminal glyph bullets
  - `docs/spec/ui/backend_contract.md`, which states that a graphical backend offers a calendar beside the typed
    field and the terminal offers typed text
  - `tests/qt_quick/test_qt_quick_widgets.cpp`
- **QML, following the design's #1024 section exactly:**
  - A trailing `ToolButton` (`objectName: "datePickerButton"`):
    - `focusPolicy: Qt.NoFocus`, `visible: !root.readOnly`, `Accessible.name: "Pick a date"`;
    - its icon is `Theme.iconSource("date.pick")`, or the text `"▦"` when that is empty;
    - the field reserves its width in `rightPadding`.
  - A `Popup` (`objectName: "datePickerPopup"`) holding:
    - a header with previous and next month buttons and the month name from `Qt.locale()`;
    - a `DayOfWeekRow`;
    - a `MonthGrid` (`objectName: "datePickerGrid"`). Its delegate declares its required properties, and each day
      cell has `objectName: "day_" + model.year + "-" + model.month + "-" + model.day`, so tests can find it.
  - **Today** is computed from `Date.now()` plus `root.offsetMinutes`, in UTC fields. Don't use `MonthGrid`'s
    `today` role for the outline.
  - **The initial month** is parsed from the field's `yyyy-MM-dd` prefix, or today's month.
  - **A day click** builds the text from the integer `year`/`month`/`day` roles. Note that `MonthGrid`'s `month` is
    zero-based. A date-time input appends the current `HH:mm`, or `00:00`. The click then:
    1. sets `field.text`;
    2. sets `root.edited = false`;
    3. calls `root.fire("edit", text)`;
    4. closes the popup.
  - Escape or a click outside closes the popup; focus returns to the field.
- **Tests,** driving real mouse clicks with the click helpers other tests in that file use:
  1. An editable input's `datePickerButton` is visible; a read-only input's is not.
  2. Date mode with value 2026-03-15: click the button and the popup opens. Click `day_2026-2-20` (zero-based
     month), and `onChange` is called exactly once, with 2026-03-20 00:00 in the zone. Blur the field, and the count
     is still one.
  3. Date-time mode with offset +120 and value 2026-03-15 14:30 in that zone: pick the 20th, and the reported
     instant is 2026-03-20 14:30 in the zone, which is 12:30 UTC.
  4. With a value two months before today, the popup's grid shows that value's month and year.
- **Control:** make the day click do nothing, and test 2 must fail.
- **Run:** `morph_qt_quick_tests`, `ctest -R qt_quick_qmllint`, and `morph_tests -R theme`, since `kFrameworkIcons`
  changed. Then build every target in the tree:
  `flock $L cmake --build build/linux-everything -j10`.

## Task 5 — #1027: the shell shows a screen's head once, with its actions at the trailing edge

- **Files:**
  - `include/morph/ui/workspace.hpp`
  - `src/client/shell.cpp`, which constructs the `Workspace` with `HeadPlacement::External`
  - `docs/spec/ui/workspace.md`, the "The head row" section
  - `docs/spec/client/client.md`
  - `tests/test_ui_workspace.cpp`, `tests/client/test_sign_in.cpp`, and a Qt Quick test, either
    `tests/qt_quick/test_qt_quick_containers.cpp` or wherever `headRowForActive` is already exercised on Qt Quick
- **Change:**
  - Add `enum class HeadPlacement : std::uint8_t { InPage, External };`, documented, in `workspace.hpp`.
  - Add a `HeadPlacement headPlacement = HeadPlacement::InPage` constructor parameter on `Workspace`, after the
    existing ones, stored in a member. Don't put it in `WorkspaceOptions`.
  - `mountPage` skips the head mount when the placement is `External`.
  - In `buildHeadRow`, give the spacer and the row `common.layout.width = Sizing::stretch()`. `ui::spacer()` may
    not take a spec; build a `Spacer` struct if needed, as other code in `include/morph/ui` does.
- **Tests:**
  - `test_ui_workspace.cpp`: keep the existing `rows == 2` test. Add a test that with `External` the recording
    backend shows no `head` test id inside a page, while a separately mounted `headRowForActive()` shows the
    subtitle and the action.
  - `test_sign_in.cpp`: after sign-in to a screen whose document declares an action, exactly one
    `head_action_<id>` exists, and its ancestor is `shell_head`.
  - Qt Quick: mount `headRowForActive()`, or a row built the same way, into a 600-wide slot. The last action's right
    edge must be within one gap unit of the slot's right edge.
- **Controls:**
  - Mount the head even for `External`, and the sign-in test must fail.
  - Drop the spacer's stretch, and the Qt Quick test must fail.
- **Run:** `morph_tests -R workspace`, `morph_client_tests`, `morph_qt_quick_tests`.

## Finally

- Build all targets: `flock $L cmake --build build/linux-everything -j10`.
- Run `flock $L timeout 1800 ctest --test-dir build/linux-everything -L 'qt_quick|client' -j6`, and
  `morph_tests` in full.
- Run the Doxygen check from AGENTS.md's CI notes in a separate build dir, `build/doc`:
  ```
  flock $L cmake -S . -B build/doc -G Ninja -DMORPH_BUILD_DOCUMENTATION=ON -DMORPH_BUILD_TESTS=OFF -DMORPH_BUILD_EXAMPLES=OFF
  flock $L cmake --build build/doc --target doc
  ```
- Write a report to
  `/tmp/claude-1000/-home-yaraslau-repo-morph/8742d70b-a059-4ed6-9415-5f45a5067fd9/scratchpad/ui-open-report.md`:
  - each task's commit hash, or why it was skipped;
  - the tests added;
  - each control and whether it failed as expected;
  - the full-suite results, with real output for any failure;
  - anything in the design that turned out to be wrong.

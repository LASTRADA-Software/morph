# Implementation plan: date strings and input commits

The design is `docs/superpowers/specs/2026-10-24-date-strings-and-commit-design.md`. Read it in full before starting.
It gives the reasoning, and this plan gives the order and the mechanics. Where the two disagree, the design wins:
stop and report the disagreement instead of picking a side.

## Ground rules (apply to every task)

- **Worktree:** `/home/yaraslau/repo/morph-ui-1039`, branch `fix/ui-996-1039`. Work only here. Do not push, do not
  open a PR, and do not touch any other checkout.
- **Read first:** `AGENTS.md`, and the spec files each task names under `docs/spec/`. The comment rules in AGENTS.md
  apply:
  - no history, no issue numbers and no commit hashes in comments or specs;
  - every public symbol gets complete Doxygen `@brief`/`@param`/`@tparam`/`@return`;
  - match the density and idiom of the code around you.
- **Builds:** every build, test or clang-format run goes through the shared lock, with at most 10 jobs:
  ```
  L=/tmp/claude-1000/-home-yaraslau-repo-morph/8742d70b-a059-4ed6-9415-5f45a5067fd9/scratchpad/morph-build.lock
  flock $L cmake --build build/linux-everything -j10 --target <targets>
  flock $L timeout 600 ctest --test-dir build/linux-everything -R '<regex>' --output-on-failure
  flock $L build/linux-everything/tests/morph_tests '<catch2 filter>'
  ```
  - The build tree `build/linux-everything` is already configured. Do not reconfigure it, and do not change its
    compiler launcher.
  - Never `pkill -f` anything. Do not run a build in the background.
  - Run Qt Quick tests with `QT_QPA_PLATFORM=offscreen`; ctest already sets it for the `qt_quick` label.
- **Targets:** `morph_tests` (core and UI unit tests), `morph_qt_quick_tests`, `morph_tui_tests`,
  `morph_client_tests`. The QML lint test is `ctest -R qt_quick_qmllint`, and it fails on any qmllint warning. If a
  target name is wrong, find the right one with `cmake --build build/linux-everything --target help | grep tests`.
- **One commit per task,** in the order below. The subject is an imperative sentence about the behaviour (see
  `git log --oneline -20` for the house style), and the body explains why. Add `Closes #1039` to the task 1 body and
  `Closes #1041` to the task 2 body. End every message with:
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
- **clang-tidy rules CI enforces** (CI runs clang-tidy 22 on changed lines with warnings as errors). Write code that
  passes them the first time:
  - identifiers at least 3 characters (no `id`, `at`, `it`, `ok`);
  - a pointer whose pointee is not modified is `T const*`;
  - use `auto` for a cast or a `findChild<T>` result;
  - `contains()`, not `find() != npos`;
  - `std::ranges::any_of`/`all_of`, not a loop that returns a bool;
  - a variable that is never modified is `const`. A test helper whose member a callback writes holds that member
    `mutable`, as `CapturedLogs` in `tests/test_ui_service_client.cpp` does;
  - no `operator[]` on containers (use `.at()`), and no C-style casts.
- **Before each commit:** run `clang-format -i` on every changed C++ file, then build all four targets and run the
  tests the task names plus the whole `morph_tests`. Keep the tree warning-free.
- **Docs:** update the `docs/spec/` files the task names in the same commit. Doxygen runs with warnings as errors.
- **If a task can't be done as designed** (an assumption in the design is false), stop that task, leave it
  uncommitted, and describe what you found. Do not improvise a different design.

## Task 0 — commit the design and this plan

Commit the design and this plan as one commit, "docs: design and plan for date strings and input commits".

## Task 1 — a date input shows and writes the value's own type (design section 1)

**Files:**
- `include/morph/ui/document.hpp`
- `include/morph/ui/interpreter.hpp`
- `docs/spec/ui/document.md`, `docs/spec/ui/interpreter.md`
- tests in `tests/test_ui_document.cpp` (load) and in the interpreter test file that already holds `dateTimeInput`
  tests (`grep -ln dateTimeInput tests/test_ui_*.cpp`). Pick the one whose existing cases are closest.

**Steps:**
1. **The load-time type.** In the document checker, `checkElement` calls `checkExpr` for every prop and drops the
   result. For a node of kind `dateTimeInput` with a `value` prop, keep the result:
   - `Timestamp` or `String` (either optional or not): record it in a new side table on the loaded `Document`,
     keyed by `DocNode const*`. Name it, for example, `dateValueKinds`, mapping to `TypeKind`. Give it full Doxygen,
     which says it records the static type of each date input's `value` that the loader could decide.
   - `Any` or `Key`: record nothing.
   - Any other kind: `refuse(<value expr path>, "`dateTimeInput` shows a timestamp or an ISO-8601 date or date-time
     string; `value` is <type name>")`. Use `Type::name()` for the name.

   Read how `Document` is built and handed to the interpreter (`_doc`) before choosing where the table lives. It must
   survive into the interpreter's `ScreenCore`. If nodes are copied between the checker and the interpreter, so that
   a `DocNode const*` would not match, stop and report.
2. **Reading.** Add a helper beside `sameShown` in `interpreter.hpp`. For example:
   `[[nodiscard]] inline std::optional<time::DateTime> dateInstantOf(Value const& value, std::int32_t offsetMinutes, bool& readable)`.
   Choose a cleaner signature if you like; `std::expected` or a small struct is fine. It implements the "What it
   shows" table:
   - a timestamp;
   - `yyyy-MM-dd` as midnight of that date in the display zone, which is the UTC instant minus `offsetMinutes`;
   - `DateTime::fromIso8601`;
   - null and `""` are empty and readable;
   - anything else is empty and **unreadable**.

   For the date-only form, reuse an existing ISO-date check if one is reachable from `ui` without a dependency
   cycle: the forms engine has `isIsoDate` and `parseWallClock`. Look before writing a new one. Write a new one only
   if `ui` can't include the forms one, and then validate the calendar date as `fromIso8601` does.
3. **The handler** (`kind == "dateTimeInput"` in `interpreter.hpp`):
   - `spec.value` reads the draft through the helper, using the same `format.utcOffsetMinutes` that is passed as
     `spec.offsetMinutes`. When the value is unreadable, call `report(pointerAppend(node.path, "value"), <fixed reason>)`.
     The fixed reason is the design's sentence and contains no value. Check how the `roleProp` path calls `report`
     from inside a slot, and do the same.
   - `spec.onChange` decides the held type as the design orders it: the recorded static kind, else the bound value's
     type now (`element->bound` holds a string), else timestamp. It then writes:
     - for a string field: `yyyy-MM-dd` (the instant plus the offset, as a date) in `DateMode::Date`,
       `toIso8601()` in `DateMode::DateTime`, and `""` on clearing;
     - for a timestamp field: as today.

     The `onChange` event value is the value written.
4. **`sameShown`:** add a `dateTimeInput` case. Read both sides with the helper and compare the instants. Both empty
   or unreadable counts as the same. `sameShown` is called with `session.formatting()`, so take the offset from
   there.
5. **Spec:**
   - `document.md` `dateTimeInput` row: the accepted value types, the load refusal, and the type written back.
   - `interpreter.md`: the type rule and the `sameShown` rule, in the input paragraph near the `onCommit` text.
6. **Tests:** the design's "Load, refused type" and the five "Interpreter, …" bullets about strings, timestamps,
   following the value, and the diagnostic. Each one gets its control. The gallery-shaped case (a null
   `state<std::string>` written through `set(when, event())`) must be in there. Build documents the way neighbouring
   tests do, with the `ui::` author helpers or JSON text.

**Run:** `morph_tests` in full, `morph_qt_quick_tests` and `morph_tui_tests`, since both frontends mount
`dateTimeInput` through the interpreter.

## Task 2 — `onCommit` runs on `dateTimeInput` and `slider` (design section 2)

**Files:**
- `include/morph/ui/view.hpp` (`Slider`), `include/morph/ui/backend.hpp` (`SliderWidget`), `include/morph/ui/mount.hpp`
- `include/morph/ui/interpreter.hpp`
- `include/morph/ui/testing/recording_backend.hpp` (`FakeSlider`, `slide`, new `drag`),
  `include/morph/ui/testing/backend_conformance.hpp` (`settersCallNoHandler`)
- `src/qt_quick/kinds.hpp`, `src/qt_quick/fields.cpp`, `src/qt_quick/qml/MorphSlider.qml`
- `src/tui/leaf_widgets.hpp`, `src/tui/leaf_widgets.cpp`
- `docs/spec/ui/view_tree.md`, `docs/spec/ui/interpreter.md`, and `docs/spec/ui/backend_contract.md` if it lists
  `SliderWidget`'s methods (check)
- tests: the interpreter test file from task 1, `tests/qt_quick/test_qt_quick_widgets.cpp`,
  `tests/tui/test_tui_dialog.cpp` (which holds the `tui slider:` cases)

**Steps:**
1. **`dateTimeInput` handler:** follow `textInput`'s order (read its `onCommit` lambda):
   - set `element->touched` to true;
   - write the draft and run `onChange`, as task 1 left them;
   - then run `node.event("onCommit")` with the same value.
2. **`ui::Slider`:** add `std::function<void(std::int64_t)> onCommit;` with Doxygen. **`SliderWidget`:** add
   `virtual void setOnCommit(std::function<void(std::int64_t)> onCommit) = 0;`, with Doxygen saying a commit is a
   finished move. **`mount.hpp`:** wire it the way `TextInput`'s `onCommit` is wired (`event(spec.onCommit)`), not
   the way `onChange` is.
3. **Slider handler** in the interpreter: `spec.onCommit` sets `touched` and runs `node.event("onCommit")` with
   `Value{value}`.
4. **Recording backend:**
   - `FakeSlider` stores the commit callback in its record. Mirror how `callbacks.slide` is stored.
   - `slide(id, value)` invokes the change and then the commit.
   - A new `drag(id, value)` invokes only the change, with Doxygen.
   - Check that `tests/ui_echoing_backend.hpp` reuses `FakeSlider` and needs nothing.
5. **Conformance:** add `.onCommit = note("Slider onCommit")` to the slider in `settersCallNoHandler`.
6. **Qt Quick:**
   - `SliderImpl::setOnCommit` relays `"commit"`, as `setOnChange` relays `"change"`.
   - In `MorphSlider.qml`, add `property bool movedWhilePressed: false`.
   - In `onMoved`, after the read-only refusal: fire `"change"` as now. Then, if `slider.pressed`, set
     `movedWhilePressed`; otherwise fire `"commit"`, since a wheel move happens while not pressed.
   - Add `onPressedChanged: if (!pressed && root.movedWhilePressed) { root.movedWhilePressed = false; root.fire("commit", root.shownValue) }`.
   - A read-only slider never commits.
   - Keep qmllint clean.
7. **Terminal** (`SliderImpl`):
   - add `_onCommit` and `setOnCommit`, plus a `_moved` flag;
   - split `moveTo` so a key step calls change then commit, and a pointer move calls change only and sets `_moved`;
   - commit on press end when `_moved`. Find the press-end hook in `src/tui/widget.hpp` (`pressEnded`) and how other
     widgets override it.
   - **A handler may destroy the slider.** Copy `_onChange` and `_onCommit` to locals before calling either, and
     touch no member after the first call. The test `tui slider: onChange may destroy its own slider…` must keep
     passing.
8. **Spec:**
   - `view_tree.md` `Slider` row: `onCommit` (a finished move: release after a drag, a key step, a wheel step).
   - `DateTimeInput` row: its change is its commit.
   - `interpreter.md`: `onCommit` on both inputs, and that it sets `touched`.
   - The `SliderWidget` method list, if `backend_contract.md` has one.
9. **Tests:** the design's `onCommit` bullets for the interpreter, conformance, Qt Quick and terminal, each with its
   control. For Qt Quick:
   - a mouse drag across two steps, using the harness's press, move and release helpers in `qt_quick_harness.hpp`.
     If there is no move helper, use `QTest::mousePress`/`mouseMove`/`mouseRelease` on the window;
   - a key click (press and release), not a bare press.

**Run:** all four targets, `ctest -R qt_quick_qmllint`, and the whole `morph_tests`.

## Final report

For each task, report:
- the commit hash;
- the files changed;
- each test added, and its control result;
- any place where you stopped or departed from the design, and why.

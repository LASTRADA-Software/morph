# Qt Quick layout fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Spacing is a step on a theme scale on both frontends, content-sized children fill their container's cross
axis on Qt Quick as on the terminal, and a content-sized splitter is as large as its panes.

**Architecture:** The mount clamps every `gap`/`padding` to a step 0..5 (`ui::spacingStep`) before it reaches a backend.
Qt Quick reads the step's pixels from `Theme.space` (fed by the new `space.0`–`space.5` dimension tokens); the terminal
maps it to cells per axis (`layout::stepCells`). Qt Quick containers tell each adopted child which way to fill
(`fillsWidth`/`fillsHeight` on `MorphWidget`).

**Tech Stack:** C++23, Qt 6 Quick/QML, Catch2, the morph TUI solver.

Spec: `docs/superpowers/specs/2026-10-17-qt-quick-layout-fixes-design.md`.

## Global Constraints

- Steps: 0..5. Qt Quick px defaults 0, 4, 8, 12, 16, 24. Terminal vertical rows 0, 0, 0, 1, 1, 2; horizontal columns
  0, 1, 1, 2, 2, 3.
- Token names `space.0` … `space.5`, DTCG `dimension`; `rem` = 16 px.
- Every build/test command runs under `flock <scratchpad>/morph-build.lock`, builds `-j10`.
- Comments state what the code does now and why; no history, no issue numbers (AGENTS.md). Doxygen complete on every
  public symbol.
- Every new test is run on the unfixed code first and must fail there.

---

### Task 1: The step clamp in the mount, and the contract

**Files:**
- Modify: `include/morph/ui/view.hpp` (add `kMaxSpacingStep`, `spacingStep`; Doxygen of `gap`/`padding` members)
- Modify: `include/morph/ui/mount.hpp:493-497,644,664,924` (pass `spacingStep(...)`)
- Modify: `include/morph/ui/backend.hpp:267-301` (Doxygen: "a spacing step, 0..5")
- Modify: `include/morph/ui/testing/backend_conformance.hpp` (clamp case)
- Test: `tests/test_ui_mount.cpp`

**Interfaces:**
- Produces: `inline constexpr int morph::ui::kMaxSpacingStep = 5;` and
  `[[nodiscard]] constexpr int morph::ui::spacingStep(int value) noexcept` (clamps to `[0, kMaxSpacingStep]`).

- [ ] Step 1: failing test in `tests/test_ui_mount.cpp`: mount `column({...}, 99)`, `row({...}, -3)`, a grid with gap 7
  and a panel with padding 6 on `RecordingBackend`; check the recorded `gap`/`padding` are `5`, `0`, `5`, `5`.
- [ ] Step 2: build `morph_tests`, run `[mount]` — FAIL (records 99).
- [ ] Step 3: add to `view.hpp`:

```cpp
/// @brief The largest spacing step: a `gap` or `padding` names a step from 0 (none) to this.
inline constexpr int kMaxSpacingStep = 5;

/// @brief A document's `gap` or `padding` as the spacing step a backend receives.
/// @param value The value as written.
/// @return @p value clamped to `[0, kMaxSpacingStep]`.
[[nodiscard]] constexpr int spacingStep(int value) noexcept { return std::clamp(value, 0, kMaxSpacingStep); }
```

  and wrap each `setGap(x)` / `setPadding(x)` call in `mount.hpp` as `setGap(spacingStep(x))`.
- [ ] Step 4: conformance case "a gap or padding outside 0..5 arrives clamped" in `backend_conformance.hpp`, mounting
  through `Mount` and reading back each backend's received step through the suite's existing probe hooks (if the
  suite has no read-back for gap, the case asserts on `RecordingBackend` only and the per-backend tests in Tasks 3–4
  check the pixels/cells).
- [ ] Step 5: run `[mount]` and the conformance suite — PASS. Commit `ui: gap and padding are spacing steps, clamped by the mount`.

### Task 2: `space.*` dimension tokens

**Files:**
- Modify: `include/morph/ui/theme.hpp:805-858` (`ThemeTokenKind::Dimension`, `ThemeTokenSpec::dimension`,
  `ThemeValues::dimensions`, six `space.<n>` entries in `kThemeTokens`, `themeValues`)
- Modify: `src/qt_quick/theme.cpp` (`applyTheme` sets `Theme.space`), `src/qt_quick/qml/Theme.qml`
- Modify: `docs/spec/ui/theme.md` (closed set, On Qt Quick table, drop scale tokens from Not yet)
- Test: `tests/test_ui_theme.cpp:117-118`, `tests/qt_quick/test_qt_quick_theme.cpp`

**Interfaces:**
- Produces: `ThemeValues::dimensions` (`std::map<std::string, double, std::less<>>`, px); QML
  `Theme.space: list<real>` of six entries.

- [ ] Step 1: failing tests: `themeValues` of a theme with `space.3 = {value: 20, unit: px}` gives `dimensions["space.3"] == 20`;
  `{value: 1, unit: rem}` gives 16; a theme without it gives the default 12; `colors.size()+numbers.size()+dimensions.size() == kThemeTokens.size()`.
  Qt: `applyTheme` with `space.3 = 20` makes `Theme.space[3] == 20` and reports nothing unset.
- [ ] Step 2: run — FAIL (no `dimensions`).
- [ ] Step 3: implement. `Theme.qml`: `property list<real> space: [0, 4, 8, 12, 16, 24]` and
  `function spaceOf(step: int): real { return space[Math.max(0, Math.min(5, step))] }`. `applyTheme` builds the list
  from `values.dimensions` in step order.
- [ ] Step 4: run — PASS. Commit `ui, qt_quick: space.0-space.5 dimension tokens`.

### Task 3: Qt Quick spacing from the scale

**Files:**
- Modify: `src/qt_quick/qml/MorphStack.qml`, `MorphGrid.qml`, `MorphPanel.qml` (and the forEach's QML if it has its own
  spacing — check `src/qt_quick/containers.cpp` for the kind ForEach uses)
- Test: `tests/qt_quick/test_qt_quick_containers.cpp`

- [ ] Step 1: failing test: a vertical stack of a text and two text inputs with `setGap(2)`; the second input's
  scene y minus the first's (y + height) is 8. A grid with `setGap(1)` has 4 px between columns and rows. A panel with
  `setPadding(3)` insets its content 12 px (+1 frame).
- [ ] Step 2: run — FAIL (34 / 17·n px).
- [ ] Step 3: `rowSpacing: Theme.spaceOf(root.gap)`, `columnSpacing: Theme.spaceOf(root.gap)`; panel margins
  `Theme.spaceOf(root.padding) + 1`, implicit sizes `+ 2 * Theme.spaceOf(root.padding) + 2`. Update the QML header
  comments ("`gap` units apart" → "a spacing step apart").
- [ ] Step 4: run — PASS. Commit `qt_quick: spacing is the theme's step`.

### Task 4: Terminal spacing per axis

**Files:**
- Modify: `src/tui/layout.hpp/.cpp` (`stepCells`, `planGrid(columns, cells, width, rowGap)`, `GridPlan::{columnGap,rowGap}`,
  `pad(area, vertical, horizontal)`), `src/tui/container_widgets.cpp/.hpp` (Stack/Grid/Panel convert at the setter)
- Modify: `docs/spec/tui/frontend.md` (step tables; per-axis gaps; the cross-axis rule)
- Test: `tests/tui/test_tui_layout.cpp`, `tests/tui/test_tui_containers.cpp`

**Interfaces:**
- Produces: `[[nodiscard]] int layout::stepCells(int step, ui::Axis axis) noexcept` — vertical `{0,0,0,1,1,2}`,
  horizontal `{0,1,1,2,2,3}`, step clamped to 0..5.

- [ ] Step 1: failing tests: `stepCells` table both axes (and -1→0, 9→5's value); `planGrid(2, cells, {.length=21,.gap=2}, 1)`
  puts row 2 at rowY = height(row 1)+1 and column 2 at x = width+2; `pad(rect, 1, 2)` insets 1 row / 2 columns;
  a vertical stack with `setGap(2)` places children with 0 blank rows, `setGap(3)` with 1.
- [ ] Step 2: run — FAIL (no symbol).
- [ ] Step 3: implement with `constexpr std::array<int, 6> kVerticalCells{0,0,0,1,1,2}, kHorizontalCells{0,1,1,2,2,3}`;
  `StackImpl::setGap(step)` stores `stepCells(step, _axis)`; `GridImpl::setGap(step)` stores both; `PanelImpl::setPadding(step)`
  stores both; update `naturalSize`/`childAreas` and `slotArea` for `columnGap`/`rowGap`. Update the existing
  solver tests that pass `.gap` to `planGrid` and `pad`.
- [ ] Step 4: run `morph_tui_tests` — PASS. Commit `tui: spacing steps map to cells per axis`.

### Task 5: Content children fill the cross axis on Qt Quick

**Files:**
- Modify: `src/qt_quick/qml/MorphWidget.qml` (`fillsWidth`, `fillsHeight`, `Layout.fill*` bindings)
- Modify: `src/qt_quick/widget.hpp/.cpp` (`ContainerCore::crossFill()` virtual, written in `adopt`), `kinds.hpp`/`containers.cpp`
  (Stack by axis, Splitter none), `context.cpp` (roots fill width)
- Modify: `docs/spec/ui/backend_contract.md`, `docs/spec/qt_quick/backend.md`
- Test: `tests/qt_quick/test_qt_quick_containers.cpp`

**Interfaces:**
- Produces: `struct CrossFill { bool width; bool height; };` and `virtual CrossFill ContainerCore::crossFill() const`
  (default `{true, false}`); `StackImpl` returns `{axis==Vertical, axis==Horizontal}`; `SplitterImpl` `{false,false}`.

- [ ] Step 1: failing tests: a text input sized content inside a vertical stack inside a stack sized `fixed(40)` wide
  is `40 * unitWidth` wide; a button in a row whose other child is a 3-line text is as tall as the row; a button with
  `fixed(10)` width in a column stays 10 units; two buttons in a row keep their natural widths (row packs at the start).
- [ ] Step 2: run — FAIL (120 px).
- [ ] Step 3: `MorphWidget.qml`: `property bool fillsWidth: false; property bool fillsHeight: false;`
  `Layout.fillWidth: widthKind === 2 || (widthKind === 0 && fillsWidth)`, same for height. `ContainerCore::adopt`
  writes both from `crossFill()` before `adopted(child)`. If the row test shows extra width spread between buttons,
  give `MorphStack`'s layout `Layout.alignment: Qt.AlignLeft | Qt.AlignTop` on children (written with the fill
  properties) and re-run.
- [ ] Step 4: run the whole `morph_qt_quick_tests` — PASS (fix any existing case that measured a content width).
  Commit `qt_quick: content children fill their container's cross axis`.

### Task 6: The splitter's implicit size

**Files:**
- Modify: `src/qt_quick/qml/MorphSplitter.qml`
- Test: `tests/qt_quick/test_qt_quick_containers.cpp`

- [ ] Step 1: failing test: a horizontal splitter (content sized) with texts "Left" and "Right" in a column: width ≥ both
  texts' implicit widths summed, right pane x ≥ left pane x + left width; with `setSizes({10, 30})` its implicit width
  is `40 * unitWidth` + handle width.
- [ ] Step 2: run — FAIL (0×0).
- [ ] Step 3: `implicitWidth`/`implicitHeight` computed by a function over `split.contentChildren` (sum along the axis of
  `sizes[i] * unit` when `sizes` matches the pane count else the pane's implicit extent, plus `(n-1)` handle widths;
  across, the max implicit extent), re-evaluated on pane implicit-size changes.
- [ ] Step 4: run — PASS. Commit `qt_quick: a content-sized splitter is as large as its panes`.

### Task 7: Rewrite every document to steps, and the specs

**Files:**
- Modify: `include/morph/forms/engine/form_view.hpp`, `wizard_view.hpp`, `collection_view.hpp`,
  `include/morph/ui/interpreter.hpp:6080`, `examples/{bank,bookmarks,lims,forms,gallery}/**`, `tests/ui/corpus/documents.json`,
  any test document whose gap is outside 0..5
- Modify: `docs/spec/ui/view_tree.md`, `document.md`, `interpreter.md`, `author.hpp` Doxygen
- [ ] Step 1: rewrite with the spec's table: pixel-ish 4, 8, 12, 16 → 1, 2, 3, 4; forms engine 1 → 4; 2 (lines) → 5;
  check each remaining value by hand (`grep -rnE '\.(gap|padding) *= *[0-9]+'`).
- [ ] Step 2: build everything (`linux-everything`), run `morph_tests`, `morph_tui_tests`, `morph_qt_quick_tests`,
  `ctest -L ladder -LE stress`. Fix golden files that record gaps.
- [ ] Step 3: screenshot bank's sign-in and the gallery (`grim`, as in the design's measurement) and compare against
  the measured symptoms: fields 8 px apart and full width, splitter panes side by side.
- [ ] Step 4: commit `examples, forms: documents use spacing steps`.

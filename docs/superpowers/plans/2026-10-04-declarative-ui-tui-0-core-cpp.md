# Declarative UI, Part 0 — core-cpp mouse tracking and pointer capture, then morph's pin Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for
> tracking.

**Goal:** Release core-cpp 0.7.0 with standard mouse tracking, pointer capture and a public `Screen::componentAt`,
then raise morph's core-cpp pin from 0.5 to 0.7 in one commit of the declarative-UI branch.

**Architecture:** In core-cpp, a `MouseTracking` enum and one pure function,
`protocols::appendMouseTrackingChange(out, from, to)`, compose every mouse-mode transition; `TerminalInput`
writes the change from `Off` when it enables its protocols, the change to `Off` when it disables them, and only the
transition when the mode changes while they are enabled. `Screen` hit-tests overlays before the tree, makes the
component that handled a press the capture target, and delivers moves and the release to it; a component's own
teardown clears the capture. morph then changes one CMake bound, one tag and the two README lines that state them.

**Tech Stack:** C++23; core-cpp `core::tui` / `core::tui_output` (Catch2 v3 through `core::testing_main`, CMake
presets, pinned clang-format/clang-tidy 22.1.8); morph CMake + CPM; `gh` for the core-cpp release steps.

**Spec:** `docs/superpowers/specs/2026-10-04-core-cpp-mouse-design.md` (spec 0, all sections);
`docs/superpowers/specs/2026-10-04-declarative-ui-tui-design.md` §2 "core-cpp pin".

This is **Part 0 of 11** of the declarative-UI program. Read the master plan
`docs/superpowers/plans/2026-10-04-declarative-ui-tui.md` first. This part spans two repositories: Tasks 1–9 run in
**core-cpp** (`/Users/christianparpart/projects/core-cpp`, GitHub `contour-terminal/core-cpp`) and follow
core-cpp's own conventions — conventional-commit subjects, `git commit -s`, no `wip(...)` commits, a pull request
merged with a merge commit; Tasks 10–11 run in **morph** on `feature/declarative-ui` and follow the master plan:
`wip(corecpp)` commits squashed into `core: build against core-cpp 0.7`.

## Global Constraints

- morph side: Part 1's Global Constraints apply unchanged (C++23, SPDX first line, `#pragma once`, naming, present
  tense comments without history or issue numbers, Doxygen `WARN_AS_ERROR`, `-Weverything -Werror`, sign-off).
- core-cpp side follows core-cpp's `AGENT.md` and `.agent/rules/` instead, where they differ from morph's:
  - constants are `CamelCase` with **no `k` prefix** (`EnableDragTracking`, not `kEnableDragTracking`);
    functions, variables, parameters and local `const` values `camelBack`; private members `_camelBack`;
    a `bool` member reads as a predicate (`_isRedirected`).
  - **no C-style `for (;;)`/`for (init; cond; step)`** (the hygiene scan refuses the three-clause form): range-for,
    `std::views`, algorithms or `while`.
  - **no `NOLINT`, no diagnostic pragma**; every enum has an explicit underlying type and its zero enumerator is
    the off/default case.
  - `.hpp` + `#pragma once`, `// SPDX-License-Identifier: Apache-2.0` on line 1, includes `<core/...>` from
    `src/`, `QualifierAlignment: Right` (`auto const`, `Component const&`), braces on their own line.
  - Doxygen `///` with `@brief`/`@param`/`@return` on every public declaration, `[[nodiscard]]` on every query.
  - every new file under `src/core/` gets a row in `.agent/reference/provenance.md`
    (`core-cpp.cmake-hygiene` refuses a file without one).
  - format with the pinned tool only: `python scripts/clang-format.py <paths>`; never `clang-format -i` from PATH.
  - a public header change gets a `CHANGELOG.md` `[Unreleased]` entry in the same commit.
  - every commit builds and passes on its own and ends with `Signed-off-by:` (`git commit -s`).
- Pushing core-cpp's branch, opening and merging its pull request, cutting and publishing `v0.7.0`, and filing the
  Shift+Tab issue are each **confirmed with the user immediately before they are taken** (master plan,
  "Outward-facing steps are confirmed first").
- morph's pin task (Task 10) cannot run before `v0.7.0` exists on GitHub: CPM fetches the tag. Before that, morph
  is verified against the feature branch with `-DCPM_core-cpp_SOURCE=…` in a throwaway build directory (Task 8);
  that override is never committed.

## Review Focus

1. **A component destroyed while handling its own press** must not be left as the capture target, and the next
   move must not reach it (Task 6 test "Screen.pointerCapture_isNotTakenByAComponentItsPressDestroyed" — ASan is
   its observer in Task 7).
2. **A release the terminal never delivered** (the window lost focus mid-drag) must not hold the pointer for ever:
   the next press re-captures (Task 6 test "Screen.pointerCapture_movesToTheComponentOfTheNextPress").
3. **An overlay and its `Screen` destroyed in either order**: the new teardown hook calls into the screen, so
   the screen detaches every overlay it still shows before it dies, and an overlay destroyed while shown leaves
   the list (Task 6 tests "Screen.destructor_detachesOverlaysStillShown" and
   "Screen.overlayDestroyedWhileShown_leavesTheOverlayList").
4. **Scroll during a drag** goes to whatever is under the pointer and leaves the capture in place (Task 6 test
   "Screen.pointerCapture_neverTakesScrollEvents").
5. **The default `Off` writes no mouse mode at all** — no 1000, 1002, 1003 or 1006 — so an existing program's
   terminal keeps its text selection (Task 4 test "TerminalInput.posix.mouse_tracking_off_writes_no_mouse_mode").

---

## File Structure

| File | Responsibility |
|---|---|
| core-cpp `src/core/tui/MouseTracking.hpp` (new) | `core::tui::MouseTracking { Off, Buttons, Drag, AnyMotion }`, in `core::tui_output` |
| core-cpp `src/core/tui/TerminalProtocols.hpp` | 1000/1002 constants, `mouseTrackingSet()`, `mouseTrackingReset()`, `appendMouseTrackingChange()` |
| core-cpp `src/core/tui/TerminalProtocols_test.cpp` | Byte-exact table of every transition |
| core-cpp `src/core/tui/TerminalInput.hpp`, `TerminalInput.cpp` | `setMouseTracking()`, `mouseTracking()`, the hover raise, `writeMouseTrackingChange()` |
| core-cpp `src/core/tui/posix/TerminalInput.cpp`, `windows/TerminalInput.cpp` | `enableProtocols()`/`disableProtocols()` write the mode |
| core-cpp `src/core/tui/Terminal.hpp`, `Terminal.cpp` | `Terminal::setMouseTracking()` forwards |
| core-cpp `src/core/tui/TerminalInput_test.cpp` (new) | Requested vs raised mode; `Terminal` forwarding |
| core-cpp `src/core/tui/posix/TerminalInput_test.cpp` (new) | The bytes the POSIX `TerminalInput` writes, read from a pipe |
| core-cpp `src/core/tui/VtParser_test.cpp` | SGR drag and any-motion reports |
| core-cpp `src/core/tui/Screen.hpp`, `Screen.cpp` | Public `componentAt`, overlay-first `hitTest`, pointer capture, `~Screen` detaching overlays |
| core-cpp `src/core/tui/Component.cpp` | Teardown tells the screen (`componentDetached`) |
| core-cpp `src/core/tui/Screen_test.cpp` | Hit-testing and capture cases |
| core-cpp `src/core/tui/CMakeLists.txt` | Header and test registration |
| core-cpp `.agent/reference/provenance.md` | Rows for the three new files |
| core-cpp `CHANGELOG.md`, `docs/modules/tui.md` | `[Unreleased]` entries; the module page |
| morph `CMakeLists.txt` | `MORPH_CORE_CPP_VERSION 0.7`, `GIT_TAG v0.7.0`, `VERSION 0.7.0` |
| morph `README.md` | The two lines that state the core-cpp version |
| morph `CHANGELOG.md` | `[Unreleased]` → Changed: "morph builds against core-cpp 0.7" |

`cmake/morphConfig.cmake.in` needs no edit: it says `find_dependency(core-cpp @MORPH_CORE_CPP_VERSION@ CONFIG)`.

## Build and test commands

core-cpp (every command runs in `/Users/christianparpart/projects/core-cpp`; trees live in `out/build/<preset>`):

```bash
cmake --preset clang-debug                                            # once
cmake --build --preset clang-debug --target core-cpp-tui-test core-cpp-tui_output-test
./out/build/clang-debug/src/core/tui/core-cpp-tui-test "<test-name pattern>"
./out/build/clang-debug/src/core/tui/core-cpp-tui_output-test "<test-name pattern>"
ctest --preset clang-debug                                            # everything
```

On macOS `clang-debug` uses Homebrew LLVM's `clang++`; if it does not configure, use `appleclang-debug` with the
same commands and `out/build/appleclang-debug/…`, and say which one ran.

morph (every command runs in `/Users/christianparpart/projects/morph`):

```bash
cmake -S . -B build/all -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMORPH_BUILD_TUI=ON -DMORPH_BUILD_QT=ON \
      -DMORPH_BUILD_QT_QUICK=ON -DMORPH_BUILD_LADDER=ON -DMORPH_LADDER_RUNGS=all -DMORPH_BUILD_BANK_EXAMPLE=ON \
      -DMORPH_BUILD_NET=ON -DMORPH_BUILD_FORMS_QML=ON
cmake --build build/all
ctest --test-dir build/all --output-on-failure
```

`MORPH_BUILD_TUI` and `MORPH_BUILD_QT_QUICK` do not exist yet (Parts 3 and 4 add them); CMake lists them under
"Manually-specified variables were not used", which is expected here. Configuring prints
`morph: warnings: ... strict=ON`; if it does not, stop and say so (CONTRIBUTING, "Warnings are errors").

---

### Task 1: Put core-cpp on a fresh `feature/mouse-tracking` branch

**Files:** none changed.

**Interfaces:** none.

- [ ] **Step 1: Leave the abandoned branch cleanly**

```bash
cd /Users/christianparpart/projects/core-cpp
git status --porcelain            # expected: empty (feature/reactive-ui is clean and pushed)
git switch master
git pull --ff-only origin master
git log --oneline -1              # expected: 04dc0eb docs(changelog): open [Unreleased] after 0.6.0, or newer
git switch -c feature/mouse-tracking
```

If `git status --porcelain` is not empty, stop and ask the user — `feature/reactive-ui` is theirs to keep or drop.
If `master` moved past `04dc0eb`, run `git diff 04dc0eb..master --stat -- src/core/tui CHANGELOG.md` and re-read
every file this plan modifies before applying its code: the plan was written against `04dc0eb` (`v0.6.0` plus the
reopened `[Unreleased]`).

- [ ] **Step 2: Install the pinned tools and record a green baseline**

```bash
cd /Users/christianparpart/projects/core-cpp
python3 scripts/tool-versions.py --install
cmake --preset clang-debug && cmake --build --preset clang-debug && ctest --preset clang-debug
```

Expected: `100% tests passed` (or every failure named and present on `master` already — then stop and tell the
user; this part does not start on a red tree). Keep the summary line for the PR body.

No commit.

---

### Task 2: `VtParser` decodes drag and any-motion reports — pinned by tests

The parser already turns `CSI < b ; x ; y M` with bit 32 into `MouseEvent{Move, button = b & 3}`
(`src/core/tui/VtParser.cpp`, `dispatchCsi`, the `else if (rawButton & 32)` arm). No test covers it; these tests
are what keep `Drag` and `AnyMotion` usable.

**Files:**
- Modify: `src/core/tui/VtParser_test.cpp` — append at the end of the file
- Test: the same file

**Interfaces:**
- Consumes: `core::tui::VtParser::feed(std::string_view) -> std::vector<InputEvent>`,
  `core::tui::MouseEvent { Type type; int button; int x; int y; Modifier modifiers; bool uiHandled; }`
  (`src/core/tui/InputEvent.hpp`).
- Produces: nothing new.

- [ ] **Step 1: Write the tests**

Append to `src/core/tui/VtParser_test.cpp`:

```cpp
// ============================================================================
// SGR mouse reports (DEC 1006), as every tracking mode delivers them
// ============================================================================

namespace
{

/// @brief Parses a VT sequence and returns every MouseEvent in it, in order.
auto parseMice(std::string_view seq) -> std::vector<MouseEvent>
{
    auto parser = VtParser {};
    auto mice = std::vector<MouseEvent> {};
    for (auto const& event: parser.feed(seq))
        if (auto const* mouse = std::get_if<MouseEvent>(&event))
            mice.push_back(*mouse);
    return mice;
}

} // namespace

TEST_CASE("VtParser.SGRMouse.motion_with_the_left_button_held_is_a_move_of_button_0", "[tui,vtparser]")
{
    // DEC 1002 and 1003 report motion with a button held as that button's code plus 32.
    auto const mice = parseMice("\033[<32;10;5M");
    REQUIRE(mice.size() == 1);
    CHECK(mice[0].type == MouseEvent::Type::Move);
    CHECK(mice[0].button == 0);
    CHECK(mice[0].x == 10);
    CHECK(mice[0].y == 5);
}

TEST_CASE("VtParser.SGRMouse.motion_with_no_button_held_is_a_move_of_button_3", "[tui,vtparser]")
{
    // Only DEC 1003 reports motion with no button held: code 3, "no button", plus 32.
    auto const mice = parseMice("\033[<35;11;6M");
    REQUIRE(mice.size() == 1);
    CHECK(mice[0].type == MouseEvent::Type::Move);
    CHECK(mice[0].button == 3);
    CHECK(mice[0].x == 11);
    CHECK(mice[0].y == 6);
}

TEST_CASE("VtParser.SGRMouse.motion_with_the_right_button_held_is_a_move_of_button_2", "[tui,vtparser]")
{
    auto const mice = parseMice("\033[<34;3;4M");
    REQUIRE(mice.size() == 1);
    CHECK(mice[0].type == MouseEvent::Type::Move);
    CHECK(mice[0].button == 2);
}

TEST_CASE("VtParser.SGRMouse.a_shift_drag_carries_the_modifier", "[tui,vtparser]")
{
    // 36 = 32 (motion) | 4 (Shift) | 0 (left button).
    auto const mice = parseMice("\033[<36;7;8M");
    REQUIRE(mice.size() == 1);
    CHECK(mice[0].type == MouseEvent::Type::Move);
    CHECK(mice[0].button == 0);
    CHECK(mice[0].modifiers == Modifier::Shift);
}

TEST_CASE("VtParser.SGRMouse.a_drag_is_a_press_moves_and_a_release", "[tui,vtparser]")
{
    auto const mice = parseMice("\033[<0;2;3M\033[<32;4;3M\033[<0;4;3m");
    REQUIRE(mice.size() == 3);
    CHECK(mice[0].type == MouseEvent::Type::Press);
    CHECK(mice[0].x == 2);
    CHECK(mice[1].type == MouseEvent::Type::Move);
    CHECK(mice[1].x == 4);
    CHECK(mice[2].type == MouseEvent::Type::Release);
    CHECK(mice[2].button == 0);
    CHECK(mice[2].x == 4);
    CHECK(mice[2].y == 3);
}
```

Add `#include <vector>` to the file's standard includes, after `<variant>`.

- [ ] **Step 2: Run them**

Run:

```bash
cmake --build --preset clang-debug --target core-cpp-tui-test \
    && ./out/build/clang-debug/src/core/tui/core-cpp-tui-test "VtParser.SGRMouse.*"
```

Expected: PASS, `All tests passed (… assertions in 5 test cases)`. These tests characterise existing behaviour, so
they pass before any change; Step 3 is what shows they measure it.

- [ ] **Step 3: Mutation check**

In `src/core/tui/VtParser.cpp`, inside `dispatchCsi`'s `else if (rawButton & 32)` arm (the `Move` event), change
`.button = buttonBits & 3,` to `.button = 0,`. Rebuild and rerun the command above.
Expected: FAIL in `motion_with_no_button_held_is_a_move_of_button_3` and
`motion_with_the_right_button_held_is_a_move_of_button_2`. Restore the line (`git diff src/core/tui/VtParser.cpp`
prints nothing afterwards).

- [ ] **Step 4: Format and commit**

```bash
python3 scripts/clang-format.py src/core/tui/VtParser_test.cpp
git add src/core/tui/VtParser_test.cpp
git commit -s -F - <<'EOF'
test(tui): VtParser decodes SGR motion with a button held, and with none

DEC 1002 and 1003 report motion as the button code plus 32, and 1003 reports
motion with no button held as code 3. The parser already decodes both into
MouseEvent::Type::Move with that button; nothing pinned it, and the standard
mouse tracking modes depend on it.
EOF
```

No CHANGELOG entry: test-only (AGENT.md, workflow checklist 5).

---

### Task 3: `MouseTracking` and the sequences that move a terminal between its modes

**Files:**
- Create: `src/core/tui/MouseTracking.hpp`
- Modify: `src/core/tui/TerminalProtocols.hpp` — include the new header; add the constants and functions after the
  passive-mouse-tracking constants (`DisablePassiveMouseTracking`)
- Modify: `src/core/tui/CMakeLists.txt` — `core_cpp_add_module(tui_output …)` `HEADERS` list
- Modify: `.agent/reference/provenance.md` — one row after `src/core/tui/Modifier.hpp`
- Modify: `CHANGELOG.md` — `## [Unreleased]`
- Test: `src/core/tui/TerminalProtocols_test.cpp`

**Interfaces:**
- Consumes: `protocols::EnableAnyMotionTracking`, `DisableAnyMotionTracking`, `EnableSGRMouse`, `DisableSGRMouse`
  (`src/core/tui/TerminalProtocols.hpp`).
- Produces:
  - `core::tui::MouseTracking : std::uint8_t { Off, Buttons, Drag, AnyMotion }` in `<core/tui/MouseTracking.hpp>`.
  - `core::tui::protocols::EnableButtonTracking`, `DisableButtonTracking`, `EnableDragTracking`,
    `DisableDragTracking` (`std::string_view` constants).
  - `[[nodiscard]] constexpr auto protocols::mouseTrackingSet(MouseTracking) noexcept -> std::string_view`,
    `[[nodiscard]] constexpr auto protocols::mouseTrackingReset(MouseTracking) noexcept -> std::string_view`.
  - `inline void protocols::appendMouseTrackingChange(std::string& out, MouseTracking from, MouseTracking to)`.

- [ ] **Step 1: Write the failing test**

In `src/core/tui/TerminalProtocols_test.cpp`, add `#include <catch2/generators/catch_generators.hpp>` after the
`catch_test_macros.hpp` include and `#include <string_view>` after `<string>`, then append:

```cpp
// ============================================================================
// Mouse tracking modes (DEC 1000, 1002, 1003 with SGR 1006)
// ============================================================================

namespace
{

/// @brief One transition and the exact bytes it writes.
struct MouseTrackingChange
{
    core::tui::MouseTracking from;
    core::tui::MouseTracking to;
    std::string_view bytes;
};

} // namespace

TEST_CASE("TerminalProtocols.mouse_tracking_change_byte_exact")
{
    using enum core::tui::MouseTracking;
    auto const change = GENERATE(values<MouseTrackingChange>({
        { Off, Off, "" },
        { Off, Buttons, "\033[?1000h\033[?1006h" },
        { Off, Drag, "\033[?1002h\033[?1006h" },
        { Off, AnyMotion, "\033[?1003h\033[?1006h" },
        { Buttons, Off, "\033[?1006l\033[?1000l" },
        { Buttons, Buttons, "" },
        { Buttons, Drag, "\033[?1000l\033[?1002h" },
        { Buttons, AnyMotion, "\033[?1000l\033[?1003h" },
        { Drag, Off, "\033[?1006l\033[?1002l" },
        { Drag, Buttons, "\033[?1002l\033[?1000h" },
        { Drag, Drag, "" },
        { Drag, AnyMotion, "\033[?1002l\033[?1003h" },
        { AnyMotion, Off, "\033[?1006l\033[?1003l" },
        { AnyMotion, Buttons, "\033[?1003l\033[?1000h" },
        { AnyMotion, Drag, "\033[?1003l\033[?1002h" },
        { AnyMotion, AnyMotion, "" },
    }));
    CAPTURE(change.from, change.to);
    auto out = std::string {};
    appendMouseTrackingChange(out, change.from, change.to);
    CHECK(out == change.bytes);
}

TEST_CASE("TerminalProtocols.mouse_tracking_change_appends")
{
    auto out = std::string { "x" };
    appendMouseTrackingChange(out, core::tui::MouseTracking::Off, core::tui::MouseTracking::Drag);
    CHECK(out == "x\033[?1002h\033[?1006h");
}

TEST_CASE("TerminalProtocols.mouse_tracking_off_has_no_mode_sequence")
{
    CHECK(mouseTrackingSet(core::tui::MouseTracking::Off).empty());
    CHECK(mouseTrackingReset(core::tui::MouseTracking::Off).empty());
    STATIC_REQUIRE(mouseTrackingSet(core::tui::MouseTracking::Drag) == "\033[?1002h");
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build --preset clang-debug --target core-cpp-tui_output-test`
Expected: compile error, `no member named 'MouseTracking' in namespace 'core::tui'` (and
`use of undeclared identifier 'appendMouseTrackingChange'`).

- [ ] **Step 3: Implement**

Create `src/core/tui/MouseTracking.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace core::tui
{

/// @brief How much mouse input the terminal is asked to report.
///
/// Reporting is opt-in: while a terminal reports the mouse, a click-and-drag there no longer selects
/// text (most terminals keep Shift+drag for selection), so the default asks for nothing. Each mode is
/// a DEC private mode, enabled together with SGR encoding (DEC 1006) so that coordinates are not
/// limited to 223 cells. The parser decodes every mode's reports into a @c MouseEvent.
enum class MouseTracking : std::uint8_t
{
    Off,       ///< No reports (the default).
    Buttons,   ///< Press and release (DEC 1000).
    Drag,      ///< Press, release and motion while a button is held (DEC 1002).
    AnyMotion, ///< Press, release and all motion (DEC 1003).
};

} // namespace core::tui
```

In `src/core/tui/TerminalProtocols.hpp`, add `#include <core/tui/MouseTracking.hpp>` as the first include (its own
group, above `<string>`), and insert after the `DisablePassiveMouseTracking` line:

```cpp
// Button-event tracking (mode 1000): press and release only.
constexpr auto EnableButtonTracking = "\033[?1000h"sv;  ///< Enable press/release mouse tracking.
constexpr auto DisableButtonTracking = "\033[?1000l"sv; ///< Disable press/release mouse tracking.

// Button-motion tracking (mode 1002): press, release, and motion while a button is held.
constexpr auto EnableDragTracking = "\033[?1002h"sv;  ///< Enable drag mouse tracking.
constexpr auto DisableDragTracking = "\033[?1002l"sv; ///< Disable drag mouse tracking.

/// @brief Returns the DEC private mode set that asks for @p mode.
/// @param mode The tracking mode.
/// @return `CSI ? 1000 h`, `CSI ? 1002 h` or `CSI ? 1003 h`; empty for @c MouseTracking::Off.
[[nodiscard]] constexpr auto mouseTrackingSet(MouseTracking mode) noexcept -> std::string_view
{
    switch (mode)
    {
        case MouseTracking::Off: return {};
        case MouseTracking::Buttons: return EnableButtonTracking;
        case MouseTracking::Drag: return EnableDragTracking;
        case MouseTracking::AnyMotion: return EnableAnyMotionTracking;
    }
    return {};
}

/// @brief Returns the DEC private mode reset that ends @p mode.
/// @param mode The tracking mode.
/// @return `CSI ? 1000 l`, `CSI ? 1002 l` or `CSI ? 1003 l`; empty for @c MouseTracking::Off.
[[nodiscard]] constexpr auto mouseTrackingReset(MouseTracking mode) noexcept -> std::string_view
{
    switch (mode)
    {
        case MouseTracking::Off: return {};
        case MouseTracking::Buttons: return DisableButtonTracking;
        case MouseTracking::Drag: return DisableDragTracking;
        case MouseTracking::AnyMotion: return DisableAnyMotionTracking;
    }
    return {};
}

/// @brief Appends the sequences that move a terminal from mouse tracking @p from to @p to.
///
/// From @c MouseTracking::Off: the mode's set, then SGR encoding (1006). To @c MouseTracking::Off:
/// the reverse, 1006's reset and then the mode's. Between two modes: the old mode's reset and the new
/// one's set, leaving 1006 on. Nothing when the two are equal. Enabling a terminal's protocols is the
/// change from Off and disabling them the change to Off, so what is reset always mirrors what was set.
/// @param out Destination to append to.
/// @param from The mode the terminal is in.
/// @param to The mode it is to be in.
inline void appendMouseTrackingChange(std::string& out, MouseTracking from, MouseTracking to)
{
    if (from == to)
        return;
    if (from == MouseTracking::Off)
    {
        out.append(mouseTrackingSet(to));
        out.append(EnableSGRMouse);
        return;
    }
    if (to == MouseTracking::Off)
    {
        out.append(DisableSGRMouse);
        out.append(mouseTrackingReset(from));
        return;
    }
    out.append(mouseTrackingReset(from));
    out.append(mouseTrackingSet(to));
}
```

`EnableSGRMouse`/`DisableSGRMouse` are declared above the any-motion constants, so the order compiles.

In `src/core/tui/CMakeLists.txt`, in `core_cpp_add_module(tui_output … HEADERS …)`, add `MouseTracking.hpp`
between `Error.hpp` and `SgrBuilder.hpp`.

In `.agent/reference/provenance.md`, after the `src/core/tui/Modifier.hpp` row, add:

```markdown
| `src/core/tui/MouseTracking.hpp` | origin: core-cpp | - | - | - |
```

In `CHANGELOG.md`, replace the `## [Unreleased]` heading (its section is empty on `master`) with:

```markdown
## [Unreleased]

### Added

- **`core::tui::MouseTracking` and `protocols::appendMouseTrackingChange()`** (`<core/tui/MouseTracking.hpp>`,
  in `core::tui_output`): the standard mouse tracking modes -- `Buttons` (DEC 1000), `Drag` (1002) and
  `AnyMotion` (1003), with `Off` the default -- and the one function that writes a change between two of
  them: from `Off` the mode's set and then SGR encoding (1006), to `Off` the reverse, and between two modes
  only the swap. `protocols::EnableButtonTracking`, `EnableDragTracking` and their `Disable` twins join the
  existing 1003 constants, with `mouseTrackingSet()` and `mouseTrackingReset()` mapping a mode to its own.
```

- [ ] **Step 4: Run the tests to verify they pass**

Run:

```bash
cmake --build --preset clang-debug --target core-cpp-tui_output-test \
    && ./out/build/clang-debug/src/core/tui/core-cpp-tui_output-test "TerminalProtocols.mouse*"
```

Expected: PASS, `All tests passed (… assertions in 3 test cases)` (the generator makes the first case 16 runs).

Mutation check: in `appendMouseTrackingChange`, delete the `if (from == to) return;` pair of lines; rebuild and
rerun. Expected: FAIL in `mouse_tracking_change_byte_exact` for `{ Off, Off, "" }` (it writes
`"\033[?1006h"`). Restore the lines.

- [ ] **Step 5: Format and commit**

```bash
python3 scripts/clang-format.py src/core/tui/MouseTracking.hpp src/core/tui/TerminalProtocols.hpp \
    src/core/tui/TerminalProtocols_test.cpp
cmake --build --preset clang-debug && ctest --preset clang-debug -L hygiene
git add src/core/tui/MouseTracking.hpp src/core/tui/TerminalProtocols.hpp src/core/tui/TerminalProtocols_test.cpp \
    src/core/tui/CMakeLists.txt .agent/reference/provenance.md CHANGELOG.md
git commit -s -F - <<'EOF'
feat(tui): MouseTracking, and the sequences that move a terminal between its modes

The standard mouse tracking modes -- DEC 1000, 1002 and 1003, each with SGR
encoding (1006) -- as an enum in core::tui_output, and one pure function that
writes the change between any two of them. Enabling is the change from Off and
disabling the change to Off, so the resets always mirror the sets; between two
modes only the swap is written. The whole table is pinned byte for byte.
EOF
```

`ctest -L hygiene` is there for the provenance row: expected `100% tests passed`.

---

### Task 4: `TerminalInput` and `Terminal` ask the terminal for the requested mouse tracking

**Files:**
- Modify: `src/core/tui/TerminalInput.hpp` — include, the `setAnyMotionTracking` doc, two new public members after
  it, one private member after `_anyMotionTracking`, one private function after `writeProtocol`
- Modify: `src/core/tui/TerminalInput.cpp` — replace `setAnyMotionTracking`, add three definitions
- Modify: `src/core/tui/posix/TerminalInput.cpp` — `enableProtocols()` and `disableProtocols()`
- Modify: `src/core/tui/windows/TerminalInput.cpp` — `enableProtocols()` and `disableProtocols()`
- Modify: `src/core/tui/Terminal.hpp` — one public member after `isSuspended()`
- Modify: `src/core/tui/Terminal.cpp` — its definition after `Terminal::isSuspended()`
- Modify: `src/core/tui/CMakeLists.txt` — `core_cpp_add_test(tui …)` `SOURCES` and `SOURCES_POSIX`
- Modify: `.agent/reference/provenance.md` — two rows
- Modify: `CHANGELOG.md`, `docs/modules/tui.md`
- Test: `src/core/tui/TerminalInput_test.cpp` (new), `src/core/tui/posix/TerminalInput_test.cpp` (new)

**Interfaces:**
- Consumes: Task 3's `MouseTracking` and `protocols::appendMouseTrackingChange`; `TerminalInput::writeProtocol`,
  `_rawMode`, `_anyMotionTracking`, `enableProtocols`, `disableProtocols` (`src/core/tui/TerminalInput.hpp`).
- Produces:
  - `void core::tui::TerminalInput::setMouseTracking(MouseTracking mode);`
  - `[[nodiscard]] auto core::tui::TerminalInput::mouseTracking() const noexcept -> MouseTracking;`
  - `void core::tui::Terminal::setMouseTracking(MouseTracking mode);`
  - `TerminalInput::setAnyMotionTracking(bool)` keeps its signature and now raises the requested mode.

- [ ] **Step 1: Write the failing tests**

Create `src/core/tui/TerminalInput_test.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The mouse tracking mode TerminalInput asks for, without a terminal: nothing here enables the
// protocols, so nothing is written. posix/TerminalInput_test.cpp reads the bytes themselves.

#include <core/tui/MockTerminalOutput.hpp>
#include <core/tui/MouseTracking.hpp>
#include <core/tui/Terminal.hpp>
#include <core/tui/TerminalInput.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>

using core::tui::MockTerminalOutput;
using core::tui::MouseTracking;
using core::tui::Terminal;
using core::tui::TerminalInput;

TEST_CASE("TerminalInput.mouse_tracking_defaults_to_off", "[tui]")
{
    auto const input = TerminalInput {};
    CHECK(input.mouseTracking() == MouseTracking::Off);
}

TEST_CASE("TerminalInput.mouse_tracking_is_the_requested_mode_raised_while_hover_is_on", "[tui]")
{
    auto input = TerminalInput {};
    input.setMouseTracking(MouseTracking::Drag);
    CHECK(input.mouseTracking() == MouseTracking::Drag);

    input.setAnyMotionTracking(true);
    CHECK(input.mouseTracking() == MouseTracking::AnyMotion);

    // The request made while raised is kept, and applies again once hover is off.
    input.setMouseTracking(MouseTracking::Buttons);
    CHECK(input.mouseTracking() == MouseTracking::AnyMotion);
    input.setAnyMotionTracking(false);
    CHECK(input.mouseTracking() == MouseTracking::Buttons);
}

TEST_CASE("Terminal.setMouseTracking_forwards_to_its_input", "[tui]")
{
    auto terminal = Terminal(std::make_unique<MockTerminalOutput>(80, 24));
    terminal.setMouseTracking(MouseTracking::Buttons);
    CHECK(terminal.input().mouseTracking() == MouseTracking::Buttons);
}
```

Create `src/core/tui/posix/TerminalInput_test.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The bytes the POSIX TerminalInput writes when it enables its protocols, when the mouse tracking
// mode changes while they are enabled, and when it disables them, read back from a pipe that stands
// in for standard output. TerminalInput writes to STDOUT_FILENO and reads STDIN_FILENO and has no
// other seam for either, so each case swaps the two descriptors for the length of the run, as
// TerminalHangup_test.cpp does for standard input. Standard input is /dev/null: raw mode is then
// refused by the kernel, which TerminalInput ignores, and nothing is read.

#include <core/tui/MouseTracking.hpp>
#include <core/tui/TerminalInput.hpp>
#include <core/tui/TerminalProtocols.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cstddef>
#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <unistd.h>

using core::tui::MouseTracking;
using core::tui::TerminalInput;
namespace protocols = core::tui::protocols;

namespace
{

/// Standard output into a pipe and standard input from /dev/null, until destroyed.
class RedirectedStandardStreams
{
  public:
    RedirectedStandardStreams()
    {
        // Anything the test runner buffered goes to the real output first, not into the pipe.
        std::cout.flush();
        std::fflush(stdout);
        if (::pipe(_pipe.data()) != 0)
            return;
        if (auto const flags = ::fcntl(_pipe[0], F_GETFL, 0); flags != -1)
            ::fcntl(_pipe[0], F_SETFL, flags | O_NONBLOCK);
        _savedOut = ::dup(STDOUT_FILENO);
        _savedIn = ::dup(STDIN_FILENO);
        auto const devNull = ::open("/dev/null", O_RDONLY);
        _isRedirected = _savedOut >= 0 && _savedIn >= 0 && devNull >= 0
                        && ::dup2(devNull, STDIN_FILENO) == STDIN_FILENO
                        && ::dup2(_pipe[1], STDOUT_FILENO) == STDOUT_FILENO;
        if (devNull >= 0)
            ::close(devNull);
    }

    ~RedirectedStandardStreams()
    {
        if (_savedOut >= 0)
        {
            ::dup2(_savedOut, STDOUT_FILENO);
            ::close(_savedOut);
        }
        if (_savedIn >= 0)
        {
            ::dup2(_savedIn, STDIN_FILENO);
            ::close(_savedIn);
        }
        for (auto const end: _pipe)
            if (end >= 0)
                ::close(end);
    }

    RedirectedStandardStreams(RedirectedStandardStreams const&) = delete;
    RedirectedStandardStreams& operator=(RedirectedStandardStreams const&) = delete;
    RedirectedStandardStreams(RedirectedStandardStreams&&) = delete;
    RedirectedStandardStreams& operator=(RedirectedStandardStreams&&) = delete;

    /// Whether both descriptors were swapped; a run that was not measured nothing.
    [[nodiscard]] bool isRedirected() const noexcept { return _isRedirected; }

    /// Everything written to standard output since the last call.
    [[nodiscard]] std::string take()
    {
        auto bytes = std::string {};
        auto chunk = std::array<char, 512> {};
        auto count = ::read(_pipe[0], chunk.data(), chunk.size());
        while (count > 0)
        {
            bytes.append(chunk.data(), static_cast<std::size_t>(count));
            count = ::read(_pipe[0], chunk.data(), chunk.size());
        }
        return bytes;
    }

  private:
    std::array<int, 2> _pipe { -1, -1 };
    int _savedOut = -1;
    int _savedIn = -1;
    bool _isRedirected = false;
};

/// What one TerminalInput wrote: when it enabled its protocols, during a change made while they were
/// enabled, and when it disabled them.
struct ProtocolRun
{
    bool isRedirected = false;
    bool isInitialized = false;
    std::string enabled;
    std::string changed;
    std::string disabled;
};

/// Runs a TerminalInput that asks for @p requested from initialize() to shutdown(), calling @p change
/// in between. The checks happen after this returns, once standard output is the test runner's again.
template <typename Change>
[[nodiscard]] ProtocolRun runProtocols(MouseTracking requested, Change change)
{
    auto run = ProtocolRun {};
    auto streams = RedirectedStandardStreams {};
    run.isRedirected = streams.isRedirected();
    if (!run.isRedirected)
        return run;

    auto input = TerminalInput {};
    input.setMouseTracking(requested);
    run.isInitialized = input.initialize().has_value();
    run.enabled = streams.take();
    change(input);
    run.changed = streams.take();
    input.shutdown();
    run.disabled = streams.take();
    return run;
}

/// One mode and the set and reset it is written with.
struct ModeBytes
{
    MouseTracking mode;
    std::string_view set;
    std::string_view reset;
};

} // namespace

TEST_CASE("TerminalInput.posix.mouse_tracking_off_writes_no_mouse_mode", "[tui]")
{
    auto const run = runProtocols(MouseTracking::Off, [](TerminalInput&) {});
    REQUIRE(run.isRedirected);
    REQUIRE(run.isInitialized);

    // The run saw enableProtocols(): Contour's passive mode is enabled as it always was.
    CHECK(run.enabled.contains(protocols::EnablePassiveMouseTracking));
    for (auto const* const mode: { "1000", "1002", "1003", "1006" })
    {
        CAPTURE(mode);
        CHECK_FALSE(run.enabled.contains(std::string("\033[?") + mode));
        CHECK_FALSE(run.disabled.contains(std::string("\033[?") + mode));
    }
}

TEST_CASE("TerminalInput.posix.a_mouse_tracking_mode_is_set_with_sgr_and_reset_in_reverse", "[tui]")
{
    auto const expected = GENERATE(values<ModeBytes>({
        { MouseTracking::Buttons, "\033[?1000h", "\033[?1000l" },
        { MouseTracking::Drag, "\033[?1002h", "\033[?1002l" },
        { MouseTracking::AnyMotion, "\033[?1003h", "\033[?1003l" },
    }));
    CAPTURE(expected.mode);
    auto const run = runProtocols(expected.mode, [](TerminalInput&) {});
    REQUIRE(run.isRedirected);
    REQUIRE(run.isInitialized);

    CHECK(run.enabled.contains(std::string(expected.set) + std::string(protocols::EnableSGRMouse)));
    CHECK(run.disabled.contains(std::string(protocols::DisableSGRMouse) + std::string(expected.reset)));
    for (auto const other: { protocols::EnableButtonTracking, protocols::EnableDragTracking,
                             protocols::EnableAnyMotionTracking })
        if (other != expected.set)
            CHECK_FALSE(run.enabled.contains(other));
    CHECK(run.changed.empty());
}

TEST_CASE("TerminalInput.posix.a_mode_change_while_enabled_writes_only_the_transition", "[tui]")
{
    auto const run =
        runProtocols(MouseTracking::Buttons, [](TerminalInput& input) { input.setMouseTracking(MouseTracking::Drag); });
    REQUIRE(run.isRedirected);
    REQUIRE(run.isInitialized);
    CHECK(run.changed == "\033[?1000l\033[?1002h");
    CHECK(run.disabled.contains("\033[?1006l\033[?1002l"));
}

TEST_CASE("TerminalInput.posix.hover_raises_the_requested_mode_to_any_motion", "[tui]")
{
    auto raised = MouseTracking::Off;
    auto const run = runProtocols(MouseTracking::Drag, [&raised](TerminalInput& input) {
        input.setAnyMotionTracking(true);
        raised = input.mouseTracking();
    });
    REQUIRE(run.isRedirected);
    REQUIRE(run.isInitialized);
    CHECK(raised == MouseTracking::AnyMotion);
    CHECK(run.changed == "\033[?1002l\033[?1003h");
    CHECK(run.disabled.contains("\033[?1006l\033[?1003l"));
}
```

Register both in `src/core/tui/CMakeLists.txt`'s `core_cpp_add_test(tui …)`: add `TerminalInput_test.cpp` to
`SOURCES` between `TerminalQuery_test.cpp` and `TreeTableView_test.cpp`, and `posix/TerminalInput_test.cpp` to
`SOURCES_POSIX` after `posix/TerminalHangup_test.cpp`. Update that block's comment: replace
"posix/TerminalHangup_test.cpp is POSIX-only: it hangs up a pseudo-terminal." with
"posix/TerminalHangup_test.cpp and posix/TerminalInput_test.cpp are POSIX-only: one hangs up a pseudo-terminal,
the other swaps standard output for a pipe with dup2."

In `.agent/reference/provenance.md`, add after the `src/core/tui/TerminalInput.hpp` row:

```markdown
| `src/core/tui/TerminalInput_test.cpp` | origin: core-cpp | - | - | - |
```

and after the `src/core/tui/posix/TerminalInput.cpp` row:

```markdown
| `src/core/tui/posix/TerminalInput_test.cpp` | origin: core-cpp | - | - | the protocol bytes, read from a pipe that stands in for standard output |
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake --build --preset clang-debug --target core-cpp-tui-test`
Expected: compile error, `no member named 'setMouseTracking' in 'core::tui::TerminalInput'` (and
`'mouseTracking'`, and `no member named 'setMouseTracking' in 'core::tui::Terminal'`).

- [ ] **Step 3: Implement**

`src/core/tui/TerminalInput.hpp`: add `#include <core/tui/MouseTracking.hpp>` after `<core/tui/InputEvent.hpp>`.
Replace the whole `setAnyMotionTracking` declaration and its comment with:

```cpp
    /// @brief Raises the mouse tracking mode to any-motion (mode 1003) while @p enabled, for hover
    /// tooltips.
    ///
    /// Call it only after confirming passive mouse tracking (mode 2029) via DECRQPM, so that a terminal
    /// without it is not asked to report every motion. While raised, @c mouseTracking() answers
    /// @c MouseTracking::AnyMotion whatever @c setMouseTracking() requested; lowered, the requested
    /// mode applies again. The change is written at once when the protocols are enabled.
    /// @param enabled True to raise the mode, false to return to the requested one.
    void setAnyMotionTracking(bool enabled);

    /// @brief Sets how much mouse input the terminal is asked to report.
    ///
    /// Effective at the next enableProtocols() -- @c initialize() or @c resume() -- and at once when
    /// the protocols are enabled now, by writing only the change from the current mode. The default
    /// is @c MouseTracking::Off: a terminal that reports the mouse stops selecting text on a
    /// click-and-drag, so an application opts in.
    /// @param mode The mode the application asks for.
    void setMouseTracking(MouseTracking mode);

    /// @brief Returns the mode the terminal is asked for.
    /// @return The mode @c setMouseTracking() requested, or @c MouseTracking::AnyMotion while
    ///         @c setAnyMotionTracking() raises it.
    [[nodiscard]] auto mouseTracking() const noexcept -> MouseTracking;
```

In the private members, replace the `_anyMotionTracking` line with:

```cpp
    bool _anyMotionTracking = false; ///< True while hover tracking raises the mode to any-motion (1003).
    MouseTracking _mouseTracking = MouseTracking::Off; ///< The mode setMouseTracking() requested.
```

and after `void writeProtocol(std::string_view data) const;` add:

```cpp

    /// @brief Writes the sequences that move the terminal from mouse tracking @p from to @p to.
    /// @param from The mode the terminal is in.
    /// @param to The mode it is to be in.
    void writeMouseTrackingChange(MouseTracking from, MouseTracking to) const;
```

`src/core/tui/TerminalInput.cpp`: add `#include <string>` after the `<core/tui/TerminalProtocols.hpp>` include
(own group), and replace `TerminalInput::setAnyMotionTracking` with:

```cpp
void TerminalInput::setAnyMotionTracking(bool enabled)
{
    auto const before = mouseTracking();
    _anyMotionTracking = enabled;
    if (_rawMode)
        writeMouseTrackingChange(before, mouseTracking());
}

void TerminalInput::setMouseTracking(MouseTracking mode)
{
    auto const before = mouseTracking();
    _mouseTracking = mode;
    if (_rawMode)
        writeMouseTrackingChange(before, mouseTracking());
}

auto TerminalInput::mouseTracking() const noexcept -> MouseTracking
{
    return _anyMotionTracking ? MouseTracking::AnyMotion : _mouseTracking;
}

void TerminalInput::writeMouseTrackingChange(MouseTracking from, MouseTracking to) const
{
    auto sequence = std::string {};
    protocols::appendMouseTrackingChange(sequence, from, to);
    if (!sequence.empty())
        writeProtocol(sequence);
}
```

`_rawMode` is false while suspended (`suspend()` calls `disableRawMode()`), so a change made then is written by
`resume()`'s `enableProtocols()`, as the hover flag's was.

`src/core/tui/posix/TerminalInput.cpp`, in `enableProtocols()`, replace

```cpp
    if (_anyMotionTracking)
        writeProtocol(protocols::EnableAnyMotionTracking);
```

with

```cpp
    writeMouseTrackingChange(MouseTracking::Off, mouseTracking());
```

and in `disableProtocols()` replace

```cpp
    if (_anyMotionTracking)
        writeProtocol(protocols::DisableAnyMotionTracking);
```

with

```cpp
    writeMouseTrackingChange(mouseTracking(), MouseTracking::Off);
```

`src/core/tui/windows/TerminalInput.cpp`: the same two replacements in its `enableProtocols()` and
`disableProtocols()`.

`src/core/tui/Terminal.hpp`, after the `isSuspended()` declaration:

```cpp

    /// @brief Sets how much mouse input the terminal is asked to report.
    ///
    /// Forwards to @c TerminalInput::setMouseTracking(): effective at @c initialize() when called
    /// before it, and at once on an initialized terminal. A terminal over a mock output never enables
    /// its protocols, so there the mode is only recorded.
    /// @param mode The mode the application asks for.
    void setMouseTracking(MouseTracking mode);
```

`src/core/tui/Terminal.cpp`, after `Terminal::isSuspended()`:

```cpp
void Terminal::setMouseTracking(MouseTracking mode)
{
    _input.setMouseTracking(mode);
}
```

The hover path in `posix/Terminal.cpp` (`if (isChangeable(queryDecMode(2029))) _input.setAnyMotionTracking(true);`)
is unchanged: it now raises the requested mode. Update its comment block to:

```cpp
    // Detect passive mouse tracking support (DEC mode 2029). Where the terminal recognises it, hover
    // tooltips raise the mouse tracking mode to any-motion (1003); a terminal that does not silently
    // ignored mode 2029 in enableProtocols() and is not asked to report every motion.
```

`CHANGELOG.md`, append to `### Added`:

```markdown
- **`TerminalInput::setMouseTracking()` and `Terminal::setMouseTracking()`: standard mouse tracking.**
  core::tui enabled only Contour's passive mode 2029, so xterm, kitty, WezTerm, iTerm2 and Windows Terminal
  reported no mouse events at all. An application now asks for a `MouseTracking` mode, which
  `enableProtocols()` writes with SGR encoding and `disableProtocols()` resets in reverse order; a change
  while the protocols are enabled writes only the transition. The default stays `Off`, so no existing
  program changes what a click-and-drag does in its terminal. The hover path that enables 1003 once the
  terminal confirms 2029 now raises the requested mode to `AnyMotion`, and so also writes 1006;
  `TerminalInput::mouseTracking()` answers the mode in effect.
```

`docs/modules/tui.md`, in the `## core::tui` list, after the `- **Input.** …` bullet, insert:

```markdown
- **Mouse.** `TerminalInput::setMouseTracking()`, or `Terminal::setMouseTracking()`, asks the terminal for a
  `MouseTracking` mode: `Buttons` (DEC 1000), `Drag` (1002) or `AnyMotion` (1003), each with SGR encoding
  (1006). The default is `Off`, because a terminal that reports the mouse stops selecting text on a
  click-and-drag; an application opts in. Contour's passive mode 2029 is enabled either way, and where the
  terminal confirms it, hover tooltips raise the mode to `AnyMotion` (`mouseTracking()` answers the raised
  mode). `VtParser` decodes every mode's reports into `MouseEvent`s: motion with a button held is a `Move`
  of that button, motion with none a `Move` of button 3.
```

and in the `## core::tui_output` list, replace

```markdown
  win32-input-mode, OSC 8), `appendHyperlinkOpen()`, and `parseSixelFromDeviceAttributes()`, which
  reads a DA1 answer.
```

with

```markdown
  win32-input-mode, OSC 8), `appendHyperlinkOpen()`, `appendMouseTrackingChange()`, which writes the change
  between two `MouseTracking` modes, and `parseSixelFromDeviceAttributes()`, which reads a DA1 answer.
```

- [ ] **Step 4: Run the tests to verify they pass**

Run:

```bash
cmake --build --preset clang-debug --target core-cpp-tui-test \
    && ./out/build/clang-debug/src/core/tui/core-cpp-tui-test "TerminalInput.*,Terminal.setMouseTracking*"
```

Expected: PASS, `All tests passed (… assertions in 7 test cases)` on POSIX (3 portable, 4 POSIX).

Mutation check: in `src/core/tui/posix/TerminalInput.cpp`'s `enableProtocols()`, delete
`writeMouseTrackingChange(MouseTracking::Off, mouseTracking());`; rebuild and rerun. Expected: FAIL in
`a_mouse_tracking_mode_is_set_with_sgr_and_reset_in_reverse` (`run.enabled.contains(...)` is false for every
mode). Restore the line. Then, to show the Off case is not vacuous, add `"2029"` to its
`{ "1000", "1002", "1003", "1006" }` list and rerun: expected FAIL on that entry, because the capture does hold
`\033[?2029h`. Restore the list.

Windows: the two `windows/TerminalInput.cpp` edits are the same calls as POSIX's and are compiled and run only by
CI's Windows legs. Its `enableProtocols()` cannot be captured the same way (`initialize()` refuses a standard
output that is not a console), so what it writes is **inferred from the shared helper, not measured** — say so in
the pull request body.

- [ ] **Step 5: Format and commit**

```bash
python3 scripts/clang-format.py src/core/tui/TerminalInput.hpp src/core/tui/TerminalInput.cpp \
    src/core/tui/posix/TerminalInput.cpp src/core/tui/windows/TerminalInput.cpp src/core/tui/Terminal.hpp \
    src/core/tui/Terminal.cpp src/core/tui/posix/Terminal.cpp src/core/tui/TerminalInput_test.cpp \
    src/core/tui/posix/TerminalInput_test.cpp
cmake --build --preset clang-debug && ctest --preset clang-debug
git add src/core/tui .agent/reference/provenance.md CHANGELOG.md docs/modules/tui.md
git commit -s -F - <<'EOF'
feat(tui): TerminalInput and Terminal ask the terminal for the requested mouse tracking

core::tui enabled only Contour's passive mode 2029, so a standard terminal
reported no mouse at all. setMouseTracking() asks for DEC 1000, 1002 or 1003
with SGR 1006, written when the protocols are enabled and reset in reverse when
they are disabled; a change while enabled writes only the transition. The
default is Off, so no existing program loses its terminal's text selection.
The 2029 hover path now raises the requested mode to AnyMotion.

The POSIX bytes are read back from a pipe standing in for standard output;
the Windows arm makes the same calls and is covered by CI only.
EOF
```

---

### Task 5: `Screen::componentAt` is public, and a mouse event over an overlay goes to the overlay

Today `Screen::componentAt` is private, ignores overlays, and returns the root for a cell no child covers. Spec 0
§3 wants it public, overlay-first, and null for an empty cell. Dispatch uses the same lookup, so an overlay
(a popup or dialog shown with `showOverlay`) now receives the mouse events over it; the screen's own tooltip is
excluded, because it sits under the pointer and would take the hover away from the component it describes. The
internal lookup keeps answering the root for an empty cell, so `HoverState` sees exactly what it saw before.

**Files:**
- Modify: `src/core/tui/Screen.hpp` — a public block after the overlay system; the private hit-testing block
- Modify: `src/core/tui/Screen.cpp` — `componentAt` becomes `hitTest` plus a public `componentAt`;
  `dispatchMouseEvent` calls `hitTest`
- Modify: `CHANGELOG.md`
- Test: `src/core/tui/Screen_test.cpp` — append

**Interfaces:**
- Consumes: `Component::screenBounds()`, `visible()`, `children()`, `zIndex()` (`src/core/tui/Component.hpp`);
  `Screen::showOverlay`, `showTooltip`, `isTooltipVisible`, `draw` (`src/core/tui/Screen.hpp`).
- Produces: `[[nodiscard]] Component* core::tui::Screen::componentAt(int row, int col) const;` (public);
  private `[[nodiscard]] Component* Screen::hitTest(int row, int col) const;`.

- [ ] **Step 1: Write the failing tests**

Add `#include <variant>` and `#include <vector>` to `src/core/tui/Screen_test.cpp`'s standard includes, then append:

```cpp
// ============================================================================
// Hit testing and pointer capture
// ============================================================================

namespace
{

/// @brief Records every mouse event it receives, in the coordinates it receives them in.
struct MouseRecorder: Component
{
    EventResult pressResult = EventResult::Handled; ///< What a press returns.
    Size size { .width = 10, .height = 3 };          ///< Preferred size, which an overlay is shown at.
    std::vector<MouseEvent> received;                ///< Every mouse event, in order.

    void render(Canvas& /*canvas*/) override {}

    [[nodiscard]] Size preferredSize() const override { return size; }

    EventResult onEvent(InputEvent const& event) override
    {
        auto const* mouse = std::get_if<MouseEvent>(&event);
        if (mouse == nullptr)
            return EventResult::Ignored;
        received.push_back(*mouse);
        return mouse->type == MouseEvent::Type::Press ? pressResult : EventResult::Handled;
    }
};

/// @brief A mouse event at a 1-based terminal cell, as the parser delivers it.
[[nodiscard]] InputEvent mouseAt(MouseEvent::Type type, int x, int y)
{
    return InputEvent { MouseEvent { .type = type, .button = 0, .x = x, .y = y } };
}

} // namespace

TEST_CASE("Screen.componentAt_findsTheDeepestVisibleComponent")
{
    auto terminal = Terminal(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(terminal);
    auto panel = MouseRecorder {};
    auto leaf = MouseRecorder {};
    auto hidden = MouseRecorder {};
    screen.root().addChild(panel, LayoutParams { .area = { .x = 2, .y = 1, .width = 20, .height = 5 } });
    panel.addChild(leaf, LayoutParams { .area = { .x = 1, .y = 1, .width = 5, .height = 2 } });
    screen.root().addChild(hidden,
                           LayoutParams { .area = { .x = 30, .y = 1, .width = 5, .height = 2 }, .visible = false });
    screen.draw();

    CHECK(screen.componentAt(2, 4) == &leaf);   // leaf covers columns 3..7, rows 2..3
    CHECK(screen.componentAt(1, 2) == &panel);  // the panel's own top-left cell
    CHECK(screen.componentAt(1, 31) == nullptr); // only the invisible component is there
    CHECK(screen.componentAt(20, 70) == nullptr); // nothing but the root
}

TEST_CASE("Screen.componentAt_prefersTheHigherZIndex")
{
    auto terminal = Terminal(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(terminal);
    auto high = MouseRecorder {};
    auto low = MouseRecorder {};
    // Added first, so insertion order alone would put it underneath.
    screen.root().addChild(high, LayoutParams { .area = { .x = 5, .y = 0, .width = 10, .height = 3 }, .zIndex = 1 });
    screen.root().addChild(low, LayoutParams { .area = { .x = 0, .y = 0, .width = 10, .height = 3 } });
    screen.draw();

    CHECK(screen.componentAt(0, 6) == &high);
    CHECK(screen.componentAt(0, 2) == &low);
}

TEST_CASE("Screen.componentAt_findsOverlaysBeforeTheTree")
{
    auto terminal = Terminal(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(terminal);
    auto tree = MouseRecorder {};
    auto first = MouseRecorder {};
    auto second = MouseRecorder {};
    screen.root().addChild(tree, LayoutParams { .area = { .x = 0, .y = 0, .width = 40, .height = 10 } });
    screen.showOverlay(first, Point { .x = 5, .y = 2 });  // columns 5..14, rows 2..4
    screen.showOverlay(second, Point { .x = 8, .y = 3 }); // columns 8..17, rows 3..5
    screen.draw();

    CHECK(screen.componentAt(2, 6) == &first);
    CHECK(screen.componentAt(4, 10) == &second); // shown last, so on top where they overlap
    CHECK(screen.componentAt(0, 0) == &tree);

    // Showing an overlay again moves it but keeps its place in the stack.
    screen.showOverlay(first, Point { .x = 5, .y = 2 });
    screen.draw();
    CHECK(screen.componentAt(4, 10) == &second);
}

TEST_CASE("Screen.componentAt_neverReturnsTheTooltip")
{
    auto terminal = Terminal(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(terminal);
    auto tree = MouseRecorder {};
    screen.root().addChild(tree, LayoutParams { .area = { .x = 0, .y = 0, .width = 80, .height = 10 } });
    screen.showTooltip("a tooltip", Point { .x = 0, .y = 0 }); // drawn from row 1 down
    screen.draw();

    REQUIRE(screen.isTooltipVisible());
    CHECK(screen.componentAt(1, 1) == &tree);
}

TEST_CASE("Screen.mousePress_overAnOverlayGoesToTheOverlay")
{
    auto terminal = Terminal(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(terminal);
    auto tree = MouseRecorder {};
    auto overlay = MouseRecorder {};
    screen.root().addChild(tree, LayoutParams { .area = { .x = 0, .y = 0, .width = 40, .height = 10 } });
    screen.showOverlay(overlay, Point { .x = 5, .y = 2 });
    screen.draw();

    CHECK(screen.dispatchEvent(mouseAt(MouseEvent::Type::Press, 7, 4)) == EventResult::Handled); // cell (3, 6)

    REQUIRE(overlay.received.size() == 1);
    CHECK(overlay.received[0].x == 2); // overlay-relative, 1-based, as for any component
    CHECK(overlay.received[0].y == 2);
    CHECK(tree.received.empty());
}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake --build --preset clang-debug --target core-cpp-tui-test`
Expected: compile error, `'componentAt' is a private member of 'core::tui::Screen'`.

- [ ] **Step 3: Implement**

`src/core/tui/Screen.hpp`: after the `isOverlayVisible(...)` declaration (end of "Overlay System"), insert:

```cpp

    // --- Hit Testing ---

    /// Returns the top-most visible component at a cell, as the last draw() laid the screen out.
    ///
    /// Overlays come first, in the order they are drawn, so the one shown last is on top; then the
    /// tree, siblings in reverse z-order and the deepest descendant that contains the cell. The
    /// screen's own tooltip is never returned. Mouse events are routed by the same lookup; a drag
    /// source asks it, at the release, which component lies under the pointer.
    /// @param row 0-based row, in the coordinates of Component::screenBounds() (rows from the top of
    ///            the inline content in Viewport::Inline).
    /// @param col 0-based column, in the same coordinates.
    /// @return The component, or nullptr when nothing but the root covers the cell.
    [[nodiscard]] Component* componentAt(int row, int col) const;
```

In the private `// Hit testing` block, replace

```cpp
    [[nodiscard]] Component* componentAt(int row, int col) const;
```

with

```cpp
    /// componentAt() without its last step: an empty cell answers the root, which is what mouse
    /// dispatch and the hover state expect.
    /// @param row 0-based row, as for componentAt().
    /// @param col 0-based column, as for componentAt().
    /// @return The top-most visible component at the cell, the root, or nullptr outside the root.
    [[nodiscard]] Component* hitTest(int row, int col) const;
```

`src/core/tui/Screen.cpp`: replace the definition of `Screen::componentAt` with:

```cpp
Component* Screen::componentAt(int row, int col) const
{
    auto* const found = hitTest(row, col);
    return found == _root.get() ? nullptr : found;
}

Component* Screen::hitTest(int row, int col) const
{
    // Overlays are drawn after the tree, in the order they were first shown, so the last of them is on
    // top. The tooltip is not a target: it opens under the pointer, and hit-testing it would end the
    // hover over the component it describes.
    for (auto const& entry: _overlays | std::views::reverse)
    {
        auto* const overlay = entry.component;
        if (overlay == nullptr || overlay == &_tooltip || !overlay->visible()
            || !overlay->screenBounds().contains(col, row))
            continue;
        return componentAtRecursive(*overlay, row, col);
    }
    return componentAtRecursive(*_root, row, col);
}
```

In `Screen::dispatchMouseEvent`, replace `Component* target = componentAt(mouseRow, mouseCol);` with
`Component* target = hitTest(mouseRow, mouseCol);`. Update the doc of `dispatchEvent` in `Screen.hpp` from
"For mouse events: hit tests to find target, then bubbles up." to
"For mouse events: hit tests to find the target (overlays first, see componentAt()), then bubbles up."

`CHANGELOG.md`, append to `### Added`:

```markdown
- **`Screen::componentAt(row, col)` is public**: the top-most visible component at a cell, as the last
  `draw()` laid the screen out -- overlays first, the one shown last on top, then the tree in reverse
  z-order, deepest descendant first -- or null where nothing but the root is. A drag source asks it for
  the component under the pointer at the release.
```

and add after the `### Added` section:

```markdown
### Changed

- **A mouse event over an overlay goes to the overlay.** `Screen` hit-tested only the component tree, so
  a click on a popup or dialog shown with `showOverlay()` reached the component beneath it. Overlays are
  now tested first, the one shown last on top. The screen's own tooltip is not a target, so the hover
  stays with the component it describes.
```

- [ ] **Step 4: Run the tests to verify they pass**

Run:

```bash
cmake --build --preset clang-debug --target core-cpp-tui-test \
    && ./out/build/clang-debug/src/core/tui/core-cpp-tui-test "Screen.*,HoverState.*"
```

Expected: PASS, every `Screen.*` and `HoverState.*` case — the existing hover/tooltip cases included, which is
what shows the root-answering `hitTest` kept their behaviour.

Mutation check: in `hitTest`, delete `overlay == &_tooltip ||`; rebuild and rerun `"Screen.componentAt*"`.
Expected: FAIL in `componentAt_neverReturnsTheTooltip`. Restore. Then delete the whole `for` loop over
`_overlays`; expected: FAIL in `componentAt_findsOverlaysBeforeTheTree` and
`mousePress_overAnOverlayGoesToTheOverlay`. Restore.

- [ ] **Step 5: Format and commit**

```bash
python3 scripts/clang-format.py src/core/tui/Screen.hpp src/core/tui/Screen.cpp src/core/tui/Screen_test.cpp
cmake --build --preset clang-debug && ctest --preset clang-debug
git add src/core/tui/Screen.hpp src/core/tui/Screen.cpp src/core/tui/Screen_test.cpp CHANGELOG.md
git commit -s -F - <<'EOF'
feat(tui): Screen::componentAt is public, and a mouse event over an overlay goes to the overlay

componentAt answers the top-most visible component at a cell -- overlays first,
the last shown on top, then the tree in reverse z-order -- or null where only
the root is, so a drag source can find its drop target. Dispatch uses the same
lookup, so a click on a popup or dialog no longer reaches what lies beneath it.
The tooltip is not a target, and dispatch still sees the root for an empty
cell, so the hover state behaves as before.
EOF
```

---

### Task 6: `Screen` captures the pointer for the component that took the press

**Files:**
- Modify: `src/core/tui/Screen.hpp` — `friend class Component;`, a public capture block, `_pointerCapture`,
  `dispatchPress`, `componentDetached` and `componentDestroyed` in the private helpers, the `dispatchEvent` doc
- Modify: `src/core/tui/Screen.cpp` — `~Screen`, `dispatchMouseEvent`, three new members
- Modify: `src/core/tui/Component.cpp` — `~Component`, `setScreen`
- Modify: `CHANGELOG.md`, `docs/modules/tui.md`
- Test: `src/core/tui/Screen_test.cpp` — append after Task 5's cases (same anonymous-namespace helpers)

**Interfaces:**
- Consumes: Task 5's `hitTest`, `componentAt`, `MouseRecorder`, `mouseAt`; `Component::setScreen` (private,
  `Screen` is its friend), `Screen::bubbleEvent` (`src/core/tui/Screen.hpp`).
- Produces: `[[nodiscard]] Component* core::tui::Screen::pointerCapture() const noexcept;`,
  `void core::tui::Screen::releasePointer() noexcept;`; private
  `[[nodiscard]] EventResult Screen::dispatchPress(Component* target, InputEvent const& event);`,
  `void Screen::componentDetached(Component const& component) noexcept;`,
  `void Screen::componentDestroyed(Component const& component) noexcept;`.

- [ ] **Step 1: Write the failing tests**

Add `#include <functional>` to `src/core/tui/Screen_test.cpp`'s standard includes. Inside the anonymous namespace
of Task 5 (after `mouseAt`), add:

```cpp
/// @brief Runs @c onPress when pressed, which may destroy this component, and handles the press.
struct PressCallback: Component
{
    std::function<void()> onPress; ///< Called on a press.

    void render(Canvas& /*canvas*/) override {}

    EventResult onEvent(InputEvent const& event) override
    {
        auto const* mouse = std::get_if<MouseEvent>(&event);
        if (mouse == nullptr || mouse->type != MouseEvent::Type::Press)
            return EventResult::Ignored;
        auto const callback = onPress; // the member dies with this component
        callback();
        return EventResult::Handled;
    }
};

/// @brief The layout of the capture cases: A at columns 2..11, B at columns 20..29, both on rows 1..3.
constexpr auto AreaA = Rect { .x = 2, .y = 1, .width = 10, .height = 3 };
constexpr auto AreaB = Rect { .x = 20, .y = 1, .width = 10, .height = 3 };
```

Then append after Task 5's test cases:

```cpp
TEST_CASE("Screen.pointerCapture_followsTheDragOutsideThePressedComponent")
{
    auto terminal = Terminal(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(terminal);
    auto source = MouseRecorder {};
    auto other = MouseRecorder {};
    screen.root().addChild(source, LayoutParams { .area = AreaA });
    screen.root().addChild(other, LayoutParams { .area = AreaB });
    screen.draw();

    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Press, 4, 2)); // cell (1, 3): inside A
    CHECK(screen.pointerCapture() == &source);
    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Move, 25, 2));    // over B
    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Release, 1, 10)); // left of and below A

    REQUIRE(source.received.size() == 3);
    CHECK(source.received[0].type == MouseEvent::Type::Press);
    CHECK(source.received[0].x == 2);
    CHECK(source.received[0].y == 1);
    CHECK(source.received[1].type == MouseEvent::Type::Move);
    CHECK(source.received[1].x == 23);
    CHECK(source.received[1].y == 1);
    CHECK(source.received[2].type == MouseEvent::Type::Release);
    CHECK(source.received[2].x == -1); // A-relative, so left of A is below 1
    CHECK(source.received[2].y == 9);
    CHECK(other.received.empty());
    CHECK(screen.pointerCapture() == nullptr);
}

TEST_CASE("Screen.pointerCapture_isTakenByTheAncestorThatHandledThePress")
{
    auto terminal = Terminal(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(terminal);
    auto panel = MouseRecorder {};
    auto leaf = MouseRecorder {};
    leaf.pressResult = EventResult::Ignored;
    screen.root().addChild(panel, LayoutParams { .area = { .x = 2, .y = 1, .width = 20, .height = 5 } });
    panel.addChild(leaf, LayoutParams { .area = { .x = 1, .y = 1, .width = 5, .height = 2 } });
    screen.draw();

    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Press, 5, 3)); // cell (2, 4): inside the leaf
    CHECK(screen.pointerCapture() == &panel);

    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Move, 40, 10)); // cell (9, 39)
    REQUIRE(panel.received.size() == 2);
    CHECK(panel.received[1].type == MouseEvent::Type::Move);
    CHECK(panel.received[1].x == 38); // panel-relative: 39 - 2 + 1
    CHECK(panel.received[1].y == 9);  // 9 - 1 + 1
    CHECK(leaf.received.size() == 1); // the press only
}

TEST_CASE("Screen.pointerCapture_isNotTakenByAPressNobodyHandled")
{
    auto terminal = Terminal(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(terminal);
    auto source = MouseRecorder {};
    auto other = MouseRecorder {};
    source.pressResult = EventResult::Ignored;
    screen.root().addChild(source, LayoutParams { .area = AreaA });
    screen.root().addChild(other, LayoutParams { .area = AreaB });
    screen.draw();

    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Press, 4, 2));
    CHECK(screen.pointerCapture() == nullptr);
    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Move, 25, 2));
    REQUIRE(other.received.size() == 1);
    CHECK(other.received[0].type == MouseEvent::Type::Move);
}

TEST_CASE("Screen.pointerCapture_endsWhenTheTargetIsDestroyed")
{
    auto terminal = Terminal(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(terminal);
    auto source = std::make_unique<MouseRecorder>();
    auto other = MouseRecorder {};
    screen.root().addChild(*source, LayoutParams { .area = AreaA });
    screen.root().addChild(other, LayoutParams { .area = AreaB });
    screen.draw();

    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Press, 4, 2));
    REQUIRE(screen.pointerCapture() == source.get());
    source.reset();
    CHECK(screen.pointerCapture() == nullptr);

    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Move, 25, 2)); // hit-tested again
    REQUIRE(other.received.size() == 1);
    CHECK(other.received[0].type == MouseEvent::Type::Move);
}

TEST_CASE("Screen.pointerCapture_endsWhenTheTargetOrItsAncestorLeavesTheTree")
{
    auto terminal = Terminal(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(terminal);
    auto panel = MouseRecorder {};
    auto source = MouseRecorder {};
    panel.pressResult = EventResult::Ignored;
    screen.root().addChild(panel, LayoutParams { .area = { .x = 0, .y = 0, .width = 30, .height = 10 } });
    panel.addChild(source, LayoutParams { .area = AreaA });
    screen.draw();

    SECTION("the target itself")
    {
        (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Press, 4, 2));
        REQUIRE(screen.pointerCapture() == &source);
        panel.removeChild(source);
        CHECK(screen.pointerCapture() == nullptr);
    }
    SECTION("an ancestor")
    {
        (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Press, 4, 2));
        REQUIRE(screen.pointerCapture() == &source);
        screen.root().removeChild(panel);
        CHECK(screen.pointerCapture() == nullptr);
    }
}

TEST_CASE("Screen.pointerCapture_endsWhenTheOverlayHoldingItIsHidden")
{
    auto terminal = Terminal(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(terminal);
    auto overlay = MouseRecorder {};
    screen.showOverlay(overlay, Point { .x = 5, .y = 2 });
    screen.draw();

    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Press, 7, 4));
    REQUIRE(screen.pointerCapture() == &overlay);
    screen.hideOverlay(overlay);
    CHECK(screen.pointerCapture() == nullptr);
}

TEST_CASE("Screen.releasePointer_endsTheCapture")
{
    auto terminal = Terminal(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(terminal);
    auto source = MouseRecorder {};
    auto other = MouseRecorder {};
    screen.root().addChild(source, LayoutParams { .area = AreaA });
    screen.root().addChild(other, LayoutParams { .area = AreaB });
    screen.draw();

    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Press, 4, 2));
    screen.releasePointer();
    CHECK(screen.pointerCapture() == nullptr);
    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Move, 25, 2));
    CHECK(other.received.size() == 1);
    CHECK(source.received.size() == 1);
}

TEST_CASE("Screen.pointerCapture_neverTakesScrollEvents")
{
    auto terminal = Terminal(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(terminal);
    auto source = MouseRecorder {};
    auto other = MouseRecorder {};
    screen.root().addChild(source, LayoutParams { .area = AreaA });
    screen.root().addChild(other, LayoutParams { .area = AreaB });
    screen.draw();

    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Press, 4, 2));
    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::ScrollDown, 25, 2)); // over B
    REQUIRE(other.received.size() == 1);
    CHECK(other.received[0].type == MouseEvent::Type::ScrollDown);
    CHECK(screen.pointerCapture() == &source); // the drag goes on

    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Move, 25, 2));
    CHECK(source.received.size() == 2);
    CHECK(other.received.size() == 1);
}

TEST_CASE("Screen.pointerCapture_movesToTheComponentOfTheNextPress")
{
    // A release the terminal never delivered -- the window lost focus mid-drag -- holds the pointer
    // only until the next press.
    auto terminal = Terminal(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(terminal);
    auto source = MouseRecorder {};
    auto other = MouseRecorder {};
    screen.root().addChild(source, LayoutParams { .area = AreaA });
    screen.root().addChild(other, LayoutParams { .area = AreaB });
    screen.draw();

    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Press, 4, 2));
    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Press, 25, 2)); // no release in between
    CHECK(screen.pointerCapture() == &other);
    (void) screen.dispatchEvent(mouseAt(MouseEvent::Type::Move, 5, 2)); // over A
    CHECK(source.received.size() == 1);
    REQUIRE(other.received.size() == 2);
    CHECK(other.received[1].type == MouseEvent::Type::Move);
}

TEST_CASE("Screen.pointerCapture_isNotTakenByAComponentItsPressDestroyed")
{
    auto terminal = Terminal(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(terminal);
    auto doomed = std::make_unique<PressCallback>();
    doomed->onPress = [&doomed] { doomed.reset(); };
    screen.root().addChild(*doomed, LayoutParams { .area = AreaA });
    screen.draw();

    CHECK(screen.dispatchEvent(mouseAt(MouseEvent::Type::Press, 4, 2)) == EventResult::Handled);
    CHECK(doomed == nullptr);
    CHECK(screen.pointerCapture() == nullptr);
    // Delivered to the capture, this would reach freed memory: under ASan, a heap-use-after-free.
    CHECK(screen.dispatchEvent(mouseAt(MouseEvent::Type::Move, 4, 2)) == EventResult::Ignored);
}

TEST_CASE("Screen.destructor_detachesOverlaysStillShown")
{
    auto terminal = Terminal(std::make_unique<MockTerminalOutput>(80, 24));
    auto overlay = MouseRecorder {};
    {
        auto screen = Screen(terminal);
        screen.showOverlay(overlay, Point { .x = 5, .y = 5 });
        screen.draw();
        CHECK(overlay.screen() == &screen);
    }
    // The overlay outlives the screen; its own teardown must not reach the destroyed one.
    CHECK(overlay.screen() == nullptr);
}

TEST_CASE("Screen.overlayDestroyedWhileShown_leavesTheOverlayList")
{
    auto terminal = Terminal(std::make_unique<MockTerminalOutput>(80, 24));
    auto screen = Screen(terminal);
    auto overlay = std::make_unique<MouseRecorder>();
    screen.showOverlay(*overlay, Point { .x = 5, .y = 2 });
    screen.draw();
    REQUIRE(screen.componentAt(3, 6) == overlay.get());

    overlay.reset();
    // Kept in the list, the destroyed overlay would be drawn and hit-tested here: under ASan, a
    // heap-use-after-free.
    screen.draw();
    CHECK(screen.componentAt(3, 6) == nullptr);
}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake --build --preset clang-debug --target core-cpp-tui-test`
Expected: compile error, `no member named 'pointerCapture' in 'core::tui::Screen'` (and `'releasePointer'`).

- [ ] **Step 3: Implement**

`src/core/tui/Screen.hpp`:

After Task 5's `componentAt` declaration (still in the public section), add:

```cpp

    // --- Pointer Capture ---

    /// Returns the component the pointer is captured by, or nullptr.
    ///
    /// A press makes the component that handled it (after bubbling) the capture target. Until the
    /// capture ends, every move and the next release go to it directly -- no hit test, no bubbling --
    /// in its own coordinates, which are below 1 or beyond its size once the pointer has left it. The
    /// release ends the capture, as do the next press, releasePointer(), and the target leaving this
    /// screen: destroyed, removed from the tree (an ancestor's removal included, and a move to another
    /// parent, which removes it on the way) or hidden as an overlay. Scroll events are never captured,
    /// and a captured move does not update the hover state.
    /// @return The capture target, or nullptr when the pointer is not captured.
    [[nodiscard]] Component* pointerCapture() const noexcept { return _pointerCapture; }

    /// Ends the pointer capture, if any: the next move or release is hit-tested again. A press handler
    /// that calls it declines the capture its press would take.
    void releasePointer() noexcept { _pointerCapture = nullptr; }
```

Change the `dispatchEvent` doc's mouse line to:

```cpp
    /// For mouse events: a move or release goes to the pointer capture target if there is one (see
    /// pointerCapture()); anything else hit tests to find the target (overlays first, see
    /// componentAt()) and bubbles up, and a handled press makes its handler the capture target.
```

At the top of the `private:` section, before `Terminal& _terminal;`, add `friend class Component;`, and between
`RenderMode _renderMode = RenderMode::Diff;` and `std::unique_ptr<RootComponent> _root;` add:

```cpp

    /// The component the pointer is captured by, or nullptr.
    Component* _pointerCapture = nullptr;

```

In the `// Event dispatch helpers` block, after `dispatchMouseEvent`, add:

```cpp

    /// Delivers a press from @p target up through its ancestors, making each the capture target while
    /// it decides, so that the one that handles it keeps the capture.
    /// @param target The component under the pointer.
    /// @param event The press, in @p target's coordinates.
    /// @return The first result that is not Ignored, or Ignored.
    [[nodiscard]] EventResult dispatchPress(Component* target, InputEvent const& event);

    /// Called by a component that leaves this screen -- removed from the tree, directly or with an
    /// ancestor, or hidden as an overlay -- so that no capture outlives its place on the screen.
    /// @param component The component leaving.
    void componentDetached(Component const& component) noexcept;

    /// Called by a component this screen still knows when it is destroyed: what componentDetached()
    /// does, and an overlay that was never hidden leaves the overlay list, so nothing draws, hit-tests
    /// or detaches it after it is gone.
    /// @param component The component being destroyed.
    void componentDestroyed(Component const& component) noexcept;
```

`src/core/tui/Screen.cpp`: add `#include <vector>` to its standard includes (for `std::erase_if`).

At the top of `Screen::~Screen()`, before `if (_enteredAlternateScreen)`, add:

```cpp
    // Overlays and the tree's components belong to the caller and may outlive this screen. Each
    // forgets it here, while every member is alive, so that no teardown -- theirs later, or the root's
    // among this screen's members -- reaches a destroyed screen.
    for (auto const& entry: _overlays)
        entry.component->setScreen(nullptr);
    _overlays.clear();
    _root->setScreen(nullptr);

```

Replace `Screen::dispatchMouseEvent` entirely with:

```cpp
EventResult Screen::dispatchMouseEvent(MouseEvent const& mouse)
{
    // Note: We intentionally ignore mouse.uiHandled to ensure hover detection
    // works even when the terminal (e.g., Contour with passive tracking) has
    // already processed the event for its own UI purposes.

    // A press starts a new gesture, so it ends whatever capture is left: a release the terminal never
    // delivered (the window lost focus mid-drag) holds the pointer only until the next press.
    if (mouse.type == MouseEvent::Type::Press)
        _pointerCapture = nullptr;

    // Moves and the release go to the capture target; presses and scroll events are hit-tested.
    auto const isCaptured = _pointerCapture != nullptr
                            && (mouse.type == MouseEvent::Type::Move || mouse.type == MouseEvent::Type::Release);

    int mouseRow = mouse.y - 1; // Convert to 0-based
    int const mouseCol = mouse.x - 1;

    // Translate mouse coordinates for inline mode
    // Use _mainContentHeight (content before overlays) to avoid tooltip affecting coordinates
    int const contentHeight = (_mainContentHeight > 0) ? _mainContentHeight : _previousContentHeight;
    if (_config.viewport == Viewport::Inline && contentHeight > 0)
    {
        // _inlineContentStartRow is calculated in flushInline() based on terminal size and peak content
        // height
        if (_inlineContentStartRow < 0)
        {
            // Not yet rendered - skip mouse handling
            if (mouse.type == MouseEvent::Type::Move)
                _hoverState.onMouseMove(mouseCol + 1, 0, nullptr);
            return EventResult::Ignored;
        }

        mouseRow = mouse.y - 1 - _inlineContentStartRow;

        // Outside the inline content only a captured event is delivered: the drag it belongs to may
        // leave the content and come back.
        if (!isCaptured && (mouseRow < 0 || mouseRow >= contentHeight))
        {
            // Still update hover state (to trigger leave if needed)
            if (mouse.type == MouseEvent::Type::Move)
                _hoverState.onMouseMove(mouseCol + 1, mouseRow + 1, nullptr);
            return EventResult::Ignored;
        }
    }

    if (isCaptured)
    {
        // No hit test and no bubbling: the capture target sees its whole gesture, in its own
        // coordinates, wherever the pointer is. A release ends the capture before it is delivered, so
        // its handler may start the next gesture or destroy the component.
        auto* const captureTarget = _pointerCapture;
        if (mouse.type == MouseEvent::Type::Release)
            _pointerCapture = nullptr;
        auto relative = mouse;
        auto const captureBounds = captureTarget->screenBounds();
        relative.x = mouseCol - captureBounds.x + 1;
        relative.y = mouseRow - captureBounds.y + 1;
        return captureTarget->onEvent(relative);
    }

    // Hit test to find target component
    Component* target = hitTest(mouseRow, mouseCol);

    // Update hover state for mouse move events
    // Use viewport-relative 1-based coordinates for consistency with component bounds
    if (mouse.type == MouseEvent::Type::Move)
    {
        _hoverState.onMouseMove(mouseCol + 1, mouseRow + 1, target);
    }

    if (!target)
        return EventResult::Ignored;

    // Create adjusted event with component-relative coordinates
    MouseEvent adjusted = mouse;
    Rect const bounds = target->screenBounds();
    adjusted.x = mouseCol - bounds.x + 1; // Back to 1-based for component
    adjusted.y = mouseRow - bounds.y + 1;

    if (mouse.type == MouseEvent::Type::Press)
        return dispatchPress(target, adjusted);
    return bubbleEvent(target, adjusted);
}

EventResult Screen::dispatchPress(Component* target, InputEvent const& event)
{
    // Each component is the capture target while it decides, and a component's teardown clears the
    // capture (componentDetached()): one destroyed or detached while handling its own press is never
    // left holding the pointer, and one that calls releasePointer() declines it.
    while (target)
    {
        _pointerCapture = target;
        EventResult const result = target->onEvent(event);
        if (result != EventResult::Ignored)
            return result;
        // Ignored, and no longer the capture target: it left this screen while handling the press, so
        // it may be gone and its parent cannot be read, or it released the pointer. The press ends here.
        if (_pointerCapture != target)
            return EventResult::Ignored;
        _pointerCapture = nullptr;
        target = target->parent();
    }
    return EventResult::Ignored;
}

void Screen::componentDetached(Component const& component) noexcept
{
    if (_pointerCapture == &component)
        _pointerCapture = nullptr;
}

void Screen::componentDestroyed(Component const& component) noexcept
{
    componentDetached(component);
    std::erase_if(_overlays, [&component](OverlayEntry const& entry) { return entry.component == &component; });
}
```

`src/core/tui/Component.cpp`: replace `Component::~Component()` and `Component::setScreen` with:

```cpp
Component::~Component()
{
    // A component the screen still knows when it dies -- an overlay never hidden, say -- tells the
    // screen first, so that neither the pointer capture nor the overlay list outlives it.
    if (_screen)
        _screen->componentDestroyed(*this);

    // Remove from parent if attached
    if (_parent)
        _parent->removeChild(*this);

    // Clear children (sets their parent to nullptr)
    clearChildren();
}
```

```cpp
void Component::setScreen(Screen* screen)
{
    // Leaving a screen -- removed from its tree, directly or with an ancestor, or hidden as an overlay --
    // ends that screen's pointer capture on this component.
    if (_screen && _screen != screen)
        _screen->componentDetached(*this);

    _screen = screen;

    // Propagate to children
    for (Component* child: _children)
        child->setScreen(screen);
}
```

`CHANGELOG.md`, append to `### Added`:

```markdown
- **Pointer capture in `Screen`.** `Screen` hit-tested every mouse event, so a drag that left the component
  it started on lost its moves and its release. A press now makes the component that handled it the
  capture target: every move and the next release go to it directly, in its own coordinates, wherever the
  pointer is. The release ends the capture, as do the next press, `Screen::releasePointer()` and the
  target leaving the screen (destroyed, removed from the tree or hidden as an overlay); scroll events are
  never captured. `Screen::pointerCapture()` answers the target.
```

and add after the `### Changed` section:

```markdown
### Fixed

- **An overlay and its `Screen` may be destroyed in either order.** The screen's destructor detaches every
  overlay still shown and its tree; before, an overlay that outlived the screen reached the destroyed one
  from its own destructor, through `invalidate()`. A component destroyed while shown as an overlay leaves
  the overlay list; before, the next `draw()` rendered the destroyed component.
```

`docs/modules/tui.md`, after Task 4's `- **Mouse.** …` bullet, insert:

```markdown
- **Pointer.** `Screen` routes a mouse event to the top-most visible component under it -- overlays first,
  the one shown last on top, then the tree -- and `componentAt(row, col)` answers the same question for a
  caller, such as a drag source looking for its drop target. A press makes the component that handled it
  the capture target: until the release, every move and that release go to it, in its own coordinates and
  wherever the pointer is, so a component sees its whole drag. The release ends the capture, as do the
  next press, `releasePointer()`, and the component leaving the screen -- destroyed, removed from the tree
  or hidden as an overlay. Scroll events are never captured.
```

- [ ] **Step 4: Run the tests to verify they pass**

Run:

```bash
cmake --build --preset clang-debug --target core-cpp-tui-test \
    && ./out/build/clang-debug/src/core/tui/core-cpp-tui-test "Screen.*,HoverState.*,Component.*"
```

Expected: PASS, every case.

Mutation check: in `Component::setScreen`, delete the two lines that call `componentDetached`; rebuild and rerun.
Expected: FAIL in `pointerCapture_endsWhenTheTargetOrItsAncestorLeavesTheTree` (both sections) and
`pointerCapture_endsWhenTheOverlayHoldingItIsHidden`. Restore. Then in `dispatchPress`, move
`_pointerCapture = target;` to after the `onEvent` call (set only once a component has handled the press); expected:
FAIL in `pointerCapture_isNotTakenByAComponentItsPressDestroyed` (`pointerCapture()` is the freed component). Restore.

- [ ] **Step 5: Format and commit**

```bash
python3 scripts/clang-format.py src/core/tui/Screen.hpp src/core/tui/Screen.cpp src/core/tui/Component.cpp \
    src/core/tui/Screen_test.cpp
cmake --build --preset clang-debug && ctest --preset clang-debug
git add src/core/tui/Screen.hpp src/core/tui/Screen.cpp src/core/tui/Component.cpp src/core/tui/Screen_test.cpp \
    CHANGELOG.md docs/modules/tui.md
git commit -s -F - <<'EOF'
feat(tui): Screen captures the pointer for the component that took the press

Screen hit-tested every mouse event, so a drag that left its component lost
its moves and its release. A press now makes the component that handled it the
capture target; moves and the next release go to it in its own coordinates
until the release, the next press, releasePointer(), or the target leaving the
screen, which its own teardown reports. Scroll is never captured.

A component destroyed while handling its press is never left holding the
pointer: each candidate holds the capture while it decides. A destroyed
overlay leaves the overlay list, and ~Screen detaches the overlays it still
shows and its tree, so neither the new teardown hook nor the existing
invalidate() reaches a destroyed screen or component.
EOF
```

---

### Task 7: core-cpp gates

**Files:** none new; fixes land in the files they concern.

- [ ] **Step 1: Format, the whole tree**

```bash
python3 scripts/clang-format.py --all --check
```

Expected: no output, exit 0.

- [ ] **Step 2: clang-tidy, the pinned one**

```bash
cmake --preset clang-tidy && cmake --build --preset clang-tidy
```

Expected: builds with no finding (`WarningsAsErrors: '*'`). Fix every finding; never `NOLINT`.

- [ ] **Step 3: Both macOS configurations, every test**

```bash
cmake --build --preset clang-debug && ctest --preset clang-debug
cmake --preset appleclang-debug && cmake --build --preset appleclang-debug && ctest --preset appleclang-debug
```

Expected: `100% tests passed` in each (skips reported as skipped, not as passed). Linux `gcc-release` and the
Windows presets run in CI (Task 9, Step 3).

- [ ] **Step 4: Sanitizers**

```bash
cmake --preset clang-asan-ubsan && cmake --build --preset clang-asan-ubsan --target core-cpp-tui-test core-cpp-tui_output-test
./out/build/clang-asan-ubsan/src/core/tui/core-cpp-tui-test "Screen.*,TerminalInput.*,VtParser.*"
./out/build/clang-asan-ubsan/src/core/tui/core-cpp-tui_output-test "TerminalProtocols.*"
nm -C out/build/clang-asan-ubsan/src/core/tui/core-cpp-tui-test | grep -c __asan_report
```

Expected: every case passes with no sanitizer report, and the `nm` count is greater than 0 (the binary is
instrumented, so a clean run means something). `Screen.pointerCapture_isNotTakenByAComponentItsPressDestroyed` and
`Screen.destructor_detachesOverlaysStillShown` and `Screen.overlayDestroyedWhileShown_leavesTheOverlayList` rely on
ASan as their observer. If the preset does not configure on
macOS, say so; CI's `clang-asan-ubsan` and `clang-tsan` legs are then the measurement, and the PR body says that.

- [ ] **Step 5: Hygiene and the documentation site**

```bash
ctest --preset clang-debug -L hygiene
python3 -m venv out/mkdocs && out/mkdocs/bin/pip install -q -r docs/requirements.txt
out/mkdocs/bin/mkdocs build --strict
```

Expected: `100% tests passed` (provenance, layering, platform sources, CMake hygiene); `mkdocs` exits 0 with no
warning (`CHANGELOG.md` is rendered into the site by a snippet).

- [ ] **Step 6: Commit any fixes**

One commit per kind of fix, with the conventional subject that describes it (e.g.
`style(tui): clang-tidy's findings in the mouse tracking change`), `git commit -s`. Skip if nothing needed fixing,
and say so in the hand-off.

---

### Task 8: Verify morph against the feature branch, locally (nothing committed)

Catches a core-cpp change that breaks morph before anything is pushed. morph builds core-cpp with
`CORE_CPP_WITH_TUI OFF`, so this exercises the 0.5.1 and 0.6.0 changes morph now takes, not the new `core::tui`
code; Part 3 builds that.

**Files:** none (a throwaway build directory in morph).

- [ ] **Step 1: Configure morph with core-cpp from the local checkout**

```bash
cd /Users/christianparpart/projects/morph
git status --porcelain   # expected: empty
cmake -S . -B build/corecpp-local -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMORPH_BUILD_NET=ON \
      -DCPM_core-cpp_SOURCE=/Users/christianparpart/projects/core-cpp
```

Expected: the configure output contains `CPM: Adding package core-cpp@` followed by
`/Users/christianparpart/projects/core-cpp` as the source (CPM's override ignores `VERSION`, so the
still-`v0.5.0` lines in morph's `CMakeLists.txt` do not matter here) and `morph: warnings: ... strict=ON`.
If the output names `.cache/cpm/core-cpp/…` instead, the override did not apply — stop.

- [ ] **Step 2: Build and run the suite**

```bash
cmake --build build/corecpp-local
ctest --test-dir build/corecpp-local --output-on-failure
```

Expected: everything passes. A failure here is a core-cpp 0.5.1/0.6.0 behaviour change that Task 10 would have to
absorb: diagnose it now, record the fix Task 10 must make (and keep it for Task 10's commit — do not commit in morph
here).

- [ ] **Step 3: Remove the override**

```bash
rm -rf build/corecpp-local
git status --porcelain   # expected: empty — nothing of the override reached the tree
```

---

### Task 9: Release core-cpp 0.7.0 and file the three core-cpp issues — every step asks the user first

Each step below is outward-facing. **Before each, ask the user with the exact command and wait for a yes**; a
"no" stops the part there and is reported.

- [ ] **Step 1: Push the branch — ask the user first**

```bash
cd /Users/christianparpart/projects/core-cpp
git log --oneline master..feature/mouse-tracking   # show the user the commits being pushed
git push -u origin feature/mouse-tracking
```

- [ ] **Step 2: Open the pull request — ask the user first**

Write the body to the session scratchpad as `core-cpp-pr-body.md`, filling the two bracketed places with real output:

```markdown
## What

core::tui asks a standard terminal for mouse reports, and gives a drag to the component that started it:

- `MouseTracking { Off, Buttons, Drag, AnyMotion }` with `TerminalInput::setMouseTracking()` and
  `Terminal::setMouseTracking()`: DEC 1000/1002/1003 with SGR 1006, default `Off`;
  `protocols::appendMouseTrackingChange()` writes each transition. The 2029 hover path raises the mode to
  `AnyMotion`.
- Pointer capture in `Screen`: a press makes its handler the capture target; moves and the release go to it in its
  own coordinates until the release, the next press, `releasePointer()`, or the target leaving the screen. Scroll
  is never captured.
- `Screen::componentAt()` is public, and hit testing, dispatch included, looks at overlays before the tree (the
  tooltip excepted).
- `~Screen` detaches the overlays it still shows.

## Why

Only Contour's passive mode 2029 was enabled, so xterm, kitty, WezTerm, iTerm2 and Windows Terminal reported no
mouse at all; and `Screen` hit-tested every event, so a drag lost its moves and its release once it left its
component. morph's terminal frontend needs both for drag-and-drop.

## Consumer impact

- contour (vendored): none; a grep of `contour/src` finds no `core/tui` include.
- endo: none until it moves onto core-cpp's `core::tui` (it still builds its own copy).
- fastcached: none; no `core::tui` use found.
- tuidu: a mouse event over its help or delete-dialog overlay now goes to the overlay, not to the tree view under
  it. On terminals other than Contour it receives mouse input only after `terminal.setMouseTracking(...)`.
- Lightweight `dbtool`: none; it links `core::tui_output`, where this adds names only.
- morph (including its WebAssembly build): none at the pin; its terminal frontend uses all of this. The
  WebAssembly build does not build `core::tui`.

## Tests

- `VtParser.SGRMouse.*`, `TerminalProtocols.mouse_tracking_*`, `TerminalInput.*` (the POSIX bytes are read back
  from a pipe standing in for standard output), `Screen.componentAt_*`, `Screen.pointerCapture_*`,
  `Screen.destructor_detachesOverlaysStillShown`, `Screen.overlayDestroyedWhileShown_leavesTheOverlayList`; each
  was seen to fail with its mutation.
- Locally: `clang-debug`, `appleclang-debug`, `clang-tidy`, [`clang-asan-ubsan`, or "not run locally: …"],
  `mkdocs build --strict`, `scripts/clang-format.py --all --check`. Local `ctest` summary: [paste].
- Not measured: the Windows arm's protocol bytes (`initialize()` needs a real console); it makes the same calls as
  the POSIX arm and is compiled and run by the Windows legs here.

- [x] CHANGELOG entry under `[Unreleased]`
- [x] Documentation updated (`docs/modules/tui.md`)
```

```bash
gh pr create --repo contour-terminal/core-cpp --base master --head feature/mouse-tracking \
    --title "tui: standard mouse tracking, pointer capture and a public Screen::componentAt" \
    --body-file <scratchpad>/core-cpp-pr-body.md --label type/feature --label module/tui
```

- [ ] **Step 3: CI to green**

```bash
gh pr checks <number> --repo contour-terminal/core-cpp --watch
```

Expected: every check passes, `ci-ok` included. A failure is fixed on the branch with a conventional commit; pushing
the fix is Step 1 again — ask the user first.

- [ ] **Step 4: Merge — ask the user first**

```bash
gh pr merge <number> --repo contour-terminal/core-cpp --merge
```

(core-cpp merges pull requests with a merge commit: `Merge pull request #59 from …`.)

- [ ] **Step 5: Cut `v0.7.0` — ask the user first**

core-cpp's release is its own commit on `master`, after the merge (`Release 0.6.0` is `afe8a18`). Run the
`contour-workflows:draft-release` skill in `/Users/christianparpart/projects/core-cpp` for version `0.7.0`. What it
must produce, and what to do by hand if it does not:

```bash
git switch master && git pull --ff-only origin master
# CHANGELOG.md: "## [Unreleased]" -> "## [0.7.0] - <today, YYYY-MM-DD>"
# CMakeLists.txt: project(core-cpp VERSION 0.6.0 ...) -> VERSION 0.7.0
# README.md: "GIT_TAG v0.6.0" -> v0.7.0 and "-DREF=v0.6.0" -> v0.7.0
# docs/getting-started/cpm.md and docs/getting-started/install.md: "GIT_TAG v0.6.0" -> v0.7.0
cmake -DTAG=v0.7.0 -DROOT=. -P tests/cmake/check-release.cmake   # expected: exit 0
git commit -s -am "Release 0.7.0"
git tag -a v0.7.0 -m "Release 0.7.0"
git push origin master v0.7.0
```

The tag push starts `.github/workflows/release.yml`, which drafts the GitHub release with the vendor archive and
`SHA256SUMS`.

- [ ] **Step 6: Publish the release — ask the user first**

Run the `contour-workflows:publish-release` skill: it refuses until CI is green on the tag and both assets are
attached, then publishes, marks it latest, and opens the next `[Unreleased]`
(`docs(changelog): open [Unreleased] after 0.7.0`).

```bash
git ls-remote --tags https://github.com/contour-terminal/core-cpp.git v0.7.0   # expected: one line, the tag
```

- [ ] **Step 7: File the Shift+Tab issue — ask the user first**

Re-run the evidence on the tags that exist now and paste the real output into the body:

```bash
cd /Users/christianparpart/projects/core-cpp
bash -c 'for t in v0.5.0 v0.5.1 v0.6.0 v0.7.0; do printf "%s: " "$t"; git grep -c "'"'"'Z'"'"'" "$t" -- src/core/tui/VtParser.cpp || echo "0 matches"; done'
```

Expected, as measured while writing this plan for the first
three: `0 matches` on each. Write `<scratchpad>/core-cpp-shift-tab-issue.md`:

````markdown
## What is missing

`VtParser` does not decode `CSI Z` (`ESC [ Z`), the sequence xterm and most terminals send for Shift+Tab
when the Kitty keyboard protocol is not in effect. `mapCsiKey()` in `src/core/tui/VtParser.cpp` maps the
final bytes `A B C D H F ~` and has no `'Z'` case, so the sequence produces no event at all.

Shift+Tab therefore reaches an application only as Kitty CSI-u (`CSI 9 ; 2 u`, decoded by the CSI-u path's
`case 9:`) and — inferred from reading, not tested — as a win32-input-mode record on Windows Terminal
(`vk::Tab` with Shift). On a terminal that does not speak the Kitty keyboard protocol, or ignores the
`CSI > 13 u` push, the key is lost: a backward focus move bound to Shift+Tab never fires.

## Expected

`VtParser{}.feed("\033[Z")` yields exactly one `KeyEvent { .key = KeyCode::Tab, .modifiers = Modifier::Shift }`,
the same event the Kitty form produces.

## Verification status

Inferred from reading the code; not reproduced in a terminal. The absence of a `'Z'` case, by grep on each tag:

```text
<paste the loop's output>
```

Not verified: which terminals still send `CSI Z` with the Kitty protocol pushed, and what Windows Terminal sends
with win32-input-mode on.

## What would close it

A `VtParser_test.cpp` case feeding `"\033[Z"` and expecting `KeyEvent{Tab, Shift}`, passing. Reopen if a
terminal is found that sends Shift+Tab some other legacy way that still produces no event.

Found while planning morph's terminal frontend, which binds Shift+Tab to moving focus backwards.
````

```bash
gh issue create --repo contour-terminal/core-cpp --title "tui: decode legacy Shift+Tab (CSI Z)" \
    --body-file <scratchpad>/core-cpp-shift-tab-issue.md --label bug --label module/tui
```

Record the issue URL for the hand-off.

- [ ] **Step 8: File the dangling focus-pointer issue — ask the user first**

Re-check on the tag that exists now (paste the output into the body):

```bash
cd /Users/christianparpart/projects/core-cpp
git grep -n "_focusedComponents" v0.7.0 -- src/core/tui/Screen.hpp src/core/tui/Screen.cpp
git show v0.7.0:src/core/tui/Component.cpp | sed -n '/Component::~Component/,/^}/p'
```

Write `<scratchpad>/core-cpp-focus-issue.md`:

````markdown
## What is wrong

`Screen` remembers the focused component of each focus group as a raw pointer
(`std::unordered_map<FocusGroupId, Component*> _focusedComponents`, `src/core/tui/Screen.hpp`).
`Component::~Component` detaches from its parent and clears its children, but nothing removes the component from
that map. A focused component that is destroyed while its `Screen` lives on leaves a dangling pointer, which the next
`dispatchEvent` (key events go to the focused component), `focusNext`/`focusPrev` or `setFocus` (it calls
`onBlur` on the old focus) dereferences.

## Expected

Destroying the focused component clears the focus of its group (the component's teardown notifies its `Screen`,
the way the 0.7.0 pointer capture is cleared), so the next key event finds no focused component instead of freed
memory.

## Verification status

Inferred from reading the code; not reproduced (an ASan run of a test that focuses a component, destroys it and then
dispatches a key would show it). Code as of v0.7.0:

```text
<paste the git grep and the destructor>
```

Downstream, morph's terminal frontend works around it by clearing focus in each widget's destructor.

## What would close it

A `Screen_test.cpp` case — focus a component, destroy it, dispatch a key event — passing under ASan with no
use-after-free, and `focusedComponent()` returning null afterwards.
````

```bash
gh issue create --repo contour-terminal/core-cpp --title "tui: Screen keeps a dangling pointer to a destroyed focused component" \
    --body-file <scratchpad>/core-cpp-focus-issue.md --label bug --label module/tui
```

- [ ] **Step 9: File the dangling hover-target issue — ask the user first**

```bash
cd /Users/christianparpart/projects/core-cpp
git show v0.7.0:src/core/tui/HoverState.hpp | grep -n "Component\*"
git grep -n "_hoverState" v0.7.0 -- src/core/tui/Screen.cpp | head
```

Write `<scratchpad>/core-cpp-hover-issue.md`:

````markdown
## What is wrong

`HoverState` keeps the component under the pointer as a raw pointer (`Component* target`,
`src/core/tui/HoverState.hpp`), set from `Screen::dispatchMouseEvent`'s hit test. Nothing clears it when that
component is destroyed, so a hover timeout (`Screen::tickHover`) or the next mouse move that compares or leaves the
old target uses freed memory — for instance when a view replaces the component under a resting pointer.

## Expected

Destroying a component that is the hover target clears it (the same teardown notification that clears the pointer
capture and, with the companion issue, the focus).

## Verification status

Inferred from reading the code; not reproduced. Code as of v0.7.0:

```text
<paste the output>
```

morph's terminal frontend does not work around this one.

## What would close it

A `Screen_test.cpp` case — move the pointer over a component, destroy it, then `tickHover()` and move the pointer
again — passing under ASan.
````

```bash
gh issue create --repo contour-terminal/core-cpp --title "tui: HoverState keeps a dangling pointer to a destroyed hover target" \
    --body-file <scratchpad>/core-cpp-hover-issue.md --label bug --label module/tui
```

Record both URLs for the hand-off.

---

### Task 10: morph builds against core-cpp 0.7

**Precondition:** `v0.7.0` exists on GitHub — Task 9 Step 6's `git ls-remote` printed it. CPM fetches the tag; this
task cannot start before it exists.

**Files:**
- Modify: `CMakeLists.txt` — `set(MORPH_CORE_CPP_VERSION 0.5)` (line 293) and the `CPMAddPackage(NAME core-cpp …)`
  `GIT_TAG`/`VERSION` lines (312–313)
- Modify: `README.md` — line 424 (`v0.5.0`) and line 470 (`find_dependency(core-cpp 0.5)`)
- Modify: `CHANGELOG.md` — `## [Unreleased]` → `### Changed`, first bullet
- Test: morph's whole suite on `build/all`

**Interfaces:**
- Consumes: core-cpp `v0.7.0` (Tasks 2–9).
- Produces: `MORPH_CORE_CPP_VERSION 0.7`; `find_dependency(core-cpp 0.7 CONFIG)` in the installed `morphConfig.cmake`
  (through `cmake/morphConfig.cmake.in:36`, unchanged).

- [ ] **Step 1: Read what changed between the pin and the new tag**

```bash
cd /Users/christianparpart/projects/core-cpp
git show v0.7.0:CHANGELOG.md | sed -n '/^## \[0.7.0\]/,/^## \[0.5.0\]/p'
```

That prints the 0.7.0, 0.6.0 and 0.5.1 sections — everything after morph's `v0.5.0`. The ones that touch code
morph calls, and what morph does with each (measured by the grep in Step 2 while writing this plan):

| core-cpp change | morph's use | Fix |
|---|---|---|
| 0.6.0 Breaking: `serve()` takes its loop | none (`morph::net::SocketServer` runs its own accept loop) | none |
| 0.6.0 Breaking: `IListener::close()` is the base's, implementations override `doClose()` | morph implements no `IListener`; it calls `accept()` (`include/morph/net/socket_server.hpp:266`) and owns one (`:401`) | none |
| 0.6.0 Breaking: exhaustion is `NetErrorCode::ResourceExhausted` | `acceptFlow` stops on `Cancelled` and backs off 50 ms on every other error (`socket_server.hpp:274`) | none: exhaustion still backs off |
| 0.6.0 Fixed: a refused registration is an error from `accept()`, not a thrown `FdRegistrationFailed` | the same `acceptFlow` | none: it now backs off where it used to see an exception |
| 0.5.1 Fixed: accept's errno classification (`EPERM`, `EINVAL` → `BadHandle`, …) | the same `acceptFlow` | none: every non-`Cancelled` code backs off as before |
| 0.7.0: `MouseTracking`, pointer capture, public `componentAt`, overlay-first hit testing | none: morph builds core-cpp with `CORE_CPP_WITH_TUI OFF` | none (Part 3 builds `core::tui`) |

If the printed changelog has a Breaking or behaviour entry not in this table (core-cpp moved on after this plan),
grep morph for the API it names and add the fix to this task's commit.

- [ ] **Step 2: Confirm morph's use is what the table says**

```bash
cd /Users/christianparpart/projects/morph
git grep -n -E "IListener|core::net::serve|doClose|NetErrorCode::|ResourceExhausted|SystemError|BadHandle|FdRegistrationFailed|core/tui/" -- include src tests examples cmake
```

Expected, exactly: `include/morph/net/detail/ws_connection.hpp:67` (a comment naming `NetErrorCode::Cancelled`),
`include/morph/net/socket_server.hpp:9`, `:266`, `:274`, `:401`. Anything else is a use the table did not cover:
read it against the changelog before going on.

- [ ] **Step 3: Raise the pin**

`CMakeLists.txt`: `set(MORPH_CORE_CPP_VERSION 0.5)` → `set(MORPH_CORE_CPP_VERSION 0.7)`; in
`CPMAddPackage(NAME core-cpp …)`, `GIT_TAG v0.5.0` → `GIT_TAG v0.7.0` and `VERSION 0.5.0` → `VERSION 0.7.0`.

`README.md` line 424: `v0.5.0, the` → `v0.7.0, the` (in "[core-cpp](…) v0.5.0, the"); line 470:
``find_dependency(core-cpp 0.5)`` → ``find_dependency(core-cpp 0.7)``.

`CHANGELOG.md`: under `## [Unreleased]` → `### Changed`, insert as the first bullet (the existing
"morph depends on core-cpp v0.5.0" entry stays as written — CONTRIBUTING: an entry stops moving):

```markdown
- **morph builds against core-cpp 0.7.** `MORPH_CORE_CPP_VERSION` is 0.7 and CPM fetches `v0.7.0`, so a
  configure no longer accepts an installed core-cpp 0.5, and an installed morph finds its core-cpp through
  `find_dependency(core-cpp 0.7)`. Nothing morph calls changed signature in core-cpp 0.5.1, 0.6.0 or 0.7.0.
  `morph::net::SocketServer`'s accept loop stops on a closed listener and backs off on every other failed
  accept, which now includes a registration the event loop refuses, where core-cpp 0.5 threw out of
  `accept()`. core-cpp 0.7 adds standard mouse tracking, pointer capture and a public
  `Screen::componentAt` to `core::tui`, for the terminal frontend.
```

- [ ] **Step 4: Build and run the whole suite**

```bash
cd /Users/christianparpart/projects/morph
cmake -S . -B build/all -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMORPH_BUILD_TUI=ON -DMORPH_BUILD_QT=ON \
      -DMORPH_BUILD_QT_QUICK=ON -DMORPH_BUILD_LADDER=ON -DMORPH_LADDER_RUNGS=all -DMORPH_BUILD_BANK_EXAMPLE=ON \
      -DMORPH_BUILD_NET=ON -DMORPH_BUILD_FORMS_QML=ON 2>&1 | tee build/all-configure.log
grep -E "CPM: Adding package core-cpp@0\.7\.0|morph: warnings: .*strict=ON" build/all-configure.log
cmake --build build/all
ctest --test-dir build/all --output-on-failure
```

(`build/` is ignored by git.) Expected: the `grep` prints both lines (the configure
fetched `0.7.0`, not a cached `0.5.0`, and strict warnings are on); the build has no warning; `ctest` reports
`100% tests passed`. If an installed core-cpp 0.7 was found first, the CPM line is absent and
`core-cpp_DIR` in `build/all/CMakeCache.txt` names it — say so; it is still 0.7.

- [ ] **Step 5: The installed package asks for 0.7, and the check sees a wrong one**

```bash
bash scripts/check_install_export.sh
```

Expected: passes — it installs morph and core-cpp to a scratch prefix and builds a consumer through
`find_package(morph)`, which runs `find_dependency(core-cpp 0.7 CONFIG)`.

Mutation check: set the `CPMAddPackage` lines back to `GIT_TAG v0.5.0` / `VERSION 0.5.0` while
`MORPH_CORE_CPP_VERSION` stays 0.7, and rerun `bash scripts/check_install_export.sh`. Expected: FAIL at the
consumer's `find_package(morph)` — the installed core-cpp is 0.5.0 and `find_dependency(core-cpp 0.7)` refuses it,
so the check does measure the bound. Restore the two lines (`git diff CMakeLists.txt` shows only the three intended
changes).

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt README.md CHANGELOG.md
git commit -m "wip(corecpp): build against core-cpp 0.7

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 11: Part verification and squash

**Files:** none new; fixes land in the files they concern.

- [ ] **Step 1: Sanitizers** (core-cpp is built in-tree and instrumented under `AF_SANITIZER`, so its 0.5.1–0.7.0
  changes are under test here; Linux, or on macOS the ASan configure that works there)

```bash
cmake --preset clang-asan && cmake --build --preset clang-asan
bash scripts/check_sanitizer_instrumentation.sh build/clang-asan asan
ctest --preset clang-asan
cmake --preset clang-tsan && cmake --build --preset clang-tsan
bash scripts/check_sanitizer_instrumentation.sh build/clang-tsan tsan
ctest --preset clang-tsan
```

Expected: both instrumentation sweeps report every ctest binary carries the mode's symbols, and both suites pass
(the presets exclude `OomInjector|morph#108` themselves — CONTRIBUTING). A preset that cannot run on this machine
is stated as not run, never as passed.

- [ ] **Step 2: The gates this part does not need, and why**

- clang-tidy-diff: this part changes no C++ (`git diff --stat master...HEAD -- '*.hpp' '*.cpp'` shows none from
  this part) — state that rather than running a gate over zero files.
- Docs build: no header changed.
- WebAssembly: the `wasm-ladder.yml`/`wasm-demo.yml` configurations fetch the same tag; they run in the master
  plan's "Finishing the branch" and in the PR's CI. core-cpp's own Emscripten legs passed on the tag (Task 9 Step 3).

- [ ] **Step 3: Commit any fixes**

```bash
git add -A CMakeLists.txt README.md CHANGELOG.md
git commit -m "wip(corecpp): fixes from the sanitizer gates

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

Skip if there was nothing to fix, and say so in the hand-off.

- [ ] **Step 4: Squash this part into its one commit**

Follow the master plan's "Squashing a part" procedure with key `corecpp` and this message:

```text
core: build against core-cpp 0.7

MORPH_CORE_CPP_VERSION is 0.7 and CPM fetches v0.7.0, so the installed
package asks find_dependency(core-cpp 0.7). core-cpp 0.7 adds what the
terminal frontend needs from core::tui: standard mouse tracking (DEC
1000/1002/1003 with SGR 1006), pointer capture in Screen and a public
Screen::componentAt. No morph code changes: nothing morph calls changed
signature in 0.5.1, 0.6.0 or 0.7.0, and SocketServer's accept loop already
backs off on every failed accept but a close.

Signed-off-by: Christian Parpart <christian@parpart.family>
```

The last line of the procedure's output must show the docs commit and then `core: build against core-cpp 0.7`.

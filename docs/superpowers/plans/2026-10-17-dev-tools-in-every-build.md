# Development tools in every build Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the `MORPH_UI_DEV_TOOLS` switch; the inspector and reload are always compiled, and a session option
decides whether screens record anything for the inspector.

**Architecture:** `ui::SessionOptions::developmentTools` (`enum class DevelopmentTools { Off, On }`) is copied into
`ScreenCore` when it is made and gates the four record sites. `Inspector::over(Screen&)` is the only way to make an
inspector and gives nothing for an Off screen. The macro, its header, the CMake option and every guard go.

**Tech Stack:** C++23 header-only `include/morph/ui`, Qt Quick `src/qt_quick`, Catch2.

Spec: `docs/superpowers/specs/2026-10-17-dev-tools-in-every-build-design.md`.

## Global Constraints

- `enum class DevelopmentTools : std::uint8_t { Off, On };` in `include/morph/ui/interpreter.hpp`, default `Off`.
- No `#if MORPH_UI_DEV_TOOLS` and no `dev_tools.hpp` anywhere afterwards (`grep -rn MORPH_UI_DEV_TOOLS` finds only
  `CHANGELOG.md` and dated `docs/superpowers/` records).
- Doxygen complete on every public symbol touched; comments state current behaviour only.
- Builds under `flock <scratchpad>/morph-build.lock`, `-j10`.

---

### Task 1: The session option and the gated recording

**Files:** `include/morph/ui/interpreter.hpp`, `include/morph/ui/inspector.hpp`, `include/morph/ui/host.hpp`,
`include/morph/ui/frontend.hpp`, `tests/ui_interpreter_support.hpp`, `tests/test_ui_inspector.cpp`, `tests/test_ui_host.cpp`,
`tests/test_ui_interpreter_detail.cpp`

**Interfaces:**
- Produces: `ui::DevelopmentTools`, `SessionOptions::developmentTools`, `ScreenCore::recording` (bool, set once),
  `static std::optional<Inspector> Inspector::over(Screen& screen)`.

- [ ] Step 1: failing test in `tests/test_ui_inspector.cpp`: a screen mounted by a `Harness` whose session has
  `DevelopmentTools::Off` gives `Inspector::over(screen) == std::nullopt`, and `DocumentScreenHost::inspector(handle)`
  nothing; the same screen with `On` lists its nodes and logs a command list it ran.
- [ ] Step 2: run — FAIL (no `DevelopmentTools`).
- [ ] Step 3: add the enum and field; `ScreenCore` gets `bool const recording` from `owner.options()`; replace each
  `#if MORPH_UI_DEV_TOOLS` in `interpreter.hpp` with unconditional declarations, and gate the lowering record (and its
  prune), `RtScope` registration/removal and `logList` on `core.recording`. `Inspector`'s constructor becomes
  private; `over` checks `screen._core->recording`. `host.hpp` and `frontend.hpp` lose their guards; `host.hpp`
  calls `Inspector::over`. `Harness` takes a `DevelopmentTools` (default `On`).
- [ ] Step 4: run `morph_tests` — PASS. Commit.

### Task 2: Remove the switch from the build, Qt Quick and the client

**Files:** `CMakeLists.txt` (option, interface define, install file set), `docs/CMakeLists.txt`, delete
`include/morph/ui/dev_tools.hpp`, `src/qt_quick/CMakeLists.txt`, `src/qt_quick/frontend.cpp`,
`include/morph/qt_quick/frontend.hpp`, `include/morph/qt_quick/inspector.hpp`, `src/qt_quick/inspector.cpp`,
`examples/client/main.cpp`, `examples/common/app/*`, `tests/qt_quick/test_qt_quick_inspector.cpp`,
`tests/qt_quick/test_qt_quick_frontend.cpp`, `examples/common/app/tests/test_sources.cpp`, `tests/test_ui_dev_reload.cpp`

- [ ] Step 1: failing Qt test in `test_qt_quick_frontend.cpp`: with `FrontendConfig::devTools = DevelopmentTools::Off`,
  Ctrl+Shift+I on the window opens no inspector window; with `On`, it opens one.
- [ ] Step 2: delete the option, define, header and guards; `FrontendConfig::devTools` becomes a `ui::DevelopmentTools`;
  the client sets both it and `SessionOptions::developmentTools` from `--dev`; its "not built into this client"
  message goes; `test_sources.cpp` runs both modes unconditionally.
- [ ] Step 3: configure and build `linux-everything` (Release) — the inspector is now built in; run `morph_tests`,
  `morph_qt_quick_tests`, `morph_qt_quick_frontend_tests`, the client tests. Doxygen check
  (`-DMORPH_BUILD_DOCUMENTATION=ON` configure, `--target doc`). Commit.

### Task 3: Specs and changelog

**Files:** spec 6 §21 in `docs/superpowers/specs/2026-10-08-app-hosting-design.md`, `docs/spec/ui/inspector.md`,
`service.md`, `interpreter.md`, `host.md`, `docs/spec/qt_quick/frontend.md`, `CHANGELOG.md`
- [ ] Step 1: rewrite as the design's "The specs that change" lists; the CHANGELOG `[Unreleased]` entry.
- [ ] Step 2: `grep -rn MORPH_UI_DEV_TOOLS` shows only the changelog and dated records. Commit.

# Development tools in every build — design

The development tools (document reload and the inspector, spec 6 §21) are compiled into every client and turned on
at run time by development mode. The `MORPH_UI_DEV_TOOLS` build option and macro are removed.

## Why

A tool that exists only in a separate build is a tool nobody has at hand. The `linux-everything` preset builds Release,
so its client had neither reload nor inspector, and finding out why took reading CMake. Every build type is a second
configuration of the interpreter's `ScreenCore` layout, which CI has to build both ways to keep honest. What the
switch saved is small: a client binary's inspector symbols and the recording a screen does, and the recording can be
skipped at run time instead.

## What a client does now

- In the generic client, `--dev`, or `MORPH_UI_DEV=1`, turns development mode on, in every build. Without it the
  client schedules no reload poll, its screens record nothing, and Ctrl+Shift+I does nothing.
- Development mode is a session option: `ui::SessionOptions::developmentTools`, a `ui::DevelopmentTools` (`Off`, the
  default, or `On`). A screen mounted in a session with the tools on records what the inspector reads: its lowered
  nodes, its live scopes and the latest `kCommandLogSize` command lists. With them off it records none of it.
  `ScreenCore` copies the option when it is made (a `SessionOptions` does not change after the session is made), so
  each record site — the lowering record and its prune, `RtScope`'s registration and removal, `logList` — is one
  comparison. The inspector's diagnostic mute (`inspecting`) stays unconditional: with the tools off no inspector
  exists to set it.
- `Inspector::over(Screen&)` is the one way to make an inspector, and gives nothing for a screen of a session with
  the tools off. `DocumentScreenHost::inspector(handle)` calls it, so it gives nothing in three cases: a closed
  handle, a foreign one, and a screen whose session has the tools off. A shell's `inspectFocused` then calls
  nothing, and the Qt Quick window shows its existing "no screen is shown" text.
- A client sets two switches from development mode, `FrontendConfig::devTools` (the window and its shortcut) and
  `SessionOptions::developmentTools` (the recording). If a client sets only the first, the window opens and shows
  "no screen is shown"; if only the second, screens record and nothing reads it. Neither is an error.
- `qt_quick::FrontendConfig::devTools` becomes a `ui::DevelopmentTools`. With `Off`, the window installs no shortcut
  and makes no inspector window.
- The reactive counters (`Runtime::stats()`) are kept as they are, in every run.
- Document reload in the library (`dev_reload.hpp`, `UiClient::poll`) was never behind the switch. Only the example
  client's poll and its "not built into this client" message were, so that is the whole reload side of this change.
- What a release client now carries: `src/qt_quick/inspector.cpp` with its QML text, and per screen three empty
  containers and a flag (a `std::deque` allocates its first block on libstdc++, about 512 bytes). This is accepted:
  it is the price of the tools being at hand.

## What is removed

- The CMake option `MORPH_UI_DEV_TOOLS`, its interface definition on `morph`, and the release-type default.
- The macro in `include/morph/ui/dev_tools.hpp`; the header now declares `ui::DevelopmentTools` alone, so the Qt Quick
  frontend's config can name it without including the interpreter.
- Every `#if MORPH_UI_DEV_TOOLS` in headers, sources, examples and tests. The declarations it guarded become
  unconditional. `src/qt_quick/inspector.cpp` and `inspector_model.hpp` are always part of `morph_qt_quick`.
- The Doxygen predefinition of the macro.
- The client's "development mode is not built into this client" message.
- The test guards that let a release build compile without the dev-tools test cases.

## The specs that change

- Spec 6 §21 (`docs/superpowers/specs/2026-10-08-app-hosting-design.md`): "neither is built into a release client"
  becomes "both are in every client, off unless development mode is on".
- `docs/spec/ui/inspector.md`: "Built only into development builds" becomes "Off unless development mode", stating
  the session option, what is recorded when it is on, and that nothing is recorded when it is off. The binary
  check (`nm`, `strings`) is replaced by the run-time one below. The design-decision row on the preprocessor switch
  is replaced by one on the session option.
- `docs/spec/ui/service.md` (its header table's `dev_tools.hpp`, and Development reload), `docs/spec/ui/interpreter.md`,
  `docs/spec/qt_quick/frontend.md` and `docs/spec/ui/host.md` (the three cases `inspector` gives nothing): the build
  option is no longer mentioned; development mode is.
- `docs/spec/ui/inspector.md`'s header table, its contents anchor, and the design-decision row on the ODR hazard the
  macro guarded against, which is gone with it.
- `CHANGELOG.md`, under `[Unreleased]`: the option and the header are removed, and the tools are in every build.

## Tests

- A screen mounted with the tools off records no node, no scope and no command list: the inspector's lists of a
  screen whose session is `Off` are empty, and `DocumentScreenHost::inspector` returns nothing for it. Control: the
  same screen with `On` lists its nodes and logs a command list it ran.
- The existing inspector and reload tests run with the session option on, in every build type: their guards go.
  `tests/ui_interpreter_support.hpp`'s `Harness` takes the option (default `On` for the inspector tests, so they do
  not pass over empty lists), and the Off control uses a second harness; `tests/qt_quick/test_qt_quick_inspector.cpp`
  likewise. `examples/common/app/tests/test_sources.cpp` runs both modes unconditionally.
- The Qt Quick window test drives `Frontend::run` (`tests/qt_quick/test_qt_quick_frontend.cpp`), not `InspectorWindow`
  directly: with `Off`, Ctrl+Shift+I opens no window; with `On`, it opens one.
- The client argument test: `--dev` is accepted and sets development mode in a Release build.

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Where the switch lives | A session option | A screen records through its session; one session is one client's run, which is what development mode describes. |
| A bool or an enum | `enum class DevelopmentTools { Off, On }` | The repository's guideline for a switch in an API surface: a call site reads `DevelopmentTools::On`, not `true`. |
| Recording when off | None | The recording keeps a weak reference per lowered node and 256 command lists; a release run should not carry that for a window nobody opens. |
| Where the Off check lives | `Inspector::over(Screen&)` | The rule is part of the inspector's contract; the type that is handed out decides when it can be. `Screen` grows no accessor. |
| ODR | No longer a concern | With no macro, every translation unit sees one `ScreenCore` layout by construction. |
| Counters | Unchanged, always on | A few increments per flush; they belong to the runtime. |

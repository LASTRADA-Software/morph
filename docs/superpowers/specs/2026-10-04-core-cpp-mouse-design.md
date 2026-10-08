# core-cpp: standard mouse tracking and pointer capture — design (spec 0)

core-cpp's `core::tui` decodes SGR mouse reports, including motion with a button held, but never
asks a standard terminal to send them: the only mouse mode it enables is Contour's private passive
mode 2029. xterm, kitty, WezTerm, iTerm2 and Windows Terminal therefore report no mouse events at
all. Its `Screen` also hit-tests every mouse event, so a drag that leaves the component it started
on loses its moves and its release. This spec closes both gaps so a `core::tui` application can
implement drag-and-drop, and morph's TUI frontend can let kanban's board move tasks by dragging.

The work lands in the core-cpp repository as release **0.7.0**, which is the version morph pins.

## Contents

[1 Mouse tracking modes](#1-mouse-tracking-modes) · [2 Pointer capture](#2-pointer-capture) ·
[3 Hit testing](#3-hit-testing) · [4 Tests](#4-tests) · [5 Release and pin](#5-release-and-pin) ·
[6 Out of scope](#6-out-of-scope)

## 1. Mouse tracking modes

```cpp
namespace core::tui {
/// How much mouse input the terminal is asked to report.
enum class MouseTracking : std::uint8_t {
    Off,        ///< No reports (default).
    Buttons,    ///< Press and release (DEC 1000).
    Drag,       ///< Press, release and motion while a button is held (DEC 1002).
    AnyMotion,  ///< Press, release and all motion (DEC 1003).
};
}
```

- **Opt-in, default `Off`.** Mouse reporting changes what a click-drag does in the user's terminal
  (it stops selecting text; most terminals keep Shift+drag for selection), so no existing
  `core::tui` program changes behaviour. An application asks for a mode.
- **API.** `TerminalInput::setMouseTracking(MouseTracking)` and `Terminal::setMouseTracking(…)`
  (forwarding), effective at the next `enableProtocols()` — before `initialize()` for the common
  case, and immediately when the protocols are already enabled.
- **Sequences.** For any mode but `Off`, `enableProtocols()` writes the mode's DEC private set
  (`CSI ? 1000 h`, `1002 h` or `1003 h`) followed by SGR encoding `CSI ? 1006 h`;
  `disableProtocols()` writes the matching resets in reverse order. Contour's passive mode 2029 is
  still enabled as today; on Contour it carries the `uiHandled` flag and is harmless beside the
  standard modes.
- **The existing hover path.** Today 1003 is enabled when the terminal reports mode 2029 as
  changeable, for hover tooltips. That stays, and is expressed as "the requested mode is raised to
  `AnyMotion`".
- **Windows.** The console input mode already includes `ENABLE_VIRTUAL_TERMINAL_INPUT`, under
  which Windows Terminal delivers VT mouse reports; the same sequences apply.

The parser needs no change: `CSI < b ; x ; y M` with bit 32 already becomes
`MouseEvent{Move, button = b & 3}` (button 3 means "none held", which only `AnyMotion` produces).

## 2. Pointer capture

`Screen::dispatchMouseEvent` gains implicit capture:

- A `Press` delivered to component `C` (after bubbling, the component that returned non-`Ignored`)
  makes `C` the **capture target**.
- While a capture target exists, every `Move` and the next `Release` go to it directly — no hit
  test, no bubbling — with coordinates relative to `C`'s screen bounds, so they may be negative or
  beyond its size.
- The `Release` ends the capture. So does the capture target's destruction or removal from the
  tree (the component's teardown clears it) and `Screen::releasePointer()`.
- Scroll events are never captured.

Without capture, a drag's motion lands on whatever is under the pointer, so a component cannot
follow its own drag; with it, the component that started the drag sees the whole gesture.

## 3. Hit testing

`Component* Screen::componentAt(int row, int col) const` becomes public: viewport-relative,
0-based, returning the top-most visible component at that cell — overlays first (last shown wins),
then the main tree in reverse z-order, deepest descendant first — or null. A drag source asks it
which component lies under the pointer when the button is released, to find the drop target.

## 4. Tests

In core-cpp's Catch2 suites:

- **Parser:** `CSI <32;10;5M` → `Move`, button 0, x 10, y 5; `CSI <35;…M` → `Move`, button 3.
- **Protocols:** for each `MouseTracking` value, the bytes `enableProtocols()` and
  `disableProtocols()` write (through a mock output), including `Off` writing no 1000/1002/1003.
- **Capture:** press on component A, move outside A, release elsewhere — A receives all three with
  A-relative coordinates, B under the pointer receives nothing; destroying A mid-drag clears the
  capture and the next move hit-tests normally; scroll is not captured.
- **`componentAt`:** overlay above tree, z-order, nested child, empty cell → null.

## 5. Release and pin

- core-cpp: CHANGELOG entry, version 0.7.0, tag `v0.7.0`. Pushing the branch, merging and tagging
  in core-cpp are outward-facing steps, each confirmed with the user before it is taken.
- morph: `MORPH_CORE_CPP_VERSION 0.7`, `GIT_TAG v0.7.0` in the root `CMakeLists.txt`; the config
  template follows (`find_dependency(core-cpp 0.7 CONFIG)`). The plan reviews core-cpp's 0.6.0 and
  0.7.0 changelogs for anything morph's existing code depends on and fixes it in the same commit.
- An issue is filed against core-cpp for decoding legacy Shift+Tab (`CSI Z`), which no release
  decodes; until then Shift+Tab reaches an application only on terminals speaking the Kitty
  keyboard protocol. Filing it is confirmed with the user first.

## 6. Out of scope

- Drag-and-drop semantics (payloads, drop targets, drag visuals) — those are morph's TUI frontend
  (spec 1 §6) built on capture and `componentAt`.
- Mouse cursor shape changes, pixel-precise (SGR-Pixels, 1016) reporting, multi-button chords.

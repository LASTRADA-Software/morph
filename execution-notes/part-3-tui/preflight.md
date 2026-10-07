# Part 3 pre-flight conflict scan

Plan: `docs/superpowers/plans/2026-10-04-declarative-ui-tui-3-tui.md` (P:NNN = plan line). Branch
`feature/declarative-ui` at 69a18881. Authority, in order: the code of Parts 0–2 (`include/morph/{reactive,ui}`,
core-cpp v0.7.0), then the spec (§2, §5–5b, §6) and the shared contract, then the plan. Read: `program-notes.md`,
Part 1/2 ledger rulings, `git show -s` of d45311e3 and 69a18881, `docs/spec/ui/{backend_contract,frontend}.md`.

## How this was measured

Scratch tree outside the repo (`$TMPDIR/.../scratchpad/tree`): `git archive HEAD include src tests`, then every
code block of the plan applied mechanically in plan order (creates, appends "inside the namespace" with the named
includes, Task 12's `fit`/`moveFocus`/`blockedByDialog` edits, Task 15's `context.*` and `pointer` replacements).
core-cpp v0.7.0 (the CPM cache copy morph pins) built in scratch with `CORE_CPP_WITH_TUI=ON CORE_CPP_WITH_IMAGES=OFF`,
libunicode 0.9.3. Compiled with morph's exact flag set (`-Weverything -Werror` minus morph's exemptions, copied from
`build/reactive/compile_commands.json`) under AppleClang 21 **and** Homebrew clang 22; linked and run with Catch2.

| What | Result |
|---|---|
| `src/tui/*.cpp` (14), `tests/tui/test_tui_*.cpp` (12 of 13) | compile clean, AppleClang 21 and clang 22 (measured) |
| `tests/tui/test_tui_conformance.cpp` (Task 18 as written) | **does not compile**: `childAt` not covariant; `TuiProbe` abstract (`dismiss`, `selectRows`, `selectedRows` missing) (measured) |
| Plan tests, per task tag | all pass with the plan's stated counts: loop_executor 4, layout 5, widgets 9, containers 6, fields 10, lists 6, dialog+busy+slider 8, table 4, backend 5, drag 4, scheduler 6, frontend 9 (measured) |
| Task 1/2 tests | `[io_loop]` 10 cases, `[timeout_scheduler]` all, `morph_net_tests "[caller]"` 2 cases pass (measured) |
| Conformance suite with a minimally completed probe (R1) | **19 of 23 pass; 4 fail**: closed dialog takes input; widget in hidden/disabled/collapsed container takes input; Multiple table reports a rowless key; handler destroying its widget frees the running handler (measured) |
| ASan (morph code instrumented, core-cpp not) | **stack-use-after-scope in `DragController::move`** (`splitLines(_source->probeText())` views a temporary), in "a target whose accepts refuses the key…"; after patching it, no other report in `[tui]` (measured) |
| Scratch extra tests | disabled ancestor: mouse click reaches the button (FAIL); hidden first table cell: next cell drawn at col 2, header at col 9 (FAIL); `moveChild` of a sibling mid-drag: drop lost, `dragging()` stays true (FAIL); LoopScheduler sibling cancel in same turn (PASS); blocked owner fires once (PASS); `quit()` from the factory ends `run` after mount (PASS); click on a table row's cell selects that row (PASS) |
| clang-tidy 22.1.8, repo `.clang-tidy`, new lines only | 17 findings in `src/tui`, 4 in `tests/tui` (+ conformance probe ones); the 5 `NOLINTNEXTLINE(bugprone-exception-escape)` are dead — no finding without them (measured) |
| Doxygen 1.18.0, repo settings, `include/morph` | HEAD: 0 warnings. With Part 3 headers: 1 warning `parameters of member morph::ui::Frontend::~Frontend are not documented`, caused by `tui::Frontend`'s `~Frontend() override;` (bisected; gone when the explicit destructor and deleted copy/move are dropped) (measured) |
| `timeout` command (Task 2 Step 3) | not present on this macOS (measured) |

Not measured: the CMake/install wiring of Tasks 5–6 (no repo-side configure was run; anchors verified by reading),
TSan, Windows, CI network behaviour.

## Table

Status: OK / NOTE / CONFLICT. M = measured, I = inferred from reading.

### A. Task pairs sharing a file or interface

| # | Tasks | Shared item (exact names) | Status | Evidence | Ruling |
|---|---|---|---|---|---|
| A1 | T1→T2,T3,T16,T17,T18 | `IoLoopDriver{OwnThread,Caller}`, `IoLoop(IoLoopDriver)`, `driver()`, `runningHere()` (P:137, 419–557) | OK | M: compiled, T1/T2 tests pass | — |
| A2 | T1→T3 | executor.md/concurrency rows describe T1's API (P:819–877) | OK | I: text matches code | — |
| A3 | T5→T16,T17,T18 | `tui::LoopExecutor(core::net::EventLoop&)`, `post(std::function<void()>)` (P:1259–1281) | OK | M | — |
| A4 | T5→T6..T18 | `add_library(morph_tui STATIC …)` source list and `FILE_SET HEADERS` grown per task; `tests/tui/CMakeLists.txt` `add_executable` list | OK | I: every task names its additions (P:1883, 3280, 4395, 5161, 6267, 6292, 7201, 7531, 8211) | — |
| A5 | T7→T8,T9,T13 | `layout::{Item,Track,distribute,crossExtent,GridCell,GridSlot,GridPlan,planGrid,slotArea,pad}` | OK | M | — |
| A6 | T8→T13 | `arrangeStack(…, StackSpec{.extents})` maps extents over **shown** children only (P:2819–2830); `TableImpl::naturalWidths` reads `shownChildren()` (P:6104) | CONFLICT | M: hidden first cell → next cell under the wrong header | R5 |
| A7 | T8→T15 | `ContainerBase::move` → `restack()` removes and re-adds **every** child (P:2773–2782); `DragController` relies on core::tui pointer capture (P:6909) | CONFLICT | M: capture ends (`Screen::componentDetached`), drop lost, gesture left armed | R6 |
| A8 | T8↔T12↔T15 | `widget.cpp`: T12 replaces `fit`/`moveFocus`, adds `blockedByDialog`, prepends to `pointer`; T15 replaces `pointer` wholesale keeping `blockedByDialog` (P:5396–5457, 7152–7199) | OK | M: applied in order, compiles, tests pass | — |
| A9 | T8↔T15 | `context.hpp/.cpp`: `Context::drag` last member, `forget` calls `drag->forget` (P:6725–6727, 7136–7150) | OK | M | — |
| A10 | T8↔T9,T12 | `container_widgets.*`, `leaf_widgets.*` appends with added includes (P:3442, 3518, 5461, 5501, 5618, 5667) | OK | M | — |
| A11 | T10→T18 | `FieldView` is an `InputField`; probe `type` must clear first | CONFLICT | M: plan probe appends; `End`+`Ctrl+U` (InputField `KillToStart`) clears | R1 |
| A12 | T12→T14 | `Context::openDialogs`, frame focus → `Backend::focusFirst` (P:6678–6685) | OK | M | — |
| A13 | T12→T18 | Dialog dismissal: `frameEvent` handles Esc without checking `_open` (P:5568–5574) | CONFLICT | M: closed-dialog case fails | R2 |
| A14 | T13→T18 | table selection read/drive: no accessor for marked rows; probe lacks `selectRows`/`selectedRows` | CONFLICT | M | R1, R4 |
| A15 | T14→T17 | `Backend::{fit,focusNext,focusPrev,focusFirst,animating,advanceAnimation}` | OK | M | — |
| A16 | T16→T17 | `LoopScheduler(EventLoop&, IExecutor&)`, `every` for the spinner | OK | M | — |
| A17 | T17→T19 | `include/morph/tui/frontend.hpp` under Doxygen `FAIL_ON_WARNINGS`; docs first built in T19 | CONFLICT | M: 1 warning from `~Frontend() override;` | R10 |
| A18 | T5↔T6 | `morph_tui` → install component `tui`, `MORPH_INSTALLED_COMPONENTS`, `morphConfig.cmake.in` `core::tui` check | OK | I: anchors read; core-cpp prints "core-cpp-tui is not installed: it links unicode, which this build fetched" (M, scratch configure) | R15 |
| A19 | T8–T17→T19 | `docs/spec/tui/frontend.md` describes widget behaviour | NOTE | I: must follow R2–R7 | R18 |

### B. Per-task self-consistency

| # | Task | Status | Finding | Ruling |
|---|---|---|---|---|
| B1 | T1 IoLoop Caller | OK | Tests and mutation coherent; header compiles; no tidy finding on new lines (M) | — |
| B2 | T2 Caller cases | NOTE | P:771–773 expects `morph_tests "[caller]"` = 1 case; it matches 6 (T1's five `[caller]` cases + 1) (M). P:781 uses `timeout`, absent on macOS (M) | R13 |
| B3 | T3 specs | OK | Anchors exist: "There are eleven types" (executor.md:32), sections at 507/530, `exec::IoLoop` row (concurrency:43), `### Added` under `[Unreleased]` (CHANGELOG:180) (I) | — |
| B4 | T4 squash | OK | — | — |
| B5 | T5 target+LoopExecutor | NOTE | "N one higher than build/reactive's" (P:1333) holds only because build/reactive has `MORPH_BUILD_NET=ON` from T1 (+2 tui targets −1 `morph_net_tests`) (I); fragile | R13 |
| B6 | T6 install/CI | NOTE | `write_basic_package_version_file` change also drops `ARCH_INDEPENDENT` for Qt-only installs (`morph_qt_impl`, `forms_qml`) — a behaviour change beyond tui (I); CI jobs now fetch libunicode + UCD.zip; `windows-everything` gains TUI with no Windows CI job building it (I) | R14, R15 |
| B7 | T7 layout | NOTE | Passes (M). tidy: `layout.cpp` modernize-use-integer-sign-comparison (planGrid row check), readability-math-missing-parentheses (slotArea) (M) | R11 |
| B8 | T8 widget base | CONFLICT | No ancestor gating: `focusable()`/`dispatch()`/`pointer()` use the widget's own `_enabled` (P:2403, 2675, 2688) (M: disabled-ancestor click fires). Handlers invoked in place (`_onClick()`, `_onToggle`, `_onDrop`) (P:3107, 3156, 2653). `restack` detaches all children (P:2773). tidy: nested conditional ×2, pointer cognitive complexity 26, arrangeStack 39, collectFocusable recursion; dead NOLINT (M) | R2, R3, R6, R11 |
| B9 | T9 grid/panel/scroll | NOTE | `PanelImpl::activate` calls `_onToggle` in place (P:3702). tidy: use-std-min-max in `skipFor` (M) | R3, R11 |
| B10 | T10 fields | NOTE | Handlers in place (`_onChange`, `_onSubmit`, `_onPicked`); `FieldView` gates on own `enabled()` only (P:4156). tidy: bugprone-optional-value-conversion in `parseLocal` (M) | R2, R3, R11 |
| B11 | T11 lists | NOTE | Select re-resolution correct (M, conformance passes). Handlers in place (`_onSelect` ×2, `_onActivate`, Tabs `_onSelect`); `ListView` gates on own `enabled()` (P:4794). tidy: inefficient-vector-operation ×3, nested conditional ×2; dead NOLINT (M) | R2, R3, R11 |
| B12 | T12 dialog/busy/slider | CONFLICT | `frameEvent` dismisses while closed (M); `_onDismiss()` in place; slider `_onChange` in place. Dead NOLINT on `~DialogImpl` (M) | R2, R3, R11 |
| B13 | T13 table | CONFLICT | `toggle` keeps rowless pending keys and reports them (P:6239–6250) (M); columns sized/placed by shown cells (M); `key()`/`click()` touch `this` after a handler (`selectOnly` → `report` → `activateRow` → `refresh`) (P:6199–6208, 6219–6224) (I) | R3, R4, R5 |
| B14 | T14 backend | OK | Factories match `ui::IViewBackend` (override compiles) (M) | — |
| B15 | T15 drag | CONFLICT | ASan stack-use-after-scope at P:7046–7047 (M). No gating of source or target (P:7091, 7175) (I; conformance drag gate unreachable until click gate fixed). `drop` calls `_onDrop` in place. Dead NOLINT on `~DragController` (M) | R9, R2, R3, R11 |
| B16 | T16 LoopScheduler | NOTE | (a)–(d) hold (M) but only (b),(c) pinned; no sibling-cancel and no blocked-owner test (P:7294–7357). tidy: make-member-function-const (`arm`), easily-swappable (`add(delay, period)`); dead NOLINT (M) | R8, R11 |
| B17 | T17 frontend | CONFLICT | `Session::run` hand-rolls factory→null-check→mount→loop→teardown (P:7944–7961) instead of `ui::runApplication`; interrupt handler capturing `this` is never cleared though `Session` dies before `TuiRuntime` (I); `FrontendConfig::terminal` doc says only a null terminal is initialised, code initialises a given one too (P:8091 vs 8185–8188) (M: test terminals are initialised) | R7, R16, R17 |
| B18 | T18 conformance | CONFLICT | Does not compile (M); `type` appends; `drag` sets the source/target views' own areas, which the parent's `paint` overwrites for nested widgets (I; the gate case drags a card 3 levels deep); tidy findings in the probe (M) | R1, R11, R12 |
| B19 | T19 docs | NOTE | Text must follow R2–R7; anchors exist (README:281/444/481, ARCHITECTURE:54/721, GETTING-STARTED:866, spec README "Declarative UI" group) (I) | R18 |
| B20 | T20 gates | NOTE | Doc build and clang-tidy findings would first surface here (M) | R10, R11 |

### C. Program-notes obligations addressed to Part 3

| # | Obligation (program-notes) | Where it lands in the plan | Status | Evidence | Ruling |
|---|---|---|---|---|---|
| C1 | LoopScheduler honours Scheduler (a)–(d) | T16 `LoopScheduler::State` (P:7431–7488) | OK (behaviour) | M: sibling cancel, cancel-in-callback, handle outlives, blocked owner fires once | — |
| C2 | Add a test: a callback cancels a sibling due in the same turn | absent from T16 tests | CONFLICT | I | R8 |
| C3 | Backend keeps a callback alive during its call (copy first), pinned with ASan | every handler call site is in-place (17 sites) | CONFLICT | M: conformance "handler may destroy its own widget" fails | R3 |
| C4 | A binding that throws → `kEffectThrew`, widget keeps last value | Mount-level; no backend work | OK | I | — |
| C5 | §7 "application destroyed before the runtime" test lives in Parts 3/4 | T17 test (P:7784–7795) | OK | M: passes; its mutation needs rewording under R7 | R7 |
| C6 | `moveChild` emulated by remove+re-add must keep native state, or not move a component under capture | T8 `restack` | CONFLICT | M: mid-drag move loses the drop | R6 |
| C7 | Select remembers requested key across `setOptions`, re-resolves | T11 Radio/Dropdown `_selected` + `labelOf` | OK | M: conformance passes | — |
| C8 | Widget inside hidden, disabled or collapsed container not actionable | hidden/collapsed via `Hosted::visible`; disabled not at all | CONFLICT | M | R2 |
| C9 | No handler after the widget's destructor returns | TUI delivers synchronously | OK | M: conformance passes | — |
| C10 | No setter fires a handler, `setOptions`/`setItems` included | all setters | OK | M: conformance passes | — |
| C11 | Backend may assume handlers never throw (`kCallbackThrew`) | `Session::handle` still catches and logs (P:7999–8013) | OK | I: defence in depth, harmless | — |
| C12 | Multiple-mode user change reports only existing selected rows (`toggle` ~P3:6237) | T13 `toggle` | CONFLICT | M | R4 |
| C13 | Table shows user's selection with no `selection` bound | T13 keeps `_selection`, paints `*` | OK | M: conformance passes | — |
| C14 | Hidden cell keeps its column (plan sizes by `shownChildren()` ~P3:6087) | T13 `naturalWidths`, T8 `arrangeStack` extents | CONFLICT | M | R5 |
| C15 | Use `ui::runApplication` instead of hand-rolled `Session::run` (~P3:7944–7965) | T17 | CONFLICT | I: header has `runApplication(AppContext&, IViewBackend&, ApplicationFactory const&, std::function<int()> const&)` | R7 |
| C16 | Probe grew: `dismiss`, `selectRows`, `selectedRows`, mutable `childAt`, Select `textOf` = marked label | T18 probe | CONFLICT (compile) | M | R1 |
| C17 | TuiProbe drags nested widgets; `type` replaces text; mutable `childAt` | T18 probe | CONFLICT | M/I | R1 |
| C18 | Part 3 tests what the probe cannot reach: gating of choose/toggle/activateRow and drag onto a gated target; other handler kinds for "no handler after destruction" and "handler may destroy its widget"; hidden-cell column geometry | none | CONFLICT | I | R2, R3, R5, R12 |
| C19 | Dialog new = closed | T12 `_open=false` | OK | M | — |
| C20 | interfaces.md Part 2 section stale; headers authoritative | T18 Interfaces (P:8253–8255) lists 11 pure virtuals and `childAt const` | CONFLICT | M | R1 |
| C21 | RecordingBackend helpers deliver nothing to unreachable widgets (Parts 6–10); Part 1 ManualScheduler/Poller notes (Parts 4, 6, 8, 9) | not addressed to Part 3 | OK (N/A) | — | R19 |

### D. Part 3 uses of Part 0–2 APIs, checked against the code

| # | API (as used) | Plan | Actual | Status |
|---|---|---|---|---|
| D1 | `ui::IViewBackend` 19 factories, widget interfaces, `ContainerWidget::moveChild` | T8–T14 | `include/morph/ui/backend.hpp` | OK (M: overrides compile) |
| D2 | `ui::Sizing{Kind,amount,content,fixed,stretch}`, `LayoutHints`, `Key`, `Action`, `TableColumn{label,width}`, `SelectOption`, enums | T7–T13 | `view.hpp` | OK (M) |
| D3 | `ui::testing::ConformanceProbe` | T18: 11 virtuals, `Widget const* childAt` | 14 virtuals; `Widget* childAt`; `dismiss`, `selectRows`, `selectedRows` | CONFLICT (M) → R1 |
| D4 | `ui::runApplication` | not used | `frontend.hpp`, the fixed teardown order | CONFLICT → R7 |
| D5 | `ui::AppContext` (`runtime`, `executor`, `scheduler`, `ioLoop`, `quit(int=0)`, `frontendName`) | T17 `Session` | matches | OK (M) |
| D6 | `ui::Mounted(Runtime&, IViewBackend&, Node)`; builders `column/textInput/button/dialog/text/busy` | T17 tests | matches | OK (M) |
| D7 | `ui::FrontendOption{name,usable,make}`, `ui::Frontend::run/name` | T17 | matches | OK (M) |
| D8 | `reactive::Runtime(IExecutor&, RuntimeOptions{.afterFlush})` | T17 | matches | OK (M) |
| D9 | `reactive::Scheduler::{after,every}`, `TimerHandle{cancel,active}`; guarantees (a)–(d) | T16 | matches; `every` throws `invalid_argument` | OK (M) |
| D10 | `reactive::detail::site::kRuntimeOutlived`; `testing::OwnerProbeRecorder::count`; `testing::StepExecutor` | T17 test | graph.hpp:49; tests/owner_probe_recorder.hpp; tests/test_support.hpp:64 | OK (M) |
| D11 | Mount's callback wrapper: `widgetEvent` defers flushes, so a handler that destroys its own widget does it synchronously only by its own action | T8–T15 | `mount.hpp:203–236` | NOTE: makes R3 necessary, not optional (M) |
| D12 | `core::tui::Component` (render, onEvent, focusable, visible, preferredSize, parent, children, addChild, removeChild, setArea, area, screenBounds, setVisible) | T8 | `Component.hpp` | OK (M) |
| D13 | `Component::removeChild` → `setScreen(nullptr)` → `Screen::componentDetached` ends capture and press target | T8 `restack` | `Component.cpp:85–141`, `Screen.cpp:1136` | CONFLICT (M) → R6 |
| D14 | `Screen::{dispatchEvent,setFocus,focusedComponent,focusNext/Prev,showOverlay,hideOverlay,positionOverlay,isOverlayVisible,componentAt,releasePointer,viewportArea,root,renderedBuffer,invalidate,draw}` | T8–T15 | `Screen.hpp` | OK (M) |
| D15 | `InputField` (`processEvent`, `InputFieldAction::{Changed,Submit}`, `text`, `setText`, `setMasked`, `setMultiline`, `setPrompt`, `prompt`, `lineCount`), `List`/`ListItem`, `Canvas` (`area()` local, `put`, `putString`, `fill`, `drawBox`), `Theme` styles, `MouseEvent::Type`, `VtParser::feed`, `test::{charKey,specialKey,canvasToString}` | T8–T15 | matches | OK (M) |
| D16 | `TuiRuntime(EventLoop&, InputSource&/Terminal&)`, `blockOn`, `nextEvent`, `inputClosed`, `setInterruptHandler`; `ScriptedInputSource`; `Terminal::{initialize,setMouseTracking}`; `ScreenConfig::alternateScreen`; `MouseTracking::Drag` | T17 | matches | OK (M) |
| D17 | `EventLoop::{post,addTimer(SteadyTimePoint,TimerCallback,void*),cancelTimer,clock,runOnce,runUntilIdle,pendingTimerCount,teardownIsSerialisedWithDispatch}`; `cancelTimer` covers a timer queued but not yet run | T1, T16 | `EventLoop.hpp:233–584` | OK (M) |
| D18 | `core::async::{AsyncQueue,whenAny,Task,OperationCancelled,ExecutorScope}`; `core::platform::{isTerminal,standardInput,createSystemPipe}` | T5, T16, T17 | matches | OK (M) |
| D19 | core-cpp CMake: `CORE_CPP_WITH_IMAGES` (CoreCppOptions.cmake:30), libunicode 0.9.3 row, `core::tui` = ALIAS of `core-cpp-tui`, fetched-libunicode install skip | T5, T6 | matches | OK (M: scratch configure message) |

### E. Plan vs spec / contract / repo facts

| # | Item | Status | Evidence | Ruling |
|---|---|---|---|---|
| E1 | Contract Part 3 names (`IoLoopDriver`, `LoopExecutor`, `FrontendConfig{terminal,input,mouse}`, `kInterruptExitCode=130`, `Frontend`, `frontendOption`, `Backend(Screen&)`) | OK | I: plan matches; `Backend` additions declared as Part 3's own | — |
| E2 | Spec §6 widget list, one dispatch per event, Tab/dialog trap, Ctrl+C 130, afterFlush one draw per turn, quit via AsyncQueue+whenAny, Scheduler on EventLoop timers | OK | M: T17 tests pass | — |
| E3 | backend_contract.md: "a widget inside a hidden, disabled or collapsed container, or inside a closed dialog, takes no input"; "calls a copy of the handler" | CONFLICT | M | R2, R3 |
| E4 | ui/frontend.md: `quit` from the factory ends `run` after the mount | OK (untested) | M: scratch test passes | R7 (add test) |
| E5 | Spec §2: install via the component loop plus explicit install; plan uses explicit install only, keeps the loop byte-identical for `test_check_install_export.sh:252` | OK | I: line 252 sed-matches it | — |
| E6 | Repo anchors: `option(MORPH_BUILD_NET` (CMakeLists:51), `# ── core-cpp` (258), `"CORE_CPP_WITH_TUI OFF"`, `unset(_morph_core_cpp_exclude_from_all)`, guard (498), `_morph_optional_components` (520), `foreach(_morph_target …)` (532), net block (974), install (1031–1180), `morphConfig.cmake.in:97`, presets `windows-/linux-everything`, ci.yml 610/2452/3036, compiler_options 474/539/742, `morph_use_cpm` 163 | OK | I | — |
| E7 | Existing APIs: `runningOn` (executor.hpp:132/147), `ScopedLoggerOverride` (logger.hpp:279), `SocketBackend` ctor/`registerModel`/`bindModel`/`execute` (105/175/249/346), `privateBind`/`echoCall` (test_socket_backend.cpp:1707/2215), sync-verb throw text (socket_backend.hpp:445), `waitForConnected` answers at once on the loop | OK | M: T2 cases compile and pass | — |
| E8 | Core comment in CMakeLists core-cpp block says "nothing is fetched on its behalf"; with TUI morph fetches libunicode for it | NOTE | I | R14 |
| E9 | Spec §6 platforms Linux/macOS/Windows | NOTE | I: no CI job compiles morph_tui on Windows | R14 |

## Proposed rulings

1. **R1 — TuiProbe implements the real `ConformanceProbe`.** `childAt` returns `ui::Widget*`; add `dismiss` (Esc
   dispatched as a user would, focus as it is), `selectRows` (focus the table; `Single`: Home, Down×row, Space;
   `Multiple`: toggle the symmetric difference between the marked rows and the wanted ones, wanted first), and
   `selectedRows` (a new `TableImpl::markedRows()` accessor, positions among shown rows); `type` clears first (End,
   Ctrl+U = InputField `KillToStart`, then the code points); `drag` lays the **root ancestors** of source and target
   side by side (walk `WidgetBase::container()`), then presses/moves/releases at the source's and target's drawn
   centres. Update Task 18's Interfaces to the header. — the header is authoritative and the plan's file does not
   compile (measured); with these the suite runs (19/23 pass before R2–R4) — cost if wrong: Task 18 cannot build.
2. **R2 — Actionability walks the ancestors.** Add `WidgetBase::actionable()`: own enabled and shown, every
   container up the `_container` chain enabled and shown, and each container letting its children act (Panel not
   collapsed, Dialog open; a virtual on `ContainerBase`). Use it in `focusable()`, `dispatch()`, `pointer()` (press
   and the release's click), `FieldView`/`ListView::onEvent`, every activate/choose/toggle/activateRow path, the
   drag source in `press` and the target in `findTarget`; `DialogImpl::frameEvent` acts only while `_open`; the
   probe focuses a widget only when its view is focusable. Pin it with widget-level tests in Tasks 8, 10, 11, 12, 13
   and 15 (disabled ancestor two levels up, Select choose, Table toggle/activateRow, drag onto a gated target, Esc on
   a closed dialog). — backend_contract.md states the rule; measured: conformance cases 12 and 13 fail and a
   disabled-ancestor click fires — cost: one ancestor walk per event; Tab skips gated widgets.
3. **R3 — Every handler call runs a local copy and touches nothing after it.** All 17 sites (`_onClick`,
   `_onToggle`×2, `_onChange`×3, `_onSubmit`, `_onPicked`, `_onSelect`×3, `_onActivate`×2, `_onDismiss`,
   `_onSelectionChange`, `_onDrop`; `_accepts` too). `TableImpl::key`/`click` call the handler last; Enter in
   `Single` mode reports the selection, then checks a liveness token before calling `onActivate`. Add, per handler
   kind, a "handler destroys its own widget" test (ASan in Task 20) and a "no handler after destruction" test. —
   measured: conformance case 20 fails (the running closure is freed) — cost: one `std::function` copy per event.
4. **R4 — A Multiple-mode user change reports exactly the existing marked rows.** `toggle` builds the new selection
   from the keys of current rows that are marked, toggles the row's key, stores that (dropping rowless pending keys)
   and reports it; `setSelection` still keeps pending keys. — Part 2 ruling, header doc; measured: conformance case
   16 reports key 9 — cost: none.
5. **R5 — A hidden cell keeps its column.** `TableImpl::naturalWidths` reads every cell (`children()`);
   `arrangeStack` in extents mode consumes one extent per child, giving a hidden child an empty area but advancing
   the offset and gap. Add a Task 13 geometry test (hidden first cell: second cell under the second header). —
   measured: the visible cell is drawn under the first header — cost: none.
6. **R6 — `moveChild` never detaches.** `ContainerBase::move` reorders morph's `_children` only (layout already
   follows it); Tab order walks the morph tree (`Context::roots`, then `ContainerBase::children()`, the innermost
   open dialog's content) in `moveFocus` for both cases, replacing `Screen::focusNext/Prev` and `collectFocusable`
   over core children; core::tui's child list stays in attach order, which only orders overlapping siblings and
   stack/grid children never overlap. Keep "moveChild reorders both rendering and focus order"; add "moving a
   sibling of the drag source mid-drag keeps the gesture". — core::tui 0.7 has no reorder, and remove ends capture
   and the press target (measured: drop lost, gesture left armed); the contract says the child keeps its native state
   — cost: a ~20-line tree walk; a core-cpp reorder API can replace it later.
7. **R7 — `Session::run` delegates to `ui::runApplication`.** Keep `setInterruptHandler` before it; the loop
   argument does `focusFirst`, `draw`, `blockOn(serve())`, `_animation.cancel()` and returns the code. Rewrite the
   "application destroyed before the runtime" mutation (a factory wrapper that stashes the application in a `Session`
   member declared before `_runtime` behind a forwarding proxy). Add a test: `quit()` from the factory ends `run`
   after the mount with its code. — Part 2 ruling; the helper exists — cost: none; the plan's own
   `invalid_argument` text becomes the helper's.
8. **R8 — Pin Scheduler guarantees (a) and (d) in Task 16.** Add "a callback cancelling a sibling due in the same
   turn stops it" and "a blocked owner fires `every` once, next deadline from the firing". — program note; both hold
   already (measured) — cost: two tests.
9. **R9 — Fix the dangling view in `DragController::move`.** Hold `_source->probeText()` in a local before
   `splitLines`. — measured ASan stack-use-after-scope in the plan's own drag test — cost: one line.
10. **R10 — Drop `tui::Frontend`'s explicit destructor and deleted copy/move** (and `Frontend::~Frontend() =
    default;` in `frontend.cpp`); `ui::Frontend` already deletes copy and move. Build the `doc` target in Task 17
    Step 5 as well as Task 19. — measured: Doxygen 1.18.0 warns with the declaration, is clean without it; the docs
    CI fails on any warning — cost if wrong: red Docs job found only at Task 19/20.
11. **R11 — Clear the measured clang-tidy findings on new lines, in the task that writes them, and remove the five
    dead `NOLINTNEXTLINE(bugprone-exception-escape)`.** src: layout.cpp (integer-sign-comparison, math-parentheses),
    widget.cpp (nested conditional; cognitive complexity of `pointer` 26 and `arrangeStack` 39 — split helpers;
    `collectFocusable` recursion — iterative or replaced by R6's walk), leaf_widgets.cpp (nested conditional ×3,
    array-to-pointer decay in the slider bar), container_widgets.cpp (use-std-min-max), field_widgets.cpp
    (optional-value-conversion), list_widgets.cpp (reserve ×3, nested conditional ×2), scheduler.cpp (`arm` const,
    swappable `add(delay, period)` — pass a struct or reorder); tests: drag (any_of, contains), fields, loop_executor
    (contains), conformance probe (qualified-auto, nested conditional, pointer arithmetic). — measured with
    clang-tidy 22.1.8 and the repo config; Part 1 ruled dead suppressions out — cost: Task 20's gate fails otherwise.
12. **R12 — Land R2–R5 in Tasks 8–15 with widget-level tests, not as Task 18 fix-ups.** Task 18 then only adds the
    probe and expects 23/23. — the program note requires Part 3 to test what the probe cannot reach; measured: four
    cases would fail at Task 18 — cost: earlier tasks grow by a few tests each.
13. **R13 — Correct the commands and counts.** Task 2: `morph_tests "[caller]"` runs 6 cases; replace `timeout 20`
    with `perl -e 'alarm 20; exec @ARGV' …` (or a ctest `--timeout`); Task 5: name the two new warned targets instead
    of "N one higher". — measured — cost: a confused implementer stalls on a count.
14. **R14 — Accept the CI/platform exposure and say it.** linux-sanitizers, linux-all-features and clang-tidy fetch
    libunicode and UCD.zip; `windows-everything` gains TUI with no Windows job compiling it; fix the core-cpp block
    comment ("nothing is fetched on its behalf" no longer holds with TUI). State all three in the `tui` squash body.
    — inferred — cost: a network-flaky CI leg or a Windows break found late.
15. **R15 — Keep Task 6's `ARCH_INDEPENDENT` change, and name it.** It now also drops `ARCH_INDEPENDENT` for Qt
    installs carrying `morph_qt_impl`/`forms_qml`, which is correct (compiled archives) but beyond tui; it is install
    configuration, fixed in place per AGENTS.md; mention it in the squash body. — inferred — cost: none.
16. **R16 — Fix the `FrontendConfig::terminal` doc:** a given terminal is initialised too (mouse tracking, then
    `initialize()`), without the alternate screen. — measured: test terminals are initialised — cost: doc only.
17. **R17 — Clear the interrupt handler** (`_input->setInterruptHandler({})`) before `Session::run` returns:
    `Session` is destroyed before the `TuiRuntime` that holds a lambda capturing it. — inferred — cost: one line.
18. **R18 — Task 19's `frontend.md` follows R2–R7:** actionability (disabled ancestors, closed dialogs), handler
    copies, `moveChild` without detaching and the morph-order Tab walk, Multiple-mode reporting, hidden-cell
    columns, `runApplication`. — spec text must state current behaviour — cost: a spec that misleads Parts 6–10.
19. **R19 — No Part 3 action** for notes addressed elsewhere (RecordingBackend helper semantics for Parts 6–10;
    Part 1's Poller/ManualScheduler notes for Parts 4, 6, 8, 9). — cost: none.

## Per-task map of rulings

| Task | Rulings |
|---|---|
| T1 IoLoopDriver::Caller | — |
| T2 Caller cases | R13 |
| T3 specify drivers | — |
| T4 verify/squash ioloop | — |
| T5 option, target, LoopExecutor | R13, R14 |
| T6 install, presets, CI | R14, R15 |
| T7 layout solver | R11 |
| T8 widget base, stacks, leaves | R2, R3, R5 (arrangeStack extents), R6, R11, R12 |
| T9 grid, panel, scroll | R2 (Panel lets children act), R3, R11 |
| T10 fields | R2, R3, R11 |
| T11 select, menu, tabs | R2, R3, R11 |
| T12 dialog, busy, slider | R2 (frameEvent while open; Dialog lets children act), R3, R6 (dialog Tab walk), R11 |
| T13 table | R1 (`markedRows()`), R3, R4, R5, R12 |
| T14 backend | R6 (`focusFirst/Next/Prev` over the new walk) |
| T15 drag | R2, R3, R6 (mid-drag move test), R9, R11 |
| T16 LoopScheduler | R8, R11 |
| T17 frontend, session | R7, R10, R16, R17 |
| T18 conformance | R1, R11, R12 |
| T19 specs, maps, changelog | R18, R10 (doc build) |
| T20 verify/squash tui | R3 (ASan per-kind tests), R11, R14, R15 (squash body) |

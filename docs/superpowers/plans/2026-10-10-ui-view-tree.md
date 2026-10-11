# `morph::ui` view tree, mount and frontend seam — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan
> task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship the part of `morph::ui` that needs no document: the view tree (`view.hpp`), the backend contract
(`backend.hpp`, `IViewBackend`), `Mounted` (`mount.hpp`), the frontend seam (`frontend.hpp`), and the headless
`testing::RecordingBackend` with the backend-conformance cases (`testing/recording_backend.hpp`,
`testing/backend_conformance.hpp`), header-only in the base `morph` target.

**Architecture:** Nodes are aggregates (`ui::Text`, `ui::Column`, …) behind `ui::Node = std::shared_ptr<NodeData
const>`. A property is a `ui::Prop<T>`: a constant, or a slot — a reactive read with an optional stable `SlotId`
that a code-generating renderer refers to. `Mounted` walks the tree once: each node gets a widget from a typed
factory, each slot becomes an equality-gated `Computed` plus an `Effect` that calls one setter, each constant is set
once. Switch, Dialog, every Tabs page and every ForEach/Table row mount into a child `reactive::Scope`, adopted
widget-first, so teardown is bindings before widgets and children before parents, and owner Effects run before the
Effects of the scopes they own. Every widget callback runs inside `Runtime::widgetEvent`, untracked; an input whose
value is a slot is controlled: after a user event the mount re-asserts the slot in a posted turn. The frontend seam
is `AppContext`, `AppSource`, `Frontend`, `FrontendOption` and `selectFrontend`; `runApp` fixes the order in which
a frontend opens the source, mounts the shell and tears both down.

**Tech Stack:** C++23, header-only; `morph::reactive` (on `master`); `morph/util/datetime.hpp`
(`time::Timestamp`); the installed owner test kit `morph/testing/step_executor.hpp` and
`owner_probe_recorder.hpp`; Catch2 v3; Doxygen with `WARN_AS_ERROR`.

**Spec:** `docs/superpowers/specs/2026-10-04-declarative-ui-tui-design.md` (spec 1): §2 (headers), §5 (view tree,
mount, backend contract, `RecordingBackend`), §5b (frontend seam), §7 ("ui", "frontend seam", "backend
conformance"), §9 (docs). Spec 5 (`2026-10-07-ui-document-design.md`) §9 is the node palette. Spec 3
(`2026-10-04-qtquick-frontend-design.md`) §2, §4 and §7 fix what a slot id and a controlled widget mean for the
other renderer. Spec 6 (`2026-10-08-app-hosting-design.md`) §4 (`host.hpp`), §5 and §9 (`AppSource`, `Backend`).
Issue: #888. Program: #885.

**Starting point.** The 2026-10-04 part-2 plan (`29ca9b1f:docs/superpowers/plans/2026-10-04-declarative-ui-tui-2-ui.md`)
predates spec 5 and is stale; it is used for structure only. Its reviewed implementation is commit `69a18881` on
`origin/feature/declarative-ui` (ten tasks, each reviewed, and a final review; ledger in
`origin/docs/declarative-ui-execution-notes:execution-notes/part-2-ui/ledger.md`). That code compiles against
`morph::reactive` as on `master` with two test-only changes, and its 134 `[ui]` cases pass there. It is taken as the
base, and the revisions below bring it to the specs as they are on `master`. Where it and the specs disagree, the
specs win, unless this plan amends the spec (§ "Spec amendments").

**Delivery.** Branch `feature/888-ui-view-tree` from `origin/master`; this plan is its first commit; then one commit
per task; its own pull request, `Closes #888`.

## Global Constraints

- C++23, header-only, every new public header in the `morph` target's `FILE_SET HEADERS` (the install
  consumability check compiles every installed public header); no new CMake option, no new dependency.
- `morph::ui` depends on `morph::reactive`, `morph/core/executor.hpp` and `morph/util/datetime.hpp`; nothing under
  `include/morph/ui/` includes a toolkit, a terminal library, `morph/core/bridge.hpp` or anything under
  `morph/net`.
- Every file starts with `// SPDX-License-Identifier: Apache-2.0`, then `#pragma once` for headers.
- Naming (`.clang-tidy`): types `CamelCase`, functions and variables `camelBack`, private members `_camelBack`,
  constants `kName`; parameter names of at least three characters (`widgetId`, not `id`).
- Headers index containers with `.at()`, never `operator[]`.
- AGENTS.md: comments and docs state what the code does now and why; no history, no issue numbers.
- Doxygen `WARN_AS_ERROR = FAIL_ON_WARNINGS`: every public symbol, `detail` included, has a brief and complete
  `@param`/`@tparam`/`@return`. A concept takes no `@tparam`.
- Strict warnings (`-Weverything -Werror` on Clang), clang-tidy-diff clean.
- Misuse in this layer is reported through `reactive::detail::RuntimeCore::report` under `ui::detail::site`
  names, then refused; a test that triggers a report installs `morph::testing::OwnerProbeRecorder`.
- Strings crossing the backend contract are UTF-8. `Key = std::variant<std::int64_t, std::string>`, never a
  double.
- Tests are tagged `[ui]`; every test states the mutation that makes it fail, and the key ones are run.
- One local build at a time, `-j 4`, examples off (`-DMORPH_BUILD_EXAMPLES=OFF`).

## Review Focus

1. **Child-first remount.** A Switch case, a Tabs page, a Dialog's content and a ForEach/Table row tear down
   bindings before their widget and children before their parent, and a failed content mount leaves nothing behind
   (Task 1).
2. **ForEach identity.** A kept key keeps its widget and its row scope; a changed row updates in place; reorders use
   the fewest `moveChild` calls (longest increasing run); a duplicate key is reported and refused; a row whose
   mount throws leaves the old rows intact (Task 1).
3. **Owner-first order across mount scopes.** Content, page and row scopes are one level deeper than the scope that
   owns them, and `Mounted` can be placed under an outer scope's depth, so an owner's Effect that removes content
   runs before that content's bindings (Task 2).
4. **Controlled inputs.** A user event on an input whose value is a slot is re-asserted from the slot in a posted
   turn after the event's flush: a refused edit snaps back, a transformed one shows the raw text for that one
   turn, an accepted one costs no setter call. A constant is not a slot and is not re-asserted (Task 3).
5. **A setter called with what the widget already shows changes nothing**: a text input keeps its cursor
   (`RecordingBackend` models one; a conformance case pins it) (Task 3).
6. **Frontend seam order.** `runApp` unmounts the shell, then closes the source, before it returns, whether the
   loop returns or throws, and a `ready` delivered after the run ended is dropped (Task 4).
7. **`selectFrontend`** removes exactly the `--ui` arguments it used, stops at `--`, and leaves `args` untouched
   when it returns an error (Task 4).

## Revisions against the spec

| # | Spec on `master` | `69a18881` | This plan | Task |
|---|---|---|---|---|
| R1 | spec 1 §2, §7: the test kit is installed under `morph/testing/` | Tests include `tests/owner_probe_recorder.hpp` | Include `<morph/testing/owner_probe_recorder.hpp>` | 1 |
| R2 | spec 1 §3 "Widget events": a flush due inside a widget event is deferred, not reported | Tests count `site::kFlushInWidgetEvent` | Those counts are removed; the behaviour checks (the handler sees the old value, the widget survives its handler) stay | 1 |
| R3 | spec 1 §1 "Ownership order"; §5 "mount their content into a child scope" | Content, page and row scopes are depth 0 | Each is `Scope(rt, owner.depth() + 1)`; `Mounted` takes the depth of the scope it is placed under | 2 |
| R4 | spec 1 §5: every property is a constant or a slot, "a reactive value with a stable id"; spec 3 §3 `v.s<slot>` | `Prop<T>` is a constant or an anonymous `std::function` | `ui::SlotId`; `ui::slot(id, read)` makes a slot with an id, a bare callable a slot without one; `Prop::slotId()` | 2 |
| R5 | spec 1 §5 "Input widgets are controlled"; spec 3 §7 | A user edit stays on the widget whatever the application does | Controlled inputs (Review Focus 4); `RecordingBackend` dismisses a dialog as a real one does (it closes, then the handler runs) and models a text cursor; conformance cases for both rules | 3 |
| R6 | spec 1 §5 "A setter called with the value the widget already shows changes nothing" | "may be called with an unchanged value" | Contract text, `RecordingBackend` cursor, conformance case | 3 |
| R7 | spec 1 §5b: `AppSource`, `Bundle`, `ConnectError`, `Backend`, `FrontendError`, `Frontend::run(AppSource&)`, `selectFrontend(span, std::vector<std::string>&, env)` returning `std::expected` | `Application`, `ApplicationFactory`, `Frontend::run(factory)`, `selectFrontend(span, argc, argv)` throwing `FrontendSelectionError`, `runApplication` | Rewritten to §5b. `runApp(AppContext&, AppSource&, ShellFactory, loop)` keeps the old helper's job (one place fixes the teardown order), over a source instead of a factory | 4 |
| R8 | spec 1 §7 "frontend seam": `refreshEvery` on a `ManualScheduler` | Not tested here | A `Query` built over `AppContext::scheduler()` refetches when a `ManualScheduler` advances, and not while a fetch is pending | 4 |

## Spec amendments

Planning against the specs found three places where the part cannot follow them as written. Each is fixed in the
spec in this pull request and said on #888.

- **S1 — spec 1 §5b: `AppSource::close()`.** §5b requires that "screens and connections are destroyed before the
  runtime", and `run` builds the runtime. But the connections are made by `AppSource::open`, and the source is
  `main`'s and outlives `run`, so nothing in §5b's interface lets `run` destroy them. The source gains
  `virtual void close() = 0`: it releases what `open` made and drops a `ready` not yet delivered; `run` calls it
  after unmounting and before the runtime goes.
- **S2 — spec 1 §5: the palette this part carries.** Spec 5 §9 is the normative palette of the document. The view
  tree carries its base kinds, its further kinds (banner, badge, progress, steps, keyValue, emptyState, drawer,
  splitter, collapsible, dropZone) and `custom`, the common properties (`a11y`, `testId`, `tooltip`, `keys`,
  `autofocus`, `surface`), the input decorations (`readonly`, `required`, `errors`, `stale`), `onCommit` and the
  richer menu items (Task 6). `boundary` and `customize` are not view-tree kinds: a boundary's choice depends on the
  queries its children read, and a customization point's editor on the customization store, both of which the
  interpreter (#890) owns; it lowers them to view-tree kinds. §5 says so.
- **S3 — spec 1 §2: `host.hpp` lands with hosting.** Spec 6 §4's `ScreenHost` needs `ParamMap =
  std::map<std::string, Value>`, where `Value` is the document's typed value (spec 5 §3), defined with the document
  in #890, and its only implementers are the Qt Quick renderer (#891) and the hosting part (#893), whose tests (spec
  6 §12 "Host contract") it ships with. Landing it here would invent `Value`. §2 says `host.hpp` comes with hosting.

Not an amendment, recorded so the next part does not rediscover it: spec 4's `main` sketch calls
`selectFrontend(built, argc, argv)` and dereferences the result; spec 1 §5b's signature (an argument vector the
call edits, and an `std::expected` result) is the normative one, and the sketch is corrected to it.

Event sinks stay `std::function`s. Spec 3's `a.fire(<sink>)` numbers sinks, and the generator (#891) is where a
sink id would be needed; adding one here would be a field nothing reads.

## Files

| File | Responsibility |
|---|---|
| `include/morph/ui/view.hpp` | `Action`, `Key`, `SlotId`, `slot()`, `Prop<T>`, the enums, `Sizing`, `LayoutHints`, `Common`, every node aggregate, `NodeData`, `Node`, the builders, `switchOn`, `forEach`, `table`, `detail::ForEachModel`/`ForEachSession`/`RowSlot`/`TypedForEach` |
| `include/morph/ui/backend.hpp` | `Widget`, `ContainerWidget`, one widget interface per node kind, `IViewBackend` |
| `include/morph/ui/mount.hpp` | `detail::site`, `detail::Mounter` (bindings, controlled inputs, content/page/row scopes, keyed reconcile), `Mounted` |
| `include/morph/ui/frontend.hpp` | `Scheduler`/`TimerHandle` aliases, `AppContext`, `Bundle`, `ConnectError`, `Backend`, `AppSource`, `Frontend`, `FrontendOption`, `FrontendError`, `EnvironmentReader`, `processEnvironment`, `selectFrontend`, `MountedShell`, `ShellFactory`, `runApp` |
| `include/morph/ui/testing/recording_backend.hpp` | `testing::RecordingBackend`: fake widgets, operation log, golden dump, interaction helpers, text cursor |
| `include/morph/ui/testing/backend_conformance.hpp` | `testing::ConformanceProbe`, `ConformanceCase`, `conformanceCases()` |
| `tests/test_ui_{view,recording_backend,mount,structure,foreach,table,frontend,conformance,controlled}.cpp`, `tests/test_ui_{common,field,kinds}.cpp` (Task 6), `tests/ui_echoing_backend.hpp`, `tests/ui_test_support.hpp` | The §7 tests |
| `docs/spec/ui/view_tree.md`, `backend_contract.md`, `frontend.md` | Authoritative specs |
| `CMakeLists.txt`, `tests/CMakeLists.txt`, `docs/spec/README.md`, `docs/ARCHITECTURE.md`, `CHANGELOG.md` | Registration, maps, changelog |
| `docs/superpowers/specs/2026-10-04-declarative-ui-tui-design.md`, `2026-10-04-examples-migration-design.md` | S1–S3 and the spec 4 sketch |

## Build and test commands (used by every task)

```bash
cmake -S . -B build/ui -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMORPH_BUILD_EXAMPLES=OFF   # once; prints "-Weverything verified"
cmake --build build/ui --target morph_tests -j 4
./build/ui/tests/morph_tests "[ui]"
```

A mutation is applied by hand, rebuilt with the same command, run against the named test, and reverted (`git diff`
checked) before the commit.

---

### Task 1: The view tree, the backend contract, `Mounted` and `RecordingBackend` (R1, R2)

**Files:** create `view.hpp`, `backend.hpp`, `mount.hpp`, `testing/recording_backend.hpp`,
`testing/backend_conformance.hpp` from `69a18881`; the tests `test_ui_{view,recording_backend,mount,structure,
foreach,table,conformance}.cpp`, `ui_echoing_backend.hpp`, `ui_test_support.hpp`; register every header in the
`morph` FILE_SET after `reactive/control.hpp` and every test file after `test_reactive_control_bridge.cpp`.

**Apply:** R1 (includes), R2 (drop the `kFlushInWidgetEvent` counts). `frontend.hpp` and its test wait for Task 4.

**Tests** (already in the base, each with its mutation in the base's review record):

| Test | Mutation |
|---|---|
| a constant never binds (no Effect, no Computed) | bind constants too: the runtime's node count grows |
| one setter call per real change | drop the Computed's equality gate (bind the binding straight into the Effect) |
| switch, tabs and dialog remount child-first (log order `destroy` child before parent, binding before widget) | in `Scope::clear`, or in `mountContent`, adopt the content's children before its widget |
| a failed case/page/dialog content mount leaves nothing behind | mount into the live scope instead of a fresh one |
| ForEach identity: kept key keeps its widget id across a refetch and a reorder; minimal `moveChild` count | remount every row on a change; or move every row in `reorderChildren` |
| int64 and string keys; `7` and `"7"` are different rows | compare keys by their text |
| duplicate key refused and reported once per snapshot | keep the later duplicate |
| a throwing row mount leaves the old rows intact | build the new row list in place |
| a hidden or disabled ancestor blocks a helper | check the widget's own flags only |

- [ ] Steps: copy, apply R1/R2, build, `[ui]` green, run the child-first and ForEach-identity mutations, commit
  `ui: view tree, backend contract, mount and RecordingBackend`.

---

### Task 2: Child scopes, `Mounted`'s depth, and slots (R3, R4)

**Files:** `view.hpp`, `mount.hpp`, `tests/test_ui_view.cpp`, `tests/test_ui_structure.cpp`.

- `SlotId` (`std::uint32_t`); `template <class F> struct SlotBinding { SlotId id; F read; }` made by
  `ui::slot(id, read)`; `Prop<T>` holds `std::variant<T, Slot>` with `Slot{std::optional<SlotId> id;
  std::function<T()> read;}`; `slotId()` returns the id or `nullopt`; `binding()` keeps returning the read.
- `Mounter::mountContent`, `mountPage`, `mountRow` make `Scope(rt, owner.depth() + 1)`; Tabs' page scope and the
  row list are made by `scope.child()`. `Mounted(rt, backend, root, parent = nullptr, depth = 0)`.

**Tests:**

| Test | Mutation |
|---|---|
| a slot keeps its id; a bare callable has none; a constant has none | drop the id in the `SlotBinding` constructor |
| an owner Effect that unmounts content runs before the content's bindings, even when the content is older: a `Mounted` at depth 1 whose Switch case binds a map entry, and an outer depth-0 Effect, made after it, that destroys the `Mounted` when the entry goes; one write removes the entry — nothing throws (no `kEffectThrew`) | make content scopes depth 0; separately, ignore `Mounted`'s depth |

- [ ] Steps: tests first, implement, mutations, commit `ui: content scopes are child scopes, and properties are
  slots with ids`.

---

### Task 3: Controlled inputs, and setters that change nothing (R5, R6)

**Files:** `mount.hpp`, `backend.hpp` (contract text), `testing/recording_backend.hpp`,
`testing/backend_conformance.hpp`, `tests/test_ui_mount.cpp`, `tests/test_ui_conformance.cpp`.

- `Mounter::controlled(scope, prop, apply)`: for a slot, the binding as before, plus a re-assert token owned by the
  scope; the event wrapper, after `runCallback` returns (after `widgetEvent`, so after the flush is posted), posts
  one task to the owner that, if the token is alive, reads the slot untracked and calls the setter when the slot
  differs from what the user requested. A constant returns no token: it is not a slot, and the widget keeps the
  user's value, as a QML literal does.
- Applied to text input (`onChange`, `onSubmit`), checkbox, select, tabs (bar highlight), panel `collapsed`,
  dialog `open` (after a dismissal), slider, date-time input, file picker and table selection. The wrapper is
  installed when the slot exists even if the application gave no handler: a bound value without a handler is
  read-only.
- `RecordingBackend`: `dismiss` closes the dialog (by the user, unlogged) and then calls the handler; a text input
  keeps a cursor: a user edit puts it where the helper says (default: the end), `setText` with the shown text keeps
  it, `setText` with another text puts it at the end; `cursor(widgetId)` reads it.
- Conformance probe gains `cursorOf` and `moveCursor`; cases: "a setter with the shown text keeps the cursor",
  "a refused edit snaps back after one turn", "a transformed edit shows the raw text for one turn, then the
  slot's", "a dismissed dialog the document keeps open is shown again".

**Tests:**

| Test | Mutation |
|---|---|
| a refused edit snaps back after `settle`, and not before the flush | skip the posted re-assert |
| an accepted edit costs no setter call | re-assert unconditionally |
| a transformed edit shows the raw text, then the transform | re-assert inside the widget event (before the flush) |
| a constant text input keeps the user's text | re-assert constants too |
| a re-assert posted for a widget that is gone does nothing (ASan clean) | hold the token strongly in the posted task |
| a checkbox, select, slider, tabs bar, panel, dialog and table selection each snap back | per kind: drop its `controlled` call |
| conformance: the setter with the shown text keeps the cursor | `FakeTextInput::setText` always moves the cursor |

- [ ] Steps: tests first, implement, mutations, commit `ui: input widgets are controlled by their slots`.

---

### Task 4: The frontend seam (R7, R8)

**Files:** `frontend.hpp`, `tests/test_ui_frontend.cpp`; S1 in spec 1 §5b.

```cpp
struct Bundle { std::string applicationId; std::string manifestDigest;
                std::map<std::string, std::string, std::less<>> documents; };   // id -> verified JSON text
struct ConnectError { enum class Kind : std::uint8_t { Unreachable, NoUiService, Unsupported, Unverified, Refused };
                      Kind kind; std::string message; };
struct Backend { std::string name; };
class AppContext { runtime(); executor(); scheduler(); ioLoop(); quit(int = 0); };
class AppSource { open(AppContext&, std::function<void(std::expected<Bundle, ConnectError>)> ready);
                  switchBackend(Backend, std::function<void(std::expected<void, std::string>)> done); close(); };
class Frontend { name() const; run(AppSource&); };
struct FrontendOption { std::string name; std::function<bool()> usable; std::function<std::unique_ptr<Frontend>()> make; };
struct FrontendError { enum class Kind : std::uint8_t { MissingName, UnknownName, NoneUsable, NotMade };
                       Kind kind; std::string requested; std::vector<std::string> built; std::string message() const; };
using EnvironmentReader = std::function<std::optional<std::string>(std::string_view)>;
EnvironmentReader processEnvironment();
std::expected<std::unique_ptr<Frontend>, FrontendError>
    selectFrontend(std::span<FrontendOption const>, std::vector<std::string>& args, EnvironmentReader const& = processEnvironment());
class MountedShell { virtual ~MountedShell(); };
using ShellFactory = std::function<std::unique_ptr<MountedShell>(AppContext&, std::expected<Bundle, ConnectError>)>;
int runApp(AppContext&, AppSource&, ShellFactory const&, std::function<int()> const& loop);
```

- `selectFrontend`: every `--ui=<name>` and `--ui <name>` before `--` is read, the last wins, and all of them are
  removed on success; an argument starting with `--` is never a name; an empty final name is `MissingName`. Then a
  non-empty `MORPH_UI`, then the first option whose `usable()` holds (empty counts as usable). On an error `args`
  is unchanged.
- `runApp`: opens the source with a `ready` that holds the run's state weakly; `ready` mounts the shell through the
  factory (a factory that throws quits the context, and `runApp` rethrows after teardown); a second `ready` is
  ignored. The loop runs; then, whether it returned or threw, the shell is destroyed, then `source.close()` runs.
  The frontend destroys its runtime after `runApp` returns.

**Tests:**

| Test | Mutation |
|---|---|
| `--ui=` beats `MORPH_UI` beats order; `--ui name`; last wins; `--` stops; unknown name lists the built ones; nothing usable; option that makes nothing | per rule: drop it |
| the `--ui` arguments are removed, the rest kept in order, and `args` is untouched on error | erase only the first; or erase before validating |
| destruction order: shell, then `close`, then the runtime, for a returning and for a throwing loop | swap the shell reset and `close()`; or skip teardown on a throw |
| a `ready` after the run ended mounts nothing | capture the state strongly |
| a source that fails to connect hands the factory the `ConnectError` | drop the error branch |
| `refreshEvery` on a `ManualScheduler` through `AppContext::scheduler()`: a tick refetches, a tick while pending does not | build the Query without the scheduler (it throws) — and, for the skip, ignore `pending()` in the reactive core |

- [ ] Steps: tests first, implement, mutations, commit `ui: the frontend seam — AppContext, AppSource, Frontend and
  selectFrontend`.

---

### Task 5: Specs, maps and changelog

- `docs/spec/ui/view_tree.md`, `backend_contract.md`, `frontend.md` from `69a18881`, updated for R3–R8 and S1–S2.
- `docs/spec/README.md` (a "Declarative UI" group), `docs/ARCHITECTURE.md` (namespace row and header map),
  `CHANGELOG.md` `[Unreleased]` → `### Added`.
- Spec 1 §2, §5, §5b for S1–S3; spec 4's `main` sketch.
- Docs build with `WARN_AS_ERROR`.

Commit `ui: specify the view tree, the backend contract and the frontend seam`.

---

### Task 6: The rest of the palette

A second implementation of this part, built from the same base, carried the common properties, the field state,
the richer menus and the further kinds. They are ported onto Tasks 1–5, one commit each, under Tasks 1–5's design:
a bound value a user can change is controlled.

**Files:** `view.hpp`, `backend.hpp`, `mount.hpp`, `testing/recording_backend.hpp`,
`testing/backend_conformance.hpp`; `tests/test_ui_{common,field,kinds}.cpp`, `tests/test_ui_{mount,controlled,
conformance,recording_backend}.cpp`; the three `docs/spec/ui/` pages; S2.

- **Common properties.** `Common` gains `a11y{name, role}`, `testId`, `tooltip`, `surface`, `keys`
  (`KeyBinding{chord, onPress}`) and `autofocus`; `Widget` gains a setter each and `focus()`. A chord goes to the
  innermost widget that declares it. The first `autofocus` widget in document order is focused once a mount pass
  completes; content mounted later is a pass of its own, and a pass that throws gives its candidate back.
- **Field state and commit.** `FieldState{readonly, required, errors, stale}` on Text and the six inputs through a
  `FieldWidget` base. `TextInput::onCommit` (Enter before the submit, or focus leaving after an edit) requests no
  value, so it is not re-asserted.
- **Menus.** `MenuItem{label, onSelect, icon, keys, checked, enabled, items}`; `setItems` takes the whole
  `MenuEntry` tree; `onActivate` reports a path, and only for an enabled leaf under enabled ancestors.
- **Further kinds.** Each kind's widget interface and factory. Drawer shares the Dialog's mount, so its `open` is
  controlled the same way; Splitter `sizes` and Collapsible `open` are controlled inputs; a Banner's dismissal
  changes nothing it shows; DropZone takes a drop whole or refuses it whole; Custom mounts its fallback.
- **Kept from Tasks 2 and 4, where the second implementation differed:** slots are `ui::slot(id, read)` with
  `Prop::slotId()`; `selectFrontend` keeps §5b's signature and `FrontendError::Kind`, and takes the arguments with or
  without the program name, so no `commandLineArguments` helper is added.
- Conformance cases 27–34, each failed by the dead-input fault probe; a probe that drives and reads nothing fails
  every case that observes something.

**Tests:** each test states its mutation; among those run: RecordingBackend's dismiss leaving a drawer open (the
refused-dismissal test fails), and Collapsible bound without `controlled` (the every-kind snap-back test fails).

---

### Task 7: Gates

- [ ] Full `morph_tests` on the Debug build (`[ui]` and everything else).
- [ ] Docs build (`MORPH_BUILD_DOCUMENTATION=ON`, target `doc`).
- [ ] A `-Wdocumentation` syntax check of the new headers with clang 22, if available.
- [ ] clang-format on every touched file.
- [ ] Push, open the PR: `Closes #888`, what was measured on which configuration, the mutations and their results,
  inline review reasoning, spec amendments, what was not verified.

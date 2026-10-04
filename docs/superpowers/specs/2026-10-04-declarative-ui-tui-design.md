# Declarative UI in morph, with a TUI frontend: design (spec 1)

A morph application describes its UI once, as state, controllers and a view tree of bindings, with
no toolkit code, and its `main` injects the frontend that renders it: a terminal UI or Qt Quick,
chosen at runtime. The same view tree mounts on a headless test backend, which fixes the contract
every frontend implements. The controller layer — the code that today lives in hand-written
`QObject` controllers and QML bridges — becomes declarative too: it says *what* it depends on and
*what* a write invalidates, and the runtime decides when to talk to the model.

This is spec 1 of a program delivered on one branch as one pull request (§1, "The program").

## Contents

[1 Intent](#1-intent) · [2 Packaging](#2-packaging-and-repository-boundaries) ·
[3 Reactive core](#3-the-reactive-core-morphreactive) · [4 Store](#4-store-and-async-morphreactive) ·
[4b Control](#4b-declarative-control-query-mutation-subscription) ·
[5 View IR](#5-the-view-ir-and-mount-morphui) · [6 TUI](#6-the-tui-frontend-morphtui-morph_build_tui) ·
[7 Tests](#7-tests-catch2) · [8 Examples](#8-examples) · [9 Docs](#9-docs) · [10 Risks](#10-risks)

## 1. Intent

**Goals:**
- morph is the framework for future UI applications.
- UIs are declarative and toolkit-agnostic, with Qt and a TUI (built on core-cpp's `core::tui`).
- **Controllers are declarative.** The list/detail controller shape — a list, a selection, a
  detail that reloads when the selection changes, `busy`, `lastError`, writes that refresh what
  they changed — is the majority pattern among morph's QML-facing classes (12 of 19, measured in
  `docs/analysis/qml-bridge-boilerplate.md`). It is written imperatively today, in
  `examples/bank/gui/controllers/` and the ladder `*_qml_bridge`s, and that imperative sequencing
  is where its defects live (see §4b). This spec makes that shape declarative.
- MFC is not a target: Qt covers Windows.

**Design decisions:**
- Two stacked layers: a typed view tree (signals, Store, controllers, nodes) is the intermediate
  representation (IR); morph's schema-driven forms, collection views, wizards and app shell render
  through it (spec 2), replacing the shipped QML renderer.
- Fine-grained signals, not Elm-style re-render plus diff. State and control live in
  **`morph::reactive`**, the view in **`morph::ui`**. An exception escaping an Effect is a defect.
- `ViewState` is a struct of `Signal<T>` fields. In morph, *model* already means the domain model
  a `BridgeHandler<Model>` talks to; UI state must not be confusable with it.
- Server interaction is `Query`, `Mutation` and `Subscription` nodes (§4b), not callbacks
  sequenced by hand.
- **The frontend is injected** (§5b): an application is a factory the frontend calls with an
  `AppContext`; `main` is the composition root that picks the frontend at runtime.
- The palette covers every screen of the migrated examples, drag-and-drop included. A TUI app runs
  on **one loop**: `IoLoop` gains a caller-driven mode.

**Success for this spec:**
- A morph application describes its UI once, as data plus bindings, with no toolkit code, and runs
  in a terminal through `MORPH_BUILD_TUI`.
- The same tree mounts on a headless `RecordingBackend`, which proves the contract every frontend
  implements; a backend-conformance suite runs the same scripted cases against each backend.
- `ui::selectFrontend` picks a frontend from `--ui=`, `MORPH_UI` or the environment, and the
  application code never names one.

### The program (one branch, one pull request)

| Spec | File (`docs/superpowers/specs/2026-10-04-…`) | Delivers |
|---|---|---|
| 0 | `core-cpp-mouse-design.md` | Standard VT mouse tracking and pointer capture in core-cpp 0.7.0; morph's pin moves to 0.7 |
| 1 | `declarative-ui-tui-design.md` (this) | `morph::reactive`, `morph::ui` with the frontend seam, `IoLoopDriver::Caller`, `morph::tui` |
| 2 | `forms-engine-design.md` | The C++ forms engine (runtime JSON and typed), rendered through the view tree |
| 3 | `qtquick-frontend-design.md` | `morph::qt_quick`, the Qt Quick frontend, native and WebAssembly |
| 4 | `examples-migration-design.md` | Every example as a toolkit-free app library plus one binary choosing its frontend; the QML renderer retired |

### Requirements the reactive core meets

core-cpp's `core::rx` / `core::mvc` prototype is not reused. Each requirement below is one it did
not meet, and each has a test in §7.

- **Graph:** glitch-free (an Effect reading `A` and `Computed(A)` never sees a stale Computed);
  unsubscribe on destruction in both directions; dependencies pruned when a run stops reading
  them; N writes in a batch are one flush; equality skip; cycle guard; a throw always restores the
  tracking context; nodes are non-copyable and non-movable; `Computed<T>` needs no default
  constructor; no Effect runs synchronously inside `set()`, so writes are safe from Qt handlers and
  `Completion` callbacks.
- **Messages:** exhaustive at compile time; `send()` inside an update is refused; composition, not
  inheritance-for-reuse; view state, control and the domain model are three separate things.
- **View:** no fixed-coordinate drawing, no per-frame geometry, one table per menu, one dispatch
  per input event.

## 2. Packaging and repository boundaries

**`morph::reactive`** (`include/morph/reactive/`)
- Header-only, in the base **`morph`** target's FILE_SET (no new option or dependency); depends on
  the standard library and `morph/core` (executor, completion, callback_scope, bridge, owner probe).
- Headers: `runtime.hpp`, `signal.hpp` (Signal, Computed, Effect), `scope.hpp`, `store.hpp`
  (Store, `ExhaustiveUpdate`, `request()`), `control.hpp` (Query, Mutation, Subscription,
  `errorMessage`); graph internals in `detail/graph.hpp`.

**`morph::ui`** (`include/morph/ui/`)
- Header-only, in the base `morph` target. Depends on `morph::reactive`, never the reverse.
- Headers: `view.hpp` (`Prop`, `Action`, `Key`, nodes, helpers), `backend.hpp` (widget interfaces,
  `IViewBackend`), `mount.hpp`, `frontend.hpp` (§5b), `testing/recording_backend.hpp` and
  `testing/backend_conformance.hpp` (the scripted cases every backend must pass).

**`morph::tui`** (`include/morph/tui/`, sources in `src/tui/`)
- A **compiled STATIC library** (`morph_tui`, alias `morph::tui`), gated by **`MORPH_BUILD_TUI`**
  (default OFF), linking `core::tui` and `morph` PUBLIC, so widgets don't recompile in every
  consumer. CONTRIBUTING gains one sentence for this exception to "compiled code only for Qt MOC":
  an optional frontend component may be compiled when it wraps a compiled toolkit.
- Public headers: `loop_executor.hpp`, `backend.hpp`, `frontend.hpp` (`tui::Frontend`). Widgets,
  the layout solver and the event pump are private `.hpp`/`.cpp` pairs in `src/tui/`.
- Registered like `morph::net` and `morph_qt_impl` together: `_morph_optional_components`, the
  header-set check, the install component loop plus an explicit install/export of the library,
  `cmake/morphConfig.cmake.in`, `apply_sanitizers`, strict warnings, the `*-everything` presets
  and CI.
- When ON: core-cpp gets `CORE_CPP_WITH_TUI ON` and `CORE_CPP_WITH_IMAGES OFF`; morph provides
  **libunicode 0.9.3** first (`find_package(libunicode 0.9.3 CONFIG QUIET)`, else `CPMAddPackage`,
  the found-first-then-fetched pattern of the root `CMakeLists.txt`'s Tracy block); configure
  fails clearly if a *found* core-cpp lacks `core::tui`. Native only: core-cpp forces the TUI off
  under Emscripten.

**core-cpp pin:** **0.7** (spec 0), which adds standard mouse tracking, pointer capture and a
public `Screen::componentAt` to what 0.5 already ships (`core::tui`, `TuiRuntime`, `EventLoop::post`,
`ScriptedInputSource`, `Screen::showOverlay`, `Screen::renderedBuffer`, `canvasToString`,
`InputField`'s multiline and masked modes). morph's widgets are `core::tui::Component` subclasses.
A generic `core::tui` defect found later is fixed in core-cpp and released before morph's pin moves
again; the CMake bound and the config template are minor-exact.

**Conventions:** AGENTS.md and CONTRIBUTING.md — present-tense comments with no issue numbers; full
Doxygen (`WARN_AS_ERROR`); a spec per subsystem listed in `docs/spec/README.md`; `ARCHITECTURE.md`
maps; CHANGELOG `[Unreleased]` (examples-only changes get none); Catch2; strict warnings;
clang-tidy-diff clean.

## 3. The reactive core: `morph::reactive`

**Types:**

| Type | Behaviour |
|---|---|
| `Signal<T>` | `get()` is tracked, `peek()` untracked. `set(v)` skips equal values when `std::equality_comparable<T>`; `mutate(f)` always notifies. |
| `Computed<T>` | Lazy, cached in a `std::optional<T>` (so `T` need not be default-constructible), equality-gated. An unchanged result stops propagation. |
| `Effect` | Runs once in its constructor, then after any of its dependencies changes. |
| `Scope` | Owns effects, computeds and adopted objects; destroys them in reverse creation order. A mounted view, a Switch case and a ForEach row each have one. |

**Runtime:** `Runtime(morph::exec::IExecutor& owner, RuntimeOptions = {})`; no `thread_local`, no
global — every node takes the `Runtime&`. `RuntimeOptions` carries `maxEffectRunsPerFlush` and
`afterFlush`, the hook a frontend uses to schedule a redraw.

**Scheduling:** a `set()` outside a batch is a batch of one; `batch(f)` nests. When the outermost
batch ends with effects pending and no flush requested, the runtime calls `owner.post(flush)`
**once** — the coalescing `QtExecutor` lacks. No flush runs synchronously inside `set()`; writes
made by effects during a flush are processed by that flush. The posted flush holds the core weakly
and is a no-op once the runtime is gone.

**Algorithm** (push-dirty/pull-value, colours Clean, Check, Dirty, as in Reactively and Preact):
`set()` marks direct observers Dirty and propagates Check, queuing effects; `flush()` pulls each
queued effect through its computeds in dependency order and runs it only if a source really changed
(glitch-free); dependencies are re-tracked every run, so stale ones are pruned; destructors unlink
both ways; every node is non-copyable and non-movable.

**Errors and misuse:**
- The RAII `TrackingFrame` always restores the previous tracking context. A throwing `Computed`
  rethrows to its reader and retries on the next read.
- Each misuse is **reported, then refused**. The report is `detail::noteOwner(site, owner, false)`:
  it asserts in a debug build, and a test's installed probe sees the site instead. The refusal is
  the same in every build, so behaviour does not depend on `NDEBUG`.
  - An exception escaping an `Effect` (a defect): that flush stops; effects still queued stay
    queued for a new flush.
  - An effect re-run more than `maxEffectRunsPerFlush` times in one flush (a write cycle): the
    flush stops and its queue is dropped.
  - `set()` inside a Computed: the write is dropped. A Computed reading itself: the read throws
    `std::logic_error`. `send()` inside an update: the Msg is dropped.
  - An operation off the owner — `OwnerAffinity`'s predicate, `runningOn(owner)` with the
    constructing-thread fallback for Qt slots and `main()`: the operation is dropped.
  - A Runtime destroyed while nodes are alive: nodes keep the core alive, and any flush is a no-op.

**Relation to the bridge:** this layer is UI-side only. It consumes `Completion` and `subscribe`
like any caller and never enters `BridgeHandler`; nothing in the bridge knows a reactive runtime
exists. `docs/spec/reactive/signals.md` says so explicitly.

## 4. Store and async: `morph::reactive`

**`Store<ViewState, Msg>`** holds user-intent state: what is selected, what is being typed, which
tab is open.
- `ViewState` is a struct of `Signal<T>` fields; `Msg` is a `std::variant`.
  `Store(Runtime&, Init, Update)`: `init(rt)` returns the `ViewState` by prvalue (signals cannot
  move), e.g. `Workout{ .powerWatts{rt, 0}, .laps{rt, {}} }`; `Update` must satisfy
  **`ExhaustiveUpdate`** — invocable as `(ViewState&, Alt const&)` for every alternative, checked at
  construction.
- `send(msg)` runs the update in one batch; `state()` returns `ViewState const&` (a field's `get()`
  tracks exactly that field); `action(msg)` returns a `ui::Action`.

**The low-level async primitive:** `request(store, handler, scope, action, toMsg, toFailMsg)`
executes `action` and sends `toMsg(result)` or `toFailMsg(exception_ptr)` as one message, on the
handler's GUI executor (the Store's owner), each delivery one batch. Errors travel **per call**
inside the message, never as a shared error string. `Query` and `Mutation` are built on it; a
controller reaches for §4b first.

**Lifetime rule (documented and tested):** the Store, the Runtime and every mounted view outlive any
callback that can still fire. The `CallbackScope` is the **last** member of the owning object, so it
dies first and gates anything in flight.

**Out of scope:** Elm-style `Cmd` values. Server interaction that a state change implies is
expressed as a `Query` keyed on that state (§4b), so an update never needs to issue a request.

## 4b. Declarative control: Query, Mutation, Subscription

Header `morph/reactive/control.hpp`: reactive nodes like `Computed` — non-copyable, non-movable,
owned by a controller or a `Scope`, affinity-checked against the Runtime's owner, which must be the
handler's GUI executor (a delivery off it is reported and dropped).

### What this replaces

The imperative controller (`TransactionController` in `examples/bank/gui/controllers/`) sequences
calls by hand, and two defects follow from the shape: **stale replies win** (select A, then B; A's
late reply shows A's history under B), and **invalidation is a call graph** (what a write refreshes
lives in whichever `.then` remembered to call `refresh()`).

### `Query<A>` — a derived async resource

```cpp
template <class A, class R = morph::model::ActionTraits<A>::Result> class Query;
Query(Runtime& rt, BridgeHandler<M, S>& handler, std::function<std::optional<A>()> key);
Query(Runtime& rt, std::function<Completion<R>(A const&)> fetch, std::function<std::optional<A>()> key);
```

The model is deduced from the handler; the fetcher constructor is the test seam and the way to
query a non-bridge source.

- An internal Effect reads `key`, **tracked**. A changed key (equality-gated when `A` is
  `std::equality_comparable`) is fetched under a new generation of the query's `CallbackScope`
  (`reset()`, the supersede verb). `nullopt` is **idle**: nothing in flight, value and error
  cleared.
- **Latest wins:** a reply from a superseded generation is dropped, success and failure alike, and
  a stoppable call is asked to stop.
- Tracked reads: `pending()`; `value()` — the last result, kept while a refetch is in flight so a
  list does not blank; `error()` — cleared by the next success. `refetch()` re-issues the current
  key; idle stays idle.
- `refreshOn<Pub>(handler)` re-fetches when the bridge publishes a `Pub` on the handler's instance
  (the handler's one slot for `Pub`: two consumers use two handlers); `QueryOptions::refreshEvery`
  re-fetches on a `Scheduler` timer (§5b).
- Each delivery is one batch; the query's `CallbackScope` is its last member, so a destroyed query
  gates replies in flight.

### `Mutation<A>` — a command

```cpp
Mutation(Runtime& rt, BridgeHandler<M, S>& handler, MutationOptions options = {});
Mutation(Runtime& rt, std::function<Completion<R>(A)> run, MutationOptions options = {});
```

- `run(A)` issues one call. Tracked reads: `pending()` (an in-flight counter), `error()` (most
  recent failure, cleared by the next success), `lastResult()`.
- `MutationOptions::invalidates` lists queries (declared before the mutation, so they outlive it);
  a success writes the result and refetches each, **in one batch**: one flush, one frame.
- `action(make)` returns a button's `ui::Action`: `make` returns the action, or `nullopt` when the
  inputs are invalid; the button's `enabled` binds to the same validity `Computed`, so a disabled
  button and a refused run agree. An idempotency key minted in `make` is fresh per click.

### `Subscription<R>`

Built from `(Runtime&, BridgeHandler<M, S>&)`. `latest()` is a tracked `std::optional<R>` fed by
`handler.subscribe<R>`. It owns its `CallbackScope` like a query does.

### `errorMessage(std::exception_ptr) -> std::string`

The one place an `exception_ptr` becomes display text: `what()` for a `std::exception`, a fixed
fallback otherwise. Views bind to `errorMessage(q.error())` inside a `Computed`.

### The controller

A **controller** is a plain user struct, not a framework base class. It owns, in this order:
handlers, a `Store` (if it has user-intent state), Queries, Mutations, Subscriptions, and
`Computed` projections — row formatting, masking, aggregates such as an account total. Its
`CallbackScope`s are its last members.

- A controller includes nothing from `morph::ui`, `morph::tui` or any toolkit. It is testable with
  a `MainThreadExecutor` and a local bridge, with no backend mounted.
- The view is bindings only: every conditional, format and validation lives in the controller,
  which is `examples/TESTING.md`'s rule for QML carried over to the IR.
- Because the same controller drives any backend, the Qt Quick frontend (spec 3) reuses it
  unchanged.

## 5. The view IR and mount: `morph::ui`

**Nodes** are immutable shared data, **not templated on `Msg`**. Events are `Action` or typed
callbacks (`onChange(std::string)`, `onToggle(bool)`, `onSelect(Key)`); strings are UTF-8, and
backends convert. `Prop<T>` holds a constant or a `std::function<T()>` binding. Every node carries
a `Common` part: `visible` and `enabled` (`Prop<bool>`), and `LayoutHints{width, height}` with
`Sizing` Content, Fixed(n) or Stretch(weight). `Key = std::variant<std::int64_t, std::string>`,
never a double, since morph ids can exceed 2^53.

**Palette:**

| Node | Props |
|---|---|
| `Text` | `text`, `role` (`TextRole{Normal, Muted, Heading, Error, Success}`) |
| `Button` | `label`, `onClick` |
| `TextInput` | `value`, `onChange`, `onSubmit`, `placeholder`, `mode` (`TextInputMode{SingleLine, Multiline, Password}`) |
| `Checkbox` | `label`, `checked`, `onToggle` |
| `Select` | `options` (`Prop<vector<SelectOption{Key, label}>>`), `selected` (`Prop<optional<Key>>`), `onSelect`, `style` (`SelectStyle{Dropdown, Radio}`) |
| `Menu` | `items` (`MenuItem{label, onSelect}`): one table, no parallel lists |
| `Column` / `Row` | `children`, `gap` |
| `Grid` | `columns`, `cells` (`GridCell{node, span}`), `gap` |
| `Spacer` | none |
| `Panel` | `title`, `padding`, `child` |
| `Scroll` | `child`, `axis` |
| `Switch` / `switchOn<E>()` | `selector`, `cases` |
| `Tabs` | `tabs` (`Tab{label, node}`), `selected`, `onSelect` |
| `Dialog` | `open`, `title`, `child`, `onDismiss` |
| `Busy` | `active`, `label` |
| `ForEach` / `forEach(rows, keyOf, rowView)` | Built from `Signal<vector<Row>>` or a binding (a `Query`'s `value()` included). The row type is erased behind `detail::ForEachModel`. |
| `Table` / `table<Row>(columns, rows, keyOf, cells)` | `columns` (`TableColumn{label, Sizing}`), keyed rows like `ForEach` whose `cells` give one node per column, `selectionMode` (`None`, `Single`, `Multiple`), `selection` (`Prop<vector<Key>>`), `onSelectionChange`, `onActivate(Key)` |
| `DateTimeInput` | `value` (`Prop<optional<time::Timestamp>>`), `onChange`, `mode` (`Date`, `DateTime`), `offsetMinutes` (display zone) |
| `Slider` | `value` (`Prop<std::int64_t>`), `minimum`, `maximum`, `step`, `onChange` |
| `FilePicker` | `path` (`Prop<std::string>`), `mode` (`Open`, `Save`), `onPicked(std::string)` |

`Panel` gains `collapsible` and `collapsed` (`Prop<bool>`) with `onToggle`, which is an accordion.
**Drag-and-drop is on every node:** `Common` gains `dragKey` (`Prop<optional<Key>>`; a node with a
key can be dragged and carries it), `accepts` (`std::function<bool(Key const&)>`) and `onDrop`
(`std::function<void(Key)>`); a node with `onDrop` is a drop target. Kanban moves a task by
dragging its card onto a column.

**Not in this palette:** themes beyond `TextRole`, RTL, images, charts; i18n catalogs (bindings
produce translated text; the forms engine uses `render/i18n.hpp`). Schema forms are spec 2's
`forms::formView`, which builds on this palette.

**Mount:** `ui::Mounted(reactive::Runtime&, IViewBackend&, Node)` builds retained widgets through
typed factories, once.
- Each bound prop becomes a `Computed` (equality-gated) plus an `Effect` that calls the widget
  setter. A constant is set once and creates no node. There is no diffing.
- **Switch, Tabs and Dialog** mount their content into a child `Scope` and tear it down child-first.
  Tabs mount lazily per tab, and an unchanged key never remounts.
- **ForEach** keeps one child `Scope` plus one row `Signal<Row>` per key. On a snapshot change:
  - existing keys are updated in place, so widget identity, focus and selection survive;
  - new keys are mounted and gone keys unmounted;
  - children are reordered with `ContainerWidget::moveChild`;
  - a duplicate key is refused.
- Widget callbacks run inside `rt.widgetEvent()`. Flushes are always posted, so a remount never
  destroys a widget whose native handler is on the stack. A flush inside `widgetEvent` is refused.
- Ordering guarantees: bindings die before their widget, and children before their parent.

**Backend contract (`ui/backend.hpp`):** `Widget` has `setVisible`/`setEnabled`, and
`setDragKey(optional<Key>)` / `setDropHandler(accepts, onDrop)` for drag-and-drop;
`ContainerWidget` has `moveChild(Widget&, index)`; each node kind has a widget interface with UTF-8
setters; `IViewBackend` has one typed factory per kind, each creating the child appended to its
parent; a widget's destructor detaches it and frees its native resources.
**`ui::testing::RecordingBackend`** — a headless fake tree with an operation log and
`click`/`edit`/`choose`/`toggle`/`drag` helpers — is the test double and the reference for the TUI and
Qt Quick backends.

## 5b. The frontend seam: `ui/frontend.hpp`

```cpp
namespace morph::ui {
using Scheduler = reactive::Scheduler;       // after(delay, fn), every(period, fn) -> TimerHandle (cancels on destruction)
class AppContext {
public:
    virtual reactive::Runtime& runtime() = 0;  // owned by the frontend
    virtual exec::IExecutor& executor() = 0;   // the runtime's owner; every BridgeHandler's guiExec
    virtual Scheduler& scheduler() = 0;
    virtual exec::IoLoop* ioLoop() = 0;        // the TUI's caller-driven loop; null on Qt Quick
    virtual void quit(int exitCode = 0) = 0;
    [[nodiscard]] virtual std::string_view frontendName() const = 0;
};
class Application { public: virtual ~Application() = default; [[nodiscard]] virtual Node view() = 0; };
using ApplicationFactory = std::function<std::unique_ptr<Application>(AppContext&)>;
class Frontend { public: [[nodiscard]] virtual std::string_view name() const = 0;
                 virtual int run(ApplicationFactory const& app) = 0; };   // owns the event loop until quit()
struct FrontendOption { std::string name; std::function<bool()> usable; std::function<std::unique_ptr<Frontend>()> make; };
std::unique_ptr<Frontend> selectFrontend(std::span<FrontendOption const> built, int argc, char const* const* argv,
                                         EnvironmentReader const& env = processEnvironment());
}
```

- `run` builds the runtime and executor first, then calls the factory, mounts `view()`, and drives
  its loop until `quit()`; the application (controllers, handlers, bridge wiring) is destroyed
  before the runtime. The factory is where an app constructs its `BridgeHandler`s, with
  `ctx.executor()` as their callback executor.
- `selectFrontend`: `--ui=<name>`, else `MORPH_UI`, else the first option in the order `main`
  passes whose `usable()` holds (main passes Qt Quick first, whose `usable` is true on macOS and
  Windows and when `DISPLAY`/`WAYLAND_DISPLAY` is set; the TUI's is "stdin is a terminal"). An
  unknown or unbuilt name is an error naming the built ones.
- `Query`'s `QueryOptions::refreshEvery(Scheduler&, period)` re-fetches on a timer; a controller's
  polling (event feeds, report jobs) is that, not a hand-run timer.

## 6. The TUI frontend: `morph::tui` (`MORPH_BUILD_TUI`)

**One loop:** `IoLoop` gains `enum class IoLoopDriver : std::uint8_t { OwnThread, Caller }`
(`OwnThread` the default, unchanged). `Caller` generalises morph's single-thread WASM host-driven
semantics to native builds: no thread is started and the constructing thread is the loop's;
`runningHere()` holds there; `runAndWait()` and a component's close run inline instead of
deadlocking; other threads still `post()` and `Weak::post()`. A TUI app does
`IoLoop io{IoLoopDriver::Caller}; core::tui::runtime::TuiRuntime tui{io.loop(), terminal};` and
`tui.blockOn(…)` drives the one loop, so sockets, timers, bridge callbacks and the UI share one
thread. Components are destroyed on the driver thread, outside a turn, where the loop is not
running and teardown is serialised with dispatch. `docs/spec/core/executor.md` ("The I/O loop") is
updated; the IoLoop tests run in both modes, and `TimeoutScheduler` and an asynchronous
`SocketBackend` round trip gain a Caller-mode case (a synchronous socket verb on the driver thread
throws, as it does on the loop thread today).

**`morph::tui::LoopExecutor : morph::exec::IExecutor`** (about 30 lines): `post` →
`EventLoop::post`, each task inside `core::async::ExecutorScope{coreExecutor()}`; a `weak_ptr`
alive-token drops posts after destruction; throwing tasks are logged. It is the reactive Runtime's
owner and every bridge's `guiExec`, generalising the test-local loop executor in
`tests/net/test_socket_backend.cpp`.

**`morph::tui::Backend : ui::IViewBackend`** over a `core::tui::Screen`. Its widgets are
`core::tui::Component` subclasses, private to `src/tui/` (compiled into `morph_tui`):

- Stack and Grid are arranged in `render()` by `morph::tui::layout`, a stack and grid solver in
  cells (Content/Fixed/Stretch weights, gap, padding, spans, deterministic remainder); valid
  because core::tui renders a parent before its children.
- Panel is `drawBox` plus a title; Label maps `TextRole` to theme colours; Button activates on
  Enter or Space; Checkbox draws `[x] label`; Spacer is empty space.
- TextInput wraps `core::tui::InputField` (single-line, multiline, masked).
- Select is a `List` with markers (Radio) or a `List` overlay through `Screen::showOverlay`
  (Dropdown); Menu is a `List` subclass; Tabs is a bar (Left/Right) plus the body.
- Dialog is an overlay with a focus trap, Esc calls `onDismiss`; Scroll keeps the focused child
  visible; Busy is a spinner advanced by a loop timer while any Busy is active.

- Table is a header row plus keyed rows in column widths from the layout solver; DateTimeInput is
  an ISO field with arrow-key stepping; Slider moves by `step` on Left/Right; FilePicker is a path
  field (no file dialog in a terminal).
- **Drag-and-drop** uses `MouseTracking::Drag` (spec 0): a press on a node with a `dragKey` starts
  a drag after one cell of motion, pointer capture follows it, an overlay label shows the dragged
  item, the drop target under the pointer (`Screen::componentAt`, then up the parents to the first
  node whose `accepts` holds) is highlighted, and the release calls its `onDrop`.

**`tui::Frontend : ui::Frontend`** (`tui::frontendOption()` names it `"tui"`, usable when stdin
is a terminal; the event pump is private): `IoLoop{Caller}`, a
`LoopExecutor`, the runtime, `Screen` and `TuiRuntime`; one dispatch per input event; Tab calls
`focusNext`, Shift+Tab `focusPrev` (Shift+Tab reaches the app only on terminals speaking the Kitty
keyboard protocol; core-cpp does not decode the legacy sequence); within an open Dialog focus
cycles inside it, because `Screen`'s own walk skips overlays; a resize re-fits the root; Ctrl+C
quits with exit code 130 (128 + SIGINT); `RuntimeOptions::afterFlush` marks the frame dirty and posts **one** draw per loop turn;
`quit()` closes an `AsyncQueue` that `whenAny` races against the input pump. Its `Scheduler` arms
`EventLoop` timers.

**Platforms:** Linux, macOS and Windows (core::tui's Windows console); CI builds it in the jobs
that build optional components.

## 7. Tests (Catch2)

`reactive`, `control` and `ui` tests are in morph's base test tree; `tests/tui/` is registered
under `MORPH_BUILD_TUI`. Owners are `morph::testing::StepExecutor`s drained by the test. Misuse is
observed through `morph::testing::OwnerProbeRecorder`. The implementation plans list each case.

- **reactive, Store, control:** glitch-freedom (the diamond; an Effect reading `A` before
  `Computed(A)`), pruning, unlinking, batching, one `post()` per idle→pending transition, the
  equality skip, throwing Computeds, teardown order, every misuse seen once by the probe; exhaustive
  updates refused at compile time; `request()` delivery and gating; Query latest-wins under forced
  reply orders (`Completion::makeSettleable`), idle, refetch, `refreshOn`; Mutation pending,
  one-flush invalidation, `action(make)`; Subscription; `errorMessage`. Part 1's plan lists each.
- **ui** (`RecordingBackend`): golden tree dump; a constant never binds; one setter call per real
  change; `visible`/`enabled`; Switch, Tabs and Dialog remount child-first; ForEach keeps identity
  on update, emits minimal create/destroy/move operations, takes int64 and string keys, refuses
  duplicates; TextInput does not echo `setText`; unmount destroys everything.
- **frontend seam:** `selectFrontend` precedence (flag, environment, first usable, unknown name);
  the application is destroyed before the runtime; `refreshEvery` fires on a fake scheduler.
- **backend conformance** (`ui/testing/backend_conformance.hpp`): the same scripted cases — build,
  bind, update, remount, ForEach reorder, drag a key onto a target — run against
  `RecordingBackend` here, the TUI backend below and Qt Quick in spec 3.
- **tui:** each widget renders (`canvasToString(screen.renderedBuffer())`) and handles its keys;
  the layout solver, table-driven; drag-and-drop through scripted SGR mouse reports; the frontend
  end to end through `ScriptedInputSource` (scripted keys then quit; a timer-sent quit);
  `LoopExecutor` lifetime; `IoLoop` `Caller` mode (inline `runAndWait`, inline close, cross-thread
  `post`).

## 8. Examples

Two examples with no rung live in `examples/tui/` and run through `ui::selectFrontend` like every
migrated app (spec 4): **`gallery`** — every node kind once, drag-and-drop included, as a visual
check for backend authors; **`workout`** — a dashboard with a Store of signal fields and Msgs, Tabs
(Dashboard / Statistics), a Menu table, a keyed lap `forEach`, a Sprint `Button` with a bound
`enabled`, a rider-name `TextInput`, a Quit confirmation `Dialog`, and a `Scheduler`-driven ticker.
The bank controller proof and every other app are spec 4.

## 9. Docs

- This file is in `docs/superpowers/specs/`. New specs, each listed in the `docs/spec/README.md`
  map: `docs/spec/reactive/signals.md`, `store.md`, `control.md`; `docs/spec/ui/view_tree.md`,
  `backend_contract.md`, `frontend.md`; `docs/spec/tui/frontend.md`.
- `docs/spec/core/executor.md` gets the IoLoop `Caller` mode; `ARCHITECTURE.md` its namespace and
  header maps; `docs/GETTING-STARTED.md` a "TUI frontend" pointer; `pinned_facts.toml` any constant
  the specs mention.
- CHANGELOG `[Unreleased]` → Added: `morph::reactive` (signals, Store, Query, Mutation,
  Subscription), `morph::ui`, `morph::tui`, `IoLoopDriver::Caller`, `MORPH_BUILD_TUI`.

## 10. Risks

- **The compiled-component exception.** `morph::tui` is the first compiled library not there for
  Qt MOC; the CONTRIBUTING sentence makes it a rule, and the install/export check covers it.
- **libunicode fetch** downloads UCD.zip at configure time (documented, cached in CI);
  **Caller-mode destruction order:** components die on the driver thread (documented, tested).
- **Query semantics are a policy.** Latest-wins, keep-value-while-refetching and clear-on-idle are
  what the migrated screens need. Another policy (append-only paging, optimistic writes) becomes a
  query option when two examples need it, rather than being hand-rolled twice.
- **Mouse reporting is opt-in** in core-cpp; morph's TUI requests `Drag`, so plain click-drag in
  a morph TUI app no longer selects terminal text (Shift+drag still does in most terminals).

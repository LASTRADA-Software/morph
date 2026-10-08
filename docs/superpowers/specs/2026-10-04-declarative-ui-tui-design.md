# Declarative UI in morph: the reactive core, the view tree and the frontends — design (spec 1)

A morph application's screens are defined on the server as UI documents (spec 5) and rendered by a
generic client. This spec defines the client's foundations: **`morph::reactive`**, the reactive core
that evaluates a document — signals, derived values, effects, and declarative server interaction
through `Query`, `Mutation` and `Subscription`; **`morph::ui`**, the view tree a document mounts into,
the backend contract every renderer implements, and the frontend seam that picks a renderer at
runtime; and **`morph::tui`**, the terminal renderer, which renders what a terminal can.

This is spec 1 of a program delivered as a series of pull requests (§1, "The program").

## Contents

[1 Intent](#1-intent) · [2 Packaging](#2-packaging-and-repository-boundaries) ·
[3 Reactive core](#3-the-reactive-core-morphreactive) · [4 Store](#4-store-and-async-morphreactive) ·
[4b Control](#4b-declarative-control-query-mutation-subscription) ·
[5 View tree](#5-the-view-tree-and-mount-morphui) · [5b Frontend seam](#5b-the-frontend-seam-uifrontendhpp) ·
[6 TUI](#6-the-terminal-renderer-morphtui-morph_build_tui) · [7 Tests](#7-tests-catch2) ·
[8 Examples](#8-examples) · [9 Docs](#9-docs) · [10 Risks](#10-risks)

## 1. Intent

**Goals:**

- morph is the framework for future desktop and web applications.
- A screen is defined once, on the server, as data (spec 5), and rendered by a generic client.
- **Server interaction is declarative.** The list/detail shape — a list, a selection, a detail that
  reloads when the selection changes, `busy`, `lastError`, writes that refresh what they changed —
  is the majority pattern among morph's QML-facing classes (12 of 19, measured in
  `docs/analysis/qml-bridge-boilerplate.md`). It is written imperatively today, and that imperative
  sequencing is where its defects live (§4b). The reactive core makes it declarative, and the
  document declares it as data.
- Qt Quick is the primary renderer, native and WebAssembly (spec 3). The terminal renderer is best
  effort.

**Design decisions:**

- Fine-grained signals, not re-render plus diff. State and control live in **`morph::reactive`**,
  the view tree in **`morph::ui`**. An exception escaping an Effect is a defect.
- Server interaction is `Query`, `Mutation` and `Subscription` nodes (§4b), not callbacks sequenced
  by hand.
- **The view tree is built from a document.** Its bindings are the interpreter's reactive values,
  each with a stable slot id, so a renderer can either set widget properties (the terminal) or
  generate code that reads the values (Qt Quick, spec 3).
- **The frontend is injected** (§5b): `main` picks a renderer at runtime, and nothing above the seam
  names one.

**Success for this spec:**

- The reactive core passes the requirement tests below, and a document from spec 5's corpus runs on
  it headless.
- The same view tree mounts on a headless `RecordingBackend`, which fixes the contract every
  renderer implements; a backend-conformance suite runs the same scripted cases against each.
- `ui::selectFrontend` picks a renderer from `--ui=`, `MORPH_UI` or the environment.

### The program

| Spec | File (`docs/superpowers/specs/…`) | Delivers |
|---|---|---|
| 0 | `2026-10-04-core-cpp-mouse-design.md` | Standard VT mouse tracking and pointer capture, released as core-cpp 0.7.0; morph's pin moves to 0.7 |
| 1 | `2026-10-04-declarative-ui-tui-design.md` (this) | `morph::reactive`, `morph::ui` with the frontend seam, `IoLoopDriver::Caller`, `morph::tui` |
| 2 | `2026-10-04-forms-engine-design.md` | The C++ forms engine: schema forms, collections, wizards and the app shell as document nodes |
| 3 | `2026-10-04-qtquick-frontend-design.md` | `morph::qt_quick`: the QML generator and renderer, native and WebAssembly |
| 4 | `2026-10-04-examples-migration-design.md` | Every example's screens defined on its server, rendered by the generic client |
| 5 | `2026-10-07-ui-document-design.md` | The UI document: structure, expressions, control, relations, server-calculated values, components, delivery |
| 6 | `2026-10-08-app-hosting-design.md` | Hosting an application: screen lifecycle, the host contract, the app root scope and session, the design package, the workspace shell, application-scope preferences, offline as a backend switch |
| 7 | `2026-10-08-table-engine-design.md` | The table engine: a table declared from a model's list action, columns from the row type, sorting, filtering, selection, paging and cell editing |

Each layer lands in its own pull request, in this order: the bridge seams for model-free dispatch
(spec 5 §2); the reactive core and view tree; the table engine (spec 7), which the renderer's list
model consumes; the document and its interpreter; the Qt Quick renderer; the forms engine; the
hosting layer (spec 6: lifecycle, `ScreenHost`, the app root scope, the design package, the workspace
shell); the generic client with delivery; the examples, a few per pull request; the terminal
renderer whenever it is ready. The shipped QML forms renderer stays until the examples no longer use
it.

### Requirements the reactive core meets

core-cpp's `core::rx` / `core::mvc` prototype is not reused. Each requirement below is one it did
not meet, and each has a test in §7.

- **Graph:** glitch-free (an Effect reading `A` and `Computed(A)` never sees a stale Computed);
  unsubscribe on destruction in both directions; dependencies pruned when a run stops reading them;
  N writes in a batch are one flush; equality skip; cycle guard; a throw always restores the
  tracking context and leaves every observer able to run again; nodes are non-copyable and
  non-movable; `Computed<T>` needs no default constructor; no Effect runs synchronously inside
  `set()`, so writes are safe from Qt handlers and `Completion` callbacks.
- **Ownership order:** an effect owned by a scope runs after the effects of the scopes that own it,
  so a row binding never evaluates against a parent that is about to remove it.
- **Messages:** exhaustive at compile time; `send()` inside an update is refused.

## 2. Packaging and repository boundaries

**`morph::reactive`** (`include/morph/reactive/`)

- Header-only, in the base **`morph`** target's FILE_SET; depends on the standard library and
  `morph/core` (executor, completion, callback_scope, bridge, owner probe).
- Headers: `runtime.hpp`, `signal.hpp` (Signal, Computed, Effect), `scope.hpp`, `store.hpp`
  (Store, `ExhaustiveUpdate`, `request()`), `control.hpp` (Query, Mutation, Subscription,
  `errorMessage`), `scheduler.hpp` (Scheduler, TimerHandle); graph internals in `detail/graph.hpp`.

**`morph::ui`** (`include/morph/ui/`)

- Header-only, in the base `morph` target. Depends on `morph::reactive`, never the reverse.
- Headers: `document.hpp` (the document model and its load-time validation, spec 5 §3),
  `expression.hpp` (the `expr/1` evaluator), `interpreter.hpp` (document to reactive nodes),
  `view.hpp` (the view tree), `backend.hpp` (widget interfaces, `IViewBackend`), `mount.hpp`,
  `frontend.hpp` (§5b: `AppContext`, `AppSource`, `Frontend`, `FrontendOption`, `EnvironmentReader`,
  `Bundle`, `ConnectError`, `FrontendError`, `Backend`), `host.hpp` (spec 6 §4: `ScreenHost`,
  `ScreenHandle`, `ParamMap`, `MountError`), `author.hpp` (the server-side builders of spec 5 §11),
  `testing/recording_backend.hpp` and `testing/backend_conformance.hpp`. The test kit they need
  (`StepExecutor`, `OwnerProbeRecorder`) is provided under `include/morph/testing/`, so
  `RecordingBackend` depends on nothing outside the installed headers.

**`morph::table`** (`include/morph/table/`)

- Header-only, in the base `morph` target, with no toolkit dependency: the table engine of spec 7.
  Only `query_rows_source.hpp` depends on `morph::reactive`.

**`morph::tui`** (`include/morph/tui/`, sources in `src/tui/`)

- A compiled STATIC library (`morph_tui`, alias `morph::tui`), gated by **`MORPH_BUILD_TUI`**
  (default OFF), linking `core::tui` and `morph` PUBLIC.
- Registered like `morph::net`: optional components list, header-set check, install and export,
  `morphConfig.cmake.in`, sanitizers, strict warnings, the `*-everything` presets and CI.
- When ON: core-cpp gets `CORE_CPP_WITH_TUI ON` and `CORE_CPP_WITH_IMAGES OFF`; morph provides
  libunicode 0.9.3 (found first, else fetched). Native only.

**CONTRIBUTING** states the packaging rule: morph is header-only, except optional components that
wrap a compiled toolkit (`morph::tui`, `morph::qt_quick`). ARCHITECTURE and README say the same.

**core-cpp pin:** 0.7 (spec 0). The config template and the CMake bound are minor-exact.

**Conventions:** AGENTS.md and CONTRIBUTING.md — present-tense comments with no issue numbers; full
Doxygen (`WARN_AS_ERROR`); a spec per subsystem under `docs/spec/`; `ARCHITECTURE.md` maps;
CHANGELOG `[Unreleased]`; Catch2; strict warnings; clang-tidy-diff clean.

## 3. The reactive core: `morph::reactive`

**Types:**

| Type | Behaviour |
|---|---|
| `Signal<T>` | `get()` is tracked, `peek()` untracked. `set(v)` skips equal values under the signal's equality policy; `mutate(f)` always notifies. |
| `Computed<T>` | Lazy, cached in a `std::optional<T>` (so `T` need not be default-constructible), gated by its equality policy. An unchanged result stops propagation. |
| `Effect` | Runs once in its constructor, then after any of its dependencies changes. |
| `Scope` | Owns effects, computeds and adopted objects; destroys them in reverse creation order. A mounted view, a switch case, a dialog and a row each have one. |

**Equality policy.** A value type is compared with `==` when `==` is usable for it, which the policy
checks by instantiating the comparison, not through `std::equality_comparable` alone: a container
declares `==` even when its element type has none. A type without a usable `==` always notifies.
`EqualityPolicy::Always` forces notification for a type that has one.

**Runtime:** `Runtime(morph::exec::IExecutor& owner, RuntimeOptions = {})`; no `thread_local`, no
global — every node takes the `Runtime&`. `RuntimeOptions` carries `maxEffectRunsPerFlush` and
`afterFlush`, the hook a renderer uses to apply a frame. The owner must be serial.

**Scheduling:** a `set()` outside a batch is a batch of one; `batch(f)` nests. When the outermost
batch ends with effects pending and no flush requested, the runtime marks a flush requested and then
calls `owner.post(flush)` once; a `post` that throws clears the mark. No flush runs synchronously
inside `set()`; writes made by effects during a flush are processed by that flush. The posted flush
holds the core weakly and is a no-op once the runtime is gone.

**Widget events.** A renderer runs every user event inside `rt.widgetEvent(f)`. A flush that becomes
due inside a widget event is deferred until the outermost widget event returns, and runs then as a
posted flush. A handler that spins a nested event loop (a modal dialog) therefore neither runs the
flush under its own stack nor spins on it.

**Algorithm** (push-dirty/pull-value, colours Clean, Check, Dirty, as in Reactively and Preact):
`set()` marks direct observers Dirty and propagates Check, queuing effects; `flush()` pulls each
queued effect through its computeds and runs it only if a source really changed (glitch-free);
queued effects run in scope-depth order, owners first; dependencies are re-tracked every run, so
stale ones are pruned; destructors unlink both ways. The cache and the graph links are `mutable`
members: reading a `const` node updates them without casting const away.

**Errors and misuse:**

- The RAII `TrackingFrame` always restores the previous tracking context. A throwing `Computed`
  rethrows to its reader and retries on the next read. A source that throws while an observer
  checks it marks the observer Dirty, so the observer re-reads it on its next pull and sees the
  error, rather than staying stuck at Check.
- Each misuse is **reported, then refused**. The report is `detail::noteOwner(site, owner, false)`:
  it asserts in a debug build, and a test's installed probe sees the site instead. The refusal is the
  same in every build.
  - An exception escaping an `Effect` (a defect): that flush stops and the runtime posts a new flush
    for the effects still queued, at most a bounded number of times in a row, so a defect cannot
    stall the queue until the next `set()` and cannot loop forever.
  - An effect re-run more than `maxEffectRunsPerFlush` times in one flush (a write cycle): the flush
    stops and its queue is dropped.
  - `set()` inside a Computed: the write is dropped. A Computed reading itself: the read throws
    `std::logic_error`. `send()` inside an update: the Msg is dropped.
  - An operation off the owner: the operation is dropped.
  - A Runtime destroyed while nodes are alive: nodes keep the core alive, and any flush is a no-op.

**Relation to the bridge:** this layer consumes `Completion` and `subscribe` like any caller and
never enters `BridgeHandler`; nothing in the bridge knows a reactive runtime exists. The
interpreter's model-free dispatch uses the bridge seams of spec 5 §2, which are in the bridge, not
here.

## 4. Store and async: `morph::reactive`

**`Store<ViewState, Msg>`** holds user-intent state for C++ code that uses the core directly: tests,
local tools, the interpreter's own internals.

- `ViewState` is a struct of `Signal<T>` fields; `Msg` is a `std::variant`. `Update` must satisfy
  **`ExhaustiveUpdate`**, checked at construction.
- `send(msg)` runs the update in one batch; `state()` returns `ViewState const&`.

**The low-level async primitive:** `request(store, handler, scope, action, toMsg, toFailMsg)`
executes `action` and sends `toMsg(result)` or `toFailMsg(exception_ptr)` as one message on the
Store's owner, each delivery one batch.

**Lifetime rule:** the Store, the Runtime and every mounted view outlive any callback that can still
fire. The `CallbackScope` is the last member of its owner, so it dies first and gates anything in
flight.

## 4b. Declarative control: Query, Mutation, Subscription

Header `morph/reactive/control.hpp`: reactive nodes like `Computed` — non-copyable, non-movable,
owned by a scope, affinity-checked against the Runtime's owner. A node built over a handler checks at
construction that the handler's callback executor is serial and runs on the Runtime's owner, by
affinity and not by pointer identity; no callback executor means the bridge's owner.

### What this replaces

An imperative controller sequences calls by hand, and two defects follow from the shape: **stale
replies win** (select A, then B; A's late reply shows A's history under B), and **invalidation is a
call graph** (what a write refreshes lives in whichever `.then` remembered to call `refresh()`).

### `Query<A>` — a derived async resource

```cpp
template <class A, class R = morph::model::ActionTraits<A>::Result> class Query;
Query(Runtime& rt, BridgeHandler<M, S>& handler, std::function<std::optional<A>()> key, QueryOptions = {});
Query(Runtime& rt, std::function<Completion<R>(A const&)> fetch, std::function<std::optional<A>()> key, QueryOptions = {});
```

The fetcher constructor is the test seam and the path the document interpreter uses, whose fetcher
is model-free JSON dispatch with `A` and `R` as JSON text.

- An internal Effect reads `key`, tracked. A changed key is fetched under a new generation of the
  query's `CallbackScope`. `nullopt` is idle: nothing in flight, value and error cleared.
- **Latest wins:** a reply from a superseded generation is dropped, success and failure alike, and a
  stoppable call is asked to stop.
- `QueryOptions::debounce` fetches a key only once it has been stable that long.
- Tracked reads: `pending()`; `value()`, kept while a refetch is in flight; `error()`, cleared by the
  next success. `refetch()` re-issues the current key.
- `refreshOn<Pub>(handler)` refetches on a published `Pub`; it fires per result type, so two actions
  with one reply type fire each other. A document's `refreshOn` is the action-id form of spec 5 §5,
  implemented over the interpreter's own dispatch. `QueryOptions::refreshEvery` refetches on a
  `Scheduler` timer and skips a tick while a request is in flight.
- A fetch that throws synchronously is recorded as the query's error; `pending()` falls.

### `Mutation<A>` — a command

```cpp
template <class A, class R = morph::model::ActionTraits<A>::Result> class Mutation;
Mutation(Runtime& rt, BridgeHandler<M, S>& handler, MutationOptions options = {});
Mutation(Runtime& rt, std::function<Completion<R>(A)> run, MutationOptions options = {});
```

- `run(A)` issues one call. Tracked reads: `pending()`, `error()` (cleared by the next success),
  `lastResult()`, and `successCount()`, which ticks once per success.
- `MutationOptions::concurrency` decides what a run does while another is in flight: `Exclusive`
  refuses it, `Serial` queues it (per key when `serialKey` is set), `Latest` sends it and applies
  only the newest reply, `Parallel` applies every reply as it arrives.
- `MutationOptions::invalidates` holds `InvalidationLink`s: a query registers itself through a link
  and unregisters when it is destroyed, so a mutation never refetches a query that is gone. A success
  writes the result and refetches each live query in one batch: one flush, one frame.
- A run that throws synchronously is recorded as the mutation's error.

### `Subscription<R>`

Built from `(Runtime&, BridgeHandler<M, S>&)`. `latest()` is a tracked `std::optional<R>` fed by
`handler.subscribe<R>`.

### `errorMessage(std::exception_ptr) -> std::string`

The one place an `exception_ptr` becomes display text: `what()` for a `std::exception`, a fixed
fallback otherwise.

### Scheduler

`Scheduler::after(delay, fn)` and `every(period, fn)` return a move-only `TimerHandle` that cancels
on destruction; `active()` is false once a one-shot timer has fired. Callbacks run on the runtime's
owner. `testing::ManualScheduler` advances time only when told.

## 5. The view tree and mount: `morph::ui`

The interpreter builds the view tree from a document (spec 5 §9): one node per document node, with
every property either a constant or a **slot** — a reactive value with a stable id — and every event
a command sink. Node kinds, their properties and their events are spec 5 §9's palette; `Key` is
`std::variant<std::int64_t, std::string>`, never a double.

**Mount:** `ui::Mounted(reactive::Runtime&, IViewBackend&, Node)` builds retained widgets through
typed factories, once.

- Each slot becomes an Effect that calls the widget's setter when the value changes. A constant is
  set once.
- **Switch, Tabs and Dialog** mount their content into a child scope and tear it down child-first.
  Tabs mount lazily per tab.
- **ForEach and Table** keep one row scope per key. On a list change, existing keys are updated in
  place, new keys mounted, gone keys unmounted, and children reordered with
  `ContainerWidget::moveChild`; a duplicate key is refused. The new row list is built completely
  before it replaces the old one, so a row whose mount throws leaves the old rows intact.
- Bindings die before their widget, children before their parent.

**Backend contract (`ui/backend.hpp`):** `Widget` has `setVisible` and `setEnabled`, which apply to
the widget and everything inside it; `ContainerWidget` has `moveChild(Widget&, index)`; each node
kind has a widget interface with UTF-8 setters; `IViewBackend` has one typed factory per kind. A
setter called with the value the widget already shows changes nothing — in particular a text input's
cursor, selection and composition are kept. Input widgets are controlled: a user action reports the
request, and the widget shows what its slot says afterwards. A slot that transforms or clamps the typed value
shows the raw text for one posted turn first; the cursor and composition rules above keep that
turn harmless for input methods.

**`ui::testing::RecordingBackend`** is a headless fake tree with an operation log and `click`,
`edit`, `choose`, `toggle`, `dismiss` and `drag` helpers. A helper acts only on a widget a user could
reach: visible and enabled together with all its ancestors. It is the test double for documents,
the forms engine and the interpreter, and the reference for the terminal renderer.

The Qt Quick renderer (spec 3) does not use `Mounted`: it generates QML that reads the same slots.
Both are checked by the same behaviour cases (§7).

## 5b. The frontend seam: `ui/frontend.hpp`

```cpp
namespace morph::ui {
class AppContext {
public:
    virtual ~AppContext() = default;
    virtual reactive::Runtime& runtime() = 0;   // owned by the frontend
    virtual exec::IExecutor& executor() = 0;    // the runtime's owner; every bridge's callback executor
    virtual Scheduler& scheduler() = 0;
    virtual exec::IoLoop* ioLoop() = 0;         // null when the frontend has none
    virtual void quit(int exitCode = 0) = 0;
};
class AppSource {                                // where the screens and the models are
public:
    virtual ~AppSource() = default;
    virtual void open(AppContext& ctx, std::function<void(std::expected<Bundle, ConnectError>)> ready) = 0;
    virtual void switchBackend(Backend backend, std::function<void(std::expected<void, std::string>)> done) = 0;
};
class Frontend {
public:
    virtual ~Frontend() = default;
    virtual std::string_view name() const = 0;
    virtual int run(AppSource& source) = 0;
};
std::expected<std::unique_ptr<Frontend>, FrontendError>
    selectFrontend(std::span<FrontendOption const> built, std::vector<std::string>& args,
                   EnvironmentReader const& env = processEnvironment());
}
```

- An `AppSource` is either **remote** (connect, handshake, catalog, verified bundle; spec 5 §12) or
  **local** (the application's models and screens linked in-process). The generic client's `main`
  builds one from its arguments.
- `run` builds the runtime and executor first, opens the source, mounts the app shell and drives its
  loop until `quit()`; screens and connections are destroyed before the runtime.
- An `AppSource` signs the user in natively when the application needs it (before the catalog is
  requested, on the owner), and `switchBackend` replaces the dispatch target under mounted screens:
  it is `Bridge::switchBackend` posted to the owner, after which the interpreter holds the queries on
  private aliases, runs each screen's `onBackendChange` and refetches (spec 6 §5, §9). `Backend` is
  the application's opaque handle. A host with its
  own shell mounts screens through `ScreenHost` instead of `run`'s app shell (spec 6 §4).
- `selectFrontend`: `--ui=<name>` or `--ui <name>`, else `MORPH_UI`, else the first option whose
  `usable()` holds; the flag is removed from `args`, which the caller passes on, and scanning stops at
  `--`. An unknown or unbuilt name is a `FrontendError` naming the built ones.

## 6. The terminal renderer: `morph::tui` (`MORPH_BUILD_TUI`)

Best effort: the terminal renders every built-in node kind it can, renders a custom component's
fallback, and skips what a terminal cannot express. The screens are designed for desktop and web.

**One loop:** `IoLoop` gains `enum class IoLoopDriver : std::uint8_t { OwnThread, Caller }`
(`OwnThread` the default, unchanged). `Caller` generalises morph's single-thread WebAssembly
semantics to native builds: no thread is started and the constructing thread is the loop's;
`runAndWait()` and a component's close run inline on that thread. A TUI app drives the one loop with
`TuiRuntime::blockOn`, so sockets, timers, bridge callbacks and the UI share one thread.
`docs/spec/core/executor.md` describes the mode; the IoLoop tests run in both.

**`morph::tui::LoopExecutor : morph::exec::IExecutor`** posts to `EventLoop::post` and drops posts
after its destruction. It must be destroyed on the loop's thread, outside a turn.

**`morph::tui::Backend : ui::IViewBackend`** over a `core::tui::Screen`. Its widgets are
`core::tui::Component` subclasses, private to `src/tui/`: a stack and grid layout solver in cells;
panels; labels with text roles as theme colours; buttons, checkboxes and text inputs over
`InputField`; selects as lists or overlays; tabs; dialogs as overlays with focus kept inside them;
scroll; busy spinners; tables; date-time fields; sliders; a file path field. A key reaches an
ancestor only when the ancestor opts in. Drag-and-drop (spec 5 §9) uses core-cpp 0.7's pointer
capture; a terminal without mouse reporting ignores `drag` and `drop`.

**`tui::Frontend`** (`tui::frontendOption()`, named `"tui"`, usable when stdin is a terminal):
`IoLoop{Caller}`, a `LoopExecutor`, the runtime, `Screen` and `TuiRuntime` with interrupt handling
enabled, so Ctrl+C and SIGINT both quit with exit code 130 after restoring the terminal. A terminal
the frontend initialises is shut down when `run` returns.

**Platforms:** Linux and macOS in CI; Windows builds when core-cpp's Windows console does.

## 7. Tests (Catch2)

`reactive`, `control` and `ui` tests are in morph's base test tree; `tests/tui/` is registered under
`MORPH_BUILD_TUI`. Owners are `morph::testing::StepExecutor`s drained by the test; misuse is observed
through `morph::testing::OwnerProbeRecorder`; both live in `include/morph/testing/` (§2). Every test states the mutation that would make it fail.

- **reactive, Store, control:** glitch-freedom; pruning; unlinking; batching; one `post()` per
  idle→pending transition and a recovered flush after a throwing `post`; the equality skip, and a
  container of non-comparable elements that compiles and always notifies; a throwing source under a
  checking observer, which errors again on the next read and lets an Effect run again; owner-first
  effect order; deferral inside a nested widget event; every misuse seen once by the probe; Query
  latest-wins under forced reply orders, idle, debounce, refetch, `refreshEvery` skipping while
  pending; each Mutation concurrency mode; invalidation after a query's destruction; synchronous
  throws; one-flush invalidation measured by counting the owner's posts; Subscription;
  `errorMessage`.
- **ui:** document load-time validation; the expression corpus; the interpreter over spec 5's
  corpus on `RecordingBackend`; a constant never binds; one setter call per real change; switch, tabs
  and dialog remount child-first; ForEach identity, minimal operations, int64 and string keys,
  duplicate refusal, and a throwing row mount that leaves the old rows intact; a hidden ancestor
  blocks a helper's action.
- **frontend seam:** `selectFrontend` precedence and argument removal; destruction order;
  `refreshEvery` on a `ManualScheduler`.
- **backend conformance:** the same scripted cases run against `RecordingBackend` and the terminal
  backend; spec 3 runs the equivalent behaviour cases against generated QML.
- **tui:** each widget renders and handles its keys; the layout solver; the frontend end to end
  through `ScriptedInputSource`; `LoopExecutor` lifetime; `IoLoop` `Caller` mode.

## 8. Examples

`examples/gallery` is one document that uses every node kind once, served by a small local
application, as a visual check for renderer authors. Every other application is spec 4.

## 9. Docs

New specs, each listed in the `docs/spec/README.md` map: `docs/spec/reactive/signals.md`,
`store.md`, `control.md`; `docs/spec/ui/document.md`, `expressions.md`, `view_tree.md`,
`backend_contract.md`, `frontend.md`; `docs/spec/tui/frontend.md`. `docs/spec/core/executor.md` gets
the IoLoop `Caller` mode; `ARCHITECTURE.md` its namespace and header maps; CHANGELOG
`[Unreleased]` → Added for each component as it lands.

## 10. Risks

- **The compiled-component exception.** `morph::tui` and `morph::qt_quick` are compiled; the
  CONTRIBUTING rule names the exception and the install/export check covers both.
- **The expression library grows.** It is versioned (`expr/1`), total and bounded; a value it cannot
  express is a server-computed field (spec 5 §1).
- **Query and mutation semantics are policies.** Latest-wins, keep-value-while-refetching,
  clear-on-idle and the four mutation concurrency modes cover the migrated screens; another policy
  becomes an option when two screens need it.
- **libunicode fetch** downloads UCD.zip at configure time for TUI builds; CI caches it.

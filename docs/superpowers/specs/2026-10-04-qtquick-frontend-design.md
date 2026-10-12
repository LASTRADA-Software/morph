# The Qt Quick renderer: `morph::qt_quick` — design (spec 3)

The primary renderer of the program, native and WebAssembly. It shows a mounted UI document (spec 5) as Qt Quick
items, one `MorphUi` component per node kind, through the view tree's backend contract, and loads the application's
custom components. Qt does the layout, the input handling and the painting; the document interpreter does every
evaluation; the mount (`ui::Mounted`) keeps the items current.

The authoritative reference for what is built is `docs/spec/qt_quick/backend.md` and `docs/spec/qt_quick/frontend.md`.
This design states the decisions and where the renderer goes next.

## Contents

[1 Packaging](#1-packaging) · [2 Pipeline](#2-pipeline) · [3 Components per kind](#3-components-per-kind) ·
[4 Values and events](#4-values-and-events) · [5 Lists](#5-lists) · [6 Components](#6-components) ·
[7 Controlled widgets](#7-controlled-widgets) · [8 Layout](#8-layout) · [9 The frontend](#9-the-frontend) ·
[10 WebAssembly](#10-webassembly) · [11 Tests](#11-tests) · [12 Risks](#12-risks)

## 1. Packaging

- **`morph_qt_quick`**, alias **`morph::qt_quick`**: a compiled STATIC library gated by
  **`MORPH_BUILD_QT_QUICK`** (default OFF; requires `MORPH_BUILD_QT` for `QtExecutor`), linking
  `morph`, `morph::qt` and Qt 6.5+ `Quick`, `QuickControls2` and `Qml` PUBLIC.
- Public headers in `include/morph/qt_quick/`, declarations only: `backend.hpp` (`qt_quick::Backend`,
  `BackendOptions`, `CustomWidget`, `CustomRequest`, `widthClassOf`), `components.hpp` (`qt_quick::Components`,
  `ComponentFailure`), `frontend.hpp` (`qt_quick::Frontend`, `FrontendConfig`, `ShellFactory`, `frontendOption`,
  `usableHere`) and `scheduler.hpp` (`qt_quick::QtScheduler`). Sources and the private QML module `MorphUi` live in
  `src/qt_quick/`.
- Application hosting (spec 6) adds `qt_quick::Renderer`, which implements `ui::ScreenHost` (spec 6 §4) for a host
  with its own shell, and the host-registry types of spec 6 §6, §14 and §15, beside these headers.
- Registered as an optional component: header-set check, install and export as component
  `qt_quick`, `find_dependency(Qt6 6.5 COMPONENTS Core Gui Qml Quick QuickControls2)` in
  `morphConfig.cmake.in`, warnings and sanitizers, and the CI jobs that build Qt. The package version
  file is not `ARCH_INDEPENDENT` when a compiled component is installed.
- CONTRIBUTING states the rule this relies on: morph is header-only, except optional components that
  wrap a compiled toolkit.

## 2. Pipeline

```text
document ──interpreter (spec 5 §2)──► view tree: constants and slots, command sinks
                                         │
                                  ui::Mounted ──► qt_quick::Backend (IViewBackend)
                                         │              │
                       bindings call setters ──►  Qt Quick items, one MorphUi component per kind
                                         ◄── user actions: host.fire(event, payload)
```

1. The interpreter mounts the document through `ui::Mounted` on the backend: every node becomes a widget, every bound
   property a binding that calls one setter, every event a command sink.
2. The backend makes each widget's item from its kind's `MorphUi` component — compiled once per engine — or from the
   component the restyle map names for the kind (§6).
3. A setter writes what the item shows; a user action in QML reaches the widget's handler through its relay.

The interpreter mounts only through `Mounted`, numbers slots per instance, and builds a row's, a switch case's and a
dialog's content per instance from closures, so the renderer is a backend of the view tree, held to the same
conformance suite as every other backend, and not a generator of per-screen QML. A per-template generator, compiled
once per document version and bound to the slots by number, would need the interpreter to lower without mounting and
to number slots per template; the backend's public API would not change for it.

## 3. Components per kind

Each node kind maps to one `MorphUi` component: a thin wrapper over a Qt Quick Controls type (`MorphButton` over
`Button`, `MorphTextInput` over `TextField`, …) whose root, `MorphWidget`, carries what every kind shares, and which
fields extend as `MorphField`. `docs/spec/qt_quick/backend.md` lists them.

- A node's `a11y` and `testId` become `Accessible.name`, `Accessible.role` and `objectName`. Automated UI tests locate
  elements by these (spec 6 §6).
- The components hold no application logic: a component shows its properties and reports the user's actions.
- Every component passes `qmllint` without a warning; a ctest case runs it.

## 4. Values and events

- **Values** reach an item through the widget's setters, called by the mount's bindings after each reactive flush, so
  a flush is one frame. A setter writes nothing when the item already shows the value.
- **Value conversion.** Text and booleans cross as `QString` and `bool`. `int`, `decimal` and `quantity` cross
  **already formatted** by the expression that binds them, never as a JavaScript number. A custom component's `number`
  prop is the only double (spec 5 §10); a slider's value crosses as a double within its range.
- **Events.** A component calls `host.fire(event, payload)` on its widget's relay; the relay calls a copy of the
  handler, and nothing once the widget is destroyed. The mount runs every handler inside the reactive runtime's
  `widgetEvent`, so the commands it triggers are applied in one batch and their flush is posted: a remount never
  destroys an item whose signal handler is on the stack.
- **Nested event loops.** A flush requested inside a widget event is deferred until the outermost
  widget event returns. A modal native dialog that spins a nested loop therefore neither blocks the
  flush forever nor runs it under its own handler.

## 5. Lists

- Every `forEach` and `table` is a container whose rows are the mount's row widgets. A list change reaches it as the
  mount's keyed operations — an append, a destroy, and the fewest `moveChild` calls, computed from the old and new key
  order — and the backend applies each as one stacking-order change. Row identity, focus, selection and scroll
  position survive a refetch and a reorder.
- Keys cross as text (`i:<int64>` or `s:<string>`), so no id becomes a double.
- An editable cell writes through its row scope's command sinks; its value reflects the draft.
- A `table` over the table engine (spec 7) receives the engine's `ViewChange` as these keyed
  operations, so the engine's view order is what the user sees, and selection,
  focus and scroll survive a sort or filter.
- Whether a row scope exists for every row or only for the rows the view shows is decided by
  spec 7 §15's mount measurement. Until it is measured, every row has a scope and an item.

## 6. Components

- **Built-in kinds** are `MorphUi` components compiled into the library.
- **Custom components** come from the bundle (spec 5 §10). The renderer loads a component only when its text matches
  its digest, and compiles it in a dedicated `QQmlEngine` whose root context holds nothing; the component's only way
  out is `host.fire`, which runs the document's commands. A component that fails to compile, whose hash does not
  verify, or that lacks a declared prop is replaced by its fallback subtree, and the failure is reported once.
- **Host components** are QML components the application registered with the client. The renderer
  loads them into the main engine, not the contained component engine, so they may import one
  another and the application's modules. One that is missing, or registered at a lower version than the
  bundle names, renders its fallback and is reported once.
- **Restyling a built-in kind** maps the kind to the bundle's component or a host component; the component must declare
  the properties and functions of the kind's `MorphUi` component, which the renderer checks when the mapping is made;
  the server runs the same check when it registers the bundle. The layout kinds are not restyled.
- The view tree does not yet carry a custom node's props and events to a backend (`ui::Custom` is a name and a
  fallback), so `Backend::createCustom` is ready and nothing above calls it yet.
- Components are cached by hash on disk with the bundle; the QML engine's own disk cache keeps their compiled form.

## 7. Controlled widgets

The document is the single source of truth: a widget shows what its slot says, and a user action is
a request the document may refuse.

- A `MorphUi` input shows the user's action at once, as the Qt Quick control does by itself, and reports it; after the
  event's commands run, the mount re-asserts the slot's value in a posted turn, so a refused change snaps back. The
  backend writes the control's own state for that, never a property bound to it, because a binding is not re-evaluated
  when its source has not changed. Select, tabs, radio groups, checkboxes and sliders all follow this rule.
- A text input given the text it shows does nothing, so the cursor, the selection and an in-progress input-method
  composition are kept.
- A dialog dismissed by Escape or a press outside closes, reports its dismissal, and the mount re-asserts `open`:
  a document that keeps it open keeps it open, and one that closes it can open it again.
- A table's double click activates the row without toggling its selection.

## 8. Layout

- `Sizing` maps to `QtQuick.Layouts`: `Content` to implicit size, `Fixed(n)` to a preferred size of
  `n` units, `Stretch(w)` to fill with stretch factor `w`. A unit is the default font's average
  character width horizontally and its line height vertically.
- `grid` spans map to `Layout.columnSpan`; gaps to spacing.
- `{"env": "widthClass"}` follows the window's width: below 600 logical pixels `compact`, below 1200
  `medium`, else `expanded`. The backend reports the class through `BackendOptions::onWidthClass` when it changes, not
  on every resize, and the shell sets it in the session's environment.
- `stale` dims an element; `errors` shows under it in the error colour; `readonly` uses the
  control's read-only state rather than disabling it, so its text stays selectable.
- **Theme.** The `MorphUi` components read one `Theme` singleton. Filling it from the bundle's `ui-theme/1` tokens
  (colour roles, spacing, radius and type scales, density; spec 6 §6), with the `MorphUi` defaults for any token the
  bundle omits, comes with application hosting; until then the singleton holds fixed defaults. The style is `Basic`
  unless the application's bundle maps kinds to its own components.

## 9. The frontend

`qt_quick::Frontend(int& argc, char** argv, FrontendConfig)`:

- `run(…)` constructs `QGuiApplication` (unless the process has one), a `QtExecutor` (the reactive runtime's owner and
  every bridge's callback executor), the runtime, a `QtScheduler`, the engine, the window and the backend; runs the
  source through `ui::runApp`, which mounts the shell through `FrontendConfig::mountShell` with the backend; and runs
  the event loop. On return it destroys the screens, then the connection, then the runtime, then Qt.
- `QtScheduler` is `QTimer`-based; a `TimerHandle` stops its timer on destruction.
- `quit(code)` calls `QCoreApplication::exit(code)`; a quit before the loop runs is kept; closing the window quits with 0.
- `qt_quick::frontendOption` is named `"qt"`. It is usable on macOS, Windows and WebAssembly, and
  elsewhere when `DISPLAY`, `WAYLAND_DISPLAY` or `QT_QPA_PLATFORM` is set.
- Rendering uses the platform's default graphics API. On the `offscreen` platform, and in the tests, it renders in
  software.

## 10. WebAssembly

The second part of the delivery, after the native renderer:

- The same library builds under Emscripten, single-threaded: `QtExecutor` is the only executor and
  the I/O loop is host-driven. The table engine runs as cooperative chunks on the owner (spec 7 §8).
- The frontend's state lives on the heap and is released at page unload, because Qt's WebAssembly
  event loop does not return to `run`'s frame.
- The `MorphUi` components and custom components are compiled at runtime, so the QML modules they import —
  `QtQuick.Controls` and its `Basic` style, `QtQuick.Layouts`, `QtQuick.Templates`, `QtQuick.Dialogs` — must be linked
  into the static WebAssembly client explicitly. The client's CMake imports them by name with
  `qt_import_qml_plugins`. A WebAssembly smoke executable built and linked in CI proves it.
- The file picker uses `QFileDialog::getOpenFileContent`, because a browser exposes file contents,
  not paths.
- **Browser history.** The focused screen's route (spec 6 §20) is the page's URL fragment, `#/<route>`.
  Navigating pushes a history entry, the browser's back and forward focus or reopen the screen whose
  route the entry holds, and a reload or a shared link opens the route's screen after sign-in. A route
  whose screen the principal may not open shows the start screen.

## 11. Tests

`tests/qt_quick/`, registered under `MORPH_BUILD_QT_QUICK`, offscreen with the software renderer:

- **Conformance:** every case of `ui::testing::conformanceCases()` on the real engine, through a probe that sends mouse
  and key events to the window; a probe that drops input or misreads text fails the cases that check them.
- **`qmllint`** over every `MorphUi` component, failing on any warning.
- **Behaviour on the real engine:** mount a document, drive it with synthesized input, observe item
  properties: a bound property follows its slot; one frame per flush; a refused selection snaps
  back; typing keeps the cursor; dismiss and reopen a dialog.
- **Lists:** insert, remove, move and update produce the fewest operations; an int64 key
  above 2^53 survives a round trip; focus survives a reorder.
- **Server-calculated values:** spec 5 §7's scenarios against a scripted dispatcher, observed through the
  rendered cells, including `stale`.
- **Components:** a custom component receives its props and fires its events; a hash mismatch, a
  compile error and a missing component render the fallback; a restyle mapping with a missing prop is
  refused on load.
- **Frontend:** run, quit, destruction order; `frontendOption().usable()` follows the environment.
- **WebAssembly:** the smoke executable builds and links in the WebAssembly CI job (§10).

## 12. Risks

- **Runtime QML compilation** costs startup time once per kind and engine, and once per custom component. Qt's disk
  cache bounds it to the first run of each version.
- **Per-kind components make one item tree per widget**, where a per-template generator would compile a screen's
  structure once. The mount measurement of spec 7 §15 says whether a large table needs windowed rows.
- **Custom components can contain arbitrary JavaScript.** Containment (§6) limits what they reach;
  their correctness is the application's.

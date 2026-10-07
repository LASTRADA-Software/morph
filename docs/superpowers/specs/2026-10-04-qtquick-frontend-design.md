# The Qt Quick renderer: `morph::qt_quick` — design (spec 3)

The primary renderer of the program, native and WebAssembly. It turns a UI document (spec 5) into
generated QML, connects that QML to the values the document interpreter computes in C++, and loads
the application's custom components. Qt does the layout, the input handling and the painting; the
document interpreter does every evaluation; the generated QML holds bindings and event calls and
nothing else.

## Contents

[1 Packaging](#1-packaging) · [2 Pipeline](#2-pipeline) · [3 Generating QML](#3-generating-qml) ·
[4 Values and events](#4-values-and-events) · [5 Lists](#5-lists) · [6 Components](#6-components) ·
[7 Controlled widgets](#7-controlled-widgets) · [8 Layout](#8-layout) · [9 The frontend](#9-the-frontend) ·
[10 WebAssembly](#10-webassembly) · [11 Tests](#11-tests) · [12 Risks](#12-risks)

## 1. Packaging

- **`morph_qt_quick`**, alias **`morph::qt_quick`**: a compiled STATIC library gated by
  **`MORPH_BUILD_QT_QUICK`** (default OFF; requires `MORPH_BUILD_QT` for `QtExecutor`), linking
  `morph`, `morph::qt` and Qt 6.5+ `Quick`, `QuickControls2` and `Qml` PUBLIC.
- Public headers in `include/morph/qt_quick/`: `frontend.hpp` (`qt_quick::Frontend`,
  `qt_quick::frontendOption`) and `renderer.hpp` (`qt_quick::Renderer`, for embedding a document
  view in an existing Qt Quick application). Sources and the private QML module `MorphUi` (the
  built-in kinds' base components) live in `src/qt_quick/`.
- Registered as an optional component: header-set check, install and export as component
  `qt_quick`, `find_dependency(Qt6 6.5 COMPONENTS Quick QuickControls2 Qml)` in
  `morphConfig.cmake.in`, warnings and sanitizers, and the CI jobs that build Qt. The package version
  file is not `ARCH_INDEPENDENT` when a compiled component is installed.
- CONTRIBUTING states the rule this relies on: morph is header-only, except optional components that
  wrap a compiled toolkit.

## 2. Pipeline

```text
document ──interpreter (spec 5 §2)──► values, lists, command sinks (C++)
    │                                         ▲          │
    └──generator──► QML text ──QQmlComponent──┤          ▼
                                     bindings read values; events call sinks
```

1. The interpreter mounts the document: every bound property becomes a reactive value with a stable
   **slot id**, every list a list model, every event a command sink.
2. The generator writes one QML document per screen scope and per row template, referring to slots
   by id.
3. The renderer compiles the text with `QQmlComponent::setData` in the screen's engine and
   instantiates it with the screen's value object, list models and action object in its context.
4. Generated text is cached by the hash of the document and the generator version, so a screen
   opened twice compiles once.

## 3. Generating QML

Each node kind maps to one `MorphUi` base component (a thin wrapper over a Controls type that fixes
its property and signal names), or to the custom component the bundle maps it to (§6):

```qml
// document: { "kind": "button", "label": "Deposit", "enabled": <expr>, "onClick": [ { "run": "deposit" } ] }
MorphButton { text: "Deposit"; enabled: v.s17; onActivated: a.fire(18) }
```

- A constant becomes a QML literal, escaped. A bound property becomes `v.s<slot>`.
- An event becomes `a.fire(<sink>)`, passing the event's payload (`a.fire(21, text)`).
- `switch` becomes a `Loader` whose `sourceComponent` follows the selector slot; each case is an
  inline `component`. `tabs` becomes a `TabBar` over a `StackLayout` whose pages load lazily.
  `dialog` becomes a modal `Popup` whose `visible` follows its `open` slot.
- `forEach` and `table` become views over list models (§5), with the row template as the delegate.
- The generator emits no JavaScript other than the event calls. The output passes `qmllint`, which
  its tests run over every golden.

## 4. Values and events

- **The value object** is one `QObject` per screen scope exposing every slot as a property with a
  change signal (a `QQmlPropertyMap`, or a generated meta-object when the property count is large).
  When the interpreter's value for a slot changes, the renderer writes the property; Qt re-evaluates
  the bindings that read it. Writes happen after each reactive flush, in one batch, so a flush is one
  frame.
- **Value conversion.** `string`, `bool` and `key` cross as `QString` and `bool`. `int`, `decimal`
  and `quantity` cross **already formatted** by the expression that binds them, or as exact text,
  never as a JavaScript number. A custom component's `number` prop is the only double (spec 5 §10).
- **The action object** exposes `fire(sink, payload)`. Every call runs inside the reactive runtime's
  `widgetEvent`; the commands it triggers are applied in one batch, and their flush is posted, so a
  remount never destroys an item whose signal handler is on the stack.
- **Nested event loops.** A flush requested inside a widget event is deferred until the outermost
  widget event returns. A modal native dialog that spins a nested loop therefore neither blocks the
  flush forever nor runs it under its own handler.

## 5. Lists

- Every `forEach` and `table` is a `QAbstractListModel` owned by the renderer. Its rows are the
  interpreter's row scopes; its roles are the slots of the row template.
- A list change is applied as keyed operations — `beginInsertRows`, `beginRemoveRows`,
  `beginMoveRows`, and `dataChanged` for rows whose values changed — computed from the old and new
  key order. Row identity, focus, selection and scroll position survive a refetch and a reorder.
- Keys cross as text (`i:<int64>` or `s:<string>`), so no id becomes a double.
- An editable cell writes through its row scope's command sinks; its value role reflects the draft.

## 6. Components

- **Built-in kinds** are `MorphUi` components compiled into the library.
- **Custom components** come from the bundle (spec 5 §10). The renderer loads each verified file into
  a dedicated `QQmlEngine` for components, whose root context holds nothing but the component's
  props and an event sink. A component that fails to compile, or whose hash does not verify, is
  replaced by its fallback subtree, and the failure is reported once.
- **Restyling a built-in kind** maps the kind's generated type name to the bundle's component; the
  component must declare the base kind's props and events, which the renderer checks on load.
- Components are cached by hash on disk. The QML engine's own disk cache keeps their compiled form.

## 7. Controlled widgets

The document is the single source of truth: a widget shows what its slot says, and a user action is
a request the document may refuse.

- A `MorphUi` input component emits `edited`, `chosen`, `toggled` or `activated` and does **not**
  change its own displayed state. After the event's commands run, the renderer re-asserts the slot's
  value, so a refused change snaps back. Select, tabs, radio groups, checkboxes and sliders all
  follow this rule.
- A text input does not re-assert while it has focus and its slot equals its text, so the cursor,
  the selection and an in-progress input-method composition are kept.
- A dialog dismissed by Escape or a click outside fires its dismiss commands and re-asserts `open`:
  a document that keeps it open keeps it open, and one that closes it can open it again.
- A table's double-click activates the row without toggling its selection.

## 8. Layout

- `Sizing` maps to `QtQuick.Layouts`: `Content` to implicit size, `Fixed(n)` to a preferred size of
  `n` units, `Stretch(w)` to fill with stretch factor `w`. A unit is the control font's average
  character width horizontally and its line height vertically, measured from the style's font.
- `grid` spans map to `Layout.columnSpan`; gaps to spacing.
- `{"env": "widthClass"}` follows the window's width: below 600 logical pixels `compact`, below 1200
  `medium`, else `expanded`. Expressions over it re-evaluate when the class changes, not on every
  resize.
- `stale` dims an element; `errors` shows under it in the error colour; `readonly` uses the
  control's read-only state rather than disabling it, so its text stays selectable.
- Text roles map to the Controls palette. The style is `Basic` unless the application's bundle maps
  kinds to its own components.

## 9. The frontend

`qt_quick::Frontend(int& argc, char** argv)`:

- `run(…)` constructs `QGuiApplication`, a `QtExecutor` (the reactive runtime's owner and every
  bridge's callback executor), the runtime, the engines and the window; connects (spec 5 §12);
  opens the app shell; and runs the event loop. On return it destroys the screens, then the
  connection, then the runtime, then Qt.
- `Scheduler` is `QTimer`-based; a `TimerHandle` stops its timer on destruction.
- `quit(code)` calls `QCoreApplication::exit(code)`; closing the window quits with 0.
- `qt_quick::frontendOption` is named `"qt"`. It is usable on macOS, Windows and WebAssembly, and
  elsewhere when `DISPLAY` or `WAYLAND_DISPLAY` is set.
- Rendering uses the platform's default graphics API. Tests and headless runs select the software
  renderer explicitly.

## 10. WebAssembly

- The same library builds under Emscripten, single-threaded: `QtExecutor` is the only executor and
  the I/O loop is host-driven.
- The frontend's state lives on the heap and is released at page unload, because Qt's WebAssembly
  event loop does not return to `run`'s frame.
- Generated QML and custom components are compiled at runtime, so the QML modules they import —
  `QtQuick.Controls` and its `Basic` style, `QtQuick.Layouts`, `QtQuick.Templates` — must be linked
  into the static WebAssembly client explicitly. The client's CMake imports them by name with
  `qt_import_qml_plugins`, because no QML file in the binary imports them for the scanner to find.
  A WebAssembly smoke executable built and linked in CI proves it.
- The file picker uses `QFileDialog::getOpenFileContent`, because a browser exposes file contents,
  not paths.

## 11. Tests

`tests/qt_quick/`, registered under `MORPH_BUILD_QT_QUICK`, offscreen with the software renderer:

- **Generator goldens:** every node kind and every expression shape of the spec 5 corpus produces
  the expected QML, and every golden passes `qmllint`.
- **Behaviour on the real engine:** mount a document, drive it with synthesized input, observe item
  properties: a bound property follows its slot; one frame per flush; a refused selection snaps
  back; typing keeps the cursor; dismiss and reopen a dialog.
- **Lists:** insert, remove, move and update produce the minimal model operations; an int64 key
  above 2^53 survives a round trip; focus survives a reorder.
- **Server-calculated values:** spec 5 §7's scenarios against a fake server, observed through the
  rendered cells, including `stale`.
- **Components:** a custom component receives its props and fires its events; a hash mismatch, a
  compile error and a missing component render the fallback; a restyle mapping with a missing prop is
  refused on load.
- **Frontend:** run, quit, destruction order; `frontendOption().usable()` follows the environment.
- **WebAssembly:** the smoke executable builds and links in the WebAssembly CI job.

## 12. Risks

- **Runtime QML compilation** costs startup time per screen. The generated-text cache and Qt's disk
  cache bound it to the first open of each screen version.
- **The generator is a second compiler** whose output Qt must accept. `qmllint` over every golden
  and the behaviour tests on the real engine are its guard.
- **Custom components can contain arbitrary JavaScript.** Containment (§6) limits what they reach;
  their correctness is the application's.

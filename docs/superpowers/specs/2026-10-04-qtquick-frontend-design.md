# The Qt Quick frontend: `morph::qt_quick` — design (spec 3)

The second implementation of spec 1's frontend seam: `qt_quick::Frontend` runs an application in a
Qt Quick window, and `qt_quick::Backend` implements `ui::IViewBackend` with `QtQuick.Controls`
items created from C++. The application is the same object the TUI runs — same controllers, same
view tree — and `main` picks between them at runtime. It also builds for WebAssembly, which is how
the browser clients of spec 4 run.

## Contents

[1 Packaging](#1-packaging) · [2 Widgets from C++](#2-widgets-from-c) · [3 Layout](#3-layout) ·
[4 Drag-and-drop](#4-drag-and-drop) · [5 The frontend](#5-the-frontend) ·
[6 WebAssembly](#6-webassembly) · [7 Tests](#7-tests) · [8 Docs](#8-docs) · [9 Risks](#9-risks)

## 1. Packaging

- **`morph_qt_quick`**, alias **`morph::qt_quick`**: a compiled STATIC library gated by
  **`MORPH_BUILD_QT_QUICK`** (default OFF; requires `MORPH_BUILD_QT`, because it uses
  `morph::qt`'s `QtExecutor`), linking `morph`, `morph::qt` and Qt 6.5+ `Quick`, `QuickControls2`,
  `Qml` PUBLIC. Like `morph::tui` (spec 1 §2) it is an optional frontend that wraps a compiled
  toolkit, which the CONTRIBUTING rule permits.
- Public headers in `include/morph/qt_quick/`: `frontend.hpp` (`qt_quick::Frontend`,
  `qt_quick::frontendOption(argc, argv)`), `backend.hpp` (`qt_quick::Backend`). Sources and a
  private QML module (URI `MorphUi`, built into the library with `qt_add_qml_module(... STATIC)`) in
  `src/qt_quick/`.
- Registered as an optional component exactly like `morph::tui`: created before the root
  `CMakeLists.txt`'s public-header guard so its headers are checked; its own install block (headers
  plus archive, `EXPORT_NAME qt_quick`), listed in `MORPH_INSTALLED_COMPONENTS`, with
  `find_dependency(Qt6 6.5 COMPONENTS Quick QuickControls2 Qml)` in `morphConfig.cmake.in`;
  `apply_warnings`, `apply_sanitizers`; the `*-everything` presets and the CI jobs that build Qt.
  The package version file stops being `ARCH_INDEPENDENT` when a compiled component is installed.

## 2. Widgets from C++

- The `MorphUi` module holds one small QML component per widget kind — `Label`, `Button`,
  `TextInput` (TextField / TextArea / password echo), `Checkbox`, `Select` (ComboBox, or a column of
  RadioButtons), `Menu` (ListView of ItemDelegates), `Stack` (ColumnLayout / RowLayout), `Grid`
  (GridLayout), `Spacer`, `Panel` (GroupBox, collapsible), `Scroll` (ScrollView), `Slot`, `Tabs`
  (TabBar over a StackLayout), `Dialog` (Popup, modal), `Busy` (BusyIndicator), `Table`,
  `DateTimeInput`, `Slider`, `FilePicker` (a field plus a `FileDialog`) — and a `Window`
  (ApplicationWindow) hosting the root. Each exposes plain properties (`text`, `checked`,
  `options`, …) and plain signals (`activated()`, `edited(string)`, `toggled(bool)`, …); the QML
  holds no application logic and no binding to anything but its own properties.
- `qt_quick::Backend` implements every factory: it instantiates the kind's component with
  `QQmlComponent::create` in the engine's context, parents the item into its container's content
  item, and returns a C++ widget wrapper. Setters convert UTF-8 to `QString` and call
  `QObject::setProperty`; signals reach C++ through one small `QObject` relay per widget connected
  with `QObject::connect`, and every callback runs inside `Runtime::widgetEvent` (spec 1's mount
  does that). A wrapper's destructor deletes its item; spec 1 guarantees no remount happens while a
  native handler is on the stack, so immediate deletion is safe.
- `setText` on a text input does not emit `edited` (the component compares and updates only on a
  real change), keeping the cursor — the contract spec 1's backend conformance checks.
- Each item gets `objectName` `"w<id>"` so tests and accessibility tooling can find it; Controls
  provide keyboard focus, accessibility roles and input methods.
- `TextRole` maps to the Controls palette (Muted → placeholder colour, Heading → larger bold font,
  Error/Success → semantic colours); the style is `Basic` unless the application sets one.

## 3. Layout

- `Sizing` maps to `QtQuick.Layouts` attached properties: `Content` → implicit size, `Fixed(n)` →
  `preferredWidth/Height` of `n` **units**, `Stretch(w)` → `fill…` with `horizontalStretchFactor` /
  `verticalStretchFactor` `w`. A unit is the font's average character width horizontally and its
  line height vertically, so `Fixed(n)` means "about n characters" on both frontends.
- `moveChild(widget, index)` reorders an item among its siblings (`stackBefore`/`stackAfter`),
  which a Layout follows.
- `Grid` spans map to `Layout.columnSpan`; gaps to `spacing` / `rowSpacing` / `columnSpacing`.

## 4. Drag-and-drop

- A widget with a `dragKey` gets a `DragHandler` and `Drag.active`, with `Drag.mimeData` carrying
  the key; a widget with an `onDrop` gets a `DropArea` whose `onEntered` asks C++ `accepts(key)` and
  whose `onDropped` calls `onDrop(key)`. The dragged item is shown as a translucent copy that
  follows the pointer; the target under it is highlighted while it accepts.
- Keys cross as text (`i:<int64>` or `s:<string>`), so no `int64` becomes a `double` in QML.

## 5. The frontend

`qt_quick::Frontend(int& argc, char** argv)`:

- `run(factory)`: constructs `QGuiApplication`, a `QtExecutor` (the runtime's owner and the
  application's executor), the `reactive::Runtime`, a `QQmlEngine` with the `MorphUi` module, and
  the `Window`; calls the factory; mounts `view()` into the window's content; shows the window;
  runs `exec()`; on return destroys the mount, then the application, then the runtime, then Qt.
- `Scheduler` is `QTimer`-based (single-shot for `after`, repeating for `every`); a `TimerHandle`
  stops its timer on destruction.
- `quit(code)` calls `QCoreApplication::exit(code)`; closing the window quits with 0.
- `afterFlush` is unused: Qt repaints changed items itself.
- `qt_quick::frontendOption(argc, argv)` returns the `ui::FrontendOption` for `selectFrontend`:
  name `"qt"`, `usable()` true on macOS, Windows and WebAssembly, and on other platforms when
  `DISPLAY` or `WAYLAND_DISPLAY` is set.

## 6. WebAssembly

The same library builds under Emscripten (single-threaded): the `QtExecutor` is the only executor,
`IoLoop` is host-driven as today, and `run` hands control to Qt's WASM event loop the way the
existing browser clients do. The browser clients of spec 4 are each app's one `main` compiled for
WASM with only this frontend built. `FilePicker` uses `QFileDialog::getOpenFileContent` there,
because a browser exposes file contents, not paths.

## 7. Tests

`tests/qt_quick/` (registered under `MORPH_BUILD_QT_QUICK`, `QT_QPA_PLATFORM=offscreen`):

- **Backend conformance:** spec 1's scripted cases (build, bind, update, remount, ForEach reorder,
  drag a key onto a target) against `qt_quick::Backend`, observed through the items' properties.
- **Per widget:** each factory's item has the right type and `objectName`; each setter changes the
  property; each user signal reaches the callback once, inside `widgetEvent`; `setText` does not
  echo; deletion removes the item.
- **Layout:** `Fixed` / `Stretch` / `Content` produce the expected geometry in a sized window.
- **Drag:** a synthesized press-move-release (`QTest` mouse events on the window) from a dragged
  item onto a target calls `onDrop` with the exact key, including an int64 above 2^53; a target
  whose `accepts` refuses is not highlighted and gets no drop.
- **Frontend:** `run` mounts the factory's view and returns `quit`'s code; the destruction order
  (application before runtime) holds; `frontendOption().usable()` follows the environment.

## 8. Docs

`docs/spec/qt_quick/frontend.md` (new) and its `docs/spec/README.md` map entry; `ARCHITECTURE.md`
namespace and header maps; README's component list; CHANGELOG `[Unreleased]` → Added
(`morph::qt_quick`, `MORPH_BUILD_QT_QUICK`).

## 9. Risks

- **Qt Quick from C++ is less travelled than QML-first.** Creating Controls items through
  `QQmlComponent` and relaying signals by hand is supported API, but every component's property and
  signal names are a contract between C++ and its QML file; the per-widget tests are what pin them.
- **Visual parity with today's hand-designed QML** (bank's own components, kanban's board styling)
  is not a goal; behavioural parity is. Spec 4 records each app's visible differences.
- **WebAssembly file access** differs from native (contents, not paths); `FilePicker`'s callback
  receives a path natively and a temporary in-memory path on WASM, which kanban's attachment upload
  (spec 4) reads the same way.

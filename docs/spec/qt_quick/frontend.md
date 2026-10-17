# Qt Quick frontend — design

Design spec for `morph::qt_quick::Frontend` (`include/morph/qt_quick/frontend.hpp`) and
`morph::qt_quick::QtScheduler` (`include/morph/qt_quick/scheduler.hpp`): a morph application in a Qt Quick window,
on the thread that runs Qt's event loop. The widgets are [`backend.md`](backend.md); the seam a frontend implements is
[`../ui/frontend.md`](../ui/frontend.md).

## Contents

- [Shape](#shape)
- [Run](#run)
- [Quit](#quit)
- [Choosing it](#choosing-it)
- [Scheduler](#scheduler)
- [Tests](#tests)
- [Design decisions](#design-decisions)

## Shape

```cpp
int main(int argc, char** argv) {
    std::vector<std::string> args{argv, argv + argc};
    auto source = makeSource(args);
    std::vector<ui::FrontendOption> built{
        qt_quick::frontendOption(argc, argv, {.mountShell = mountShell, .components = &components}),
        tui::frontendOption({.mountShell = mountTuiShell})};
    auto frontend = ui::selectFrontend(built, args);
    if (!frontend) { std::cerr << frontend.error().message() << '\n'; return 2; }
    return (*frontend)->run(*source);
}
```

- `qt_quick::ShellFactory` is `ui::ShellFactory` with the backend the shell mounts on, since `ui::AppContext` names
  no renderer: `(AppContext&, IViewBackend&, expected<Bundle, ConnectError>) -> unique_ptr<MountedShell>`.
- `FrontendConfig{mountShell, components, title, onWidthClass}`: the shell factory (it must not be empty), the
  components and restyle map the backend uses (borrowed), the window's title, and the width class callback
  (`BackendOptions::onWidthClass`), and `devTools`, a `ui::DevelopmentTools`: with `On`, Ctrl+Shift+I in the
  window opens the inspector over the shell's focused screen ([`../ui/inspector.md`](../ui/inspector.md)).
- `Frontend(argc, argv, config)`; `name()` is `"qt"`.

## Run

`run(source)` makes, in this order:

1. the `QGuiApplication`, with Qt Quick Controls' `Basic` style — unless the process has an application already,
   which an embedding host or a test owns;
2. a `qt::QtExecutor`, the reactive runtime's owner and every bridge handler's callback executor;
3. the `reactive::Runtime`;
4. a `QtScheduler`;
5. the `QQmlEngine`, the `QQuickWindow` (1024 × 768, the configured title) and the `Backend` on its content item;
6. the `AppContext` it lends: `runtime()`, `executor()`, `scheduler()`, `ioLoop()` (null: Qt's loop is the only one),
   `quit()`.

Then `ui::runApp` opens the source, mounts the shell through `mountShell` when the source is ready, shows the window
and runs a `QEventLoop`. With `devTools` `On`, a `qt_quick::InspectorWindow` is made before the loop and attached to the
window; it reads the mounted shell through `inspectFocused`, and is hidden when the loop ends, before the shell is
destroyed. On the `offscreen` platform the window renders in software. Teardown is the reverse: `runApp`
destroys the screens and closes the source; then the backend, the window and the engine go, then the scheduler, the
runtime and the executor, and Qt last. A task posted to the executor after that runs nothing.

## Quit

- `quit(code)` while the loop runs calls `QCoreApplication::exit(code)`, and `run` returns `code`.
- A `quit` before the loop runs — from `open`, or from mounting the shell — is kept, and the loop returns it at once.
- Closing the window quits with 0.
- A shell factory that throws quits with 1 inside `runApp`, and `run` rethrows once the teardown is done; an empty
  factory is `std::invalid_argument`, before anything is made.

## Choosing it

`frontendOption(argc, argv, config)` is the option named `"qt"`; `make` builds a `Frontend` with `config`. `usable` is
`usableHere()`: true on macOS, Windows and WebAssembly; elsewhere when `DISPLAY`, `WAYLAND_DISPLAY` or
`QT_QPA_PLATFORM` is set and not empty. `QT_QPA_PLATFORM` counts because it names a platform Qt can show a window on
without a display server, such as `offscreen` or `vnc`.

## Scheduler

`QtScheduler(owner, context = nullptr)` is `reactive::Scheduler` on `QTimer`s parented to `context` (or to an object it
owns). Every callback runs with `owner` as the current executor, so it may write the reactive state the owner's runtime
holds. It keeps every guarantee of [the `Scheduler` contract](../reactive/control.md#the-scheduler-contract): a handle
cancelled on the owner never fires, even when its timer is due; a callback may cancel its own timer; a handle may
outlive the scheduler; a periodic timer the loop could not serve for several periods fires once. A callback that throws
is logged. `pendingTimers()` counts the armed timers.

## Tests

`tests/qt_quick/test_qt_quick_frontend.cpp` is an executable of its own, with no `QGuiApplication` in `main`, since
`run` makes one: a run opens the source, mounts the shell, and tears the shell down before it closes the source; a
quit posted from the shell, a quit before the loop and a closed window each end the run with their code; a throwing
factory ends it after the teardown; the option is named `qt` and is usable as the environment says.
`tests/qt_quick/test_qt_quick_scheduler.cpp` drives `QtScheduler` on Qt's own timers, in real time with short delays.

## Design decisions

| Decision | Why |
|---|---|
| `run` makes the application only when there is none | An application that embeds the frontend, or a test, already owns one; a process holds one at a time. |
| A `QEventLoop`, ended by `QCoreApplication::exit` | `exit` ends every event loop of the thread, a nested one a native dialog spins included, so `run` returns. |
| The shell factory receives the backend | `ui::AppContext` names no renderer; the terminal frontend passes its backend the same way. |
| `QT_QPA_PLATFORM` makes it usable | A headless run on a platform plugin such as `offscreen` shows its window without a display server. |

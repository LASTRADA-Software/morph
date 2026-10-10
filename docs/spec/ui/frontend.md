# Frontend seam — design

Design spec for `include/morph/ui/frontend.hpp`: how the generic client runs an application's screens on
whichever frontend `main` picks at runtime. A frontend owns an event loop, a reactive runtime and a renderer; it
runs whatever an `AppSource` opens, and nothing above the seam names a toolkit.

Read this before writing a client's `main`, an `AppSource`, or a frontend. The view a frontend mounts is
[`view_tree.md`](view_tree.md); the widgets its renderer makes are [`backend_contract.md`](backend_contract.md).

## Contents

- [Shape](#shape)
- [`AppSource`](#appsource)
- [`runApp`](#runapp)
- [What `run` guarantees](#what-run-guarantees)
- [Choosing a frontend](#choosing-a-frontend)
- [Design decisions](#design-decisions)

## Shape

```cpp
int main(int argc, char** argv) {
    std::vector<std::string> args{argv, argv + argc};
    auto source = makeSource(args);                    // remote (--server) or local (--app), the client's own code
    std::vector<ui::FrontendOption> built{qt_quick::frontendOption(), tui::frontendOption()};
    auto frontend = ui::selectFrontend(built, args);   // removes the --ui arguments it used
    if (!frontend) { std::cerr << frontend.error().message() << '\n'; return 2; }
    return (*frontend)->run(*source);
}
```

- `ui::AppContext` is what a frontend lends the source it opens and the screens it mounts. Everything it lends
  outlives both: a frontend destroys the screens and the source's connections, through `runApp`, before its
  runtime.

| Member | Meaning |
|---|---|
| `runtime()` | The reactive runtime the screens are mounted in; owned by the frontend |
| `executor()` | The runtime's owner — the same executor as `runtime().owner()` — and the callback executor of every bridge handler the source and the screens make |
| `scheduler()` | Timers that fire on the owner executor (`ui::Scheduler` is `reactive::Scheduler`, `ui::TimerHandle` is `reactive::TimerHandle`); a `Query`'s `refreshEvery` and `debounce` run on it. It honours every guarantee of [the `Scheduler` contract](../reactive/control.md#the-scheduler-contract) |
| `ioLoop()` | The I/O loop the frontend runs on, for sockets and timers that share its one thread; null when the frontend has none (Qt Quick) |
| `quit(exitCode = 0)` | Makes `run` return `exitCode` once the current event has been handled. Called before the loop runs — from `open`, or from mounting the shell — it still ends the run once the loop starts |

- `ui::Frontend` is a frontend: `name()`, the name `selectFrontend` matches, and `run(AppSource&)`.
- `ui::FrontendOption{name, usable, make}` is one frontend a binary was built with: the name `--ui=` and
  `MORPH_UI` match, whether it can run here (a terminal on stdin, a display; empty counts as usable), and the
  function that makes it.

## `AppSource`

Where the screens and the models are. `main` builds one from its arguments; the frontend opens it.

- **Remote**: connect, handshake, catalog, verified bundle (UI document design, §12).
- **Local**: the application's models and screens linked in-process, dispatching to `LocalBackend`.

| Member | Meaning |
|---|---|
| `open(ctx, ready)` | Connects (or sets up the local models), signs the user in natively when the application needs it — on the owner, before the catalog is requested — and calls `ready` once, on the owner, now or later, with a `Bundle` or a `ConnectError` |
| `switchBackend(backend, done)` | Replaces the dispatch target under the mounted screens: `Bridge::switchBackend` posted to the owner. `done` is called once, on the owner, when the switch committed, or with why not; a failed switch leaves the old backend |
| `close()` | Releases what `open` made — connections, bridges, handlers — and drops a `ready` not yet delivered. `runApp` calls it once the screens are gone and before the runtime is; the source may be opened again |

Every call is made on the context's owner, and every callback is called there.

- `ui::Bundle` carries the application's id, the manifest's SHA-256 digest, and every document — the screens, the
  app shell, and the app document (`app`) when there is one — by id, as verified JSON text. The document model and
  the interpreter read it; the seam only carries it from the source to the shell.
- `ui::ConnectError{kind, message}`: `Unreachable`, `NoUiService` (the peer answers `unknown envelope kind:
  ui-hello`), `Unsupported` (a vocabulary the client does not speak, named in the message), `Unverified` (a digest
  mismatch), `Refused` (a sign-in, a licence or an authoriser refused). The frontend shows it before any screen
  exists.
- `ui::Backend{name}` is the application's opaque handle for one of its backends; its name (`primary`, `local`,
  `remote`) is what `{"env": "backend"}` reads.

## `runApp`

`ui::runApp(context, source, mountShell, loop)` runs one source inside a frontend, and every frontend's `run`
calls it:

1. It calls `source.open(context, ready)`.
2. `ready`, whenever it comes, calls `mountShell(context, opened)` with the bundle or the connect error, and keeps
   the `MountedShell` it returns (null mounts nothing). A local source delivers during `open`; a remote one
   delivers later, from the loop.
3. It calls `loop`, which runs the frontend's event loop and returns the exit code once `AppContext::quit` ends
   it.
4. It destroys the shell, then calls `source.close()`, and returns what `loop` returned.

The order is fixed here so that every frontend keeps it. The shell goes first, while the source's connections its
screens use are still open; the source is closed next, while the runtime and the executor its handlers were made
with are still alive; the frontend destroys those after `runApp` returns. The order holds whether `loop` returns or
throws.

| Case | What happens |
|---|---|
| A second `ready` | Ignored |
| A `ready` during `close`, or after `runApp` returned | Mounts nothing: the run counts as opened from the teardown on, and the callback holds the run weakly |
| `mountShell` throws | The context quits with exit code 1; the exception leaves `runApp` after the teardown |
| `open`, `loop` or `close` throws | The teardown still runs; the first exception leaves `runApp` |
| An empty `mountShell` or `loop` | `std::invalid_argument`, before the source is opened |

`tests/test_ui_frontend.cpp` pins the order with a frontend whose context, and with it the runtime, is a local of
`run`, a source that records `open` and `close`, and a shell that records its unmounting: `open`, mount, loop,
shell, close, runtime — after a loop that returns, and after one that throws.

## What `run` guarantees

1. The runtime and its owner executor exist before the source is opened.
2. The shell is mounted once, from the source's `ready`.
3. The loop runs until `AppContext::quit`; `run` returns the exit code passed to it. A frontend may also end the
   run on its own — end of input, an interrupt — with a code it documents.
4. Teardown is the reverse, in this order: the screens, then the source's connections, then the runtime, then the
   loop and the toolkit. A binding or a handler never outlives what it points at.

A frontend keeps that order by leaving the first two to `runApp` and by declaring its members or locals in
construction order. A host with its own shell mounts screens through `ScreenHost` instead (application hosting
design, §4), which lands with hosting.

## Choosing a frontend

`selectFrontend(built, args, env = processEnvironment())` picks from the frontends a binary was built with, listed
in order of preference:

1. the last `--ui=<name>` or `--ui <name>` on the command line;
2. else `MORPH_UI`, when it is set and non-empty;
3. else the first option, in the order given, whose `usable()` holds; an empty `usable` counts as usable.

It returns `std::expected<std::unique_ptr<Frontend>, FrontendError>`.

Command-line parsing:

- `args` may hold the program name first; it is never a `--ui` flag. `--` ends option parsing: a `--ui` after it is
  an operand, not a choice.
- After `--ui`, the next argument is the name unless it starts with `--`. A `--ui` with no name — followed by such
  an argument, by `--`, or by nothing — is an error whatever comes after it: parsing stops there.
- Otherwise the last `--ui` wins, so an empty `--ui=` followed by a named one takes the named one, and a named one
  followed by an empty `--ui=` is an error.
- Names match exactly, case included.
- On success, every `--ui=<name>`, `--ui` and the name after it that parsing read is removed from `args`, and the
  rest keep their order, for the caller to pass on (to a toolkit, or its own option parser). On an error `args` is
  left as it was.

A frontend named on the command line or in `MORPH_UI` is used even when its `usable()` is false: the user asked for
it. An empty `env` is never read. A `FrontendError` carries its `kind`, the `requested` name, and the `built` names;
`message()` joins the built names with a comma and a space, or says `none`:

| Kind | Message |
|---|---|
| `UnknownName` | `morph::ui: no frontend named '<name>' is built (built: qt, tui)` |
| `MissingName` | `morph::ui: --ui needs a frontend name (built: qt, tui)` |
| `NoneUsable` | `morph::ui: no built frontend is usable here (built: qt, tui)` |
| `NotMade` (no `make`, or it returned null) | `morph::ui: frontend '<name>' could not be made` |

`EnvironmentReader` (`std::function<std::optional<std::string>(std::string_view)>`) is the seam a test replaces.
`processEnvironment()` reads the process environment through `std::getenv`, which races only with a concurrent
`setenv`; selection runs in `main` before any thread exists. MSVC deprecates `getenv` (C4996), and clang-cl
reports the same deprecation as `-Wdeprecated-declarations`; both are suppressed at that one call, so a consumer
built with warnings as errors does not fail on a morph header.

## Design decisions

| Decision | Why |
|---|---|
| The frontend is injected, the source is data the frontend opens | Client code never names a toolkit; one binary carries every frontend it was built with, and the same source runs on each. |
| The frontend owns the runtime and the loop | The runtime's owner must be the loop's thread, and only the frontend knows which thread that is. |
| `AppSource::close` | The source is `main`'s and outlives `run`, but its connections are made with the run's executor; `run` must be able to end them before the runtime goes. |
| `runApp` fixes the teardown order | Every frontend gets "shell, then connections, then runtime" from one function instead of re-deriving it. |
| The shell factory receives the connect error too | Showing why a source did not open is the frontend's view, mounted like any other. |
| Command line, then environment, then the first usable option | An explicit request wins; a default that works where the binary runs needs no request at all. |
| A named frontend is used even when not usable | `usable()` is a guess made for the default; the user's explicit choice is not second-guessed. |
| Errors are values that name the built frontends | `main` decides how to report them; "not built" and "misspelled" look the same to the user, and the list answers both. |

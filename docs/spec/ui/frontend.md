# Frontend seam — design

Design spec for `include/morph/ui/frontend.hpp`: how an application is written once and run on
whichever frontend `main` picks at runtime. A frontend owns an event loop, a reactive runtime and a
backend; the application never names one.

Read this before writing an application's `main`, and before implementing a frontend. The view a
frontend mounts is [`view_tree.md`](view_tree.md); the widgets its backend makes are
[`backend_contract.md`](backend_contract.md).

## Contents

- [Shape](#shape)
- [`runApplication`](#runapplication)
- [What `run` guarantees](#what-run-guarantees)
- [Choosing a frontend](#choosing-a-frontend)
- [Design decisions](#design-decisions)

## Shape

- `ui::Application` is the application: its controllers, handlers and bridge wiring, and `view()`,
  the tree to mount, called once after construction and never null.
- `ui::ApplicationFactory`, `std::function<std::unique_ptr<Application>(AppContext&)>`, makes it.
  The factory is where an application constructs its `BridgeHandler`s, with `ctx.executor()` as
  their callback executor.
- `ui::AppContext` is what the frontend hands the factory. Everything it lends outlives the
  application, so an application may hold references to it and use them in its destructor; the
  context itself lives at least as long as the application.

| Member | Meaning |
|---|---|
| `runtime()` | The reactive runtime the view is mounted in; owned by the frontend |
| `executor()` | The runtime's owner — the same executor as `runtime().owner()` — and every `BridgeHandler`'s callback executor |
| `scheduler()` | Timers that fire on the owner executor (`ui::Scheduler` is `reactive::Scheduler`, `ui::TimerHandle` is `reactive::TimerHandle`); a `Query` with a timed refresh uses it. It honours every guarantee of [the `Scheduler` contract](../reactive/control.md#the-scheduler-contract) |
| `ioLoop()` | The I/O loop the frontend runs on, for sockets and timers that share its one thread; null when the frontend has none (Qt Quick) |
| `quit(exitCode = 0)` | Makes `run` return `exitCode` once the current event has been handled. Called from the factory, before the loop runs, it still ends `run` once the view has been mounted: a mount runs to completion |
| `frontendName()` | The running frontend's name, the one `selectFrontend` matched |

- `ui::Frontend` is a frontend: `name()`, the name `selectFrontend` matches, and
  `run(factory)`.
- `ui::FrontendOption{name, usable, make}` is one frontend a binary was built with: the name
  `--ui=` and `MORPH_UI` match, whether it can run here (a terminal on stdin, a display; empty
  counts as usable), and the function that makes it, which must not return null.

## `runApplication`

`ui::runApplication(context, backend, factory, loop)` runs one application inside a frontend, and
every frontend's `run` calls it:

1. It calls `factory(context)` once, then `view()` once, and mounts the view on `backend` as a
   `Mounted`.
2. It calls `loop`, which runs the frontend's event loop with the view mounted — showing the window
   or focusing the first widget first, if the frontend needs to — and returns the exit code once
   `AppContext::quit` ends it.
3. It destroys the mounted view, then the application, and returns what `loop` returned.

The order is fixed here so that every frontend keeps it. The mounted view goes first, while the
application whose state its bindings and callbacks read is still alive; the application next,
while the runtime and the backend it was made with are still alive. The order holds whether `loop`
returns or throws, and a mount that throws destroys the application before the exception leaves.

Refusals: an empty `factory` or `loop` throws `std::invalid_argument` before anything is made; a
factory that makes no application throws `std::invalid_argument`
(`morph::ui: frontend '<name>': the application factory made no application`) before anything is
mounted or the loop runs. Whatever the factory, the mount or `loop` throws propagates.
`context`'s runtime and `backend` must outlive the call.

`tests/test_ui_frontend.cpp` pins the order with an application whose destructor records how many
widgets and reactive nodes are still alive: none of the view's widgets, and only the application's
own signal — after a loop that returns, a loop that throws, and a mount that throws.

## What `run` guarantees

1. The runtime and its owner executor exist before the factory is called.
2. The factory is called once, then `view()` once, and the view is mounted.
3. The loop runs until `AppContext::quit`; `run` returns the exit code passed to it. A frontend may
   also end the run on its own — end of input, an interrupt — with a code it documents.
4. Teardown is the reverse, in this order: the mounted view, then the application, then the
   runtime and the backend, then the loop and the toolkit. A binding or a handler never outlives
   what it points at.

A frontend keeps that order by declaring its members or locals in construction order, and by
leaving the application and its mount to `runApplication`, which destroys both before it returns.
The step that is easiest to get wrong is the application outliving the runtime — an application
holds signals made in it — so each frontend's own suite must assert that the runtime's
destruction reports no live node (`reactive::detail::site::kRuntimeOutlived`).

## Choosing a frontend

`selectFrontend(built, argc, argv, env = processEnvironment())` picks from the frontends a binary
was built with, listed in order of preference:

1. the last `--ui=<name>` or `--ui <name>` on the command line;
2. else `MORPH_UI`, when it is set and non-empty;
3. else the first option, in the order given, whose `usable()` holds; an empty `usable` counts as
   usable.

Command-line parsing:

- `argv[0]` is skipped, and `--` ends option parsing: a `--ui` after it is an operand, not a choice.
- After `--ui`, the next argument is the name unless it starts with `--`. A `--ui` with no name —
  followed by such an argument, by `--`, or by nothing — is an error whatever comes after it:
  parsing stops there, so `--ui --x --ui=tui` fails with "needs a frontend name".
- Otherwise the last `--ui` wins, so an empty `--ui=` followed by a named one takes the named one,
  and a named one followed by an empty `--ui=` is an error.
- Names match exactly, case included.

A frontend named on the command line or in `MORPH_UI` is used even when its `usable()` is false:
the user asked for it. An empty `env` is never read. Every failure throws
`FrontendSelectionError`, a `std::runtime_error`, whose message names the built frontends —
joined by a comma and a space, or `none` when nothing is built:

| Failure | Message |
|---|---|
| A name no option has | `morph::ui: no frontend named '<name>' is built (built: qt, tui)` |
| `--ui` with no name, or a last `--ui=` that is empty | `morph::ui: --ui needs a frontend name (built: qt, tui)` |
| Nothing named and nothing usable | `morph::ui: no built frontend is usable here (built: qt, tui)` |
| An option without `make`, or whose `make` returns null | `morph::ui: frontend '<name>' could not be made` |

`EnvironmentReader` (`std::function<std::optional<std::string>(std::string_view)>`) is the seam a
test replaces. `processEnvironment()` reads the process environment through `std::getenv`, which
races only with a concurrent `setenv`; selection runs in `main` before any thread exists. MSVC
deprecates `getenv` (C4996), and clang-cl reports the same deprecation as
`-Wdeprecated-declarations`; both are suppressed at that one call, so a consumer built with
warnings as errors does not fail on a morph header.

## Design decisions

| Decision | Why |
|---|---|
| The frontend is injected, the application is a factory | Application code never names a toolkit; one binary carries every frontend it was built with. |
| The frontend owns the runtime and the loop | The runtime's owner must be the loop's thread, and only the frontend knows which thread that is. |
| `runApplication` fixes the teardown order | Every frontend gets "view, then application, then runtime" from one function instead of re-deriving it from member order. |
| Command line, then environment, then the first usable option | An explicit request wins; a default that works where the binary runs needs no request at all. |
| A named frontend is used even when not usable | `usable()` is a guess made for the default; the user's explicit choice is not second-guessed. |
| Errors name the built frontends | "Not built" and "misspelled" look the same to the user; the list answers both. |

# The development inspector — design

The inspector shows a developer what a mounted screen is doing: its nodes as mounted, with their evaluated
properties; its queries and mutations; the command lists it ran; and the reactive runtime's counters. It implements
spec 6 §21's second tool and §12's "Inspector" test (`docs/superpowers/specs/2026-10-08-app-hosting-design.md`).

| Header | What it holds |
|---|---|
| `include/morph/ui/inspector.hpp` | `Inspector`, `InspectedNode`, `InspectedQuery` |
| `include/morph/ui/interpreter.hpp` | `CommandLogEntry`, `LoggedCommand`, and what the screen records for the inspector |
| `include/morph/reactive/runtime.hpp` | `RuntimeStats`, `Runtime::stats()` |
| `include/morph/qt_quick/inspector.hpp` | `qt_quick::InspectorWindow`, `qt_quick::InspectFocused` |

## Contents

- [Built only into development builds](#built-only-into-development-builds)
- [Read-only](#read-only)
- [What it lists](#what-it-lists)
- [Reaching the focused screen](#reaching-the-focused-screen)
- [The Qt Quick window](#the-qt-quick-window)
- [Design decisions](#design-decisions)
- [Not yet](#not-yet)

## Built only into development builds

Everything here except the reactive counters is compiled only when `MORPH_UI_DEV_TOOLS` is 1. That is the CMake option
of the same name, defined on the `morph` target's interface and off by default for `Release`, `MinSizeRel` and
`RelWithDebInfo` ([`service.md`](service.md#development-reload)). With it at 0:

- `inspector.hpp` and `qt_quick/inspector.hpp` declare nothing;
- the screen records no nodes, scopes or command log;
- `MountedShell::inspectFocused` does not exist;
- `src/qt_quick/inspector.cpp` is not added to `morph_qt_quick`.

The library is header-only, so "compiled out" is a preprocessor switch, not a separate library. Every translation
unit sees the same value, because it is on the target's interface, so a screen's layout never differs between two
translation units.

Check the claim on a client binary rather than assume it. Build the same Release configuration twice, with the switch
off and then on. `nm -C morph_client | grep -i inspect` must find nothing in the first binary and the inspector's
symbols in the second, and `strings` must find the window's QML text only in the second. The second build is the
control: without it, an empty result could mean a stripped binary or a wrong pattern.

The reactive counters (`Runtime::stats()`) are kept in every build. They are a few increments per flush, they belong to
the runtime rather than to the inspector, and a release binary can read them without the inspector.

## Read-only

The inspector reads the interpreter's state and never writes it, except through a command a user could run.

- Every read (`nodes()`, `queries()`, `commandLog()`, `counters()`) returns values: copies of strings, `Value`s and
  numbers. No pointer, reference or handle into a scope, a signal or a cache entry leaves it.
- Its reads are untracked, so calling one inside an Effect subscribes the Effect to nothing.
- Evaluating a node's properties can meet an expression error. While the inspector evaluates, the screen's `report`
  is muted, so the inspector adds no diagnostic. A `let` or a state init evaluated for the first time is computed and
  kept, exactly as a binding's first read would.
- `refetch(query)` is the one action. It runs the document command `{"refetch": "<query>"}` in the scope that declares
  the query, through `ScreenCore::runList` inside `Session::asUser` and one batch: the path a button's `onClick`
  takes. It is logged like any other command list, with the event `/inspector`, and it refetches nothing a user's
  `refetch` would not.
- The host hands out an `Inspector`, never the `Screen`: `DocumentScreenHost::inspector(handle)`.

## What it lists

**Nodes.** Each node is recorded as it is lowered (`ScreenCore::lower`), with the scope and the bindings it was
lowered with, both held weakly. `nodes()` lists, in lowering order (a parent before its children), every recorded
node whose scope and bindings are still alive, with its kind, `id`, `testId`, JSON Pointer and every expression
property evaluated now in that scope.

- A dialog's content, a switch case and a row are lowered when they mount and listed while they are.
- A row's nodes are listed once per row.
- A switch's `default` and a custom node's fallback are lowered with their enclosing scope and listed whether or not
  they are shown.
- Entries whose scope is gone are dropped every `kPruneLoweredAt` (256) recordings, so a dialog opened and closed many
  times does not grow the list without bound.

**Queries and mutations.** Every query and mutation of the screen's live scopes (each `RtScope` adds itself to its
screen on construction and removes itself on destruction), the screen scope's first, then the app document's.

| Field | Query | Mutation |
|---|---|---|
| `scope` | `screen`, `nested` or `app` | the same |
| `key` | the cache entry's key: model type, instance, action and canonical body; empty while idle | empty |
| `state` | `idle` with no entry, or an entry with neither a value nor a request; `pending` while a request is in flight; `error` when the last one failed; `stale` when the entry was invalidated or its value is at least `staleTime` old; `fresh` otherwise | `pending` while a run is in flight or queued; `error` after a failed run; `fresh` after a successful one; `idle` before any |
| `age` | the session clock's now minus the entry's `fetchedAt` | none |
| `lastError` | the entry's error text | the last failure's text |

**The command log.** `runList` records each command list it runs: the list's JSON Pointer (the event, lifecycle key or
step that holds it), each command's key, target and path, and the number of the reactive batch it ran in
(`RuntimeCore::currentBatch`, 0 outside every batch). The log keeps the latest `kCommandLogSize` (256) lists.

**The counters.** `counters()` is `Runtime::stats()` for the screen's runtime:

- flushes that ran a queued Effect;
- Effects processed, in all, in the latest flush, and the most in one flush;
- outermost batches opened;
- misuses reported, with the sites of the latest 16.

Misuses are counted with atomics, because a misuse off the owner is reported from the thread that made it.

## Reaching the focused screen

A frontend does not know the shell's screens. `MountedShell::inspectFocused(visit)` hands the focused screen's
inspector to `visit` before it returns, or calls nothing. The default calls nothing. The generic client's shell passes
its shown screen's inspector (`DocumentScreenHost::inspector`). The inspector is a temporary of the call, so nothing
keeps it past a screen that closes or is remounted.

## The Qt Quick window

`qt_quick::InspectorWindow(focused)` is a separate `QQuickWindow` with its own `QQmlEngine`. Its QML is compiled from
a string in `src/qt_quick/inspector.cpp`, so it is not part of the `MorphUi` module.

- `attachTo(window)` installs an event filter on the application's window. Ctrl+Shift+I toggles the inspector before
  any item of that window sees the key, wherever focus is.
- While shown, it reads the focused screen through `focused` twice a second (`refresh()`). It shows the counters, a
  line per query or mutation with a *Refetch* button for each query, a line per mounted node with its evaluated
  properties, and a line per logged command list.
- *Refetch* calls `Inspector::refetch`.
- `text()` is what it shows, section by section, as plain text, for a test.
- `qt_quick::Frontend` builds one when `FrontendConfig::devTools` is set: the generic client sets it from `--dev` or
  `MORPH_UI_DEV=1`. It reads the shell through `inspectFocused`. When the loop ends it hides the window and stops
  reading, before `runApp` destroys the shell.

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| How a release build leaves it out | A preprocessor switch on the target's interface, plus a source file added only when it is on | The library is header-only: the only way to compile code out of every consumer is a macro they all see. A macro decided per translation unit would change `ScreenCore`'s layout between them. |
| What the inspector returns | Copies | A reference into a scope or a signal would let a caller write state. |
| How it refetches | The document's own `refetch` command, as a user event | The spec allows writes only through commands a user could run. A direct `QueryRuntime::refetch` would skip the log and the user-event path. |
| Where nodes come from | Recorded at lowering, held weakly | The view tree has no document kinds, ids or expressions. The document alone does not know which scopes are mounted, so a node is listed while the scope it was lowered in lives. |
| How the window finds the screen | A visitor on `MountedShell` | The frontend owns the window and the shell owns the screens. A visitor keeps the inspector a temporary, so nothing outlives a screen. |
| The counters | In every build | They cost a few increments, and a counter that exists only in development builds could not be compared with a release run. |

## Not yet

- **The node under the pointer** is not highlighted in the screen (spec 6 §21). The window lists nodes; it does not map
  the pointer to one.
- **The TUI frontend** has no inspector window.

# The host contract — design

How a host mounts UI documents and closes them. A host that owns its own shell and the framework's workspace shell
use the same contract, so a document does not know which one shows it. It implements spec 6 §3's close protocol,
§4's host contract and §20's routes (`docs/superpowers/specs/2026-10-08-app-hosting-design.md`).

| Header | What it holds |
|---|---|
| `include/morph/ui/host.hpp` | `ParamMap`, `MountError`, `ScreenHandle`, `ScreenHost`, `CloseGuard`, `CloseChoice`, `DocumentScreenHost` |
| `include/morph/ui/route.hpp` | `Route`, `routeOf`, `parseRoute` |
| `include/morph/ui/route_history.hpp` | `IHistory`, `RouteHistory`, `fragmentOf`, `routeOfFragment` |
| `include/morph/ui/testing/memory_history.hpp` | `MemoryHistory`, the tests' `IHistory` |

It sits on the interpreter ([interpreter.md](interpreter.md)): `DocumentScreenHost` mounts through an `Interpreter`
and is its `InterpreterHost` for `navigate`.

## Contents

- [The contract](#the-contract)
- [DocumentScreenHost](#documentscreenhost)
- [The close protocol](#the-close-protocol)
- [Routes](#routes)
- [Route history](#route-history)
- [Design decisions](#design-decisions)
- [Not yet](#not-yet)

## The contract

| Type | Role |
|---|---|
| `ParamMap` | A screen's params by name: `ValueRecord`, what `Interpreter::mount` takes. |
| `MountError` | `{screenId, reason}`: an unknown screen, a refused param, a refused mount. |
| `ScreenHandle` | One mounted screen: `title()`, `dirty()` and `identity()` as `reactive::Computed`, `requestClose(done)`, `forceClose()`, and the save seam `canSave()` and `save(done)`. |
| `ScreenHost` | `mount(screenId, params)`, `identityOf(screenId, params)`, and `onNavigate(screenId, params)`, which the host implements. |

`identity()` is beside spec 6 §4's `title` and `dirty` because a host re-keys a tab when a screen's identity
changes (§15). Spec 6 §4 lists it.

A host never destroys a handle whose close is pending, except after `forceClose`. Destroying a handle closes its
screen as `forceClose` does.

## DocumentScreenHost

The framework's `ScreenHost` over UI documents. An application's shell derives from it and implements `onNavigate`.
It is built over the interpreter's `Session` and `Dispatcher`, and `DocumentScreenHostOptions`:

| Option | Meaning |
|---|---|
| `documents` | The loaded document of a screen id, or null. A loaded bundle answers it. |
| `backend` | The `IViewBackend` screens mount on. |
| `guard` | The `CloseGuard` for a dirty screen without `onClose`. Empty keeps every such screen open. |
| `saveFailed` | Called when the guard's *save and close* left the screen dirty, so the host shows the failure. Optional. |
| `application` | The application's `InterpreterHost`, for every command but `navigate`. |
| `diagnostics` | Called with each diagnostic the screens report. |

- **`mount`** looks the document up and mounts it. An unknown id is `MountError{…, "unknown screen"}`; the
  interpreter's refusal (a missing or mistyped param, a document without a view) is `MountError` with its path and
  reason. `rootOf(handle)` gives the mounted root widget, which the host places in its layout; on Qt Quick,
  `qt_quick::Backend::itemOf(*rootOf(handle))` is the screen's root `QQuickItem`.
- **`mountInto(screenId, params, parent)`** mounts as `mount` does, with the screen's root widget appended to the
  host's own container instead of made a backend root: a tab's page, a pane. The container outlives the handle. A
  refused mount leaves the container as it was.
- **`identityOf`** evaluates the document's `identity` over the params alone, without mounting, and answers its
  canonical JSON text. A screen with no `identity`, one whose `identity` reads anything but params (state, a query,
  the session), and one whose identity is `null` answer `std::nullopt`: their identity is known only once mounted,
  through the handle.
- **`prefetch(screenId, params)`** starts the screen's `prefetch` queries before it mounts (spec 5 §5).
  - Each named query's body is evaluated over the params alone, and its cache entry is warmed under the token the
    mounted screen's alias will have: `shared:<key>` for a keyed alias, `shared` for a stateless model's.
  - A private alias's token is made fresh at mount, so nothing could read what a prefetch warmed: its queries are
    skipped.
  - Also skipped: evaluations, a key or body that reads more than the params, and an entry already fresh or in
    flight.
- **`routeOf(screenId, params)` and `parseRoute(route)`** are `routeOf` and `parseRoute` of [Routes](#routes) over
  this host's documents.
- **`navigate`.** A document's `navigate` to a screen reaches `onNavigate(NavigateRequest)`, which by default
  calls `onNavigate(screenId, params)`; a host that shows dialogs, drawers or windows overrides the first to read
  `as`. A `navigate` that names no screen (a route) goes to the application's host.
- **Call and return** (§16). A `navigate` with `onResult` or `onCancel` files a call. The next `mount` of that
  screen with equal params adopts it, so the host decides how to show the callee and the framework still ties the
  answer to the opener. The callee answers exactly once: `{"return": e}` runs `onResult` with `e`; `close: self` and
  any other close (the platform's, `forceClose`, a destroyed handle) run `onCancel`. After `return` or `close: self`
  the host closes the callee (posted, since the command is still running in it) and calls `onScreenClosed(handle)`,
  so the host drops its tab or dialog. A `return` from a screen that answers no call goes to the application's
  host.
- `title()` is the document's `title` as text: a string as is, `null` as empty, anything else as its canonical JSON.
  `dirty()` is whether `dirty` is `true`. `title()` and `identity()` keep their last values once the screen has
  closed, and `dirty()` is then `false`.

## The close protocol

`requestClose(done)` follows spec 6 §3:

1. **With `onClose`**, the handle runs it (`Screen::beginClose`). The screen closes once `onClose`'s batch has
   ended and `Screen::pending()` is `false`, unless `cancelClose` ran. The flag `cancelClose` sets is shared with
   every command list `onClose` starts, such as the `onResult` of a confirm dialog it opened, so the decision is read
   when the pending work ends, not when `onClose` returns.
2. **Without `onClose`**, a screen whose `dirty` is not `true` closes at once. A dirty one is put to the
   `CloseGuard`, which answers once with a `CloseChoice` (below). Without a guard it stays open.
3. `done(closed)` is called exactly once per request, **posted** to the runtime's owner, never inside the document's
   handler. A request made while one is pending joins it and gets the same answer. A request on a closed screen is
   answered `true`.
4. **`forceClose()`** skips `onClose` and the guard, destroys the screen (which runs `onUnmount`), and answers a
   pending request with `true`.

**Saving.** `canSave()` is `true` for a document screen that declares `onSave`; a host screen's handle overrides it
with `save`. `save(done)` runs `onSave`, waits until `Screen::pending()` is `false`, and answers whether `dirty` is
then not `true`. The answer is read in a task posted after the work settles, so a mutation's `onSuccess` has cleared
`dirty` first. A save while one waits joins it. A screen that cannot save answers `false` at once, and a save whose
screen closes first is never answered, so nothing calls back into a closed screen. A save never closes the screen. A host screen's handle implements `requestClose` itself, so its own close decides
whether to offer saving; its `canSave` and `save` let a shell save any tab the same way, a document screen or not.

**The three-choice guard** (spec 6 §15). The `CloseGuard` is the host's drawing, typically a dialog. It is called
with the screen and a `choose` callback, and answers once with a `CloseChoice`:

- It offers *save and close* only when `screen.canSave()`.
- `Discard` closes the screen without running `onSave`, and `Cancel` keeps it.
- `Save` runs `save`, then closes the screen if it is clean. Otherwise it calls the options' `saveFailed(screen)` and
  keeps the screen open. `Save` on a screen that cannot save keeps it open.

The handle carries the choice out from its own state, which it holds weakly, so a dialog that answers after its tab's
handle was destroyed does nothing.

**What a save waits for** is `Screen::pending()`, as `onClose` does: dialogs and mutation runs. A transfer that has not
yet started its mutation is not counted, and a mutation queued offline stays pending until its replay, so a
*save and close* made offline waits for it.

The workspace's close-others, close-all and close-saved walks go through `requestClose`, so they stop at a kept
screen as at any other veto.

**What counts as pending.** `Screen::pending()` is a tracked read of the screen's `pendingWork` counter: an open
dialog or drawer, and a mutation run in flight, in any of the screen's scopes. A dialog's content scope counts from
its creation to its destruction, which is from opening to closing. A mutation counts from `run` to its settle, and a
mutation destroyed with a run in flight gives its count back. The handle watches `pending()` with an Effect, so it
waits without polling.

## Routes

A route is the string form of a screen instance (spec 6 §20): `<screen id>?<param>=<value>&…`, the params in their
declaration order.

| Type | Canonical text |
|---|---|
| `int` | decimal |
| `bool` | `true` / `false` |
| `decimal` | `num/den@dp`: the canonical fraction and the places |
| `key` | `i:<int64>` or `s:<text>` |
| `string` | percent-encoded: every byte but RFC 3986's unreserved characters |

- **`routeOf(document, params)`** writes it. A null optional is left out. A required param that is missing, a
  value not of its declared type, and a param of a type with no route form (`quantity`, `timestamp`, a list, a
  record) give no route, with the reason. Spec 6 §20 names no form for those types, and a route never invents one.
- **`parseRoute(text, lookup)`** reads one back against the screen's declared `params`. Refused, as
  `route <text>: <reason>`: an unknown screen, a param the screen does not declare or gives twice, a value that does
  not parse as its type (including a truncated or non-hexadecimal escape, a zero denominator, places past
  `kMaxDecimalPlaces`), and a missing required param.
- `parseRoute(routeOf(id, p)) == p` for every representable instance.

## Route history

A shell keeps the route of the screen it shows in a history the user moves through with back and forward (spec 3 §10):
the browser's on WebAssembly, where the route is the page's fragment, `#/<route>` (`fragmentOf`, `routeOfFragment`).

- **`IHistory`** is the seam: `current()`, `push(route)`, `replace(route)`, and `onPop(callback)`, which reports a
  move back or forward, or an address the user edited. A history reports no move for the address it is already at.
- **`RouteHistory(history, open)`** follows one. `initial()` is the route the history held when it was made: a
  reloaded page's, or a shared link's. The shell opens it first, when the session lists its screen, and otherwise
  opens its start screen and tries `initial()` again after a sign-in.
- **`shown(route)`**, called after each mount, records the shown screen. The first call replaces the entry the history
  started on, so the first screen adds no step back to an empty page. A later one adds an entry, unless the route is
  the current entry's. A screen with no route (a param with no route form) leaves the history as it is.
- **A move back or forward** calls `open(route)`, which shows the route's screen, or the start screen for a route the
  session cannot open, before it returns. The first screen it shows replaces the moved-to entry, so a refused route
  does not stay in the history. A screen shown after that, such as a redirect, adds an entry. `open` may destroy the
  `RouteHistory`.
- **A screen with no route** (`shown("")`) leaves the history as it is. After a move back or forward, the next screen
  with a route then replaces the moved-to entry.
- **Destroying a `RouteHistory`** leaves its callback in the history, where it does nothing. It does not clear it,
  because that could clear the callback of a newer `RouteHistory` over the same history.

A refused route the user typed leaves two equal entries, the start screen's twice, because a browser cannot remove an
entry. The first back then reopens the same screen.

`MemoryHistory` holds the entries in memory, with `back()`, `forward()` and `follow(route)` for a typed address.

**`qt_quick::BrowserHistory`**, built on WebAssembly only, is the page's history. `push` and `replace` call
`history.pushState` and `history.replaceState` with the route's fragment, which change the fragment without a
`hashchange`. A move back or forward, or an edited address, is found by reading `location.hash` on a 100 ms `QTimer`.
The callback therefore runs on the Qt event loop, never inside a JavaScript handler, and no JavaScript function is
bound to C++. `push` and `replace` are skipped while the address holds a move the timer has not reported yet. The user's move wins,
and the next tick reports it. The generic client passes one in its WebAssembly build. An explicit `--screen` there
wins over the route the history holds. A move made while a sign-in reloads the catalog, and the link the page was
opened with, are opened once the reload ends, before any navigation the sign-in asked for.

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Where the pending count lives | A counter signal on the screen's core, moved by dialog scopes and mutation runs | A scope knows only its parent, so a screen cannot walk down to its dialogs; the places that start and end work can count it. |
| When `cancelClose` is read | When the pending work has ended | A confirm dialog opened by `onClose` answers later; reading the flag when `onClose` returns would close under the user. |
| Answers | Posted to the owner, joined while pending | A host's callback may destroy the handle; it must not run inside the handler that is still using it. |
| Who decides a dirty screen without `onClose` | The host, through `CloseGuard` | The choice (save, discard, cancel) is the shell's UI (spec 6 §15), not the document's. |
| `identityOf` | Evaluated over params only | Opening a second instance must be decided before mounting one; an identity that reads more cannot be. |

## Host screens

A catalog entry may name a `host` screen instead of a document (spec 6 §15).
- The application registers a `HostScreenFactory` under that name in a `ScreenHostRegistry` (`add(name, factory)`,
  `find(name)`).
- A factory takes the params and the shell's page container, mounts the native screen into the page, and returns
  its `ScreenHandle`, or a `MountError`. The shell reads that handle's `title`, `dirty`, `identity` and close
  protocol as it reads a document screen's.
- A host screen has no `identity` before it mounts, so opening one is never matched to an open tab by identity.
  Its route is its catalog id.
- Spec 6 §15 has the factory return a `QQuickItem`. A factory here mounts into a backend container instead, so one
  registry serves every backend, the recording backend's tests included.

## Not yet

- **The guard's dialog** (§15) is the host's drawing, given as `guard`. The generic client installs none, so a dirty
  screen without `onClose` stays open there.
- **The Qt Quick renderer as a `ScreenHost`** (§4), exposing each screen's root item.

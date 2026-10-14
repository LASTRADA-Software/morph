# The host contract — design

How a host mounts UI documents and closes them. A host that owns its own shell and the framework's workspace shell
use the same contract, so a document does not know which one shows it. It implements spec 6 §3's close protocol,
§4's host contract and §20's routes (`docs/superpowers/specs/2026-10-08-app-hosting-design.md`).

| Header | What it holds |
|---|---|
| `include/morph/ui/host.hpp` | `ParamMap`, `MountError`, `ScreenHandle`, `ScreenHost`, `CloseGuard`, `DocumentScreenHost` |
| `include/morph/ui/route.hpp` | `Route`, `routeOf`, `parseRoute` |

It sits on the interpreter ([interpreter.md](interpreter.md)): `DocumentScreenHost` mounts through an `Interpreter`
and is its `InterpreterHost` for `navigate`.

## Contents

- [The contract](#the-contract)
- [DocumentScreenHost](#documentscreenhost)
- [The close protocol](#the-close-protocol)
- [Routes](#routes)
- [Design decisions](#design-decisions)
- [Not yet](#not-yet)

## The contract

| Type | Role |
|---|---|
| `ParamMap` | A screen's params by name: `ValueRecord`, what `Interpreter::mount` takes. |
| `MountError` | `{screenId, reason}`: an unknown screen, a refused param, a refused mount. |
| `ScreenHandle` | One mounted screen: `title()`, `dirty()` and `identity()` as `reactive::Computed`, `requestClose(done)`, `forceClose()`. |
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
   `CloseGuard`, which calls `decide(true)` to close it and `decide(false)` to keep it. Without a guard it stays open.
3. `done(closed)` is called exactly once per request, **posted** to the runtime's owner, never inside the document's
   handler. A request made while one is pending joins it and gets the same answer. A request on a closed screen is
   answered `true`.
4. **`forceClose()`** skips `onClose` and the guard, destroys the screen (which runs `onUnmount`), and answers a
   pending request with `true`.

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

- **The guard's three-choice dialog** (§15) is the host's drawing: the shell decides through the `CloseGuard`.
- **The Qt Quick renderer as a `ScreenHost`** (§4), exposing each screen's root item.

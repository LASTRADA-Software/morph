# Application hosting: implementation plan (part 8, #893)

**Goal.** A host mounts UI documents through one contract, `ScreenHost`, and closes them through one protocol,
whether it owns its own shell or is the framework's workspace shell. Every screen instance has a route.

**Specs.** Spec 6 (`docs/superpowers/specs/2026-10-08-app-hosting-design.md`), all of it; its tests are §12. In-tree:
`docs/spec/ui/interpreter.md`, `include/morph/ui/interpreter.hpp` (`Screen`, `Interpreter`, `InterpreterHost`),
`include/morph/ui/frontend.hpp` (`AppSource`, `MountedShell`).

## What planning found

1. **Most of spec 6 §3 is already in the interpreter.** `title`, `identity`, `dirty`, `onMount`, `onUnmount`,
   `onClose` with `cancelClose`, `onSave` and `onBackendChange` are parsed by `loadDocument` and run by `Screen`;
   `signIn`/`signOut`, `{"session": …}`, `{"env": …}`, the app document and `navigate` with `onResult`/`onCancel`
   reach the `InterpreterHost`. What does not exist is the **host side**: `ScreenHandle`, `ScreenHost`, `MountError`,
   the asynchronous close protocol (`requestClose(done)` waiting for pending work, `done` posted once, a second
   request joining the first), `forceClose`, `identityOf` and `onNavigate`. `Screen::requestClose()` today runs
   `onClose` synchronously and answers at once.
2. **Only two sections depend on the forms engine (#892).** §7, the workspace shell, is `workspace.hpp`, which
   #892's plan delivers in its slice (c); §10 is the forms overlay. Everything else can be built now.
3. **The part is four pull requests.** Spec 6 is 846 lines over twenty-one sections. Split by what each layer
   depends on:
   - **(a) this PR, the host contract:** `ui/host.hpp` (§4), the close protocol (§3), `identityOf`, `onNavigate`,
     call and return through the host (§16), and routes (§20). `Part of #893`.
   - **(b) session and scope:** the app root scope and session (§5), preferences (§8), offline as a backend switch
     (§9), the host vocabulary (§14), settings and messages (§19).
   - **(c) the design package on Qt Quick:** tokens, host components, fallbacks, a11y (§6), development tools
     (§21), and the Qt Quick renderer as a `ScreenHost` (§4's root item).
   - **(d) the workspace shell and native panes:** §7, §15, §17's reserved chords, after #892 (c). `Closes #893`.

## Design (a)

### The contract (`ui/host.hpp`)

As spec 6 §4, with the interpreter's types:

- `ParamMap` is `ValueRecord` (`std::map<std::string, Value, std::less<>>`), which `Interpreter::mount` already
  takes. `MountError { screenId, reason }`.
- `ScreenHandle`: `title()` (`reactive::Computed<std::string>`), `dirty()` (`reactive::Computed<bool>`),
  `requestClose(done)`, `forceClose()`. Added beside spec 6 §4: `identity()` (`reactive::Computed<Value>`), which
  §15's re-keying needs and §4 omits; the spec is updated in this PR.
- `ScreenHost`: `mount(screenId, params)`, `identityOf(screenId, params)`, `onNavigate(screenId, params)`.

### The framework's host over documents (`DocumentScreenHost`)

An abstract `ScreenHost` that mounts documents through an `Interpreter`; an application's shell derives from it
and implements `onNavigate`. It takes, at construction, the interpreter's session and dispatcher, a document lookup
(`std::function<std::shared_ptr<Document const>(std::string_view)>`, which a loaded bundle answers), the view
backend, and the close guard (below). It is the interpreter's `InterpreterHost` for `navigate`, `closeSelf` and the
call-and-return results, and forwards the rest to an optional application `InterpreterHost`.

- **`mount`** looks the document up, mounts it, and wraps the `Screen` in a handle. An unknown id, a refused param
  and a refused mount are each a `MountError` with the interpreter's reason.
- **`identityOf`** evaluates the document's `identity` over the params alone, without mounting. An identity that
  reads anything but params cannot be known before the screen exists; it answers `std::nullopt`, and the handle's
  `identity()` is then the only source. A screen with no `identity` answers `std::nullopt`.
- **Navigation.** The interpreter's `navigate` reaches `onNavigate` with the screen id and the params; `as`, the
  `onResult`/`onCancel` callbacks and `return`/`close: self` (§16) are kept per opened screen, so `closeSelf` on
  the callee runs the opener's callback exactly once and closing it by the platform runs `onCancel`.

### The close protocol (§3)

The handle owns it; the interpreter gains what the handle has to observe.

- **`Screen::pending()`**, a reactive `bool`: a mutation of the screen's scopes is in flight, or a dialog of the
  screen is open. The mutation runtime already holds its in-flight count in a signal, and a dialog its `open`
  state; `pending` reads them, so the handle can wait for it to become `false` without polling.
- **`requestClose(done)`.** With `onClose`, the handle runs it; `cancelClose` anywhere before the close completes
  keeps the screen open. The screen closes once `onClose`'s batch has ended and `pending()` is `false`. Without
  `onClose`, a screen whose `dirty` is not `true` closes at once, and a dirty one is put to the **close guard**, a
  callback the host gives `DocumentScreenHost` (`std::function<void(ScreenHandle&, std::function<void(bool)>)>`):
  the workspace shell's three-choice dialog in (d), anything an embedding host wants before. The default guard keeps
  the screen open.
- `done(closed)` is called exactly once per `requestClose`, posted to the runtime's owner (`Runtime::owner()`), never
  from inside the document's handler. A second `requestClose` while one is pending joins it: both are answered with
  the same outcome.
- **`forceClose()`** skips `onClose` and the guard, ends any pending request with `done(true)`, and destroys the
  screen, which runs `onUnmount`.
- A closed handle has no screen; its `title` and `dirty` keep their last values.

### Routes (§20, `ui/route.hpp`)

`routeOf(screenId, params, declared)` writes `<id>?name=value&…` in the declared params' order, omitting null
optionals; `parseRoute(text, lookup)` reads one back against the screen's declared `params`.

- Canonical text: `int` in decimal; `decimal` as `num/den@dp`; `key` as `i:<int64>` or `s:<text>`; `string` and
  every text percent-encoded (RFC 3986 unreserved characters kept); `bool` as `true`/`false`. `quantity`,
  `timestamp`, lists and records have no route form in §20: a screen whose params include one has no route, and
  `routeOf` says so instead of inventing a spelling. The spec is updated to state it.
- Refused, with the route and the reason: an unknown screen, a missing required param, a value that does not parse
  as its type, a param the screen does not declare.
- Round trip: `parseRoute(routeOf(id, p)) == p` for every representable `p`, which the tests check per type.

### Not in (a), and why

- Re-keying a tab, the guard's dialog, MRU, `reuseEmpty` and the session record are the workspace shell's (d).
- The Qt Quick renderer as a `ScreenHost` exposing the root item is (c); (a) delivers the contract it implements.
- A `window` falling back to a dialog is a renderer decision (c); (a) passes `as` through.

## Tasks (one commit each)

1. This plan.
2. `ui/route.hpp`: `routeOf`, `parseRoute`, the canonical text per type; tests for the round trip per type and every
   refusal.
3. `Screen::pending()` and `Screen::identityFor(params)` in the interpreter; tests that a mutation in flight and an
   open dialog each make it `true`.
4. `ui/host.hpp`: `ParamMap`, `MountError`, `ScreenHandle`, `ScreenHost`, `DocumentScreenHost` with the handle and
   the close protocol; tests for spec 6 §12 "Lifecycle" and "Host contract" with a fake shell over
   `RecordingBackend`.
5. Navigation and call and return through `DocumentScreenHost`: `onNavigate`, `onResult` exactly once, platform
   close as `onCancel`; tests for §12 "Call and return" except `window`, which is (c).
6. `docs/spec/ui/host.md` (new), `docs/spec/ui/interpreter.md`, spec 6 §4 (`identity()`) and §20 (params with no
   route form), CHANGELOG.

## Verification

- Each test above fails with its feature removed: the close protocol answering at once, `done` called inline, a
  second request starting a second `onClose`, `forceClose` running `onClose`, `identityOf` mounting, a route value
  written in a non-canonical form.
- GCC Debug `morph_tests` in full; clang-format on every touched file; Doxygen-complete public API.

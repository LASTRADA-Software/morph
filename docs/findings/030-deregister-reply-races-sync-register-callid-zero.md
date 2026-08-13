---
id: 030
title: a fire-and-forget deregister's "ok" reply can be misrouted to an unrelated later synchronous register, permanently zeroing the new binding's ModelId
subsystem: qt-transport
severity: major
source: rung 2 (bookmarks) task 17 follow-up — TagPresenter::merge flake investigation
disposition: open
test: spec-cited
issue: https://github.com/LASTRADA-Software/morph/issues/65
---

Found while root-causing a reproducible (roughly 1-in-8) flake in
`TagPresenter::merge`'s own test
(`examples/bookmarks/tests/test_tag_presenter.cpp`), which constructed two
short-lived `BridgeHandler<BookmarkModel>` objects back to back in
`Mode::Socket` to seed two bookmarks. The observed symptom was an uncaught
`std::runtime_error("handler not bound")` escaping the *second* handler's
`execute()` call — a message that can only come from `Bridge::executeVia`'s
fast-fail path (`include/morph/core/bridge.hpp:701-704`), which fires when
`binding->currentId.load() == 0`.

## Why this was surprising

`BackendRig::Socket` (the ladder testkit's socket-mode fixture) never opts
into `QtWebSocketBackend::Config::asyncRegistrationEnabled` (defaults
`false`), so every `BridgeHandler` construction there takes the
*synchronous* registration path: `Bridge::registerHandlerImpl` calls
`registerModelWithContext`, which blocks via `QtWebSocketBackend::sendSync`
(a nested `QEventLoop`) until the server's reply arrives. That path's own
doc comment promises exactly this: "every existing embedder ... keeps
registering synchronously, immediately usable the line after
`BridgeHandler`'s constructor returns." So the initial hypothesis — finding
`024`'s async registration-settlement race — did not apply here at all
(that finding is specifically about the opt-in async path `AppContext`
uses); confirmed by instrumented reruns showing the failure is not a slow
round trip but a *permanent* one (a bounded retry-and-repump loop burned its
entire deadline on every failing run rather than ever recovering).

## The actual bug

`QtWebSocketBackend::onTextMessage` (`src/qt/qt_websocket_backend.cpp:341-`)
routes every incoming reply by `env.callId`: non-zero ids go to the
`_pending`/`_pendingRegistrations` maps (the async paths); `callId == 0`
is treated as *the* one outstanding synchronous call and unconditionally
handed to `_pendingReply` + `_syncLoop->quit()`.

But `callId == 0` is not unique to synchronous calls. Two client-side call
sites both leave the envelope's `callId` at its default-constructed `0`:

- `QtWebSocketBackend::registerModel` (`sendSync(makeRegister(typeId))`) —
  the synchronous register path described above, which *does* park a
  `_syncLoop` and wait.
- `QtWebSocketBackend::deregisterModel` (`sendTextMessage(encode(makeDeregister(mid.v)))`)
  — explicitly fire-and-forget, sent without parking anything, precisely so
  destroying a `BridgeHandler` never blocks.

The server replies to *both* the same way: `deregister` gets an ordinary
`makeOk(env.callId)` reply (`include/morph/core/remote.hpp:1119`), which
therefore also carries `callId == 0`.

If a `BridgeHandler` is destroyed (sending its fire-and-forget deregister)
and a **different** `BridgeHandler` on the same connection is constructed
immediately after (parking a `sendSync` for its own register), the
deregister's reply and the register's reply are indistinguishable on the
wire — both `callId == 0`. Whichever arrives first is handed to the parked
`_syncLoop`. If it is the deregister's stray "ok" (which carries no
`modelId`), `registerModel` decodes it, reads a zero/default `modelId`, and
stores `ModelId{0}` into the *new* binding's `currentId` — permanently: the
real register reply that arrives moments later has nowhere to go
(`_syncLoop` was already reset to `nullptr` when the mismatched reply quit
the loop), so it is silently dropped. Every subsequent dispatch on that
binding then fails fast with `"handler not bound"`, forever, not just for a
transient window.

## Reproduction

`examples/bookmarks/tests/test_tag_presenter.cpp`'s `TagPresenter::merge`
test seeded two bookmarks via two short-lived `BridgeHandler<BookmarkModel>`
objects (construct, dispatch, destruct, construct again) immediately
followed by `TagPresenter`'s own handler construction — three
register/deregister boundaries on one connection in quick succession, each
an opportunity for this race. Empirically: roughly 1 run in 6-15 in
isolation; verbose (`--success`) output, which adds enough per-assertion I/O
to perturb timing further, pushed the observed rate as high as 70-90%. The
same pattern (`seedBookmark` constructing a fresh handler per call, called
twice) was independently confirmed to trigger the identical failure in
`examples/bookmarks/tests/test_shared_feed_presenter.cpp`.

A third, structurally distinct reproduction site: rung 3 (polls)'s
`examples/polls/tests/test_shared_instance_lifecycle.cpp` hit the identical
`callId == 0` bucket-sharing hazard not via a synchronous *register*, but via
a synchronous **`instances()`** call — `BridgeHandler::instances()` is also
an ordinary `sendSync` caller competing for the same bucket. Reusing a
connection that had just sent a fire-and-forget `deregister` (from a
`BridgeHandler` going out of scope) for a subsequent `instances()` probe
reliably risked the deregister's stray "ok" being delivered to the parked
`instances()` wait instead. Worked around identically to the other two
sites: use a genuinely fresh connection (never a party to a recent
deregister) for the probing call, rather than reusing one of the
just-released connections. This confirms the hazard is general to *any*
`sendSync`-based call type (`register`, `attach`, `instances`, ...), not
specific to registration — consistent with this finding's own "What morph
would need" direction 2 ("every `sendSync`-based call... needs a real
per-call `callId`"), which a fix scoped to `register` alone would not have
closed.

A fourth site, in production code rather than a test — `QtWebSocketBackend::attachModel`'s
own empty-`identity.primary` branch (`src/qt/qt_websocket_backend.cpp:283-287`,
`registerModelShared`'s identical branch at `:273-274` is the same shape one
call shallower) does exactly this: a fire-and-forget `deregisterModel(current)`
immediately followed by the synchronous `registerModelWithContext(...)` — a
deregister-then-sendSync-register pair on the same connection, with no event
processing in between. This is not a test artifact or a testkit-only pattern;
it is the framework's own code taking the two-step "release the empty-key
instance, then plainly re-register" path any `AllowShared` handler resolves to
whenever it re-points to an unkeyed action. Confirmed independently across two
separate reviews of this codebase before being written down here.

## What shipped instead (test-level workaround, not a framework fix)

Both files were changed to construct **one** `BridgeHandler<BookmarkModel>`
per test case and reuse it across every seed call, declared before the
presenter under test so it is destroyed *after* — deferring its one
deregister to the end of the test, past every synchronous registration that
test still needs to make. This removes the adjacency the race depends on
(a deregister immediately followed by an unrelated register on the same
connection) without touching `QtWebSocketBackend`/`Bridge`. Verified via
140+ repeated runs of the originally-flaking test case and 35+ full
`ladder_bookmarks_tests` runs (`--order rand`, multiple seeds including the
two that reproduced it during review) with zero failures; `ladder_pastebin_tests`
and `ladder-0` (112 tests total) re-verified unaffected — pastebin's own
tests never construct two handlers back to back on the same connection
index, so this bug was latent there but never triggered.

## What morph would need

`callId == 0` should not be an overloaded "the one synchronous reply I'm
waiting for" bucket that any fire-and-forget reply can also land in. Two
directions, either sufficient on its own:

1. Give `deregisterModel`'s request a real (non-zero) `callId` and either
   drop its reply unmatched (nobody is waiting for it — `onTextMessage`'s
   non-zero-`callId`-with-no-`_pending`-entry path already handles an
   unmatched async reply gracefully) or track it in `_pending`/a dedicated
   map and discard the result once it lands, so it can never again collide
   with an unrelated synchronous wait.
2. Give every `sendSync`-based call (register, registerShared, attach,
   assign, instances) a real per-call `callId` too, and have
   `onTextMessage`'s sync branch match on that id specifically rather than
   accepting *any* `callId == 0` message as "the" parked reply.

Either change is scoped to `include/morph/qt/qt_websocket_backend.hpp` /
`src/qt/qt_websocket_backend.cpp` (and, for direction 2, the reply-routing
branch in `onTextMessage`) plus, for direction 1, `deregister`'s handling in
`include/morph/core/remote.hpp` if it should stop replying to deregister at
all. Out of scope for the ladder task that found it (rung 2 testkit, not
`include/morph/`).

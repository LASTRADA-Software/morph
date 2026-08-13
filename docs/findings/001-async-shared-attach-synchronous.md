---
id: 001
title: Shared/keyed model attach has no async path (aborts WASM's page)
subsystem: bridge
severity: blocker
source: LADDER.md framework prerequisite 1 (round-7 review); TESTING.md "WASM reality"
disposition: fixed
test: tests/test_async_registration.cpp; tests/qt/test_qt_websocket.cpp
---

`IBackend::registerModelShared` and `IBackend::attachModel`
(`include/morph/core/backend.hpp`, ~lines 179–214) are synchronous virtuals;
`Bridge`'s shared/keyed attach path (`include/morph/core/bridge.hpp`, the
`registerModelShared`/`attachModel` call sites around lines 296–315 and 594)
calls them inline from the caller's thread. `IBackend::registerModelAsync`
(`backend.hpp` ~line 146) covers only the *plain* (non-shared) registration
path — there is no `registerModelSharedAsync`/`attachModelAsync`.

On WASM, a synchronous call that nests an event loop while waiting for a
server round-trip aborts the page (the same class of bug `registerModelAsync`
was built to fix for plain registration — see
`tests/qt/test_qt_websocket.cpp`'s `[issue26]`-tagged tests, which prove the
plain async path but not the shared one).

**What should happen:** a `registerModelSharedAsync`/`attachModelAsync` pair
with the same non-blocking contract as `registerModelAsync` (returns
immediately, delivers the bound id via a callback pumped through the event
loop), so a WASM client's first `GetPaste`/`AttachBoard`-style call cannot
abort the page.

**What happens instead:** any WASM client that resolves burn/board/poll
atomicity via a shared keyed instance must avoid the synchronous attach path
entirely today, or accept the abort risk. Rung 1's pastebin README documents
choosing SQL-level atomicity instead of a shared instance specifically to
duck this gap (see `examples/pastebin/README.md`, "Shared vs. unshared
instance"); rung 3 cannot duck it (`AllowShared`-over-WebSocket is rung 3's
mandate) and needs this finding resolved or explicitly re-scoped first.

**Resolution (rung 3 framework prerequisite, Task 2 of
`docs/superpowers/plans/2026-08-07-ladder-rung3-framework-prereqs.md`).** The
pair this finding asked for exists:
`IBackend::registerModelSharedAsync` (`include/morph/core/backend.hpp:187`)
and `IBackend::attachModelAsync` (`backend.hpp:286`), both with
`registerModelAsync`'s exact opt-in contract — return `true` and later invoke
exactly one callback, or return `false` and let the caller fall back to the
synchronous path unchanged, so no backend that has not opted in changes
behavior. `Bridge` prefers them wherever it previously called the synchronous
virtuals: `attachModelAsync` at `include/morph/core/bridge.hpp:468` and
`registerModelSharedAsync` at `bridge.hpp:576`, with
`ensureBoundAsync` covering the result-keyed (creating) path.
`morph::qt::QtWebSocketBackend` implements both, which is what makes a
browser tab's first keyed attach non-blocking. Covered by
`tests/test_async_registration.cpp` (async preference, synchronous fallback,
inline completion, inline failure, stale reply after `switchBackend()`, reply
after `~Bridge()`, and the result-keyed mirror of all three) and by
`tests/qt/test_qt_websocket.cpp`'s `[issue26][shared-instances]` cases over a
real WebSocket.

Rung 3's `polls` is the first consumer: `BridgeHandler<PollModel,
AllowShared>` dispatching the payload-keyed `OpenPoll` is exactly the
"first `OpenPoll` a WASM tab makes" this finding named
(`examples/polls/gui_wasm/main_wasm.cpp`, `examples/polls/README.md`).

**Closed.** The disposition stays `fix-scheduled` only because
`examples/FINDINGS.md` defines no `closed` value; nothing further is
scheduled against it. Caveat kept honest: the WASM half is verified by
compile gate and by the non-blocking contract's tests on the native
WebSocket backend — no Emscripten toolchain exists in this repository, so
no browser tab has actually exercised it.

---
id: 017
title: registerModelAsync fails permanently if called before the socket connects (no queueing)
subsystem: qt
severity: blocker
source: examples/LADDER.md rung 0 Task 10 (WASM-remote spike); examples/TESTING.md "WASM reality"
disposition: fixed
test: examples/common/testkit/test_wasm_registration_path_native.cpp; tests/qt/test_qt_websocket.cpp
issue: https://github.com/LASTRADA-Software/morph/issues/54
---

`QtWebSocketBackend::registerModelAsync()` now queues a registration attempt
made before the socket connects and retries it once the `connected` signal
fires — see `tests/qt/test_qt_websocket.cpp`'s "registerModelAsync called
before the socket connects queues and retries once connected fires" and this
finding's own test, whose "immediately after Bridge construction" case now
resolves natively instead of hanging. The history below (originally: no
queueing, a permanent silent failure) is preserved as the record of how the
gap was found; the fix closes exactly the case it describes.

`QtWebSocketBackend::registerModelAsync()` (`src/qt/qt_websocket_backend.cpp`,
~lines 152–176) checks `if (!_connected) { onError("disconnected"); return
true; }` before assigning a call-id and sending the register message. This
check fires — and fails the registration permanently — whenever
`registerModelAsync` is invoked before the underlying `QWebSocket` has
finished its handshake, which is exactly the situation immediately after
constructing a `QtWebSocketBackend` and a `Bridge` around it: `_socket.open()`
runs in the constructor but is inherently asynchronous, so `_connected` is
still `false` at the moment `Bridge`'s constructor returns control to the
caller (no event-loop turn has run yet). There is no queueing: the register
attempt is not retried once the connection later comes up.

`Bridge` does install a `setReconnectHandler` that re-registers every live
binding — but `QtWebSocketBackend`'s `connected` signal handler explicitly
fires that only on a *subsequent* reconnect (`isReconnect && _reconnectHandler`),
never on the first connect (see its own comment: "initial registration is
handled by the BridgeHandler ctors"). So a `Bridge::registerHandler()` /
`BridgeHandler` construction called synchronously right after wiring up the
`Bridge` has no path to ever succeed if the socket was not already connected
at that exact instant.

Every existing test that exercises the async registration path
(`tests/qt/test_qt_websocket.cpp`'s `[issue26]` tests) sidesteps this by
calling `REQUIRE(backendPtr->waitForConnected())` *before* constructing the
`Bridge` and registering — which blocks (nests an event loop) until the
connection is up. `TESTING.md`'s own "WASM reality" section says
`waitForConnected()` is exactly what a WASM client must **not** do (it hangs
the page), which means every piece of prior evidence that
`asyncRegistrationEnabled=true` is "WASM-safe" was gathered in a call order a
real WASM client cannot use.

**How this was found.** Task 10 (the WASM-remote spike) wrote
`main_wasm.cpp` and `test_wasm_registration_path_native.cpp` following the
call sequence the task's own plan drafted: construct the backend with
`asyncRegistrationEnabled=true`, `setConnectHandler`, then call
`bridge.registerHandler(binding)` immediately, then poll `binding->currentId`
via a WASM-safe `QTimer`/`pumpUntil` loop (no `waitForConnected()`). That
native test reliably timed out — `binding->currentId` never left `0`.
Deferring `bridge.registerHandler(binding)` to fire from inside the
`setConnectHandler` callback (still no nested event loop — fully WASM-safe)
resolves correctly and the round-trip action executes.
`test_wasm_registration_path_native.cpp` ships both as permanent regression
coverage: one `TEST_CASE` proves the broken ordering never resolves (guards
against this gap silently regressing further, and gets updated deliberately
if a future fix adds pre-connect queueing), the other proves the corrected
ordering works end-to-end. `main_wasm.cpp` ships with the corrected ordering;
see both files' comments for the same explanation.

**What should happen:** `registerModelAsync` (or `Bridge::registerHandlerImpl`
above it) should queue a register attempt made before the socket is connected
and retry it once the `connected` signal fires, the same way the reconnect
handler already does for a *subsequent* reconnect — so a WASM caller does not
have to know to defer `registerHandler()`/`BridgeHandler` construction until
after its own `setConnectHandler` callback has fired once. Short of that
framework fix, `qt_websocket_backend.hpp`'s `asyncRegistrationEnabled` doc
comment and `TESTING.md`'s "WASM reality" section should state the ordering
requirement explicitly (register only after the first connect), since
nothing in either place says so today and the task-10 plan's own first draft
got the ordering wrong as a direct result.

**What happens instead:** any caller — this task's own first draft included
— that registers a handler immediately after wiring up a fresh
`QtWebSocketBackend`/`Bridge` pair, without knowing to gate on the first
`setConnectHandler` callback, gets a silent, permanent registration failure
(`binding->currentId` stays `0` forever; no exception, no retry — just a
logged `[registerHandler] async registration ... failed: disconnected` and
nothing else). On a WASM page this would surface as: the "connected" console
log fires, but "result=" never does, matching this rung's own written
fallback plan's second failure mode
(`examples/common/wasm_spike/README.md`) — except the true root cause is a
missing pre-connect queue in `registerModelAsync`, not the `Completion`
execute-deadline gap (finding `002`) that fallback plan's second bullet
guessed at.

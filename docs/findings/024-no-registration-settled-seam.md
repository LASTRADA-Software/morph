---
id: 024
title: no "registration settled" seam — a dispatch issued on connect fails "handler not bound" until the async registration round-trip lands
subsystem: bridge
severity: major
source: rung 1 (pastebin) task 12 — desktop GUI shell against a real server
disposition: open
test: spec-cited
issue: https://github.com/LASTRADA-Software/morph/issues/60
---

This is the neighbouring half of finding `017`. That one is
*register-before-connect*: an async registration issued before the socket is
up fails **permanently**, because `registerModelAsync` rejects it outright and
nothing retries. This one is *dispatch-before-registration-settles*: a
registration issued at exactly the right moment (on connect, as `017`
prescribes) still leaves a window in which every dispatch through the handler
fails, **transiently**, until a server round-trip completes. Same missing
seam, different trigger — and following `017`'s own remedy is what walks you
straight into it.

## The window

`AppContext` (`examples/common/gui/app_context.cpp:41-57`) detects readiness
with `setConnectHandler`, per `017`:

```cpp
rawBackend->setConnectHandler([this] { markReady(); });
```

So every `AppContext::onReady()` callback runs on **socket connect**. That is
where a client builds its `BridgeHandler`s — the earliest point `017` permits.

But `Bridge::registerHandlerImpl` (`include/morph/core/bridge.hpp:895-938`)
does not make the handler usable at that point. It calls
`backend->registerModelAsync(...)` and assigns the binding's id only from
inside the `onRegistered` callback (`bridge.hpp:927`):

```cpp
strongBinding->currentId.store(newId.v);
```

which fires when the server's register reply arrives — a full round trip after
`registerHandlerImpl` returned. Until then `binding->currentId` is still `0`,
and `Bridge::executeVia` (`bridge.hpp:696-704`) fails fast:

```cpp
uint64_t const raw = binding->currentId.load();
...
if (raw == 0U) {
    typedState->setException(std::make_exception_ptr(std::runtime_error("handler not bound")));
    return typed;
}
```

The net effect: for a transient window that opens on connect and closes when
registration settles, a handler that exists, is correctly constructed, and was
registered in exactly the mandated order still rejects every action with
`"handler not bound"`.

## What should happen

`onReady()` — or any equivalent "you may now use the bridge" signal — should
not fire, or should be joinable with something that does not fire, until the
handlers built inside it can actually dispatch. Equivalently: `Bridge` should
either queue a dispatch made against an unbound-but-registering binding until
its id arrives, or expose a seam to wait on ("`whenBound()`", "`isBound()`",
"`registrationSettled()`"). Grepping `include/` and `src/` for all three names
returns nothing: **no such seam exists today**, so a caller cannot even poll
the condition through public API — the only observable is the
`"handler not bound"` exception itself, i.e. you learn the handler was not
ready by failing an action the user asked for.

## What happens instead

Verified, not theorised, on rung 1's desktop client against a real server: an
unconditional `refresh()` from `Component.onCompleted` (i.e. immediately
inside the `onReady()` path) reported `rows=0, status='handler not bound'` on
**every** launch in `Remote` mode. `Local` mode registers synchronously and
never shows it, so the gap is invisible to in-process tests and to the whole
model/presenter suite — it only appears against a socket.

## Shipped mitigation, and the in-repo precedent

Rung 1 mitigates in the view layer, where `examples/TESTING.md` presenter
rule 4 puts timers: `examples/pastebin/gui/qml/Main.qml` runs a `Timer` that
re-issues `refresh()` every 150 ms and stops permanently on the first
`listed` reply (empty or not), clearing the bootstrap error it provoked from
the status line.

This is not a new workaround invented for rung 1. `examples/common/wasm_spike/
main_wasm.cpp:85-101` — written for finding `017`, and predating this task —
already carries the identical shape for the identical reason: after deferring
`BridgeHandler` construction into the `setConnectHandler` callback, it still
cannot dispatch, so it polls `binding->currentId.load() == 0U` on a `QTimer`
and fires its one action only once the id is non-zero. Two independent
consumers, written months apart, both had to hand-roll the same
wait-for-binding loop because the framework offers none.

The same window applies to *every* handler a client builds on connect, not
just the one the bootstrap retries cover: rung 1's forms handler has it too,
so a user who clicks "Create paste" within milliseconds of launch sees the
same error once, with no retry behind it.

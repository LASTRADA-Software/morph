---
id: 019
title: The ladder testkit reaches into four morph detail:: namespaces that have no public seam
subsystem: core
severity: minor
source: rung 0 final review (whole-branch)
disposition: open
test: spec-cited
issue: https://github.com/LASTRADA-Software/morph/issues/55
---

`subsystem: core` is the nearest single value: the reach-ins span
`morph::async`, `morph::exec`, `morph::bridge` and `morph::model`, and the
question they raise — what belongs in morph's public surface — is one
question, not four.

Every `detail::` namespace listed below is excluded from the generated docs
(`docs/CMakeLists.txt`'s `DOXYGEN_EXCLUDE_SYMBOLS`), which is the repo's own
statement that these are not API. Rung 0's testkit nevertheless depends on
all four, because morph offers no public alternative for what each one does.
None of these is a bug; each is a gap with a name.

## The four reach-ins

**1. `morph::async::detail::CompletionState<T>` — constructing a
`Completion<T>` a test controls.**

- `examples/common/testkit/test_pump.cpp:36`, `:44`, `:73`

`pump.hpp`'s `awaitQt`/`pumpUntil` are the things under test, so their tests
need a `Completion<T>` they can resolve, fail, or leave pending on demand —
including resolving one *after* `awaitQt` has already timed out and unwound
(the dangling-reference regression at `:73`). `Completion<T>` has no public
"make me a settleable promise" factory; `CompletionState` is the only way to
get one. Every async library that ships a `Future` also ships a `Promise`;
morph currently ships only the reading half publicly.

**2. `morph::exec::detail::StrandExecutor` and `morph::exec::detail::ModelId`
— testing strand ordering.**

- `examples/common/testkit/test_strand_interleaver.cpp:15`, `:18`, `:19`,
  `:83`, `:86`, `:87`

`DeterministicExecutor` (`strand_interleaver.hpp`) exists to make
strand-ordering bugs reproducible, which means its own tests must place it
underneath a real `StrandExecutor` keyed by real `ModelId`s — the production
component whose ordering is the point. A stand-in would prove nothing.
Per-key serialization is a load-bearing morph guarantee that application and
testkit code has no public vocabulary to talk about.

**3. `morph::bridge::detail::HandlerBinding` — observing registration
completion.**

- `examples/common/testkit/test_wasm_registration_path_native.cpp:66`, `:102`
- `examples/common/wasm_spike/main_wasm.cpp:55`

Under `asyncRegistrationEnabled`, registration completes some time after
`BridgeHandler`'s constructor returns, and `binding->currentId != 0` is the
only observable signal that it succeeded — which is precisely what finding
017's two regression tests assert on, and what the WASM spike polls before
firing its first action. `BridgeHandler` exposes no `registered()` predicate
and no registration callback, so a caller that must gate on registration has
to hold the binding itself.

**4. `morph::model::detail::defaultDispatcher()` /
`defaultRegistry()` — passing a `Config` to `QtWebSocketBackend`.**

- `examples/common/gui/app_context.cpp:33`
- `examples/common/testkit/test_wasm_registration_path_native.cpp:60`, `:98`
- `examples/common/testkit/test_fault_proxy.cpp:79`
- `examples/common/wasm_spike/main_wasm.cpp:51`

This one is purely positional. `QtWebSocketBackend`'s constructor is
`(QUrl, dispatcher = defaultDispatcher(), registry = defaultRegistry(),
[tls,] Config = {})`, so any caller that wants to set `Config` — every WASM
caller must, for `asyncRegistrationEnabled` — has to name the two default
arguments in front of it, and the only names for those defaults live in
`morph::model::detail`. The caller wants neither object; it wants the last
parameter. Five call sites now spell out two internal function names purely
as padding.

## What should happen

`examples/IMPLEMENTATION.md`'s promotion rule (rule of three) says a gap
consumed by 3+ call sites is either promoted to public API or explicitly
dispositioned as app/testkit-layer by design. Reach-ins 3 and 4 are over that
line today (three and five call sites); 1 and 2 are at three and six *uses*
across two files each. So each of the four needs one of:

- a public seam — e.g. a settleable `Promise<T>` companion to `Completion<T>`;
  a public strand/`ModelId` vocabulary; a `BridgeHandler::registered()`
  predicate or `onRegistered` callback; a `QtWebSocketBackend` constructor
  overload (or designated-initializer options struct) that takes `Config`
  without the dispatcher/registry pair — or
- an explicit, recorded "testkit-layer by design; these types are internal and
  the testkit accepts breaking with them" disposition, so a future
  `detail::`-namespace refactor knows it may break the ladder and that this is
  accepted rather than accidental.

## What happens instead

Nothing announces the coupling. A refactor inside any of these four
namespaces compiles morph and its own test suite green and breaks
`ladder_common_tests` — a target the `ladder-tests` CI job only builds when
its path filter matches. The cost is small today (rung 0 is the only
consumer) and grows with every rung that copies these call patterns, which is
the argument for dispositioning it now rather than at rung 4.

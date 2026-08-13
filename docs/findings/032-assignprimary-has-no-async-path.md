---
id: 032
title: a result-keyed creating action's promote step (assignPrimary) has no async path, so it still blocks a WASM main thread
subsystem: core-backend
severity: major
source: rung 3 (polls) framework prerequisite — async shared/keyed attach, task 2 review
disposition: open
test: spec-cited
issue: https://github.com/LASTRADA-Software/morph/issues/67
---

Found while closing `examples/LADDER.md`'s "Framework prerequisites" #1
(async shared/keyed attach) ahead of rung 3 (`polls`). That work added
`IBackend::registerModelSharedAsync`/`attachModelAsync` and wired
`Bridge::attachHandlerAsync`/`ensureBoundAsync` to prefer them, which makes
a **payload-keyed** action's attach step (e.g. `OpenPoll{pollId}`) genuinely
non-blocking on `QtWebSocketBackend` when `asyncRegistrationEnabled` is set.
It does not close the equivalent problem for a **result-keyed** action.

## The actual gap

`BridgeHandler<Model, AllowShared>::execute()`'s result-keyed path
(`::morph::model::detail::ResultKeyed<Action>`, e.g. a `CreatePoll`-shaped
action whose result carries the new instance's key) has two steps:

1. **Bind** — `Bridge::ensureBoundAsync` gives the handler an anonymous
   instance to run on. This step is now async (this task's own work).
2. **Promote** — once the action's result names the generated key,
   `Bridge::assignHandlerPrimary` (`include/morph/core/bridge.hpp`) calls
   `IBackend::assignPrimary` to file the instance into the shared directory
   under that key. `QtWebSocketBackend::assignPrimary`
   (`src/qt/qt_websocket_backend.cpp:296`) is `sendSync` — a nested
   `QEventLoop` — exactly the blocking shape `registerModelAsync` and this
   task's own additions exist to avoid. `grep -rn assignPrimaryAsync` across
   `include/`, `src/`, `tests/`, `docs/`, `examples/` finds zero matches:
   no such method exists anywhere in the tree.

So a WASM client dispatching a result-keyed *creating* action — the
`CreatePoll`-shaped case rung 3's own README names as its very first
action — reaches the promote step and aborts the page there, even after
this task's fix. The framework prerequisite LADDER.md names is therefore
only half-closed: the **attach** path (participants joining an existing
shared instance via a payload-keyed action) is fully fixed; the
**create-and-become-shared** path (an organizer minting a new shared
instance via a result-keyed action) is not.

## Impact

Any rung whose WASM client both creates *and* attaches to shared instances
hits this the moment it tries to create one from WASM. Rung 3's own
disclosed workaround (see `examples/polls/README.md`'s design decisions):
`CreatePoll` runs from the native/desktop client only, never from a WASM
tab; WASM tabs are strictly the participant-attach story (`OpenPoll`,
payload-keyed, already safe). This is a real, workable scoping — Rallly's
own anchor UX matches it (an organizer creates via the main site, shares a
link, participants open it in whatever browser tab they have) — but it is
a constraint imposed by this gap, not a free design choice, and any future
rung that wants a WASM client to be able to *create* a shared instance will
hit this immediately without a workaround this clean available.

## What morph would need

An `IBackend::assignPrimaryAsync` opt-in virtual, mirroring
`registerModelSharedAsync`/`attachModelAsync`'s exact shape (default
returns `false` and invokes neither callback; a backend that opts in
returns `true` and later invokes exactly one of `onRegistered`/`onError`),
with a real `QtWebSocketBackend` implementation reusing the same
`_pendingRegistrations`-based reply routing this task's two new methods
already established (the wire reply shape for `assign` already carries a
`modelId` the same way `register`/`registerShared`/`attach` do — confirmed
via `include/morph/core/remote.hpp`'s `acquireSharedInstance`-based reply
construction, shared across all four verbs). `Bridge::assignHandlerPrimary`
would need the same "prefer async, fall back to sync" restructuring
`attachHandlerAsync`/`ensureBoundAsync` already went through — including
this task's own inline-completion handoff discipline
(`AsyncDispatchHandoff`, `include/morph/core/bridge.hpp`), which a
straightforward copy of the pattern would need to reuse or re-derive
rather than skip. Scoped to `include/morph/core/backend.hpp`,
`include/morph/core/bridge.hpp`, `include/morph/qt/qt_websocket_backend.{hpp,cpp}`
— the same files this task touched. Out of scope for the task that found
it (closing exactly the attach half of the prerequisite, not the promote
half); tracked here as a follow-up, not fixed.

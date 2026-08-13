---
id: 034
title: BridgeHandler::executeJson silently skips the payload-keyed attach step on an AllowShared handler
subsystem: core/bridge
severity: major
source: rung 3 (polls) task 16 — GUI shell, discovered while wiring PollFormsController
disposition: open
test: none (worked around at the call site; see examples/polls/gui_lib/poll_forms_controller.hpp)
issue: https://github.com/LASTRADA-Software/morph/issues/68
---

Found while building `polls::gui::PollFormsController` — this rung's
`AllowShared`, keyed model (`PollModel`) needed its one payload-keyed action
(`OpenPoll`) dispatched generically, exactly the way `submitIfValid`/
`executeJson` dispatch every other schema-driven action. It does not do what
it looks like it does.

## The actual bug

`ActionExecuteRegistry::registerAction<Model, Action>` — the template that
`BRIDGE_REGISTER_ACTION` instantiates once per `(Model, Action)` pair, and
that `BridgeHandler<Model, Sharing>::executeJson` looks up by string id at
call time — stores an executor closure that reads (`include/morph/core/bridge.hpp`,
around line 1777):

```cpp
_executors[key] = [](void* handlerVoid, std::string_view bodyJson) -> ... {
    auto* handler = static_cast<BridgeHandler<Model>*>(handlerVoid);
    ...
    handler->template execute<Action>(std::move(action))
        .then(...)
        .onError(...);
    ...
};
```

`BridgeHandler<Model>` here means `BridgeHandler<Model, NoSharing>` — the
default template argument. This is **not parameterized by the real handler's
`Sharing` argument at all**: `registerAction<Model, Action>` is instantiated
exactly once, from `BRIDGE_REGISTER_ACTION(Model, Action, "...")`'s own
expansion, with no `Sharing` template parameter anywhere in that macro or in
`ActionExecuteRegistry::registerAction`'s own signature. Every `executeJson`
call for that `(Model, Action)` pair — no matter which concrete
`BridgeHandler<Model, Sharing>` instance actually issued it — reinterprets
its `this` pointer as `BridgeHandler<Model, NoSharing>*` and calls the
`NoSharing`-instantiated `execute<Action>()`.

For most actions this is harmless: `BridgeHandler::execute`'s `if constexpr`
chain only diverges by `Sharing` for `PayloadKeyed`/`ResultKeyed` actions
(`kShared && PayloadKeyed<Action>` / `kShared && ResultKeyed<Action>`); every
other action falls to the same final `else` branch
(`_bridge.executeVia<Model,Action>(_binding, ...)`) regardless of `kShared`,
and `_binding` is a real member accessed at its real memory offset (the two
template instantiations have identical layout), so the call behaves exactly
as if the real handler's own `execute<Action>()` had run.

For a **payload-keyed** action dispatched on a real `AllowShared` handler,
it does not. `kShared` resolves to `false` at compile time inside the
`NoSharing`-instantiated `execute<Action>()`, so
`if constexpr (kShared && PayloadKeyed<Action>)` is `false` unconditionally —
the attach-then-dispatch branch never runs, and the call falls straight to
`_bridge.executeVia<Model,Action>(_binding, ...)` using whatever `currentId`
the binding already happens to have. On a handler that has never attached,
that is `0`, and the call fails fast with `"handler not bound"` — silently,
with no indication that the *reason* is a mismatched `executeJson` dispatch
path rather than a genuine "you forgot to attach" caller error.

## Impact on rung 3

`polls::PollModel` is this rung's one `AllowShared`, keyed model
(`BRIDGE_MODEL_KEY(PollModel, OpenPoll, &OpenPoll::pollId)`). Routing
`OpenPoll` through a generic schema-driven `submitIfValid("OpenPoll", ...)`
path — the obvious, `bookmarks::gui::BookmarkFormsController`-mirroring
choice — hits this exactly: the handler never attaches, and every
subsequent action on the same (nominally open) handler also fails "handler
not bound." `polls::gui::PollFormsController::openPoll(std::string pollId)`
works around it by calling the templated `_handler.execute(OpenPoll{...})`
directly (never `executeJson`), which resolves the real `AllowShared`
template instantiation and its real `PayloadKeyed` branch. `OpenPoll` is
excluded from `poll_schemas.hpp`'s document and from `PollFormsController`'s
`submitIfValid` allow-list for exactly this reason — see that class's own
doc comment.

Every future rung with a schema-driven form for a payload- or result-keyed
action on an `AllowShared` model will hit this the moment it tries to
dispatch that one action through the generic path.

## What morph would need

`ActionExecuteRegistry::registerAction` (or the macro that instantiates it)
would need to become `Sharing`-aware — either registering one executor per
`(Model, Action, Sharing)` combination actually used, or (simpler) having
`executeJson` itself dispatch through the *caller's own* `Sharing`-correct
`execute<Action>()` rather than through a type-erased closure that
re-derives the handler type from scratch. The entry point for a fix is
`include/morph/core/bridge.hpp`'s `ActionExecuteRegistry::registerAction`
(around line 1771) and its one call site inside
`BridgeHandler<Model, Sharing>::executeJson` (around line 1709). Scoped to
`include/morph/core/bridge.hpp`; out of scope for the ladder task that found
it (rung 3 GUI shell, not the framework itself).

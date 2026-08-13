---
id: 023
title: Completion<T>::onError() keeps only the last-attached handler, silently discarding an earlier one
subsystem: core
severity: minor
source: rung 1 (pastebin) task 10 — PastePresenter/forms-controller glue
disposition: open
test: spec-cited
issue: https://github.com/LASTRADA-Software/morph/issues/59
---

`morph::async::detail::CompletionState<T>::attachOnError`
(`include/morph/core/completion.hpp:90-108`) stores the error handler in a
single field:

```cpp
void attachOnError(std::function<void(std::exception_ptr)> handler) {
    ...
    if (ready && error) {
        ...
    } else if (!ready) {
        onErr = std::move(handler);
    }
    ...
}
```

Calling `.onError(...)` a second time on the same (still-pending)
`Completion<T>` — even via a separate `Completion&` returned from the first
call, since `.then()`/`.onError()` both return `*this` — replaces `onErr`
outright. The first handler never runs, is never diagnosed as replaced, and
(because `onErrAttached` is set `true` by the second `attachOnError` call)
the orphan-error logger in `~CompletionState()` stays silent too — the
failure is not merely mis-routed, it becomes unobservable.

## What should happen

`examples/common/gui/presenter.hpp`'s `Presenter::track<T>()` — every ladder
rung's shared busy-counter wrapper — documented (before this task) a
composition pattern built on this exact double-attach: "a subclass wanting
to *display* the error must attach its own `.onError` before handing the
completion to `track()`, since `track()` is the last handler attached." That
description assumed `.onError()` composes (both handlers fire, in some
order) the way `QObject::connect()` or a typical observer-list API would.

## What happens instead

Verified empirically (throwaway harness, not checked in): attaching
`.onError(displayHandler)` and then, on the same `Completion<T>`,
`.onError(finishHandler)` — exactly `Presenter::track()`'s pre-existing
shape plus a subclass's pre-attached display handler — leaves only
`finishHandler` observable. `displayHandler` never runs. Applied to
`PastePresenter` as originally sketched (task 10's brief), this would have
meant `PastePresenter::failed(QString)` never fired for any real error: the
busy counter would still clear correctly (the surviving handler is
`track()`'s own), so the bug is invisible to `busy()`/`idle()` assertions
and would only show up as "errors are silently swallowed" from the UI's
perspective — precisely the failure mode task 10's own self-review
instructions called out to check for.

## What shipped instead

`Presenter::track<T>()` (`examples/common/gui/presenter.hpp`) gained a third,
optional parameter:

```cpp
template <typename T>
void track(::morph::async::Completion<T> completion, std::function<void(T)> onOk,
           std::function<void(const std::exception_ptr&)> onErr = {});
```

`onErr`, if supplied, is invoked from *inside* the one `.onError()` handler
`track()` itself installs, immediately before `finishOne()` — so display and
busy-counter decrement are folded into a single attach, never a second
competing one. `PastePresenter` (`examples/pastebin/gui_lib/paste_presenter.cpp`)
passes its `reportError` member as this third argument instead of
pre-attaching `.onError()` on the completion. Existing two-argument
`track()` call sites (`examples/common/testkit/test_presenter.cpp`) are
unaffected — the new parameter defaults to a no-op, matching the prior
behavior exactly. Regression-verified: `ladder_common_tests` (146
assertions) and `ladder_pastebin_tests` (506 assertions) both still pass
after the change.

## What morph would need

Nothing strictly — this is a documented single-slot design, not a bug in
`Completion<T>` itself; the bug was in a downstream doc comment's assumption
about it composing. But `Completion<T>::onError()`'s doc comment
(`include/morph/core/completion.hpp:191-198`) does not mention that a second
call replaces rather than composes with the first, and nothing in its
`Completion&` return-for-chaining API signals that chaining two `.onError()`
calls is a foot-gun rather than a supported pattern. A doc-comment addendum
("only the most recently attached handler runs; attaching twice silently
discards the first") would have caught this at review time instead of
requiring an empirical repro.

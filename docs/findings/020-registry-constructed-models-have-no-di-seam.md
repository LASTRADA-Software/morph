---
id: 020
title: Registry-constructed models have no per-instance dependency-injection seam
subsystem: core
severity: major
source: rung 1 (pastebin) journal-split design investigation
disposition: open
test: spec-cited
issue: https://github.com/LASTRADA-Software/morph/issues/56
---

Generalizes finding [003](003-datetime-now-not-injectable.md) (which is the
clock-shaped instance of this same gap) to the root cause: a model
constructed by the server-side registry (`include/morph/core/registry.hpp`,
the path every `Socket`-mode/remote registration goes through) is always
**default-constructed** — there is no parameter, no factory hook, and no
post-construction injection point a caller can use to hand it anything
instance-specific beyond what `IModelHolder::attachActionLog` already
covers (a log sink + a context key, set from the server's `LogProvider`).

**What does exist, and why it doesn't close the gap:** `Bridge::modelFactory`
(`include/morph/core/bridge.hpp:140`, used by `registerHandler(binding)`,
`bridge.hpp:236-242`) lets a *client-side, `Local`-mode* registration supply
a custom factory closure that captures arbitrary dependencies. This is a
real, working seam — but it only ever runs for the local, in-process
backend. A `Socket`-mode (or any real remote) registration is served by
`RemoteServer`'s registry, which knows only the model's default
constructor. Any dependency a model needs — an injectable clock (finding
003), a second `IActionLog` reference so a model could author a synthetic
journal entry distinct from the one action it was actually dispatched with
(see below), a feature flag, anything — is therefore injectable in `Local`
mode and not injectable in `Socket` mode, silently, unless the app avoids
needing per-instance injection at all.

**Concrete instance that surfaced this (rung 1 / pastebin):** the
recommended design for `GetPaste` was to split it into an unlogged read
plus an internally-journaled `RecordRead` mutation, so replaying the
journal never re-triggers a burn-after-read deletion. `RecordRead` would
need to be authored *from inside* `GetPaste`'s own `execute()` — a second,
independent `LogEntry` distinct from the auto-recorded entry for `GetPaste`
itself. `IModelHolder::recordIfAttached`
(`include/morph/core/model.hpp:145`) is called only by the two built-in
dispatch runners (`ActionDispatcher`'s registered-action runner and
`Bridge::executeVia`'s local op — see that function's own doc comment,
"model code and application code never call this directly"), for the one
action actually dispatched; it exposes no way to author a second entry.
The only way to get a model a reference it could call `->append(...)` on
directly is `Bridge::modelFactory` constructor injection — which, per
above, doesn't reach `Socket` mode. Rung 1's resolution: `GetPaste` stays
the one journaled action (default `Loggable::Yes`); the resurrection risk
this creates for replay/undo is documented as the concrete example in the
ladder-wide journal-honesty position (`examples/LADDER.md` § Journal
honesty; `examples/pastebin/README.md`'s journal design-question).

**What happens instead:** any future rung wanting per-instance model
dependencies beyond a clock hits this same wall and either (a) restricts
itself to `Local`-mode-only behavior (silently, unless it remembers to
test `Socket` mode and gets a construction-time surprise), or (b) works
around it as rung 1 did — accept the action-granularity the framework
already gives instead of the finer one the app wanted.

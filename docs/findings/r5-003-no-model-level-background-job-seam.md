---
id: r5-003
title: No framework seam for a model's own execute() to post background work and later update its own state
subsystem: core
severity: minor
source: ledger rung 5, design spec §9
disposition: open
test: spec-cited
issue: https://github.com/LASTRADA-Software/morph/issues/129
---

`morph::exec::IExecutor`/`ThreadPoolExecutor` (include/morph/core/
executor.hpp) has no usage anywhere inside a model's own `execute()` in
this codebase. Every "background job" in the ladder (bookmarks'
metadata-fetch worker, pastebin's expiry sweep, ledger's report runner)
lives at the App layer, re-entering its model as a fresh, ordinary,
fully-authorized client dispatch -- not something a bare model with no
App/Bridge around it can do.

Ledger rung 5's report job (`SubmitReport`/`GetReportStatus`) needed this
and found no existing seam, so `LedgerModel` grew its own
`std::shared_ptr<morph::exec::IExecutor>` member as a local workaround.

**Update (morph#160).** That workaround is gone: rung 5 was given the App
layer it was missing (`examples/ledger/include/ledger/app/app.hpp`), and
the aggregation is now an ordinary `RunReportJob` action the runner
dispatches, so `LedgerModel` owns no executor and includes no morph
executor header. The finding's *premise* -- that there is no framework
primitive for a model to post its own background work -- is unchanged and
still open; what changed is that rung 5 no longer needs one, having taken
the same App-layer route every other rung already takes. So the remaining
question this finding poses is narrower than it was: is a
"background task from inside a model" primitive worth having *at all*,
given that the App-layer route exists, is testable without a deferred
executor double, and additionally makes a job survive the process that
accepted it? Whoever triages this should answer that rather than the
original "ledger needs this" framing.

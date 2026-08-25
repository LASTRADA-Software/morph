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
this codebase. Every existing "background job" (bookmarks' metadata-
fetch worker, examples/bookmarks/src/app/app.cpp) lives at the App/
Bridge/RemoteServer layer, re-entering the model as a fresh, ordinary,
fully-authorized client dispatch through a service-principal token --
not something a bare model with no App/Bridge/RemoteServer around it
can do. Ledger rung 5's report job (`SubmitReport`/`GetReportStatus`)
needed this and found no existing seam, so `LedgerModel` grew its own
`std::shared_ptr<morph::exec::IExecutor>` member as a local workaround.
A framework-level "background task from inside a model" primitive
(with a defined service-principal/session-propagation story for the
worker thread) would let future rungs avoid re-inventing this
per-model, and would let a future report job be tested with a
deferred/deterministic executor double instead of always spinning a
real thread pool.

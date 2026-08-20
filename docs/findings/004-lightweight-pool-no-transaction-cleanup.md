---
id: 004
title: Lightweight's DataMapperPool::Return performs no transaction cleanup -- a connection returned mid-transaction is silently inherited by the next caller
subsystem: backend
severity: minor
source: ledger rung 5, Task 16 review
disposition: open
test: examples/ledger/src/models/ledger_model.cpp (WalSnapshotGuard's own doc comment)
issue: https://github.com/LASTRADA-Software/Lightweight/issues/583
---

This finding is in the vendored `Lightweight` dependency, not morph itself -- filed against `LASTRADA-Software/Lightweight` (issue linked above), recorded here because it was discovered from inside a morph rung and shapes how that rung's code had to be written.

`Lightweight::DataMapperPool::Return` (and `~PooledDataMapper`) performs no transaction cleanup on a connection being returned to the pool: no `SQLEndTran`, no autocommit reset, no cursor close. A connection returned while a SQL transaction is still open (e.g. a raw `BEGIN DEFERRED`/`COMMIT` pair around a WAL-style read snapshot, per `IMPLEMENTATION.md` rule 4's escape tier) is silently handed to whichever unrelated caller acquires that connection next, which then blocks on its first write for the driver's own `busy_timeout` (60000ms in this codebase) before surfacing `SQLITE_BUSY`.

Discovered and fixed at the application layer in `ledger::LedgerModel::execute(SubmitReport)`'s background report-job worker: two real paths could leave a connection returned mid-transaction (the raw `BEGIN DEFERRED` itself throwing before any cleanup ran, or a recovery `COMMIT` on an exception-unwind path itself throwing and replacing the in-flight exception). Fixed with an RAII guard (`WalSnapshotGuard`) whose destructor always issues exactly one `COMMIT`, swallowing any failure, on every exit path including exception unwinding -- but this is a per-caller workaround for a gap in the pool's own connection-lifecycle contract, not a framework-level fix.

---
id: 018
title: DbFaultFixture cannot fault an ordinary DataMapper call, so the 100%-coverage store-error promise is unsatisfiable
subsystem: offline
severity: major
source: rung 0 final review (whole-branch)
disposition: documented-limitation
test: examples/common/testkit/test_db_busy_fixture.cpp
---

`subsystem: offline` is the nearest value `examples/FINDINGS.md`'s enum
offers — this is a persistence-layer gap, and `offline` is morph's own
durable-store subsystem. Nothing in `src/offline/` is implicated; the gap is
in the rung-0 testkit and in two governing documents' promises about it.

## The promise

`examples/IMPLEMENTATION.md` rule 5 ("Testing: models are 100% unit tested"):

> **The store-error half is covered honestly, not excluded** (round-7 T3):
> branches reachable only through database failure (`SQLITE_BUSY`, constraint
> violations, `SqlTransaction` rollback) are exercised via the testkit's
> **`db_fault_fixture`** (a failing ODBC-level driver, part of the rung-0
> testkit — see `TESTING.md`); only a branch that fixture provably cannot
> reach may carry a reviewed per-line exclusion tag with a comment naming
> why.

`examples/TESTING.md`, "Multi-client stress harness", makes the same promise:

> `db_fault_fixture.hpp` — a failing ODBC-level driver for exercising
> store-error branches (`SQLITE_BUSY`, constraint violations, rollback) that
> the 100%-coverage rule requires (see `IMPLEMENTATION.md` rule 5);
> wire-level faults are the proxy's job, database faults are this fixture's.

Both name the fixture as *the* mechanism, and rule 5's escape hatch (a
per-line exclusion tag) is explicitly gated on the fixture "provably" not
reaching the branch — i.e. the fixture is the thing that decides whether an
exclusion is legitimate.

## What actually shipped

`examples/common/testkit/db_fault_fixture.hpp` is not a failing ODBC driver.
It wraps a `DbFixture` and holds a real `Lightweight::SqlScopedLock` on a
second, independent `SqlConnection` to the same shared database:

```cpp
explicit DbFaultFixture(std::string lockName = "morph_ladder_db_fault_fixture")
    : _fixture{}, _lockingConnection{}, _lock{_lockingConnection, lockName, std::chrono::milliseconds{50}} {}
```

That produces genuine, non-simulated cross-session contention — but only for
code that itself calls `SqlScopedLock` with the *same lock name* on a
different connection. An advisory lock is advisory: it is a row in
Lightweight's own lock table plus a wait/timeout protocol between
participants who opt in. It does not sit in the path of `SqlStatement`
execution.

So an ordinary model store call — `DataMapper::Create`, `Update`, `Query`,
`Delete`, or a `SqlTransaction` commit — is entirely unaffected while this
fixture holds its lock. It succeeds normally. There is no `SQLITE_BUSY`, no
constraint violation, no rollback. The three failure classes both documents
name are exactly the three the fixture cannot produce against the calls a
model actually makes.

## Why there is no cheap fix

The same reason the fixture became `SqlScopedLock`-based in the first place:
Lightweight exposes no injectable seam between `DataMapper` and the ODBC
driver. There is no `SqlConnection` interface to substitute, no statement
hook to fail, and no supported way to swap in a driver that returns
`SQLITE_BUSY` on the *n*-th execute. Hand-rolling a mock driver was rejected
during rung 0 for that reason — a mock that isn't in the real call path
proves nothing about the real call path. The options that remain all cost
real design work:

- Have models take their locks through `SqlScopedLock` deliberately, so the
  fixture's contention is on a path they genuinely use (narrow: only covers
  lock-contention branches, not constraint violations or rollback).
- Drive real failures through the schema instead of the driver: hold a
  conflicting row so a `UNIQUE`/FK insert genuinely violates, `DROP` a table
  mid-test so a query genuinely errors, open a competing write transaction on
  a second connection so SQLite genuinely returns `SQLITE_BUSY`. This reaches
  all three classes with no framework change, but it is a different fixture
  from the one that shipped.
- Add a fault seam upstream in Lightweight (or wrap it), which is a
  third-party change.

## Disposition

Deferred, deliberately. Rung 0 ships no model of its own, so nothing in this
branch is blocked: the 100%-coverage gate binds a rung with model code, and
the first of those is rung 1 (pastebin). Whichever rung first needs
store-error branch coverage owns resolving this — either by extending
`db_fault_fixture` (most likely along the "real failures through the schema"
line above) or by rewriting the two passages quoted at the top so they
promise what the fixture can actually deliver. It must not be resolved by
quietly widening rule 5's per-line exclusion tags: that is the exact
exclusion-by-default outcome round-7 T3 rejected.

`examples/TESTING.md`'s `db_fault_fixture.hpp` bullet carries a pointer to
this finding so the next implementer meets it before writing the coverage
plan, not after.

## Closed as `documented-limitation` — what rung 1 shipped

Rung 1 (pastebin), this finding's designated owner, took the second option
above — "real failures through the schema" — and it is on disk:

- **`examples/common/testkit/db_busy_fixture.hpp`** — `DbBusyFixture` holds a
  genuine, uncommitted `BEGIN IMMEDIATE` write transaction open on a second
  `SqlConnection` to the shared test database for its lifetime, so a
  concurrent write from the connection under test collides for real and
  SQLite returns a real `SQLITE_BUSY`. No mock driver, no simulated ODBC
  layer: the failure happens in the same call path production takes. Its own
  doc comment records the two empirically-verified gotchas — `BEGIN
  IMMEDIATE` (not a plain `Lightweight::SqlTransaction`, which only flips
  `SQL_ATTR_AUTOCOMMIT` and defers lock acquisition), and Lightweight's
  unconditional `PRAGMA busy_timeout = 60000` in `PostConnect()`, which the
  *other* connection must re-issue with a small value or the "failure" is a
  sixty-second block instead.
- **`examples/common/testkit/test_db_busy_fixture.cpp`** — the fixture's own
  suite, which is what this finding's `test:` field now names.
- **`examples/pastebin/tests/test_paste_model.cpp`** — the two store-error
  cases that consume it: "GetPaste surfaces a real SQLITE_BUSY as a thrown
  error, not as silent data loss" (the raw conditional `UPDATE` path) and
  "CreatePaste surfaces a real SQLITE_BUSY rather than mistaking it for an id
  collision" (the `DataMapper::Create` path, proving the retry loop's
  unique-violation classifier does not swallow an outage). The
  zero-rows-affected branch of the conditional update is reached the third
  way this finding named — a row already at `read_count == burn_after_reads`
  — in "GetPaste against a row already at its burn budget throws Burned, not
  NotFound".

`documented-limitation`, not `fix-scheduled` or a plain close, because the
gap this finding actually described is only partly gone. The original
promise, quoted at the top from `examples/IMPLEMENTATION.md` rule 5 and
`examples/TESTING.md`, names **`db_fault_fixture`** — "a failing ODBC-level
driver" — as *the* mechanism for all three failure classes. That is still not
what exists. `db_fault_fixture.hpp` is unchanged and still cannot fault an
ordinary `DataMapper` call; what shipped is a *second, differently-shaped*
fixture beside it, covering the `SQLITE_BUSY` class (plus, incidentally, the
guarded-update zero-rows class through the schema rather than through a
fault). Constraint violations and mid-transaction rollback still have no
general fixture, and there is still no injectable seam between `DataMapper`
and the ODBC driver — the "why there is no cheap fix" section above stands
verbatim. So: the accepted behavior is that store-error branch coverage is
obtained per failure class, through the real schema, by whichever fixture can
genuinely provoke that class — not from one failing driver — and the two
governing documents' `db_fault_fixture` wording is the part that is now
inaccurate rather than the code. Crucially, the outcome round-7 T3 rejected
did **not** happen: no store-error branch was closed by widening rule 5's
per-line exclusion tags. Whoever next revises `IMPLEMENTATION.md` rule 5 and
`TESTING.md`'s fixture bullet should rewrite them to promise this shape.

---
id: 022
title: sqliteodbc reports a result set for UPDATE ... RETURNING but SQLFetch fails with SQLSTATE 24000, so the single-statement atomic-read design is unavailable
subsystem: offline
severity: minor
source: rung 1 (pastebin) task 5 — PasteModel burn-atomicity spike
disposition: open
test: spec-cited
issue: https://github.com/LASTRADA-Software/morph/issues/58
---

`subsystem: offline` is the nearest value `examples/FINDINGS.md`'s enum
offers — this is a persistence-layer (Lightweight/ODBC) finding, exactly as
[finding 018](018-db-fault-fixture-cannot-fault-datamapper.md) argued for
itself. Severity is `minor` because a fully equivalent, equally atomic
fallback exists and shipped; what is lost is one statement's worth of
concision, not a capability.

## What should happen

`examples/pastebin/README.md`'s resolved burn-atomicity decision names a
single conditional statement issued through Lightweight's raw-query facility:

```sql
UPDATE pastes
   SET read_count = read_count + 1
 WHERE id = ?
   AND (expires_at_ms IS NULL OR expires_at_ms > ?)
   AND (burn_after_reads IS NULL OR read_count < burn_after_reads)
RETURNING content, syntax, created_at_ms, expires_at_ms,
          burn_after_reads, read_count, is_private, is_editable
```

executed as `SqlStatement::Prepare` → `Execute(...)` → `FetchRow()` →
`GetColumn<T>(i)`. SQLite has supported `RETURNING` since 3.35 and this
environment runs 3.53.4, so the statement itself is valid; the question the
README left open (and this rung owns) was whether the *driver* surfaces its
result set. No existing Lightweight test or example anywhere in this
codebase uses `RETURNING`.

## What happens instead

The driver accepts and executes the statement — the update is applied, and
`SqlResultCursor::NumColumnsAffected()` correctly reports the `RETURNING`
column count — but the first `FetchRow()` throws:

```
24000 (0) - [unixODBC][Driver Manager]Invalid cursor state
```

Reproduced against `DRIVER=SQLite3;Database=<tmp>.db` (sqliteodbc via
unixODBC 2.3.14, SQLite 3.53.4, macOS/arm64), linking the vendored
Lightweight `v0.20260625.0`:

```cpp
Lightweight::SqlStatement stmt;
(void) stmt.ExecuteDirect("CREATE TABLE probe (id INTEGER PRIMARY KEY, n INTEGER NOT NULL)");
(void) stmt.ExecuteDirect("INSERT INTO probe (id, n) VALUES (1, 41)");

stmt.Prepare("UPDATE probe SET n = n + 1 WHERE id = ? RETURNING n");
auto cursor = stmt.Execute(1);
cursor.NumColumnsAffected();  // => 1          (the driver knows about the column)
cursor.NumRowsAffected();     // => 1          (the update did happen)
cursor.FetchRow();            // throws 24000 "Invalid cursor state"
```

Both entry points fail identically — `ExecuteDirect(...)` and
`Prepare(...)` + `Execute(...)` — so this is not a prepared-statement
binding problem. Controls run in the same process, on the same connection,
confirm the failure is specific to `RETURNING`:

- a plain `SELECT` prepared and executed the same way fetches normally;
- a plain conditional `UPDATE ... WHERE ...` reports
  `NumRowsAffected() == 1` when it matches and `== 0` when it does not, so
  the affected-row count *is* a trustworthy signal.

The driver appears to execute the statement through a non-cursor path and
never opens a result set over the returned rows, leaving the statement
handle in a state where `SQLFetch` is invalid.

## What shipped instead

`pastebin::PasteModel::execute(const GetPaste&)`
(`examples/pastebin/src/models/paste_model.cpp`) uses the fallback the plan
pre-specified: a `Lightweight::SqlTransaction` on the model's own connection
wrapping (1) the identical conditional `UPDATE` minus its `RETURNING`
clause, dispatched on `NumRowsAffected()`, and (2) an ordinary `DataMapper`
read-back of the row by primary key.

The atomicity argument is unchanged, because it never depended on
`RETURNING`: the entire guard (`id` matches, not expired, budget not yet
spent) lives inside the `UPDATE`'s own `WHERE`, which SQLite evaluates and
applies as one indivisible statement under a write lock. Of N clients racing
for the last allowed read of a burn-after-N paste, exactly one gets a
non-zero affected-row count. The transaction's job is only to keep the
read-back consistent with the write it is reading back, and to make the
burn-delete part of the same commit.

Verified empirically (throwaway harness, not checked in — Task 9 owns the
durable tests): 40 rounds × 6 concurrent threads, each with its own
`PasteModel` and therefore its own connection, all calling `GetPaste` on the
same `burnAfterReads = 1` paste at a `std::barrier`. Exactly one winner per
round, 240 total attempts, 200 losers all `NotFound`, zero driver errors —
with and without an explicit ODBC `Timeout=` busy timeout.

## What morph would need for the original design

Nothing in morph — this is a driver capability. Either a sqliteodbc build
that opens a cursor for `RETURNING` statements, or a different SQLite ODBC
driver. If a future rung wants the single-statement form back, re-run the
probe above before designing around it. Until then, the transaction-wrapped
two-statement form is the ladder's answer for "atomic conditional
read-modify-return", and any other rung reaching for `RETURNING` should
expect the same failure.

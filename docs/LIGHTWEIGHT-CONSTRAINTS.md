# Lightweight constraints an example author hits

The ladder's persistence layer is the [LASTRADA Lightweight](https://github.com/LASTRADA-Software/Lightweight)
ORM, pinned to one commit SHA by `examples/common/CMakeLists.txt` and, in
lockstep, `examples/bank/CMakeLists.txt`. Some of what that revision can and
cannot do is not obvious from its headers, and every rung that
rediscovered a limit wrote it down in **its own plan document** — so the next
rung rediscovered it again. This page is the single place those belong.

**Scope.** This is not morph's design (that lives in `docs/spec/`) and not a
plan (those live in `docs/superpowers/`). It is a record of *someone else's*
library's behaviour, kept here because example authors are the people who trip
over it.

**What every entry carries**, and what the plan documents did not:

- the constraint, stated as something you can act on;
- **the pinned revision it was verified at**, and how it was verified;
- the file and line in Lightweight that decides it;
- the workaround actually used in this tree;
- **what would retire it** — the condition under which the entry should be
  deleted.

**An entry is only as good as its last verification.** When the pin moves,
re-run the probes; two of the three entries below were folklore that the pin
had already retired, and nobody noticed because nothing re-checked them.

Pinned revision at the time of writing:
`bbb972a78e1962b968a2c6ad93f7dade736eaa01`.

---

## 1. `Where()` cannot bind a binary value — **live**

**Constraint.** The fluent query builder's literal `Where()` overload cannot
take a `SqlBinary` or `SqlDynamicBinary<N>`. A comparison against a binary
column has to be expressed some other way.

**Verified at** `bbb972a78e1962b968a2c6ad93f7dade736eaa01`, by compiling it
(`g++ 16.2.1`, `-std=c++23 -fsyntax-only`, non-reflection build):

```cpp
Lightweight::SqlDynamicBinary<32> blob;
mapper.Query<Child>().Where(Lightweight::FieldNameOf<&Child::label>, "=", blob).All();
```

```
/usr/include/c++/16/bits/alloc_traits.h:716:28: error: no matching function for call to
  'construct_at(Lightweight::SqlVariant*&, const Lightweight::SqlDynamicBinary<32>&)'
```

The identical call with `std::string{"x"}` in place of `blob` compiles, so the
refusal is about the value type and not about the call shape.

**Why.** `Where()`'s literal path stores the bound value in a
`std::vector<SqlVariant>` (`src/Lightweight/SqlQuery/Core.hpp:137`), and
`SqlVariant::InnerType` (`src/Lightweight/DataBinder/SqlVariant.hpp:49`) has
**zero** binary alternatives — the variant runs `SqlNullType`, `SqlGuid`,
`bool`, the integer and floating types, the string types, `SqlText`, `SqlDate`,
`SqlTime`, `SqlDateTime`, and stops. `SqlDataBinder<SqlDynamicBinary<N>>` exists
and works everywhere else; `Where()` simply does not go through it.

**Workaround used in this tree.** Store the value in a text column as
lower-case hex and compare against the hex string.
`examples/bank`'s offline queue does exactly that for
`OfflineQueueRecord::idempotencyKeyHex`, because enqueue-time dedup needs a
`WHERE` on that column. Note what it does *not* do: the adjacent `payload`
column stays a real `SqlDynamicBinary`, because nothing ever puts it in a
`Where()`. The constraint is on the *comparison*, not on binary storage, so hex
only where a `WHERE` reaches. Hex is NUL-free by construction, so the text
column carries an arbitrary byte string without truncating it or colliding two
keys that share a NUL prefix, and the conformance round-trip
(`tests/offline_queue_conformance.hpp`'s `checkNulPayloadRoundTrip`) passes
unedited.

**What retires this.** A Lightweight release that either adds a binary
alternative to `SqlVariant::InnerType` or routes `Where()`'s literal path
through `SqlDataBinder<>`. Tracked upstream as
[LASTRADA-Software/Lightweight#618](https://github.com/LASTRADA-Software/Lightweight/issues/618).
When it lands, the hex column can become a plain `SqlDynamicBinary<N>` again.

---

## 2. `Query<Record>()` has no `Update()` — **live, but not the constraint it was recorded as**

**Constraint.** The record-typed fluent builder
(`mapper.Query<Record>().Where(...)`) offers `All()`, `First()`, `Count()`,
`Delete()` and friends, but **no `Update()`**. To update, use
`DataMapper::Update(record)` on a fetched record, or drop to the untyped
builder on the connection.

**Verified at** the pin, by compiling it:

```cpp
mapper.Query<Parent>().Where(Lightweight::FieldNameOf<&Parent::id>, "=", 1).Update();
```

```
error: 'class Lightweight::SqlAllFieldsQueryBuilder<Parent, Lightweight::DataMapperOptions{true},
  Lightweight::SqlQueryExecutionMode::Synchronous>' has no member named 'Update'
```

**This corrects what the plan archive says.** Three plan documents record this
as *"fluent `Query`/`Update` refuse `HasMany`-bearing records in the
non-reflection build"* (`docs/superpowers/plans/2026-08-16-kanban-backend.md`,
`docs/superpowers/specs/2026-08-16-kanban-rung4-design.md`,
`docs/superpowers/plans/2026-08-19-ledger-rung5.md`). At the pinned revision
that is **false in both halves**, and the control probes say so:

| probe | at the pin |
| --- | --- |
| `DataMapper::Update(parent)` where `Parent` has a `HasMany` member | **compiles** |
| `mapper.Query<Parent>().Where(...).All()` where `Parent` has a `HasMany` member | **compiles** |
| `mapper.Query<Parent>().Where(...).Update()` (`Parent` has `HasMany`) | fails |
| `mapper.Query<Child>().Where(...).Update()` (`Child` has **no** `HasMany`) | **fails identically** |

The last row is the one that matters: the refusal has nothing to do with
`HasMany`. `DataMapper::Update` skips relations explicitly — *"Relations
(HasMany, HasManyThrough, HasOneThrough, ...) have no column of their own"*,
`src/Lightweight/DataMapper/DataMapper.hpp:2047` and three sibling sites — so a
`HasMany` member costs an update nothing.

**Workaround.** `DataMapper::Update(record)`, which is what every ladder rung
already does.

**What retires this.** A Lightweight release that adds `Update()` to
`SqlAllFieldsQueryBuilder`. Re-run the probe when the pin moves.

---

## 3. `HasMany` resolves its foreign key by ordinal member index — **retired; do not believe this**

**This constraint is no longer true**, and it is recorded here only because
three plan documents still assert it and a fourth rung would otherwise design
around a limit that no longer exists.

**What the plans say.** That `HasMany` locates the child's foreign key by
matching **member position**, so reordering a record's members silently
repoints the relation.

**What the pinned revision does.** Matches by relationship **type**.
`src/Lightweight/DataMapper/Record.hpp:242-255` says so in its own words —
*"This is how `HasMany`, `HasManyThrough` and `HasOneThrough` locate their
foreign key column: by matching the relationship type, never by member
position"* — and `detail::InverseBelongsToResolver` (`Record.hpp:213-238`)
carries three `static_assert`s that turn every failure mode into a named
compile error: no `BelongsTo` at all, a `BelongsTo` whose column name does not
match, and an ambiguous pair of them.

**Verified at** the pin, with the child's `BelongsTo` declared **last**, which
positional resolution would get wrong:

```cpp
static_assert(Lightweight::InverseBelongsToIndexOf<Parent, Child> == 2);
static_assert(Lightweight::InverseBelongsToFieldNameOf<Parent, Child> == std::string_view{"parent"});
```

Both hold.

**What would re-open this.** A `HasMany` relation resolving to the wrong column
at a pin where the `static_assert`s above are absent. If that happens, this
entry becomes live again and the pin's `Record.hpp` is the first thing to read.

---

## 4. The migration DSL cannot express a partial index — **live**

**Constraint.** `SqlMigrationQueryBuilder`'s index API takes a name, a table
and a column list, and nothing else. There is no predicate, so
`CREATE UNIQUE INDEX … WHERE <expr>` — a partial index — has no spelling in the
DSL.

**Verified at** the pin, by reading the API surface. Every overload, in
`src/Lightweight/SqlQuery/Migrate.hpp:553-582`:

```cpp
CreateIndex(std::string indexName, std::string tableName, std::vector<std::string> columns, bool unique = false);
CreateUniqueIndex(std::string indexName, std::string tableName, std::vector<std::string> columns);
CreateIndex(std::string tableName, std::vector<std::string> columns, IndexType type = IndexType::NonUnique);
```

Also `SqlCreateTableQueryBuilder::Unique()` / `::UniqueIndex()`
(`Migrate.hpp:74`, `:80`) and `SqlAlterTableQueryBuilder::AddUniqueIndex(column)`
(`:207`) — none takes a predicate. `MigrationPlan.cpp` renders a trailing
`WHERE` only for UPDATE and DELETE plans (`:56-71`), never for an index.

**Why it bites.** Dedup on an optional key wants exactly a partial index:
`UNIQUE(key) WHERE key <> ''`. An *unconditional* unique index is not a
substitute — it makes the second empty-key row fail, which
`IOfflineQueue::enqueue` forbids.

**Workaround used in this tree.** `examples/bank`'s offline queue does the
check as a `SELECT` under its own mutex instead, and says so in
`LightweightOfflineQueue`'s class comment: as strong as `FileOfflineQueue`'s
linear scan (single process), deliberately weaker than
`SqliteOfflineQueue`'s real index (any writer). The other option, not taken
there, is `SqlMigrationQueryBuilder::Native(callback)` (`Migrate.hpp:594`),
which hands the plan a raw SQL string — an escape hatch that gives up the DSL's
cross-dialect rendering for that one statement.

**What retires this.** A `CreateIndex` overload taking a predicate expression,
or an equivalent in the plan renderer.

---

## 5. SQLite connections are rollback-journal, with a 60 s busy timeout — **live**

**Constraint.** Lightweight sets **one** SQLite pragma of its own, and it is
not the one people assume. Every connection it opens to a SQLite data source
gets `busy_timeout = 60000`; `journal_mode` it deliberately leaves alone, so
the database stays in SQLite's default **rollback-journal** mode unless
something in this tree sets it.

Two consequences an example author gets wrong:

- **`BEGIN DEFERRED` is not a snapshot here.** Under `journal_mode=WAL` a
  deferred read transaction lets writers continue; under a rollback journal it
  escalates to a `SHARED` lock on first read and blocks every writer on the
  file until it is released. A long read transaction is therefore a *write
  barrier*, not a free consistent view.
- **The busy timeout is 60 s, not whatever the connection string says.**
  `Timeout=5000` in an ODBC connection string is the ODBC login/query timeout;
  it does not reach SQLite's `busy_timeout`, which `PostConnect()` has already
  set to 60000 ms. A contended writer that is going to fail stalls for a
  minute first.

**Verified at** `bbb972a78e1962b968a2c6ad93f7dade736eaa01`, by reading
`src/Lightweight/SqlConnection.cpp:388-398` — the whole of what `PostConnect()`
does for SQLite:

```cpp
if (m_serverType == SqlServerType::SQLITE)
{
    // Set a busy timeout to prevent "database is locked" errors during concurrent access.
    // 60 seconds should be sufficient for most operations.
    SqlStatement stmt(*this);
    [[maybe_unused]] auto cursor = stmt.ExecuteDirect("PRAGMA busy_timeout = 60000");

    // We could also enable WAL mode here, but that changes the database file structure.
    // However, for high-concurrency restoration, it is highly recommended.
    // Let's stick to busy_timeout for now as it's purely a runtime behavior change.
}
```

and by grepping this tree for the pragma nobody issues:

```
$ git grep -n "journal_mode" d03c66f3 -- examples/ledger examples/crm examples/bookmarks examples/polls scripts/scenario
$ echo $?
1
```

(pinned to `d03c66f3`, the revision before morph#739's own comments added
the word to `ledger_model.cpp`; exit 1 is "no match").

The only `journal_mode` under `examples/` is in kanban's *tests*
(`examples/kanban/tests/test_kanban_offline.cpp:629`), which issue it
optionally, for one fixture, and whose own comments record that it is
per-database-file rather than per-connection. `morph::offline::SqliteOfflineQueue`
does set WAL, but on its **own** sqlite3 handle and its own queue file — not on
the ODBC database the rung models use.

**Workaround used in this tree.** Name the thing after what it does in the mode
that is actually in force: `examples/ledger/src/models/ledger_model.cpp`'s
`DeferredReadTransactionGuard` was called `WalSnapshotGuard` until morph#739,
and the old name described the opposite of its real contention behaviour. Keep
raw read transactions as narrow as the aggregation that needs them.

**What retires this.** A rung that sets `journal_mode=WAL` on the ODBC database
deliberately (a decision with its own consequences — WAL lives in the database
file header, so it reaches every other opener and the scenario runner's
fresh-database-per-run assumption), or a Lightweight revision whose
`PostConnect()` sets it. Re-read `PostConnect()` when the pin moves.

---

## 6. `DataMapperPool::Return` performs no transaction cleanup — **live**

**Constraint.** Returning a pooled `DataMapper` to
`Lightweight::GlobalDataMapperPool()` does **not** end a transaction open on
its connection. All three growth-strategy overloads of `Pool<Config>::Return`
do the same three things — drop the async backend, announce the hand-off to
the `SqlLogger`, park the mapper — and none of them issues `SQLEndTran` or
restores `SQL_ATTR_AUTOCOMMIT`:

```cpp
void Return(std::unique_ptr<DataMapper> dm) noexcept
{
    DropAsyncBackend(*dm);                                     // = dm.Connection().DisableAsync();
    SqlLogger::GetLogger().OnConnectionIdle(dm->Connection());
    std::scoped_lock lock(_mutex);
    _idleDataMappers.push_back(std::move(dm));
}
```

A mapper parked with autocommit still `OFF` therefore carries an open
transaction into the pool, and the next — unrelated — borrower inherits it.
Combined with entry 5, the cost of that lands sixty seconds later, on a caller
that opened no transaction, as `database is locked`.

What keeps this tree safe is **declaration order, not the library**:
`SqlTransaction::Commit()` and `::Rollback()` restore autocommit themselves and
immediately, and a handler that relies on the destructor instead is safe only
because its `SqlTransaction` local is declared *after* the pooled mapper and so
destructs *before* it. Hoisting the transaction into a member, a
`std::optional`, or any longer-lived scope silently undoes that.

**Verified at** `bbb972a78e1962b968a2c6ad93f7dade736eaa01`, by reading
`src/Lightweight/DataMapper/Pool.hpp:139-146` (the `UnboundedGrow` overload
quoted above), `:150-162` (`BoundedWait`) and `:198-209` (`BoundedOverflow`,
which is the one actually in force — `DefaultPoolConfig` at `:173-177` selects
it), `:133-136` (`DropAsyncBackend`, the whole of what a returned connection is
cleaned with), and `src/Lightweight/SqlTransaction.cpp:52-93`
(`TryRollback()`/`TryCommit()` restoring autocommit there and then).

**Workaround used in this tree.** morph#740's `PoolTransactionAudit`
(`examples/common/db/pool_transaction_audit.hpp`) installs itself as the
process-wide `SqlLogger` and reads `SQL_ATTR_AUTOCOMMIT` at every hand-off, so
a leak aborts at the leak instead of stalling an innocent caller a minute
later. All seven ladder rungs' `db::setup()`/`db::configure()` and
`testkit/DbFixture` install it.

**`examples/bank` deliberately does not, and that is not a coverage gap**
(morph#752). Bank does not use the pool at all: `bank::db::WithMapper::mapper()`
(`examples/bank/include/bank/db/db_model.hpp`) constructs a `DataMapper`
directly, one per model, and `LightweightOfflineQueue` owns another.
`OnConnectionIdle` and `OnConnectionReuse` — the only two hooks the audit reads
— are emitted from `Pool.hpp` and from nowhere else at this pin, so an audit
installed in bank would inspect zero hand-offs and could never fail. Measured
rather than argued, by `examples/bank/tests/test_pool_scope.cpp`: the same
counter records **2** hand-offs for one deliberate `Acquire()`/return and **0**
across a bank `OpenAccount` + `Deposit` + `Transfer`, the two latter being the
paths that open a `SqlTransaction`. Mutating `TransactionModel::execute(Deposit)`
to acquire from the pool turns that 0 into a 2 and fails the case, so the zero
is a measurement and not an unwired counter.

**What retires this.** A Lightweight revision whose `Pool::Return` ends the
transaction (or asserts that none is open), at which point the audit and this
entry both go. Re-read all three `Return` overloads when the pin moves. Bank's
half of the entry retires the moment any bank code names
`GlobalDataMapperPool()` — `test_pool_scope.cpp` fails when it does.

---

## Adding an entry

File the upstream issue first, so the entry has a retirement condition somebody
is tracking, then add a section here with the five bullets from the top of this
page. An entry with no verification revision is worth less than no entry: it
cannot be re-checked, and it will outlive the constraint it describes — which
is exactly how entries 2 and 3 above became folklore.

# Implementation rules for ladder applications

Binding rules for building every rung of the [application ladder](LADDER.md).
[`TESTING.md`](TESTING.md) governs how the apps are tested; this document
governs how they are *written*. The rules exist to keep the ladder honest:
these applications exist to **stress-test morph**, not to be products.

**The prime directive: every line of custom code that morph (or Lightweight)
could have provided is a defect in the stress test.** If the framework can't
provide it, that inability is a *finding* — record it per
[`FINDINGS.md`](FINDINGS.md), don't quietly code around it.

**The promotion rule (rule-of-three, from the round-7 review):** an
app-built answer to a framework gap (the polling helper with its timeout,
an op-id ledger, epoch tokens, a recursive validator, redaction-on-serve)
may be built twice in `examples/`. The moment a **third** rung consumes it,
it must either be **promoted into `include/morph`** (with its full docs
tax, drawn from the fix budget) or **explicitly dispositioned in the spec
as app-layer by design**. Without this rule the ladder ends with a shadow
framework living in `examples/common` — which would be the program's
biggest finding, permanently unfiled.

## 1. Models are the application

The user-code contract is: **you implement Models; morph exposes them.**

- All business logic, all invariants, and all persistence access live in
  plain, single-threaded model classes with typed actions — nothing
  domain-shaped may live in presenters, QML, `main()`, or free functions.
  If logic can't be expressed in a model, that is a finding.
- Follow [`bank`](bank/README.md)'s established shape: `BRIDGE_REGISTER_*`
  macros in the model header so every call site sees the `ActionTraits`
  specialisation; stateful models keyed with `BRIDGE_KEY_FROM`/
  `BRIDGE_MODEL_KEY` where the domain has identity (account, poll, board,
  sample); the model instance is a cache with identity — hydrated on first
  use, written through on every mutation, dropped when the instance dies;
  the store stays authoritative.
- Models must re-check their own preconditions and authorization
  (`Context::principal`) — the schema's `required` and the client gates are
  UX, not security (`docs/spec/security.md`).
- Action failures are thrown as the app's typed error set (one
  `core/errors.hpp`-style header per rung, as bank does) and surface through
  `Completion::onError`; never encode failure as a magic value in a result
  DTO.

## 2. GUI minimalism

The GUI is deliberately the *least* interesting part of every rung. We are
not building UIs; we are proving morph can drive them.

- **Schema-driven first, always.** Every form is rendered from
  `morph::forms::schemaJson<A>()` through the shipped renderer
  (`MorphForms` QML / `FormsControllerCore`); every list/table goes through
  `morph::forms` views; navigation uses the workflows/app-shell machinery.
  Hand-built input widgets, hand-built tables, and hand-rolled layouts are
  **forbidden by default**.
- **A custom GUI element requires a written justification** in the rung
  README, and the only two acceptable justifications are: (a) the generated
  UI *cannot* express the interaction — which is precisely a forms-subsystem
  finding, so file it on the gap ledger (this is how the ladder found the
  missing explicit-submit mode, the child-table renderer gap, and the
  sum-type gap — see [`LADDER.md`](LADDER.md)); or (b) pure glue with no
  domain logic (an app shell frame, a connection-status indicator).
- Presenters follow [`TESTING.md`](TESTING.md) exactly: Qt-Core-only
  `gui_lib`, thin QObject presenters over `BridgeHandler`s, QML
  bindings-only, timers in the view layer. Presenters translate and route;
  they never decide.
- **Zero styling effort.** Default Qt Quick controls, default fonts, no
  theming, no animations, no custom drawing. A rung that looks pretty has
  spent effort in the wrong place.

## 3. Type discipline: strong types only

Action and result DTOs are the library's public stress surface — every field
must exercise morph's typed machinery.

**The only plain type permitted in an action/result field is
`std::string`** (for genuinely textual data: names, descriptions, paste
content, URLs). Everything else is a strong type:

| Data | Required type |
|---|---|
| Money, measurements, counts, durations | `morph::units::Quantity<Unit, dp>` over the rung's unit system (consteval algebra, `UnitTraits` relations for entry units) |
| Exact unitless numbers | `morph::math::Rational` |
| Points in time | `morph::time::Timestamp` / `DateTime` |
| Foreign keys / lookups chosen by a user | `morph::forms::Choice<T, "ListAction">` |
| Entity identity | A per-entity strong id type (e.g. `struct PasteId`) exposing `hasValue()` so it joins the forms palette as an empty-capable field |
| Closed sets of states/options | `enum class` (never a bare integer, never `bool` — a two-state flag is a two-enumerator `enum class`, per the readability rule that call sites must not read `f(true)`) |
| Optional fields | empty-capable state (`hasValue()` / empty `Quantity`) or the action's `optionalFields` opt-out — not `std::optional<Quantity>`, which silently loses schema annotations (see the round-5 review finding in [`LADDER.md`](LADDER.md)) |
| Line items / sub-objects | nested aggregates of the same palette |
| Protocol scalars — pagination cursors, event ids / epoch tokens, op-ids / idempotency keys, base versions, job ids, capability & confirmation tokens | A named opaque newtype per role (e.g. `struct EventId`, `struct Cursor`), `hasValue()`-capable, serialising as its underlying scalar — **never** a bare `int64_t` and never a loose `std::string`. If morph offers no cheap `Tagged<T, "Name">` helper that joins glaze and the forms palette, that is a **day-one finding filed once**, not eight hand-rolled wrapper sets (round-7 T2). |

**Forbidden in any DTO field: `int`, `int64_t`, `double`, `float`, `bool`,
raw enums.** This deliberately supersedes bank's DTO style (integer minor
units, integer ids, enums-as-integers) — bank predates this rule; the
ladder exists to stress the exact-value and schema machinery, and every
bare `int` in a DTO is a missed stress test. Where a strong type doesn't
fit the palette, that is a finding, not a license for `int64_t`.

Each rung defines its unit system once (`<rung>/include/<rung>/units.hpp`,
modelled on `examples/forms/lab_units.hpp`): the enum, `UnitTraits`
metadata, the consteval algebra, and the exact entry-unit relations. Money
is a unit system too (currency units with per-currency `dp` — respecting
the `DecimalPlaces >= 1` floor and the documented JPY/KRW convention from
the ledger rung).

Every action declares `validate()` (via `allRequiredEngaged` +
domain checks) and carries `fieldMetadata`/`formRules` where the form needs
them — the DTO *is* the form definition; there is no second source of
truth.

## 4. Persistence: Lightweight, exclusively

All persistence goes through the
[Lightweight](https://github.com/LASTRADA-Software/Lightweight) ORM, the
same way [`bank`](bank/README.md) does. **No rung implements any database
code itself.**

- **Entities** are Lightweight `Field<>`-wrapped records in
  `include/<rung>/db/*_entity.hpp`, kept strictly separate from the wire
  DTOs; the model maps DTO ⇄ entity (bank's two-type-layer architecture).
- **Access** is through `Lightweight::DataMapper`: a model holds no
  connection of its own — each `execute()` acquires one from
  `Lightweight::GlobalDataMapperPool()` for its own duration and returns it
  before returning, rather than a model owning a permanent connection for
  its whole lifetime. Still correct without locks: morph runs each model on
  its own strand, so no two `execute()` calls on the same instance ever
  overlap, and each acquisition is entirely self-contained within one call.
  The database is an on-disk SQLite file, never `:memory:` (private per
  connection).
- **Schema** is owned by `LIGHTWEIGHT_SQL_MIGRATION` definitions (bank's
  `src/db/schema.cpp` pattern). Migrations are the *only* DDL mechanism —
  no `PRAGMA user_version` scheme, no hand-run SQL scripts.
- **Relations** use `BelongsTo`/`HasMany` with declared foreign-key
  constraints, and ownership authorization is expressed *through the
  relation* (bank's `loadOwned` pattern), not by string-building WHERE
  clauses.
- **Transactions**: cross-row atomicity uses `SqlTransaction`; the
  cross-*instance* caveat and row-version re-hydration pattern are
  documented in bank's README ("The honest edge") and apply unchanged.
- **Forbidden**: direct `sqlite3_*` calls; hand-written SQL strings outside
  Lightweight's facilities; custom connection pools, caches, retry
  wrappers, or ORM-lookalike helper layers. If Lightweight cannot express
  something a rung needs (a query shape, a constraint, a quirk like bank's
  documented `HasMany` ordinal-index and `Update`/`Query<T>` limitations),
  **record it as a finding and work within Lightweight's own documented
  idioms** (e.g. bank's relation-free projection rows).
- **The sanctioned escape tier (round-7 T1)**: where `DataMapper` cannot
  express a *required mechanism*, the rung may use **Lightweight's own
  raw-query facility, invoked from inside the model, with a mandatory
  finding entry** — never the sqlite3 API, never a parallel helper layer.
  Known escapees, pre-enumerated so nobody relitigates them: conditional
  atomic updates with `RETURNING` (pastebin's burn-atomicity answer), FTS5
  virtual tables (forge search fallback), and WAL-read-transaction snapshot
  pinning (ledger reports). Without this tier, rung 1's *recommended*
  design was illegal under this rule — rule erosion or silent workarounds
  would have followed, both defects by the prime directive's own standard.
- **WASM**: Lightweight (ODBC) cannot run in the browser, and no
  browser-side substitute store may be written. The ladder's WASM clients
  are **remote clients** — persistence lives server-side, behind the model.
  (Bank's local-only in-memory WASM store predates this rule and is not the
  ladder pattern.)
- The framework's own durable stores are unaffected by this rule: morph's
  `SqliteOfflineQueue`, journal logs, etc. are library code under test, not
  app database layer.

## 5. Testing: models are 100% unit tested

- **Every model is 100% unit tested** — line and branch coverage of
  `src/models/` + `include/<rung>/models/` at 100%, enforced as a
  **blocking `codecov.yml` component gate** scoped to those paths (the
  recipe rung 0 proved out on `examples/common`: a
  `component_management.individual_components` entry naming the paths,
  `informational: false`, wired to the `clang-coverage` CI leg's
  `scripts/coverage.sh` output — see [`TESTING.md`](TESTING.md)'s "Build
  system and CI"). The DTO⇄entity mapping and error paths count as model
  code. **The store-error half is covered honestly, not excluded**
  (round-7 T3): branches reachable only through database failure
  (`SQLITE_BUSY`, constraint violations, `SqlTransaction` rollback) are
  exercised by provoking each failure class *for real, through the schema*
  — `db_busy_fixture.hpp` for `SQLITE_BUSY` (a genuine, uncommitted `BEGIN
  IMMEDIATE` write transaction on a second connection), a conflicting row
  or a dropped table for the rest (see [`TESTING.md`](TESTING.md)'s testkit
  section). There is no injectable seam between Lightweight's `DataMapper`
  and the ODBC driver, so a mock failing driver is not on offer and not
  planned — only a real, schema-level failure counts. The escape hatch is
  unchanged and still narrow — a
  per-line exclusion tag is legitimate only for a branch no such fixture can
  provably reach, which is the outcome round-7 T3 rejected being reopened by
  the back door.
- **The gate's numeric target is the measured ceiling, not a blind
  100%** (rung-0 finding, `examples/common`'s coverage work): llvm-cov's
  source-based coverage places its own counters on constructs that are not
  really branches — a `switch`/`case` block's closing `}` after `break;`,
  or the closing `}` of a scope whose one statement is a
  `std::function<void()>` call — and those counters can read 0 even though
  the statement immediately above them, per its own hit count, ran. There
  is no llvm-cov equivalent of gcov's inline `LCOV_EXCL_LINE` to suppress
  a single line. When every remaining "missed" line is one of these
  (verified, not assumed, by reading the hit count on the preceding
  statement) or Qt AUTOMOC-generated code, compute the real ceiling
  (`covered / total` from `llvm-cov export`'s JSON, not the rounded
  percentage in the human-readable report) and set the component's
  `target:` a small margin below it, with a comment enumerating every
  known-artifact line and why it's benign. A rung that hits this should
  not spend further cycles chasing a display artifact — reroute that
  effort at genuinely uncovered logic instead.
- **Before writing a test to chase an apparently-unreachable branch,
  trace it into the vendored library first** — the branch may be
  genuinely dead, not just hard to trigger. Rung 0's `db_fixture.hpp`
  shipped a `sqlite_sequence`-skip guard in its table-drop sweep, believed
  to be a genuine (if hard-to-exercise) edge case, until reading
  Lightweight's own `SqlSchema.cpp` showed that `ReadAllTables()` already
  filters that table out before any caller ever sees it — the guard could
  not execute under any input. The fix was deleting the dead branch, not
  writing a test for it. Coverage tooling cannot tell "hard to reach" apart
  from "impossible to reach"; only reading the dependency's source can.
- **Use dependency injection to make hard-to-trigger branches directly
  testable, rather than reaching for a process/subprocess harness.**
  Two recurring shapes in rung 0's own testkit, both reusable for model
  code: (1) a `static const X = [...]()` once-per-process env-var read
  (e.g. a connection-string override) — no two tests in the same binary
  can ever be first to observe a different value once an earlier test has
  already forced the guard's decision. Extract the parsing/branching logic
  into a small, pure, `noexcept`-where-possible function taking the raw
  value as a plain parameter (`computeConnectionString(const char*)`,
  `computeDeadlineScale(const char*)`); the `static const` site becomes a
  one-line, branch-free delegation, and the function is tested directly
  with whatever inputs a test likes. (2) A throw-on-I/O-failure branch
  (`listen()` returned false, `waitForConnected()` timed out) that can't
  be forced deterministically without flakiness or a test-only seam on a
  third-party class (`QWebSocketServer`, an ODBC driver). Extract the
  decision (`throwIfListenFailed(bool)`) so the *decision* is what's
  tested with a plain `bool`, and the real I/O call site becomes a
  trivial, branch-free one-liner. Reach for this before building a
  subprocess helper or a mock layer — it is less machinery, and it is what
  rung 0 was redirected toward after first trying the subprocess route.
- Model tests run the full backend-mode matrix (`Local` /
  `LocalSingleThread` / `Socket`) per [`TESTING.md`](TESTING.md); every
  invariant named in the rung README ("required tests", DoD) exists as a
  named test before the feature is called done.
- Invariants are tested property-style where the README says so (ledger's
  per-currency zero-sum, kanban's dense-unique positions) with seeds
  printed on failure.
- GUI/presenter testing follows `TESTING.md`; there is no separate GUI
  logic to test if rule 2 was followed — presenter tests verify routing,
  error surfacing, and quiescence, not business behavior.

## 6. Rung pull-request checklist

Every rung PR states, in its description:

1. No domain logic outside models (rule 1) — where it was tempting, the
   finding filed instead.
2. Custom GUI elements present, each with its written justification and
   gap-ledger entry (rule 2) — ideally none.
3. `grep`-clean DTO surface: no `int`/`double`/`bool`/raw-enum fields
   (rule 3); `std::string` only where the data is text.
4. No database code outside Lightweight entities/migrations/mappers
   (rule 4) — `grep sqlite3_` returns nothing in the rung.
5. Model coverage gate green (rule 5) — the blocking `codecov.yml`
   component, target set from a measured ceiling with every known-artifact
   line documented, matrix green.
6. The rung README's design questions are resolved in writing
   ([`LADDER.md`](LADDER.md) discipline rule).

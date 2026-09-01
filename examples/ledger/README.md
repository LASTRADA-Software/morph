# ledger — rung 5 of the [application ladder](../LADDER.md)

**Status: design annex** ([round-7 program decision](../LADDER.md)) — this
README is the deliverable; construction is a post-rung-4 decision, and
ledger is first in line among the annex rungs (the only one with a
genuinely app-shaped core; its sharpest content runs earlier as the
Rational fuzz and journal-evolution spikes). Double-entry personal finance: accounts, transactions
with multiple legs that must balance exactly, budgets, multi-currency, rules,
and a full audit trail. This rung exists to put morph's exact-value types
(`math::Rational`) under *invariants*, not just arithmetic — and to benchmark
morph's journal against the two opposing sync philosophies in the wild.

It deliberately **upgrades, not duplicates, [`bank`](../bank)**: bank has
accounts/payments/statements; ledger adds what bank lacks — the double-entry
invariant, multi-currency, budget math, and rule cascades.

## Reference implementations

- **[Firefly III](https://github.com/firefly-iii/firefly-iii)** (PHP/Laravel,
  AGPL) — the anchor. Its data model documentation is unusually explicit:
  `TransactionJournal` (the financial event) contains ≥2 `Transaction` rows
  (debit/credit legs) that must sum to zero — double-entry enforced
  structurally. Fully specified JSON API = a ready action catalog:
  <https://api-docs.firefly-iii.org/>. Its audit-log currency bug
  ([firefly-iii#12014](https://github.com/firefly-iii/firefly-iii/issues/12014))
  is field evidence that exact-money audit trails are genuinely hard — the
  bug class this rung must show morph prevents by construction.
- **[Actual Budget](https://github.com/actualbudget/actual)** (TypeScript,
  MIT, SQLite everywhere) — the sync counter-reference. Every mutation
  becomes field-level CRDT messages `(dataset, row, column, value)` with
  hybrid-logical-clock timestamps and a merkle tree for divergence detection;
  the sync server is ~300 lines; undo is layered on the same messages
  (`packages/loot-core/src/server/undo.ts`). Best explanation:
  [Using CRDTs in the Wild](https://archive.jlongster.com/using-crdts-in-the-wild)
  and the annotated companion
  [crdt-example-app](https://github.com/clintharris/crdt-example-app_annotated).
- [Kimai](https://github.com/kimai/kimai) — supplementary for one hard
  numeric corner: documented duration-rounding and rate policies
  (<https://www.kimai.org/documentation/rounding.html>) as explicit action
  parameters.

## What to implement

Models: `LedgerModel` (accounts + transactions, keyed by ledger/book id),
`BudgetModel`, `RuleModel`. Entities (Firefly subset): account
(asset/expense/revenue/liability), transaction journal, transaction leg,
currency, category, budget + budget limit, rule (trigger/action pairs).

**Bootstrapping a book.** `CreateLedger { name }` on `LedgerModel` creates the
book everything else keys off, and answers `{ id }`. It is the only action
here that carries no `ledgerId` — it is what mints one — so it is dispatched
keyless, the same shape `polls::PollModel` gives `CreatePoll`. Any
authenticated principal may call it; a caller with no token is refused
`unauthorized` by `LedgerAuthorizer` before the model is entered, and an empty
principal by the model itself. Added by morph#361: until then a `ledgers` row
was created by no registered action at all, and a freshly started
`ladder_ledger_server` against a new database served a book nobody could open
(`OpenAccount` refused with `OpenAccount: no such ledger`). There is no
per-book ownership — see morph#382.

Build order (status as of rung 5's implementation, see
`docs/superpowers/plans/2026-08-19-ledger-rung5.md`):

- **Steps 1-7: implemented.** Accounts and composite transactions,
  multi-currency with foreign-amount pairs, budgets, rules, undo as a
  compensating action, CSV import with dedup, and the submit->poll report
  pair -- each with model, presenter, QML bridge and tests. The desktop
  client wiring all four bridges is in `gui/`.
- **Step 8: prose and both scenarios delivered.**
  `SYNC-BENCHMARK.md` states the philosophy and both scenarios in full.
  Scenario B is reproducible against `UpdateRule`'s real version conflict
  (`expectedVersion`, `VersionConflict`). Scenario A -- two offline clients
  editing the same *transaction* -- is recorded as **inapplicable**, not
  implemented: this rung ships no transaction-edit action by design (a posted
  journal entry is an audit record, corrected by a new compensating entry per
  design spec §6, never edited in place), so §10's scenario presumes a
  capability §6 rules out. Running the collision this rung *can* express
  instead -- two clients both reversing the same transaction offline -- found
  a real bug (both `UndoTransaction`s applied, doubling the reversal), fixed
  by `causal_parent_id` naming what a compensating entry reverses and a second
  reversal being rejected with `AlreadyReversed`. morph#144 tracked both
  halves and is closed.


1. Accounts + `StoreTransaction { description, date, legs[] }` — one
   composite, all-or-nothing action creating the journal and all legs.
   **Server-side invariant: legs sum to exactly zero, checked in `Rational`
   arithmetic** — the model rejects, never rounds. Review correction:
   *define the invariant per-currency first* — legs in different currencies
   cannot sum, so the rule is "legs sum to zero within each currency, with
   foreign-amount pairs balancing across" (Firefly's actual model); the
   property test below is unfalsifiable until this definition is written.
2. Multi-currency: legs carry amount + currency, foreign-amount pairs with
   exact exchange rates (`Rational`), per-currency decimal precision via
   `withDecimalPlaces`.
3. Budgets: monthly limits, spent-so-far aggregation — exact summation over
   many rows; measure `Rational` overflow headroom (int64 pair, no bignum)
   and document the practical magnitude/precision envelope.
4. Rules: "description contains X ⇒ set category Y" applied during store —
   reuse the cascade-journaling answer from [`kanban`](../kanban), with the
   money-grade sharpening: **rules are runtime data, so replay must pin the
   rule-set version** (journal entries carry the rule version, or replay
   suppresses rule evaluation entirely). Edit a rule between record and
   replay and the naive audit trail lies — exactly the Firefly bug class.
   Named test, not a bullet.
5. **Undo = compensating action, by design.** Review verdict: replay-based
   undo is the wrong tool for a SQLite+outbox model (the journal spec says
   replay is exact only for pure in-memory models, and `undoLast()`'s
   replay is O(all remaining actions) — a performance cliff at ledger
   scale). Undo of `StoreTransaction` is a reversing journal entry,
   Firefly-style. Test the compensation path.
6. **CSV/OFX import with dedup** (added per review — table stakes in every
   anchor): chunked bulk actions, content-hash idempotency keys at scale,
   duplicate detection across re-imports — the natural production home of
   the exactly-once discipline from [`kanban`](../kanban).
7. Reports (monthly statement, budget report) — **the document-generation
   pattern**, this rung's framework-level deliverable: `SubmitReport` →
   job id → `GetReportStatus` polling → fetch result; the submit→poll idiom
   for long-running work that `Completion<T>`'s one-shot callbacks can't
   express directly. **Snapshot semantics must be specified**: the job can
   otherwise see mid-action state across `LedgerModel`/`BudgetModel` — use a
   SQLite WAL read transaction; the byte-identical DoD is only meaningful
   against that snapshot.

   **Who runs the job (morph#160).** `SubmitReport` writes a `Pending` row
   and returns; it schedules nothing and starts no thread. `ledger::app::App`
   — this rung's App layer — sweeps for `Pending` rows on a timer and
   dispatches `RunReportJob` back at `LedgerModel`, where the aggregation
   itself lives. That split is `IMPLEMENTATION.md` rule 1 applied literally:
   the monthly-statement aggregation is business logic and stays in a model;
   only the decision of *when* it runs is orchestration, and orchestration
   belongs to the App. It is also the shape `bookmarks::app::App`'s metadata
   worker already had. `LedgerModel` owned a `ThreadPoolExecutor` before
   this, and was the one ladder model that included
   `<morph/core/executor.hpp>`.

   Two consequences worth naming. The run now happens on the strand for its
   own ledger, so a report and a concurrent `StoreTransaction` against the
   same book serialise instead of racing — the WAL read snapshot is still
   needed, because `BudgetModel` writes from a strand of its own. And a job
   outlives the process that accepted it: it is a row, so a runner that
   starts later — after a crash, after a restart — picks it up. The previous
   design's queued lambda died with its process.

   **The App owns a `RemoteServer`, fronted by `ladder_ledger_server`
   (morph#242).** `RemoteServer` clears the session principal for any
   authorizer that does not authenticate (`docs/spec/security.md`), so a real
   login story needed a real, verifying authorizer: `LedgerAuthorizer`
   (`ledger/auth/ledger_authorizer.hpp`) plus `AuthModel`/`Login`
   (`ledger/models/auth_model.hpp`, `ledger/dto/auth_dto.hpp`), the same
   signed-token shape bookmarks'/kanban's authorizers use. The report
   runner's internal client now dispatches through a `SimulatedRemoteBackend`
   over this `App`'s own `RemoteServer`, carrying a genuinely signed token
   for `kReportRunnerPrincipal` — an ordinary authenticated dispatch, not a
   `LocalBackend` bypass. `ladder_ledger_gui` mints a Local-mode session via
   `AppContext::login()` in Local mode, and dispatches `Login` against the
   server in Remote mode, mirroring bookmarks'/kanban's own `gui/main.cpp`.
8. **Sync benchmark** (written deliverable, not code): reproduce one
   concurrent-edit scenario from Actual (two offline clients edit the same
   transaction's different fields) and one from ODK-style base-version
   conflict, run both through morph's action-replay journal + offline queue,
   and document where action-level replay (intent-preserving, coarser) lands
   versus field-level LWW merge (fine-grained, intent-blind). State
   explicitly: **morph's ordering authority is server arrival order, full
   stop** (no HLC), and show one scenario where that differs from Actual's
   hybrid-logical-clock merge. Include the clock-skew test: two clients
   with injected ±5-minute clocks writing to one ledger — the audit view
   orders by journal order and displays payload timestamps as
   claimed-not-authoritative.

Forms: transaction entry uses `morph::forms` schemas — amount fields as
`Rational` with per-currency `x-decimalPlaces`, category combo via
`forms::Choice` backed by a list action.

## How money is represented

Every money value in this rung — a transaction leg, a budget limit, an
account balance, a report total — is a `morph::math::Rational` carrying a
**whole number of the currency's minor units**, with `decimalPlaces` naming
the scale those units are counted in. `$4.50` is `{num: 450, den: 1, dp: 2}`;
`¥500` is `{num: 500, den: 1, dp: 0}`. `ledger/core/money.hpp` owns the
encoding and the two operations it needs.

**This is not `Rational`'s own reading of that triple.** `rational.hpp`
defines the value as `numerator/denominator` and calls `decimalPlaces` a
display tag that "never changes a stored value"; comparison is "purely
value-based on the canonical (numerator, denominator) pair and ignores
`decimalPlaces` entirely". `Rational` therefore reads `{450, 1, dp 2}` and
`{450, 1, dp 1}` as the same number, where this rung reads `$4.50` and
`$45.00`. The two readings agree only when every operand is on one scale.

**The model is what guarantees that.** `LedgerModel::execute(StoreTransaction)`
and `storeJournalImpl` restate every leg onto *its own account currency's*
scale (`ledger::restateMinorUnits`, `ledger::currencyDecimalPlaces`) before
the per-currency zero-sum check runs and before any row is written;
`BudgetModel::execute(SetBudgetLimit)` restates a limit the same way. Restating
is exact or nothing — an amount with more precision than its currency has
(`$4.505` in a USD account) or a non-integral minor-unit count off the wire
(`{"num":9,"den":2}`) is rejected with `ValidationError`. **The model never
rounds money.**

Without that step the invariant is unsound in both directions, because
`Rational::operator+` adds numerators and propagates `std::max` of the two
precisions: `$4.50` at `dp 2` and `-$45.00` at `dp 1` both have numerator
±450, so they net to zero and a journal booking four dollars fifty against
forty-five dollars is *accepted*; and `$4.50` written `{45, dp 1}` against
`-$4.50` written `{-450, dp 2}` nets to -405 and a balanced pair is
*rejected*. Both are pinned as tests in `tests/test_ledger_model.cpp`.
Restating also keeps every stored leg of an account on that account's own
scale, which `buildLedgerState` relies on when it seeds each balance at the
currency's precision — one leg stored at a wider scale would otherwise pull
that account's rendered balance off by a factor of ten permanently.

**Why not `morph::units::Quantity`.** `Quantity` takes its unit as a
*compile-time* non-type template parameter (`Quantity<auto U, std::uint32_t
DeclaredDecimals>`), and a leg's currency is the account's runtime data —
there is no `Quantity` spelling meaning "whichever currency this account
happens to hold", which is the conclusion the design spec's §2 already
reached. Typing every leg `Money<Currency::USD>` would put a false unit tag
on every EUR, JPY and KRW leg. Leg amounts therefore stay `Rational` and the
encoding above is the rung's binding convention. `Money<C>` *is* used where
the currency is known at the point of use — the display path.

**Display.** `ledger::formatMoney(currency, amount)` is the single rendering
path: it recovers the decimal value from the minor-unit count, hands it to
`Money<C>` for the named currency, and lets `morph::units::toDecimalString`
produce the digits by exact integer long division. The QML views bind the
pre-rendered `balanceText` / `limitText` / `spentText` / `amountText` the
bridges publish.

### Findings this encoding surfaced

- **No fixed-fraction-width rendering on `Quantity`.**
  `morph::units::toDecimalString` renders shortest-form, so `$4.50` comes
  back as `"4.5"` and a zero balance as `"0"`. A money column wants `"4.50"`
  and `"0.00"`; there is no width knob to ask for it. The rung renders
  shortest-form rather than hand-rolling a second formatter.
- **No public integer power of ten.** `morph::math::detail::powerOfTen` is
  exactly what restating between scales needs, but it lives in the
  framework's `detail` namespace; `ledger::detail::powerOfTen` writes it out
  again rather than depend on a private symbol.
- **No runtime-unit `Quantity`.** The gap under "Why not
  `morph::units::Quantity`" above is the reason this rung's money type is a
  bare `Rational` with an out-of-band encoding at all.

## morph subsystems exercised

Exact `Rational` arithmetic under a hard invariant; schema-driven money
forms; journal-as-audit with the store/log divergence handled via
`setOutboxManaged` + `journal::OutboxRelay` (the SQLite-transactional model
opts in — see `docs/spec/journal/journal.md`); offline queue with financial
data; the submit→poll job idiom.

## Expected strain points

- `Rational` is a fixed-width int64 pair: budget aggregation over thousands
  of rows probes overflow behavior (currently UB on overflow — document what
  the app must do to stay safe). **Sharper, per review: intermediates
  overflow before results do** — `amount × exchange-rate` with high-dp
  currencies can overflow the num/den pair even when the final value is
  representable. Ship a property/fuzz test over `Rational` arithmetic at
  ledger-realistic magnitudes; expect it to motivate a checked-arithmetic
  mode [probable framework gap].
- Wire input is clamped, not rejected, on malformed rationals — and the
  round-5 review verified **there is no pre-decode seam to catch it**:
  every dispatch path decodes first, then validates the already-clamped,
  perfectly plausible value (`{"num":5,"den":0,"dp":2}` arrives as exactly
  `5/1`; `{}` as canonical zero). The test to write (D2): prove only the
  model's own zero-sum invariant (or an app-added num/den echo check)
  rejects — i.e. the mitigation is app-built scaffolding, and a pre-decode
  validation hook is a named framework gap.
- **Zero-decimal currencies (JPY/KRW)**: correction to the round-5 draft —
  `DecimalPlaces` has **no floor of 1**. `Quantity<U, 0>` is a fully legal,
  tested first-class configuration (`rational.hpp`'s own doc comment,
  `docs/spec/util/rational.md`, `docs/spec/util/quantity_type.md`, and
  `tests/test_quantity.cpp` all assert `DecimalPlaces{0}` round-trips
  correctly), so JPY/KRW need no app-side workaround — declare the currency
  unit at `dp=0` and the type system carries it natively. Named test: a
  JPY leg stores and displays as a true integer, with no `x-rules` gate
  required.
- **Locale entry**: in de-DE the group separator is "." and the shipped
  normalizer strips it anywhere — typing `1.5` submits **15**, a silent 10×
  money error. Pin the behavior, fix (positional grouping validation or
  reject), and mirror the vectors through `normalizeLocaleNumber` (D5).
  Related: result *display* in the shipped forms renderer goes through
  `double` division — balances beyond 2^53 drift on readback while the
  payload is exact. This rung's own views do not: every money label binds
  text the bridge pre-rendered through `ledger::formatMoney`, which is exact
  integer long division (see "How money is represented" below). No QML file
  divides anything.
- **Recurring transactions (time-scheduled jobs — this rung owns the
  shape)**: Firefly-style schedules are the ladder's one cron-shaped
  server job — who ticks, on what thread, under what principal, journaled
  how. Forge's webhook retry loop assumes this answer exists.
- **Empty-principal writes**: a token expiring between authorize and
  authenticate dispatches with a cleared principal; deterministic test via
  the injectable `TokenVerifier` clock — assert no successful mutating
  journal entry ever carries an empty principal (the model must refuse).
- Local-time month boundaries vs. UTC storage: the 23:30 local transaction
  landing in the right budget month is a presenter-layer conversion — a
  dual-mode GUI test.

## Definition of done

- Property test: no sequence of stores/edits/undos ever leaves any journal
  violating the per-currency zero-sum invariant defined in step 1.
- Rule-version pinning proven: editing a rule after recording does not
  change what replay reconstructs.
- Statement generation via submit→poll, output byte-identical on re-run
  against its declared snapshot.
- The sync-philosophy comparison (including the arrival-order-vs-HLC
  scenario) written up in this folder.

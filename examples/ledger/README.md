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

Build order:

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
   express directly. **Snapshot semantics must be specified**: the job runs
   off the strand and can otherwise see mid-action state across
   `LedgerModel`/`BudgetModel` — use a SQLite WAL read transaction; the
   byte-identical DoD is only meaningful against that snapshot.
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
  Related: result *display* in the shipped renderer goes through `double`
  division — balances beyond 2^53 drift on readback while the payload is
  exact; presenter display must use the exact formatter.
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

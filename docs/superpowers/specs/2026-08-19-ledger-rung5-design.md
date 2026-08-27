# ledger (rung 5) — implementation design

Status: approved for implementation. This document resolves the design
questions `examples/ledger/README.md` leaves open, in writing, per the
[application ladder](../../../examples/LADDER.md)'s own discipline rule
("design questions... must be resolved in writing before the next rung
starts"). It does not restate the README — read that first for scope,
reference implementations, build order, and the Definition of Done.

**Program-status note**: rung 4 (kanban, PR #121) has not merged as of this
writing. LADDER.md states rung 5 construction is "a separate decision taken
after rung 4 with the findings pipeline scoreboard in hand" — that decision
was made explicitly to proceed in parallel rather than block on the merge
(kanban's CI is green on its substantive content; the open PR is process,
not open design work). This branch (`ladder-ledger-rung5`) is cut from
`master`, not from `ladder-kanban-impl`, and cherry-picks only the one
framework commit ledger's own step 4 hard-depends on for compilation
(`LogEntry::causalParentId` + `journal::isReplaying()` — see §5). Every
other citation of kanban's rung-4 content below is to its written design
spec (`docs/superpowers/specs/2026-08-16-kanban-rung4-design.md`, unmerged),
quoted or restated rather than assumed, since that file does not exist on
`master` yet.

**Update: PR #121 has since merged.** Rung 4 (kanban) and its design spec
are both on `master` now; the parallel-construction rationale above is
historical context for why this branch was cut before that merge, not a
description of the tree's current state.

**Scope**: steps 1–7 of the README's build order (accounts + transactions
with the per-currency zero-sum invariant, multi-currency, budgets, rules +
cascade-journaling, undo-as-compensation, CSV/OFX import with dedup,
reports via submit→poll). Step 8 (the sync-philosophy benchmark) is a
**written deliverable, not code** per the README itself, and is produced as
part of this same spec (§10) rather than deferred — it requires no new
model code, only the comparison write-up and the two reproduced scenarios
as tests.

## 1. Models, entities, and the double-entry core (steps 1–2)

**Models**: `LedgerModel` (accounts + transaction journal, keyed by ledger
id — one ledger per book, mirroring `kanban::BoardModel`'s per-project
keying), `BudgetModel` (keyed by ledger id), `RuleModel` (keyed by ledger
id). Three models rather than one, following the ladder's established
per-concern-model pattern (`polls::PollModel` / `kanban::BoardModel` +
`ProjectAdminModel`), not because the concerns are independent — budgets and
rules both read `LedgerModel`'s committed state — but because each has its
own lifecycle and RBAC surface, and a single god-model would violate
`IMPLEMENTATION.md` rule 1's "models are the application" by forcing
unrelated invariants into one `execute()`.

**Entities** (Lightweight, `include/ledger/db/*_entity.hpp`, strictly
separate from wire DTOs per bank's two-layer architecture):

- `AccountRecord` — `id`, `ledgerId` (`BelongsTo`), `name`, `kind` (int:
  asset/expense/revenue/liability — Lightweight `Field<int>`, the DTO layer
  is what wraps this as an `enum class`, per bank's precedent of the entity
  layer staying close to the column type), `currencyCode`.
- `TransactionJournalRecord` — `id`, `ledgerId`, `description`, `date`
  (`Timestamp` at rest, see §9's UTC-storage note), `causalParentId`
  (nullable `SqlAnsiString`, see §5).
- `TransactionLegRecord` — `id`, `journalId` (`BelongsTo`), `accountId`
  (`BelongsTo`), `amountNum`/`amountDen`/`amountDp` (the `Rational`'s three
  fields stored as plain columns — Lightweight has no `Rational`-aware
  column type, so the model's DTO⇄entity mapping does the pack/unpack; this
  is the ORM boundary, not a framework gap worth filing), `currencyCode`,
  `foreignAmountNum`/`Den`/`Dp` + `foreignCurrencyCode` (nullable triple,
  present only on a foreign-amount leg — see §2).
- `CategoryRecord`, `BudgetRecord`, `BudgetLimitRecord`, `RuleRecord` — one
  table each, following the same `Field<>` + `BelongsTo` shape as bank's
  `AccountRecord`/`TxnRecord` (`examples/bank/include/bank/db/*_entity.hpp`).

This is a genuine structural upgrade over `bank::db::TxnRecord`
(`examples/bank/include/bank/db/txn_entity.hpp`), which stores one row per
transaction with a single `amountMinor`/`currency` pair and an optional
`counterparty` `BelongsTo` for transfers — adequate for bank's two-party
transfers but structurally incapable of expressing Firefly's N-leg journal
entry (a paycheck split three ways, one journal, three legs). `LedgerModel`
is where the ladder's first true one-journal-to-many-legs schema lives.

**DTOs** follow `IMPLEMENTATION.md` rule 3's strong-type palette
throughout — this is itself a deliberate contrast with `bank::Money`
(`examples/bank/include/bank/core/money.hpp`: `struct Money { int64_t
minor; Currency currency; }`, whose `operator+`/`-` do not check currency
match — exactly the class of bug this rung exists to make structurally
impossible). Account kind and rule trigger/action types are `enum class`;
account/journal/category/budget/rule identity are per-entity strong id types
(`AccountId`, `JournalId`, ...) with `hasValue()`.

**Money is the one field the palette cannot type.** `morph::units::Quantity`
takes its unit as a *compile-time* non-type template parameter
(`Quantity<auto U, std::uint32_t DeclaredDecimals>`), and a leg's currency is
the account's runtime data — §2 below sets out why there is no
`Quantity<Currency, dp>` spelling meaning "whichever currency this account
happens to hold", and `ledger::Money<C>`
(`examples/ledger/include/ledger/core/units.hpp`) fixes `C` at compile time by
construction. A leg amount is therefore a `morph::math::Rational` carrying a
**whole number of the currency's minor units**, with `decimalPlaces` naming
the scale those units are counted in: `$4.50` is `{num: 450, den: 1, dp: 2}`,
`¥500` is `{num: 500, den: 1, dp: 0}`. Never `bank::Money`, and never a bare
`int64_t minor` — the scale travels with the value. `Money<C>` *is* used where
the currency is known at the point of use: the display path (§7).

That encoding is deliberately **not** `Rational`'s own reading of the same
triple. `include/morph/util/rational.hpp` defines the value as
`numerator/denominator` with `decimalPlaces` a display tag that "never changes
a stored value", and compares "purely value-based on the canonical
(numerator, denominator) pair", ignoring `decimalPlaces` entirely. `Rational`
therefore reads `{450, 1, dp 2}` and `{450, 1, dp 1}` as the same number where
this rung reads `$4.50` and `$45.00`. **The two readings agree only when every
operand is on one scale, so the model puts them on one scale before it does
any arithmetic** — see step 1 of the zero-sum decision below. The full
rationale, the encoding's rules, and the framework gaps it surfaced are in
`examples/ledger/README.md`'s "How money is represented"
(`ledger/core/money.hpp` is the code).

**`StoreTransaction { description, date, legs[] }`** — one composite,
all-or-nothing action. `legs: std::vector<TransactionLeg>` where
`TransactionLeg { accountId: AccountId, amount: Rational }` (the currency
lives in the account, so a leg's amount carries only a minor-unit count and a
scale, and the account's own currency supplies the denomination at validation
time — see §2 for why this can't be a compile-time
`Quantity<SpecificCurrency, dp>` per leg). `validate()` requires
`allRequiredEngaged` plus: at least two legs, every `accountId` engaged.

**Decision: the per-currency zero-sum invariant, defined precisely.**
The README's own review correction is adopted verbatim and made executable:
*legs sum to exactly zero within each currency, with foreign-amount pairs
balancing across.* Concretely, `LedgerModel::execute(StoreTransaction)`:

1. Partitions `legs` by `currencyCode` (the leg's account's currency, looked
   up from `AccountRecord`, never a client-supplied field — the client
   cannot assert a leg's currency independent of its account).
   **Restates every leg onto that account currency's scale** before it
   enters a partition (`ledger::restateMinorUnits` against
   `currencyDecimalPlaces` of the currency just looked up). Leg amounts are
   minor-unit counts and nothing on the wire constrains the scale a client
   sends them at; without this step the check is unsound in *both*
   directions, because `Rational::operator+` adds numerators and propagates
   `std::max` of the two precisions. `$4.50` (`{450, dp 2}`) and `-$45.00`
   (`{-450, dp 1}`) net to numerator zero and are **accepted**; `$4.50`
   written `{45, dp 1}` and `-$4.50` written `{-450, dp 2}` net to -405 and
   are **rejected**. Restating also keeps every stored leg of an account on
   that account's own scale, which `buildLedgerState` relies on when it seeds
   each balance at the currency's precision. Restating is exact or nothing —
   an amount with more precision than its currency has (`$4.505` in USD), or
   a non-integral minor-unit count off the wire (`{"num":9,"den":2}`), is
   rejected with `ValidationError`. **The model never rounds money.**
2. For each currency partition, sums the restated `Rational` amounts (via
   `Rational::operator+`) and asserts the sum is canonical zero (`0/1`).
   Rejects with `ZeroSumViolation{currency, actualSum}` on any partition
   that fails — **never rounds, never auto-balances**, per the README.
3. A **foreign-amount pair** is two legs on accounts of different
   currencies that are explicitly linked as one exchange (the
   `foreignAmountNum/Den/Dp` + `foreignCurrencyCode` fields on
   `TransactionLegRecord`): leg A (home currency, e.g. USD -50) carries a
   foreign-amount annotation stating "this leg also represents EUR +45.23 at
   this leg's booked rate", and leg B (EUR account, EUR +45.23) is the
   matching real leg in EUR's own partition. The foreign-amount annotation
   is **display/audit metadata only** — it does not enter either
   partition's zero-sum check, which stays purely per-real-currency. This
   is Firefly's own model (a `foreign_amount`/`foreign_currency_id` pair on
   each `transactions` row) and is the only way multi-currency legs can
   coexist with a *per-currency* (not global) zero-sum rule: a global
   cross-currency sum would require choosing an exchange rate to make it
   meaningful, which is exactly the rounding the invariant forbids.

**Why per-currency, not global-with-conversion**: a global invariant needs
a rate to convert every leg into one reporting currency before summing —
that rate is itself a fact with provenance (booked-at-transaction-time vs.
current-rate), and baking a rate into the *invariant check* would silently
make the invariant's pass/fail depend on which rate was chosen, defeating
its purpose as a structural (not judgment-based) guarantee. Per-currency
zero-sum is checkable from the legs alone, with no external input — the
same property that makes it a rule the model can enforce mechanically
rather than one an app author could get subtly wrong.

**Overflow discipline** (grounded in `include/morph/util/rational.hpp`'s
actual behavior, not assumed): `Rational::operator+` is fixed-width int64
arithmetic, UB on overflow, *not* saturating and *not* exception-throwing
by signature. `LedgerModel` must never let unchecked summation reach that
edge silently — see §7 for the checked-arithmetic mode this rung is
expected to motivate as a framework gap, and the property/fuzz test that
proves the boundary before it is hit in practice, not after.

## 2. Multi-currency (step 2)

**Decision**: currency is a property of the *account*, not a
per-transaction choice — an account is opened in exactly one currency
(Firefly's model: asset/expense/revenue/liability accounts each have a
fixed `currency_id`), and every leg on that account is denominated in it.
This is why `TransactionLeg.amount` cannot be `Quantity<SomeFixedCurrency,
dp>` at the type level: the set of accounts (and their currencies) is
runtime data, not a compile-time enum of every currency the app will ever
see. `Currency` is declared as a `morph::units` unit enum
(`enum class Currency { USD, EUR, JPY, ... }`), with `UnitTraits<Currency>`
supplying `meta()` with each currency's `defaultDecimals` — 2 for USD/EUR,
**0 for JPY/KRW**.

**Correction to the README's original draft**: `DecimalPlaces` has no
floor of 1 — `Quantity<U, 0>` is a legal, tested, first-class
configuration (`include/morph/util/rational.hpp`'s own doc comment,
`docs/spec/util/rational.md`, `docs/spec/util/quantity_type.md`, and
`tests/test_quantity.cpp` all confirm `DecimalPlaces{0}` round-trips
correctly), so JPY/KRW need no app-side workaround or `x-rules` gate — see
also the corresponding fix to `examples/ledger/README.md`'s "Expected
strain points" section, made alongside this spec.

A leg's wire-level amount is a bare `morph::math::Rational` — a minor-unit
count plus the scale it is counted at (§1) — and the model derives the
*actual* scale from the account's currency at validation time: every leg is
restated onto `currencyDecimalPlaces(theAccountsCurrency)` before the
zero-sum check and before it is written. The client's `dp` is therefore an
input to that restatement, never the authority; the stored
`decimalPlaces` is always the account currency's own.

**Exchange rates** are `Rational`, exact by construction — never `double`.
A foreign-amount pair (§1) carries its booked rate implicitly as the ratio
of the two legs' magnitudes; the rate is not stored as a separate field
because it is fully recoverable from the pair and storing it separately
would be a second source of truth that could drift from the legs
themselves.

**Per-currency decimal precision** uses `withDecimalPlaces` (grounded:
`Rational`'s canonical form always carries a `decimalPlaces` field,
`0 ≤ value ≤ 18`) at the account/currency level, not hardcoded — this is
where the correction above matters operationally: JPY and KRW accounts
declare `dp=0` and every leg on them stores true integers, with no
`x-rules` gate and no separate "integer-only" mode. The one thing dp=0
*does* need, named as its own test: **the forms renderer must not silently
insert a `.00` for a dp=0 currency** — this is a presenter/schema
concern (`x-decimalPlaces` driving the input mask), not a `Rational` gap,
and is verified with a dedicated GUI test rather than assumed.

## 3. Budgets (step 3)

`BudgetModel` holds `BudgetRecord` (name, ledger id, category link) and
`BudgetLimitRecord` (budget id, month, limit amount as `Quantity<Currency,
dp>`). `GetBudgetReport(budgetId, month)` aggregates "spent so far" by
summing every `TransactionLegRecord` whose account's category matches the
budget's category and whose journal's date falls in the month, exact
`Rational` summation over potentially thousands of rows.

**Decision on aggregation strategy**: sum in the model (in-memory,
after a bounded `Query<T>` fetch), not in SQL. Lightweight's `DataMapper`
has no `Rational`-aware `SUM()` — a raw SQL `SUM` would operate on the two
plain int64 columns independently and produce a nonsense combined value (a
`SUM(amountNum)` divided by nothing meaningful, since rows can carry
different denominators/precisions). This is a case of
`IMPLEMENTATION.md`'s "sanctioned escape tier" *not* applying — the
constraint here isn't that `DataMapper` lacks a query shape, it's that
`Rational` arithmetic is not expressible in SQL at all without a rewrite
per row, so in-model summation over a `Query<T>`-fetched row set is the
correct answer, not an escape.

**Overflow headroom, measured not assumed** (README's own ask): a
property/fuzz test (§7) sums N synthetic legs at ledger-realistic
magnitudes and decimal places and reports the row count at which the
partial sum's numerator would exceed `int64_t`'s range for a given `dp`,
documented as a comment in the test and restated in this spec's §7 table
once measured — this is empirical, not a static claim, and belongs in the
test file per `FINDINGS.md`'s "a finding that cannot be expressed as a
failing test is not yet understood."

## 4. Rules (step 4)

**Decision — reuses kanban's cascade-journaling answer verbatim**, cited
from kanban's design spec (`docs/superpowers/specs/
2026-08-16-kanban-rung4-design.md`, §9, unmerged as of this writing, PR
#121 — not yet in `master`, hence not restated as settled framework
behavior anywhere outside this citation):

> journal cascades with a causal parent-id, suppress rule evaluation on
> replay ... journaling the cascade with a causal link does double duty —
> it is also what the activity stream needs to render "caused by X" ...
> [`ledger`] reuses this same answer for its own rule cascades.

Concretely: `RuleModel` holds `RuleRecord{ trigger: RuleTrigger (enum
class), matchText: std::string, action: RuleAction (enum class),
actionValue: std::string }` (e.g. trigger = `DescriptionContains`, action =
`SetCategory`). `LedgerModel::execute(StoreTransaction)`, after committing
the journal+legs and before returning, evaluates every active rule against
the new journal's description; a match produces a *second*, distinct
`LogEntry` for the cascaded `SetCategory` mutation, with `causalParentId`
set to the triggering `StoreTransaction` entry's own app-minted identity
(never `LogEntry::seq` — per the framework's own constraint, confirmed in
`include/morph/journal/action_log.hpp`'s field comment: "must not be a
`LogEntry::seq` value"). `journal::isReplaying()` gates rule evaluation:
`if (!morph::journal::isReplaying()) { evaluateRules(...); }` — so a
replay re-applies the cascade's own recorded entry rather than re-firing
the rule.

**The money-grade sharpening the README names — rule-version pinning —
made concrete**: a rule is runtime data (editable via `UpdateRule`), so a
journal entry alone does not say *which version of the rule* produced a
given cascade. Decision: **journal entries carry the rule version**, not
"replay suppresses rule evaluation entirely" (the README's other named
option) — the two are not actually alternatives once cascade-journaling
(above) is chosen, because cascade-journaling *already* suppresses rule
re-evaluation on replay (that is what makes it convergent). What
version-pinning adds on top is for a different consumer: **the activity
stream and audit view**, which render "category set by rule X" and must
say which *edition* of rule X fired, even after the rule has since been
edited. `RuleRecord` gains a monotonic `version` column (bumped on every
`UpdateRule`); the cascade's `LogEntry.payload` (already a full serialized
DTO per the existing journal contract) includes the `ruleId` and the
`ruleVersion` that fired, not just the ruleId. This is app-level
data-in-payload, not a framework field — `causalParentId` is the only
framework-level addition rules need (§5), and it is already shared with
kanban.

**Named divergence test** (not a bullet, per the README's own emphasis):
record a `StoreTransaction` that fires `RuleX` v1 (sets category A);
edit `RuleX` to v2 (sets category B); `replay()` the journal; assert the
replayed state has category A (from the recorded cascade entry, which
pins v1's outcome), never category B (which would mean the naive
"re-derive from trigger + current rules" answer silently rewrote history —
exactly the Firefly bug class this rung exists to demonstrate morph
prevents by construction, per the README's citation of
[firefly-iii#12014](https://github.com/firefly-iii/firefly-iii/issues/12014)).

## 5. Framework/testkit dependencies (four cherry-picks, expanded from the original one)

`LogEntry::causalParentId` and `morph::journal::isReplaying()` are
framework additions designed and implemented on `ladder-kanban-impl`
(commit `5c4d577`, unmerged), not yet in `master`. Ledger's rules step
(§4) cannot compile against them without either waiting for PR #121 or
obtaining the field independently. **Decision**: this branch cherry-picks
the framework-only half of that commit (`include/morph/journal/
action_log.hpp`, `include/morph/journal/journal.hpp`,
`docs/spec/journal/journal.md`, `tests/test_action_log.cpp` — verified
clean of any kanban app-code entanglement) rather than branching from
`ladder-kanban-impl` itself, so this branch's history stays anchored to
`master` and does not carry kanban's own unmerged app code.

**Discovered during implementation-plan writing, not anticipated at
spec-approval time**: the plan's offline (§9-consuming Task 17), sync-
benchmark (§10-consuming Task 24), and multi-client-stress (Task 23)
tasks all depend on four `examples/common/testkit/` files —
`action_driver.hpp`, `offline_rig.hpp`, `client_pool.hpp`,
`convergence.hpp` — that `TESTING.md`'s own ownership table says predate
this rung, but that likewise only exist on `ladder-kanban-impl`, not
`master`, as of this writing. Each of the three commits introducing them
(`ad491c4`, `66717e7`, `3630a15`) was verified scoped strictly to
`examples/common/testkit/` + `examples/common/CMakeLists.txt`, each
ships its own test file, and none touches kanban's own app code — the
same clean-cherry-pick shape as the `causalParentId` commit. All three
were cherry-picked onto this branch alongside the original one (four
cherry-picks total), each verified building and passing its own tests
before any ledger-specific task began.

When PR #121 merges, this branch rebases onto `master` and all four
cherry-picked commits become no-ops (already-applied patches), resolved
by the ordinary rebase conflict-free fast-forward through identical
patch-ids.

## 6. Undo as compensating action (step 5)

**Decision, per the README's own review verdict, restated with the
concrete mechanism**: `UndoTransaction(journalId)` is a *new* action, not
a call into `morph::journal::undoLast()`. It looks up the target
`TransactionJournalRecord` and its legs, constructs a **reversing journal
entry** — one new `TransactionJournalRecord` whose legs are the originals
negated (`Rational::operator-` unary negation per leg, same accounts, same
currencies) — and commits it through the exact same `StoreTransaction`
path (zero-sum invariant re-checked on the reversal, trivially satisfied
since negating every leg of an already-zero-sum set is itself zero-sum).
The reversal's own journal entry carries `causalParentId` pointing at the
undone entry, so the activity stream renders "reverses transaction X."

**Why not `undoLast()`**: two independent disqualifiers, both already
established framework fact rather than new findings —
(a) `docs/spec/journal/journal.md`'s own stated contract is that
`undoLast()` pops the newest entry *regardless of principal* and returns a
*detached* holder with no API to install it back into a live shared
instance, which is structurally wrong for a multi-user ledger where "undo
my last edit" must mean *my* last edit, not the ledger's; (b) its replay is
`O(all remaining actions)` — a real performance cliff once a ledger has
years of transactions, as the README states. A compensating action is
`O(1)` regardless of ledger age and needs no special undo API at all — it
is exactly `StoreTransaction` called with negated legs, which is also why
it required no new framework capability to design.

**Test**: `UndoTransaction` on a multi-currency, multi-leg journal
produces a reversal whose legs are the exact negation, re-passes the
zero-sum check per currency, and the resulting account balances match
their pre-transaction values exactly (not "close," via `Rational`
equality, not floating-point tolerance).

## 7. Rational overflow and the pre-decode gap (step 3 headroom test, step 1 invariant hardening)

**Property/fuzz test** (`tests/test_ledger_rational_fuzz.cpp` — ships in
`tests/`, not `examples/ledger/tests/`, because it exercises
`morph::math::Rational` itself, not ledger's model code; ledger's own
model tests separately assert the zero-sum invariant holds under the
model's real validation path). Generates sequences of `StoreTransaction`-
shaped leg sets at ledger-realistic magnitudes (dp 2 currencies up to
10^9 minor units, matching README's motivating case of "amount ×
exchange-rate with high-dp currencies") and asserts: (a) that
`Rational::operator+` is **scale-blind** — two legs at different
`decimalPlaces` are summed on their numerators alone, so a USD leg at dp=2
and one at dp=4 net to canonical zero whenever their numerators cancel, no
matter what money they denote. That is a property of `Rational`, not a
guarantee about the invariant: this test cannot say anything about false
accepts or false rejects, because it never reaches the model. What makes
the invariant sound is §1's restatement step, which restates every leg onto its
account currency's scale *before* summing, and the false-accept and
false-reject cases are pinned where the model can actually be driven, in
`examples/ledger/tests/test_ledger_model.cpp`. A leg at dp=4 in a USD
account stays legal, but only when it carries no digit below a cent
(`{45000, dp 4}` restates to `{450, dp 2}`; `{45001, dp 4}` is rejected).
And (b) *documents* — as a comment plus this section, once
measured, not asserted defensively in production code — the row count and
per-leg magnitude at which an intermediate cross-term (the multiplication
inside `amount × exchangeRate` that a foreign-amount pair's rate
computation would perform, if this rung computed rather than stored rates)
would overflow before any final result does. Per the README: **this is
expected to motivate a checked-arithmetic mode as a probable framework
gap**, filed as `docs/findings/NNN-rational-checked-arithmetic-mode.md`
once the fuzz test's actual overflow boundary is measured (a finding
without a measured boundary is not yet understood, per `FINDINGS.md`'s own
definition — this spec does not pre-file the finding with a guessed
number).

**Pre-decode validation gap** (README's own strain point, re-verified
against `Rational`'s actual wire codec rather than assumed): `setWire`
clamps a hostile `{"num":5,"den":0,"dp":2}` to a plausible `5/1` rather
than rejecting it — confirmed in `include/morph/util/rational.hpp`'s
codec. **Decision**: `StoreTransaction::validate()` cannot catch a clamped
leg amount as anything other than a plausible value; the only thing that
*does* catch it is the model's own zero-sum invariant (§1) — a clamped leg
is exceedingly unlikely to still sum to zero across its partition, so the
existing invariant is incidental protection, not designed protection. This
rung does **not** build an app-level echo-check scaffold on top (the
README names this as an option) — the zero-sum invariant already exists
for a real business reason and its incidental catch of clamped input is
sufficient; building a redundant validation layer whose only job is
"notice `Rational` clamps silently" would be exactly the kind of app-code
workaround `IMPLEMENTATION.md`'s prime directive calls a defect. Instead:
**file the pre-decode validation seam as a named framework finding**
(`docs/findings/NNN-rational-no-predecode-validation-seam.md`, disposition
left to the repo owner's triage per `FINDINGS.md`) with a test proving the
clamp-then-incidentally-caught path, so the gap is on record rather than
silently absorbed by an invariant that happens to catch it this time.

**The no-float rule, and where rendering happens.** No money value becomes a
`double` anywhere on the path from database to screen. The bridges still
publish each amount's exact `numerator`/`denominator`/`decimalPlaces`, so a
view that wants to format differently can, but they also publish the
**rendered text** (`balanceText`, `limitText`, `spentText`, `amountText`) and
that is what every QML label binds. The rendering is
`ledger::formatMoney(currency, amount)`
(`examples/ledger/include/ledger/core/money.hpp`): it recovers the decimal
value from the minor-unit count, wraps it in `Money<C>` for the currency —
the one place in this rung where the currency *is* known at compile time, and
therefore the one place `morph::units` can type money at all — and lets
`morph::units::toDecimalString` produce the digits by exact integer long
division.

Formatting in the view was the earlier design, and it was wrong: QML has only
IEEE doubles, so `numerator / denominator / Math.pow(10, places)`
re-introduced in the last three lines of the path exactly the imprecision
`Rational` exists to remove, and drifted for balances past 2^53 while the
payload beneath it stayed exact. `toDecimalString` renders shortest-form
(`$4.50` as `"4.5"`); that is a `Quantity` gap recorded in the rung README,
not a reason to hand-roll a second formatter.

## 8. CSV/OFX import with dedup (step 6)

**Decision**: generalizes the same op-id + applied-ops-ledger pattern
kanban generalized from `bookmarks::ImportBookmarks`
(`examples/bookmarks/include/bookmarks/dto/import_export_dto.hpp`,
`ImportOpId`/`ImportedOpRecord`), at chunk granularity — `ImportLedgerChunk
{ csvChunk: std::string, opId: ImportOpId }` (reusing bookmarks' own
`ImportOpId` type/shape rather than minting a parallel one, since the
contract — `hasValue()`, opaque per-chunk client identity — is identical
across rungs; if a third rung needs it after ledger, `IMPLEMENTATION.md`'s
rule-of-three promotes it into `include/morph` per that rule's own
threshold, tracked as a finding at that point, not before).

**Content-hash dedup, distinct from the chunk-level opId**: a *chunk* retry
(same client, same connection drop) is caught by `opId` exactly like
bookmarks. A **re-import of the same statement** (different `opId` per
chunk, since it's a new client-initiated import run, but the same
underlying rows) is a different problem — the README calls for "duplicate
detection across re-imports." Decision: each imported transaction row
computes a content hash (description + date + legs, canonicalized) and a
`ledger_imported_txn_hashes(ledgerId, hash)` unique index rejects (skips,
reports as "duplicate" in the result DTO, does not throw) a row whose hash
already exists for that ledger — this is the layer that answers "I
re-uploaded January's statement by mistake," which `opId` alone cannot,
since `opId` only defends one call's own retries.

## 9. Reports: submit→poll and snapshot semantics (step 7)

**Decision — the submit→poll shape**: `SubmitReport(ledgerId, kind,
params)` returns a `ReportJobId` immediately (no synchronous computation);
`GetReportStatus(jobId)` polls `{ status: Pending | Done | Failed, result:
optional<ReportResult> }`. The job runs off the model's own strand (a
worker-pool task, following the ladder-wide "background jobs" pattern from
LADDER.md's six recurring strains — the internal-client-with-service-
principal seam that rung 2 establishes and this rung consumes, not a new
mechanism).

**Snapshot semantics, specified precisely per the README's own demand**:
the job opens a **SQLite WAL read transaction** at submit time (not at
job-start time, which could be measurably later if the worker pool is
busy) and runs its entire aggregation against that transaction's
consistent view. This is the sanctioned-escape-tier raw-query use
`IMPLEMENTATION.md` rule 4 pre-enumerates by name ("WAL-read-transaction
snapshot pinning (ledger reports)") — a `DataMapper`-level `Query<T>`
cannot pin a snapshot across multiple queries, so this rung's report job
is one of the pre-cleared cases for Lightweight's raw-query facility,
invoked from inside `LedgerModel`, with the finding entry already
pre-filed by that rule (no new finding needed here — the rule text itself
is the disposition). **The byte-identical-on-rerun DoD bullet is only
meaningful against this snapshot**: re-running `GetReportStatus` after the
same job (not a fresh `SubmitReport`) must reproduce the exact same bytes,
since the job computed once against a pinned view and cached its result;
a *second* `SubmitReport` for the same period, issued after new
transactions landed, is legitimately allowed to differ — the DoD is about
one job's own idempotent result retrieval, not the report being frozen in
time forever.

**Local-time month boundary vs. UTC storage** (README's own strain
point): `TransactionJournalRecord.date` is stored as UTC
(`morph::time::Timestamp`, consistent with every other timestamp in the
ladder). A monthly report's boundary ("all transactions in local March")
is a presenter/report-parameter concern: `SubmitReport`'s `params` carries
the caller's timezone offset (or a named IANA zone, if
`morph::time`/vendored zone data supports it — grounded at implementation
time, not assumed here) explicitly, and the job converts the local month
boundary to a UTC range *once*, at submit time, before opening the
snapshot — never storing local-time boundaries and never comparing
local-time strings against UTC-stored rows row-by-row. The dual-mode GUI
test asserts a transaction at 23:30 local time lands in the report for its
local month even when that crosses a UTC day/month boundary.

## 10. Sync-philosophy benchmark (step 8 — written deliverable)

Per the README, this is prose plus two reproduced scenarios as tests, not
new model surface. Produced as `examples/ledger/SYNC-BENCHMARK.md`
(sibling to the README, following the pattern of a rung having exactly one
README plus named companion docs — same shape as this spec file being a
sibling of `examples/kanban/README.md`), containing:

1. **Scenario A (Actual-style)**: two offline `LedgerModel` clients edit
   different fields of the *same* transaction while disconnected (e.g.
   client 1 edits the description, client 2 edits a leg's category-linked
   budget) via `SqliteOfflineQueue`; both reconnect. Reproduced as an
   offline-stack integration test (`tests/test_ledger_offline.cpp`, the
   ladder's `offline_rig.hpp` pattern from kanban). Documented outcome:
   morph's action-level replay applies both queued actions in **server
   arrival order** — whichever client's queued action reaches the server
   first wins entirely for any field both actions touched, and the loser's
   action either reapplies cleanly (non-overlapping fields) or fails
   validation against the now-changed state (overlapping fields, surfaced
   through `onBackendChanged` reconciliation per kanban's own precedent).
   This is coarser than Actual's field-level CRDT merge (which would keep
   *both* edits, one per field, unconditionally) but intent-preserving:
   morph's replayed action is the *whole edit the user actually made*, not
   a field-diff a CRDT reconstructed after the fact.
2. **Scenario B (ODK-style base-version conflict)**: two clients fetch the
   same journal, one commits a change (bumping an implicit base version —
   the journal's own row-version/last-modified), the second's queued edit
   arrives with a stale base and is rejected outright rather than merged.
   Reproduced as a second offline test asserting the explicit rejection
   (not a silent overwrite, not a merge) and the typed error the second
   client's presenter surfaces.
3. **The clock-skew test**: two clients with injected `TokenVerifier`-clock
   skew of ±5 minutes both write to one ledger; the activity/audit view
   orders strictly by journal (server arrival) order and labels each
   entry's client-supplied timestamp as **claimed, not authoritative** —
   a dedicated presenter test asserts the audit view's display never uses
   the claimed timestamp for ordering, only for display.
4. **The explicit statement, stated once and not hedged elsewhere in this
   spec**: morph's ordering authority is server arrival order, full stop —
   no hybrid-logical-clock, no vector clock, no per-field merge. This is
   coarser-grained than Actual's CRDT approach and cannot express "keep
   both edits" automatically; it is finer-grained and more auditable than
   a last-write-wins-on-the-whole-row approach, because the *unit* of
   conflict is one action (one user's one logical edit), not one field or
   one row. The write-up states this trade-off plainly as the rung's
   answer, not as an unresolved gap.

## 11. Empty-principal writes (README strain point, cross-rung convention)

Per LADDER.md's binding "known limits" list, a token expiring between
`authorize` and `authenticate` dispatches with an empty principal, and
rungs 5–6 are named as the ones that must refuse this at the model.
**Decision**: every mutating action in `LedgerModel`/`BudgetModel`/
`RuleModel` checks `context.principal.hasValue()` (or the framework's
equivalent non-empty check) as the *first* statement in `execute()`,
before any business validation, throwing a typed `EmptyPrincipalError`
(this rung's `core/errors.hpp`, alongside `ZeroSumViolation` etc.) — never
silently proceeding with an empty principal on a financial mutation. Test:
deterministic via the injectable `TokenVerifier` clock (per the README),
asserting no successful mutating journal entry ever carries an empty
`principal` field.

## 12. Testkit and CI

No new testkit component is needed — `client_pool.hpp`, `convergence.hpp`,
`action_driver.hpp`, `offline_rig.hpp`, `db_busy_fixture.hpp` all predate
this rung (per `TESTING.md`'s ownership table) and are reused as-is; the
first four exist only on `ladder-kanban-impl` as of this writing and
reached this branch via §5's four cherry-picks (`db_busy_fixture.hpp`
predates rung 4 itself and is already on `master`).
`action_driver.hpp`'s per-burst invariant hook for this rung is "legs sum
zero" (already named in `TESTING.md`). Test files follow the naming
convention: `test_ledger_model.cpp`, `test_budget_model.cpp`,
`test_rule_model.cpp`, `test_ledger_offline.cpp`, `test_ledger_import.cpp`,
`test_ledger_reports.cpp`, `test_multiclient.cpp [stress]`, plus the
framework-level `tests/test_ledger_rational_fuzz.cpp` (§7). Model coverage
gate (100%, measured-ceiling per `IMPLEMENTATION.md` rule 5) scoped to
`examples/ledger/src/models/` + `include/ledger/models/`, wired into
`codecov.yml` alongside kanban's existing component once #121 merges (this
branch adds its own component entry independently; a merge conflict there
is expected and mechanical — two new named paths, not two changes to the
same line).

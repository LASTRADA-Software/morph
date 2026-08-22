# SDD ledger — plan: docs/superpowers/plans/2026-08-19-ledger-rung5.md

> Snapshot committed to the repo as a point-in-time record of Tasks 1–16
> (of 26) — the working copy at `.superpowers/sdd/` (git-ignored) may have
> moved further ahead by the time you read this; treat the plan document's
> own checkbox state as the current source of truth for what's done.

Spec: docs/superpowers/specs/2026-08-19-ledger-rung5-design.md (read in full).
Branch: ladder-ledger-rung5 (cut from master; NOT master itself — isolated
workspace requirement satisfied by this branch, confirmed with the user
earlier in this session).

## Pre-flight conflict scan

Task 0 (framework/testkit cherry-picks) is already complete — done directly
in the controller session before this SDD run started (verified building +
all-green on its own tests: journal tests green, 7 testkit test cases / 48
assertions green). Starting the task loop at Task 1.

Scan table — one row per pair of tasks sharing a file/interface, one row per
task's internal self-consistency:

| Tasks | Shared file/interface | Check | Finding |
|---|---|---|---|
| 2 → all | core/types.hpp, core/errors.hpp | Every later task's strong-id/error usage matches Task 2's exact names (AccountId, LedgerId, JournalId, RuleId, BudgetId, CategoryId, ReportJobId; ZeroSumViolation, EmptyPrincipalError) | Consistent throughout — verified during plan self-review (writing-plans skill pass) |
| 3 → 6,7,9 | core/units.hpp (Currency, UnitTraits) | Task 6's AccountInfo.balance field shape depends on resolving the Quantity<Unit,dp>-is-not-generic-over-runtime-currency tension Task 6 itself flags | Task 6 already documents the resolution inline (plain Rational + sibling Currency field, not Quantity<SpecificCurrency,dp>) — not a cross-task conflict, a within-task design note the implementer must apply consistently in Tasks 7-9 too. Ruling: carry this note explicitly into Task 7's dispatch since Task 7 defines TransactionLeg. |
| 7 → 8,9,14 | ledger_model.hpp/.cpp (LedgerModel) | Task 7 creates the class with OpenAccount/GetLedger only; Tasks 8/9/14 add StoreTransaction/foreign-amounts/UndoTransaction to the same file | Sequential modify-in-place, as the plan's File Structure table states explicitly (ledger_model.cpp listed against Tasks 7-9,12,14-16). No conflict — later tasks are additive to the same file, must not be dispatched out of order. |
| 11 → 7,8,10 | Empty-principal check placement | Task 11 retrofits the check into LedgerModel/BudgetModel's *existing* execute() overloads from Tasks 7/8/10 | Task 11 runs after 10, so all target overloads exist by then. No conflict. |
| 12 → 5 (cherry-pick) | causalParentId, isReplaying() | Task 12 is the only task that actually consumes the Task-0-cherry-picked journal fields | Confirmed present and tested on the branch already. No conflict. |
| 13 → 8 | ZeroSumViolation, Rational | Task 13's pre-decode-gap test asserts Task 8's zero-sum check catches a clamped leg | Requires Task 8's StoreTransaction to exist first — sequential, correct order in the plan. |
| 16 → 4 | ledger_report_jobs table | Task 16 (SubmitReport/GetReportStatus) needs Task 4's schema table already migrated | Task 4 precedes Task 16 in the plan. No conflict. |
| 18-21 → 7,10,12,16 | Presenter/bridge tasks each consume one model's action surface | Each presenter task's "Consumes" block names the exact prior task's DTOs | Verified consistent naming across Tasks 18-21 during the plan self-review. |
| 21 → common/gui/event_poller.hpp | ReportJobPoller vs EventPoller | Plan explicitly says NOT to force reuse of EventPoller's generic shape; write a distinct class | This is intentional per the design spec §9 discussion the plan cites — not a defect, a deliberate divergence. Ruling: no change; carry the "write a distinct class, do not template-reuse EventPoller" instruction into Task 21's dispatch verbatim, since it is easy for an implementer to over-apply DRY here. |
| 22 → 18-21 | gui/main.cpp wires all 4 bridges | Depends on all four QmlBridge classes existing | Sequential; Task 22 is after 18-21. No conflict. |
| 23 → Task 0 (action_driver/client_pool/convergence cherry-picks) | testkit consumption | Already on branch and verified | No conflict. |
| 24 → Task 0 (offline_rig cherry-pick), 17 | offline_rig.hpp, test_ledger_offline.cpp | Task 24 adds MORE tests to the same file Task 17 created | Sequential append, consistent with File Structure table (test_ledger_offline.cpp listed against both 17 and 24 implicitly via "Modify"). No conflict, but implementer of Task 24 must not overwrite Task 17's existing tests — dispatch note added. |
| 25 → all | Coverage gate, README reconciliation | Final wrap-up task, depends on everything else being complete | Correctly placed last among the "real" tasks (26 is the deferred post-merge task). |

**Plan-mandated pattern that could read as a review "defect" — pre-cleared:**
Several task briefs intentionally leave an implementation decision open
with "confirm exact signature/path against X before finalizing" (e.g.
Task 3's `UnitMeta` field names, Task 7's `Quantity` constructor, Task 17's
`Timestamp` factory name, Task 22's `AppContext` constructor). This is
because this plan was written without the ability to grep the exact
morph header signatures interactively for every framework type touched
across 25 tasks. **Ruling**: this is not a plan defect — it is a deliberate,
explicit instruction to the implementer to verify against the real header
before writing code, not a placeholder in the plan-authoring sense (the
plan gives a fully-reasoned best-grounded guess, not a blank). Task
reviewers should NOT flag "the plan doesn't know the exact signature" as a
spec gap; they SHOULD flag it if an implementer skipped the verification
step and shipped code that doesn't compile against the real header.

**Scan verdict**: clean. No blocking conflicts found. One dispatch-note
ruling recorded per row above where a task's own text needs an explicit
carry-forward note to the next task's brief (Task 6→7's amount-field
resolution, Task 21's EventPoller-non-reuse, Task 24 append-don't-overwrite).

Ruling: proceeding to Task 1.

## Task 1

BASE: 79713bc9d5287de3780e031f0b64d7849de56495

Dispatch 1 (sonnet): BLOCKED. Implementer's own `cmake --fresh` desynced
build/clangcl-release's CPM/FetchContent `_deps` subbuild graph (Catch2
specifically), on top of a generator/flag drift it partially self-repaired.
Left working tree with two untracked files (CMakeLists.txt, schema.cpp),
Step 3 (rung registration) and Step 5 (commit) never reached. Also flagged
an open question: no test files exist yet, is a placeholder test needed?

Controller investigation (environment repair, not a task-content fix):
verified a brand-new build dir configures cleanly (proving directory-local
corruption, not a project regression); deleted build/clangcl-release,
reconfigured via `cmake --preset clangcl-release` +
`-DMORPH_BUILD_LADDER=ON -DMORPH_BUILD_QT=ON`; verified morph_tests builds
+ links + all [journal] tests pass (82 assertions/28 cases, confirms
Task-0's causalParentId cherry-pick intact); verified ladder_ledger_lib
builds cleanly against the implementer's schema.cpp (the C++17-nested-
namespace diagnostic on `namespace ledger::db {}` is a stale IDE lint
against a pre-C++23 standard, not a real compiler error under this
project's actual C++23 config — confirmed by a clean link).
Traced cmake/morph_add_rung.cmake directly: ladder_<rung>_tests is only
created via add_executable when tests/*.cpp glob is non-empty — an empty
tests/ dir for one commit is the same shape every other rung bootstrapped
with. Ruling: no placeholder test needed; this was never a real blocker,
just an unanswered question in the original brief.

Un-staged the implementer's two files (controller must not commit task
content) and resumed the implementer with: the environment-repair
explanation, instruction to finish Step 3 (one-line rung registration,
exact list entry given) and re-verify Step 4 for real, the placeholder-test
ruling above (do not create one), and to self-review + commit per Step 5.

Dispatch 2 (sonnet, same task): DONE. Commit 8292c23.
Review (haiku): Spec compliant, no findings, Approved.

Task 1: complete (commits 79713bc..8292c23, review clean)

## Environment note (applies to every future task in this SDD run)

Qt-linked ladder test binaries (ladder_ledger_tests.exe etc.) need
C:\Qt\6.11.1\msvc2022_64\bin on PATH to load Qt6Core.dll, or they exit
immediately with no output (exit 127 in Git Bash, exit 57 in PowerShell —
neither is a real test failure, both are the Windows PE loader failing to
resolve the DLL). The controller's own verification runs must set this
PATH; implementer/reviewer subagents should be told the same if they need
to execute (not just build) a Qt-linked binary. Confirmed via: building
ladder_ledger_tests.exe cleanly, running it with the DLL path added
("All tests passed"), versus without it (silent non-zero exit).

## Task 2

BASE: 8292c23ea41c8296936416f299670e7b17424dce

Dispatch (haiku): DONE. Commit 325e9bc. Report claimed 4/4 tests passing.
Controller independently re-verified: `cmake --build` reports "no work to
do" (already compiled clean), and running the binary with Qt's DLL path
added confirms "All tests passed (10 assertions in 4 test cases)".

IDE/LSP diagnostics fired on the new files (stale-index "file not found"/
"no template optional" parse garbage, an operator<=> spacing pedantry, and
clang-tidy performance notes on the deliberate by-value-sink-then-move
error-constructor idiom) — controller investigated all of them directly
against the real build/compile output and confirmed none are real defects
(same false-positive class as Task 1's stale C++17-extension warning).
Pre-cleared for the task reviewer so it doesn't re-litigate them.

Review (haiku): Spec compliant, no findings, Approved.

Task 2: complete (commits 8292c23..325e9bc, review clean)

## Task 3

BASE: 325e9bcbb75eb361eabc87de7a90a8fd3d53f33b

Dispatch (sonnet): DONE. Commit daf54ae. Controller pre-verified UnitMeta's
real field names (id/display/defaultDecimals) and Rational/Quantity
constructor shapes directly against the headers before dispatch, so the
implementer transcribed rather than guessed. Test run confirmed by
implementer: "All tests passed (15 assertions in 8 test cases)" (full
suite, including prior tasks' tests).

Three deviations from the brief's literal text, all investigated and
ruled on by the controller:

1. Ruling: the brief's own test code calls `ledger::UnitTraits<Currency>`
   (plan bug — I wrote the test against a `ledger::`-qualified name but
   the implementation snippet specialized `morph::units::UnitTraits`
   directly, an inconsistency in the plan itself, not the implementer's
   invention). Implementer added a forwarding alias template
   `template<typename E> using UnitTraits = morph::units::UnitTraits<E>;`
   in `ledger::` to make both spellings resolve to the same type. Correct,
   minimal fix for a real plan defect — stands.
2. `default:` case added to the `meta()` switch — required by this repo's
   `-Weverything -Werror` policy on exhaustive-switch warnings. Sensible,
   matches the pattern elsewhere in the plan (e.g. AccountRecord::kind
   handling). No issue.
3. Ruling: `Money<C>` implemented as `template<Currency C> using Money =
   ::morph::units::Quantity<C>;` rather than the plan's literal
   `Quantity<Currency, 2>`. Verified directly against
   `include/morph/util/quantity.hpp` line 461:
   `template <auto U, ...> struct Quantity` — U is a concrete enumerator
   VALUE, not the enum type, so `Quantity<Currency, 2>` cannot compile
   (Currency is a type, not a value). The plan's own text was imprecise
   here; the implementer's alias is the only correct shape and matches
   the plan's own §2 discussion of this exact tension (Task 6's file
   structure note flagged this ahead of time). Stands as a plan
   correction, not a defect.

IDE/LSP diagnostics fired again on the new files (same stale-index
false-positive class as Tasks 1-2 — "file not found", "undeclared morph",
"explicit specialization of non-template struct" — all contradicted by
the real, verified test run). Pre-cleared for the reviewer.

Review (sonnet): Spec compliant, 0 Critical/Important, 2 Minor (doc-comment
splitting suggestions, units.hpp) — deferred, not fixed (per skill: Minor
findings never enter the fix loop).

Task 3: complete (commits 325e9bc..daf54ae, review clean)
Task 3: minor (deferred): units.hpp Money<C> doc comment could split into
  two @brief blocks for skimmability (cosmetic only)
Task 3: minor (deferred): units.hpp's template<> UnitTraits specialization
  block lacks a one-line comment explaining why it must sit outside
  namespace ledger (C++ specialization scoping rule) — matches sibling
  rungs' own layout exactly, just under-commented

## Task 4

BASE: daf54ae24d6c133570b23e8f4c1e1fd601903ee9

Ruling (pre-dispatch, plan defect found during my own pre-verification):
the plan's original Task 4 was doubly wrong — (1) `ledger::db::setup()`
took no `connectionString` parameter, contradicting the real
bank/polls-established convention `setup(const std::string&)`; (2) the
plan's own test called `ledger::db::setup()` directly, but
`polls::db::database.hpp`'s own doc comment states outright "tests never
call this" — the real pattern is `morph::ladder::testkit::DbFixture`,
which configures its own connection and applies migrations independently.
Also rewrote Task 4's migration DDL from prose bullet points into real,
verified code: cross-checked every Lightweight::SqlMigration method
against bank/bookmarks/pastebin's actual schema.cpp files
(RequiredForeignKey/ForeignKey with SqlForeignKeyReferenceDefinition,
Column vs RequiredColumn for nullability, CreateUniqueIndex as a separate
plan call, NVarchar(0) as the unbounded-text convention — never Text()).
Edited the plan file directly (not just the generated brief) so this
correction is permanent for anyone re-reading the plan later, then
regenerated the brief from the corrected plan.

Dispatch (sonnet): DONE_WITH_CONCERNS. Commit 2edc381.
Controller independently re-verified: `ninja: no work to do` (already
compiled), direct binary run "All tests passed (26 assertions in 9 test
cases)", filtered `[ledger][db]` run "All tests passed (11 assertions in
1 test case)" — all 11 tables confirmed.

Ruling: implementer diagnosed a real local machine-cache bug (fastcache-cc
serving a stale empty object for schema.cpp.obj regardless of content,
reproduced directly bypassing ninja in two modes) and opted
`ladder_ledger_lib` out of the compiler-cache launcher as a minimal,
well-documented, reversible per-target CMakeLists.txt fix. Accepted as
sound engineering judgment for a genuine problem hit during the task —
not reverted, not treated as scope creep. Cost if this ruling is wrong:
negligible (slightly slower builds for one small target on machines
where the daemon actually works fine); benefit if right: prevents
silently linking a stale/corrupt object into a shipped binary.

Also noted: commit 8f5d58f (a plan-doc correction I made and applied to
the working-tree brief BEFORE dispatching Task 4) landed in git history
AFTER Task 4's own implementation commit — I forgot to commit the doc
edit until after dispatch. Purely cosmetic ordering; the brief the
implementer worked from already had the correction, so no functional
effect. Not fixed (would require rewriting published branch history for
a cosmetic-only issue).

Review (sonnet): Spec compliant, 0 Critical/Important, 2 Minor (missing
migration-numbering-convention comment; database.hpp doc comment density)
— deferred, both cosmetic.

Task 4: complete (commits daf54ae..2edc381, review clean)
Task 4: minor (deferred): schema.cpp lacks bank's own explicit
  "timestamps are monotonically increasing" convention comment (numbers
  are in fact correct/non-colliding, just undocumented as a rule)
Task 4: minor (deferred): database.hpp's file-level doc comment is denser
  than bank's equivalent (readability nit only)

## Task 5

BASE: 2edc3811109a43e6b9f3c8da41faa359db06aae4 (plan-doc commit 830c05e
sits on top but touches no source file this task creates)

Ruling (pre-dispatch): same setup()-in-test defect as Task 4, plus the
plan's original Task 5 left 9 of 11 entities as a "follow the same shape"
ellipsis. Rewrote with complete, real-API-verified code before dispatch:
Field<std::optional<T>,...> for nullable plain columns (confirmed via
Lightweight's StdOptional.hpp), BelongsTo assignment + Query<T>().Where()
.All() copied verbatim from polls' own real schema test. Flagged
ReportJobRecord::resultJson (nullable+unbounded) as the one field with no
existing precedent to copy — build-verify specifically, don't just trust.
Committed the plan correction (830c05e) before dispatch this time, fixing
the ordering slip from Task 4.

Dispatch (sonnet): DONE. Commit 39311a0. Controller independently
re-verified: real rebuild ("no work to do" initially, later a clean
incremental build after the comment fix below), direct binary run
"All tests passed (30 assertions in 10 test cases)".

Reviewer caught one real (if cosmetic) finding: Task 4's schema.cpp
comment on `causal_parent_id` said "empty-string 'no parent' sentinel"
but the actual DDL is nullable and Task 5's entity wraps it as
`std::optional<SqlAnsiString<64>>` — comment was wrong, behavior was
right. Fixed directly (commit dc08131), re-verified build+tests green
(30/10) after the fix.

Review (sonnet): Spec compliant, 0 Critical/Important, 2 Minor
(AccountRecord::kind missing a `{0}` default init for consistency with
sibling int columns; the causal_parent_id comment issue above, now fixed)
— AccountRecord::kind deferred as cosmetic-only.

Task 5: complete (commits 830c05e..dc08131, review clean + 1 fix applied)
Task 5: minor (deferred): AccountRecord::kind lacks a `{0}` default
  initializer, unlike every sibling int-typed column in the same file
  (cosmetic only — the test explicitly sets it before Create)

## Task 6

BASE: 16c0955d89338aa4976e66135b6e740e136ebf19

Ruling (pre-dispatch): fixed the same Quantity<SpecificCurrency,dp>
tension the plan itself flagged but left unresolved with two options --
`AccountInfo::balance` is now a plain `morph::math::Rational` alongside
the sibling `currency` field, matching Task 3's `Money<C>` precedent and
design spec §2's own stated answer. Verified `morph::forms::
allRequiredEngaged`'s real signature and confirmed `LedgerId`'s
`hasValue() const noexcept -> bool` satisfies `EmptyCapableField`'s
concept exactly, so `GetLedger::validate()`'s body compiles as written.

Dispatch (haiku): DONE. Commit 4ef192f. Controller independently
re-verified: real rebuild ("no work to do"), direct binary run "All tests
passed (33 assertions in 13 test cases)".

Review (haiku): Spec compliant, no findings, Approved.

Task 6: complete (commits 16c0955..4ef192f, review clean)

## Bulk fix: recurring `ledger::db::setup()` plan defect

Found while pre-verifying Task 7: the same `ledger::db::setup()`-called-
directly-in-a-test error (already fixed in Tasks 4/5's own text) recurs
13 more times across Tasks 7-16's model/rule/import/report test snippets
— every one of these was drafted before I caught and fixed the pattern
in Task 4, and I never went back to sweep the rest of the plan. Doing a
single bulk pass now rather than re-discovering and re-fixing this once
per task for the next 10 tasks.

## Task 7

BASE: ee317b90c7c443facdaae87e957a5e0b485d126f

Ruling (pre-dispatch, significant plan defect): the plan assumed a keyed
model takes its key as a constructor argument (LedgerModel model
{LedgerId{1}}) and that BRIDGE_KEY_FROM applies to the model's first
keyed action too. Both wrong, verified against polls::PollModel's real
class (plain default-constructible, `PollModel model;`, no key arg
anywhere) and model_key.hpp's real macro definitions: BRIDGE_MODEL_KEY
is used exactly once (establishes ModelKeyTraits<M> as a side effect;
using BRIDGE_KEY_FROM there instead fails to compile), BRIDGE_KEY_FROM
for every other action sharing the same key type. LedgerModel needs no
private caching member at all (every ledger action carries its own
ledgerId explicitly, unlike polls::GetPollState's reliance on PollModel's
private _pollId). Also fixed a genuine bug I introduced: execute
(OpenAccount)'s body fabricated a stub LedgerRecord for a BelongsTo
assignment instead of querying the real persisted parent row (BelongsTo
assignment needs an actual round-tripped record); added a missing
ledger/core/errors.hpp include; and resolved that ledger provisioning
(no CreateLedger action in this rung's scope) means the test itself
seeds a ledgers row, not execute(OpenAccount).

This is the deepest plan-defect this task's SDD run has hit so far --
worth flagging in the final rulings list.

Dispatch (sonnet): DONE. Commit 527d794. Controller independently
re-verified: real rebuild ("no work to do"), direct binary run "All
tests passed (35 assertions in 14 test cases)".

Review (opus, given genuine technical depth -- ModelKeyTraits/
ActionKeyTraits hand-written specializations): all four of the
implementer's brief-blind discoveries independently CONFIRMED REAL
against the actual framework headers (model_key.hpp, registry.hpp,
bookmarks::BookmarkId's glz::meta precedent) -- not rubber-stamped.
2 Important findings: (1) OpenAccount's forced-into-existence return
value never asserted by any test; (2) AccountInfo::balance hardcoded to
DecimalPlaces{2} regardless of account currency (wrong for JPY/KRW).
4 Minor findings (codeToCurrency/currencyToCode silent-default-to-USD
behavior x2, unused <vector> include, thread-safety confirmed clean).

Fix round 1/5 dispatched for the 2 Important findings (fresh implementer,
same reasoning as resuming -- carried full context). DONE. Commit 2b9be32.
Controller re-verified independently: rebuild clean, "All tests passed
(36 assertions in 14 test cases)".

Re-review (haiku) dispatched, in progress.

Re-review (haiku): both findings ADDRESSED, no new breakage. Fix round
1/5 (2 addressed, 0 open; commits 527d794..2b9be32).

Task 7: complete (commits ab7ab86..2b9be32, fix round 1/5, review clean)
Task 7: minor (deferred): codeToCurrency silently defaults an unknown
  currency code to USD with no trace (read-path decode of the model's
  own prior write, sound but invisible on corruption)
Task 7: minor (deferred): currencyToCode's unreachable default: also
  returns "USD" rather than a detectable sentinel like "???" (write-path,
  worse than the read-path case above since it corrupts data going in)
Task 7: minor (deferred): transaction_dto.hpp includes <vector> unused
  at this task's scope (Task 8 will use it)

## Framework convention discovered while pre-verifying Task 8 (binding for later tasks)

examples/common/clock.hpp: every ladder rung's SERVER-STAMPED timestamp
(an audit "when did the server record this" field, e.g.
ImportedOpRecord::appliedAtMs, ReportJobRecord::createdAtMs) must read
morph::ladder::now(), never Timestamp::now()/DateTime::now() directly --
LADDER.md's framework prerequisite 3, examples/common/clock.hpp's own
file comment states this as a binding ladder-wide convention with a
ScopedClockOverride test seam. This does NOT apply to StoreTransaction's
own `date` field (a genuine client-supplied "when did this purchase
happen" value per design spec §1, distinct from a server audit stamp) --
Task 8's use of Timestamp::now() in its own test is a test constructing
a client-supplied value, not a server-stamped one, so it's correct as
written. Ruling: no fix needed for Task 8; this convention must be
applied when writing Tasks 15 (import, appliedAtMs) and 16 (reports,
createdAtMs) — noted now so it isn't missed later. Also verified the
real DateTime->epoch-millis conversion idiom for Task 8's own
TransactionJournalRecord.date storage:
`(*timestamp.value).value.time_since_epoch().count()`, copied verbatim
from bookmarks::db's own nowMs()/fromEpochMs() helpers
(bookmark_model.cpp:61-82) -- my plan's guessed `toEpochMillis()` method
name does not exist and needs fixing in Task 8's own body before
dispatch.

## Task 8 dispatch

BASE: 230aa1cccc1d6e27d488399563ce0137c9012345

Ruling (pre-dispatch): fixed two more real API guesses (SqlTransaction's
constructor, DateTime's epoch-millis conversion), both now verified
against bank::LoanModel/bookmarks::db real code. Confirmed
StoreTransaction's client-supplied date field is correctly exempt from
the morph::ladder::now() convention. Propagated Task 7's execute()-
cannot-return-void discovery into Task 10's LinkAccountToCategory/
SetBudgetLimit (already fixed pre-dispatch, see the earlier Task 10 note)
and added the real schema/entity addition LinkAccountToCategory needs.

Dispatch (sonnet): DONE. Commit e894c33. Controller independently
re-verified: real rebuild ("no work to do"), direct binary run "All
tests passed (42 assertions in 16 test cases)". One deviation: added
missing <Lightweight/SqlTransaction.hpp> include (verified against
bank::LoanModel's real includes).

Review (opus, given this is the rung's central invariant) dispatched,
in progress.

Review (opus): Spec compliant. Deep verification: traced actual Rational
arithmetic by hand (-5000+5000=0, -5000+4000=-1000!=0), confirmed
SqlTransaction's real ROLLBACK-destructor contract against SqlTransaction.cpp
source (not assumed), confirmed same-account-in-two-legs works correctly,
confirmed cross-precision (dp mixing) safety via widenPrecisionTo's real
behavior, confirmed execute(GetLedger) reads post-commit data (correct
ordering). 0 Critical/Important. 1 durable-correctness note (cross-ledger
leg not rejected -- out of this task's own scope, a later-rung concern,
not a defect in the invariant this task owns) + 3 Minor (uint64_t/int64_t
signedness inconsistency in a helper param; ZeroSumViolation's message
lacks the actual sum; a repeated comment).

Task 8: complete (commits 230aa1c..e894c33, review clean)
Task 8: minor (deferred): sumAccountLegs takes accountId as uint64_t
  while the rest of the chain is int64_t-backed (works, just inconsistent)
Task 8: minor (deferred): ZeroSumViolation's thrown message doesn't
  include the actual non-zero sum, only the currency + a generic string
Task 8: minor (deferred): "never a raw SQL SUM()" rationale restated in
  two nearby comments
Task 8: note (not a finding, informational): a StoreTransaction leg
  naming an account in a DIFFERENT ledger than action.ledgerId is not
  rejected -- the global zero-sum invariant still holds, so this isn't a
  defect in this task's own scope, but a later task should consider
  asserting ledger membership per leg

## Task 9

BASE: e894c33da4dd8b7a2458d23e01b5457186ed69cb

Dispatch (sonnet): DONE. Commit dbca676. Controller independently
re-verified: real rebuild ("no work to do"), direct binary run "All
tests passed (46 assertions in 17 test cases)". Minor additive includes
needed (units.hpp, <optional>), plus cleanup of two stray untracked
build-artifact files. Review dispatched, in progress.

Review (sonnet): Spec compliant. Critical check verified by tracing
control flow: foreign-amount/foreignCurrency fields confirmed never
read in the partitioning/sum loops, only touched after the zero-sum
check passes and the transaction has started committing. 0
Critical/Important. 1 Minor (repeated ternary pattern, non-blocking).

Task 9: complete (commits e894c33..dbca676, review clean)
Task 9: minor (deferred): foreign-amount persistence's 4 ternary
  assignments could be a small helper, but each maps a genuinely
  different field so duplication is only shape-level, not logic-level

## Task 10 dispatch

BASE: dbca6760b6307a82e5f20396ceb0b203b0a38322

Substantial task (new DTOs, new model, ALTER TABLE migration, real
budget-report leg summation left for the implementer to complete per the
brief's own flagged remaining work). Dispatched to sonnet given scope.

## Task 11 pre-verification (done while Task 10 was in flight)

Ruling: same class of plan defect as Tasks 4/5/7 -- guessed
Context::principal as std::optional/.hasValue() when it's really a
plain std::string (empty = unauthenticated), and morph::session::
current() returns const Context* (nullptr outside dispatch), not an
optional-wrapping accessor. Real test mechanism is
morph::session::detail::ScopedContext, copied from bookmarks' own real
ScopedPrincipal helper. Also added the missing BudgetModel-side test
(brief scoped BudgetModel in but only had a LedgerModel test) and
extended the fix to every mutating execute(), not just StoreTransaction.
Committed (d6ab9cf) before Task 10 finished so this correction is ready
whenever Task 11 is dispatched.

## Task 10 outcome

Dispatch (sonnet): DONE. Commit 31f267a. Controller independently
re-verified: real rebuild ("no work to do"), direct binary run "All
tests passed (48 assertions in 18 test cases)". Substantial commit
(473 lines): real ALTER TABLE migration, CategoryRecord/AccountRecord
reorder for complete-type BelongsTo requirement, real std::chrono
date-range parsing + WhereIn join for spent computation.
Review dispatched to opus given the date-arithmetic + schema-migration
risk surface, in progress.

Review (opus): Spec compliant on all 5 checks, with hand-executed
verification (compiled and ran monthRangeMs standalone to confirm real
UTC month boundaries, leap-year Feb 29 handling, half-open range
correctness). LinkAccountToCategory's hasKey=false claim CONFIRMED
against the real primary-template default. 0 Critical. 3 Important:
(1) the date-range filter itself is untested -- deleting the date Where
clauses would leave the suite green; (2) journalIds query has no
ledger_id filter, collecting every journal across ALL ledgers in that
month, unbounded IN-list growth risk; (3) monthRangeMs silently accepts
malformed months ("2026-13") producing a 255-day garbage range instead
of validating. 3 Minor (limit defaults to spent not zero when unset;
spent silently mixes currencies across accounts; hardcoded dp=2 not
derived from account currency).

Fix round 1/5 needed for the 3 Important findings before Task 10 can be
marked complete.

Fix round 1/5: DONE. Commit ea2e61e. Controller independently
re-verified: rebuild clean, "All tests passed (50 assertions in 19 test
cases)". Re-review dispatched, in progress.

## Task 11a inserted into the plan (user-approved, per the earlier
## journaling-retrofit question)

Task 11a written and committed (02772d5): LedgerModel/BudgetModel gain
attachActionLog()/logAction(), copied from kanban's real, verified
pattern (ladder-kanban-impl's unmerged board_model.{hpp,cpp}). No
renumbering of Tasks 12-25 -- inserted as a lettered task per the user's
own preference to avoid touching every later task's cross-references.

Re-review (sonnet): all 3 findings ADDRESSED with concrete evidence
(traced the arithmetic showing the out-of-month leg would change the
asserted total if wrongly included; confirmed the ledger_id filter uses
the budget's own ledger field; confirmed validation runs at the DTO
boundary before monthRangeMs, plus defense-in-depth inside it). No new
breakage. Fix round 1/5 (3 addressed, 0 open; commits 31f267a..ea2e61e).

Task 10: complete (commits dbca676..ea2e61e, fix round 1/5, review clean)
Task 10: minor (deferred): limit defaults to spent (not zero) when no
  SetBudgetLimit exists for the month, reading as "always exactly at
  limit" rather than "no limit set"
Task 10: minor (deferred): spent silently sums across differing
  currencies if a category links accounts of more than one currency
  (invisible with this test's USD-only setup)
Task 10: minor (deferred): spent's Rational seeds at hardcoded dp=2
  rather than deriving from the accounts' own currency (harmless for
  USD, inconsistent with LedgerModel's own UnitTraits-derived pattern)

## Sequencing note (controller error, corrected)

Dispatched Task 11 before Task 11a's actual implementation (only Task
11a's plan TEXT was committed, not its code) -- a real ordering mistake.
Task 11's own requirement (empty-principal check as execute()'s first
statement) has no hard dependency on Task 11a's logAction existing, so
letting Task 11 proceed is harmless (its own dispatch brief's caveat
about "not disturbing existing logAction call sites" is simply moot
since there's nothing there yet). Ruling: proceed with Task 11 now,
dispatch Task 11a immediately after -- Task 11a's own retrofit will
insert logAction calls AFTER Task 11's empty-principal checks in
execute() bodies, which is still the correct relative order (principal
check first, then business logic, then journaling last) regardless of
which task's commit adds which line.

## Task 11 outcome

Dispatch (sonnet): DONE. Commit 44ad762. Controller independently
re-verified: real rebuild ("no work to do"), direct binary run "All
tests passed (52 assertions in 21 test cases)". Correctly identified
and reported the Task 11a sequencing gap itself (no actual blocker).
Wrapped every pre-existing mutating test call with ScopedPrincipal
(necessary consequence, following bookmarks' own established pattern).
Review dispatched (security-relevant check), in progress.

Review (sonnet): Spec compliant. All 6 mutating execute() overloads
verified individually (file:line each) to carry the exact check as the
genuinely first statement, before validate(). Both read-only methods
confirmed exempt. 0 Critical/Important. 2 Minor (duplicated check body
across 6 sites -- brief explicitly permits either approach; a non-const
ScopedPrincipal in the two new tests vs const elsewhere).

Task 11: complete (commits 02772d5..44ad762, review clean)
Task 11: minor (deferred): empty-principal check duplicated verbatim 6x
  rather than factored into a shared helper (brief permits either)
Task 11: minor (deferred): ScopedPrincipal empty{""} is non-const in the
  two new tests, inconsistent with const elsewhere (no functional impact)

## Task 11a dispatch

BASE: 44ad762a4382e8e22cb3287ebc3be5c40c2b8278

Dispatched with kanban's real, verified attachActionLog/logAction
pattern. Flagged the template-instantiation-in-.cpp mechanics as a real
open risk for the implementer to resolve if hit.

## Task 11a outcome

Dispatch (sonnet): DONE. Commit 87da87b. Controller independently
re-verified: real rebuild ("no work to do"), direct binary run "All
tests passed (60 assertions in 23 test cases)". No template
instantiation issue hit (logAction's callers all in the same TU as its
definition, matching kanban's real pattern). Necessary deviation:
ScopedPrincipal added to the two new journal tests since Task 11's
empty-principal check would otherwise fire first. Review dispatched.

Review (sonnet): Spec compliant, all 6 mutating execute() methods
verified individually (file:line each). Independently confirmed the
morph::ladder::now() timestamp idiom byte-for-byte against 4 other real
examples in the repo (bookmarks x2, pastebin, polls). 0 Critical/
Important. 2 Minor (stylistic optional<string> vs plain string; implicit
default LogEntry::error).

Task 11a: complete (commits 44ad762..87da87b, review clean)
Task 11a: minor (deferred): _entityKeyStr as optional<string> vs plain
  string (harmless, _log's shared_ptr is the real "attached" signal)

## Task 11b inserted (user-approved, discovered pre-verifying Task 12)

Ruling: StoreTransaction is a pure insert (unlike kanban's naturally-
idempotent MoveTaskPosition), so morph::journal::replay() would
double-insert it. Added Task 11b: opId + applied-ops ledger on
StoreTransaction, copying kanban's real, verified lookup-before-mutate/
write-after-commit pattern exactly. Backward-compatible by construction
(opId defaults to disengaged; existing Task 8/9 StoreTransaction{...}
calls with no .opId still compile and take the ordinary-insert path
unchanged -- verified this reasoning directly, no fix needed to already-
shipped Tasks 8/9 code).

Also fully corrected Task 12 itself while investigating: RuleModel's
stale constructor/principal-check pattern; the causal-parent-id minting
mechanism (previously "resolve the exact mechanism", now copied
verbatim from kanban's real evaluateRules -- mint from
TransactionJournalRecord's own autoincrement id, call the cascade's
impl function directly bypassing any public execute() to avoid double-
logging); SetCategory's registration requirement (verified against
morph::journal::replay()'s real dispatcher, which requires the action
type registered regardless of who created the entry -- confirmed via
kanban's own ApplyTagMutation); the "which account/which category"
design questions (resolved concretely: first Expense/Revenue leg;
lookup-never-auto-create); and the divergence test itself (previously
comments-only, now real code including IModelHolder::into<Model>(),
copied from kanban's own real divergence test).

Committed: baacdaf.

## Task 11b: StoreTransaction exactly-once via opId + applied-ops ledger

- Implementer: DONE. Commit `2765491` — ImportOpId (new file), opId field on
  StoreTransaction, AppliedOpRecord entity + `ledger_applied_ops` migration
  (20260819000013, unique index on (ledger_id, op_id)), lookup-before-mutate
  gated on `action.opId.hasValue()`, write-after-mutate-before-commit inside
  the same SqlTransaction, new `buildLedgerState` helper shared by
  execute(GetLedger) and execute(StoreTransaction) so the applied-ops
  resultJson and the returned result are the same value on the same
  in-flight mapper/transaction. New test: repeated opId is a safe no-op.
- Two self-reported deviations, both reviewed and accepted: (1) test
  designated-initializer field order fixed to match declaration order
  (real -Werror failure, cosmetic); (2) buildLedgerState extraction, not in
  the brief's literal diff but load-bearing for atomicity — ruled
  in-scope.
- Controller-independent verification: `cmake --build build/clangcl-release
  --target ladder_ledger_tests` (no-op, already current) +
  `ladder_ledger_tests.exe` direct run — 100% pass, 62 assertions / 24 test
  cases, 0 failures. Confirms Task 8/9's pre-existing StoreTransaction
  tests (no `.opId` set) are unaffected.
- Task reviewer (agent a4c5bba2d6dd1ddbb): spec-compliance PASS,
  code-quality PASS, zero findings (Critical/Important/Minor).

Task 11b: complete

## Task 12: RuleModel + cascade-journaling (causal parent-id)

- Implementer: DONE. Commit `4a30f10` — RuleModel (CreateRule/UpdateRule,
  plain default-constructible, hand-written ModelKeyTraits/ActionKeyTraits
  keyed by LedgerId, empty-principal check, UpdateRule bumps
  RuleRecord.version), execute(StoreTransaction) gains a post-mutation,
  pre-commit rule-evaluation cascade: on a RuleTrigger::DescriptionContains
  match against a category that exists (lookup-never-auto-create),
  setCategoryImpl(mapper, cascadeAction) runs atomically inside the same
  SqlTransaction; causalParentId minted as
  "transactionJournal:<journalRow.id>" (a real, already-populated
  auto-increment id, not LogEntry::seq); cascade logAction calls deferred
  until after the trigger's own logAction so seq order is always
  trigger-then-cascade. Cascade block gated on !isReplaying() and runs
  after Task 11b's opId early-return, so a replay hit or a journal replay
  never re-evaluates rules or double-fires SetCategory.
- Three self-reported deviations from the brief's literal snippets, all
  independently re-verified by the reviewer against actual code (not
  taken on the implementer's word): (1) deferred cascade logging order,
  required by the brief's own Step 6 seq-order test; (2) setCategoryImpl
  takes the mapper by reference for atomic same-transaction commit;
  (3) several real brief-snippet bugs fixed (missing includes,
  nonexistent BelongsTo::hasValue(), invalid ToStringView().data(),
  missing CreateCategory call in the Step 6 test fixture).
- Controller-independent verification: `cmake --build build/clangcl-release
  --target ladder_ledger_tests` (no-op, already current) +
  `ladder_ledger_tests.exe` direct run — 100% pass, 79 assertions / 28 test
  cases, 0 failures (matches implementer's own report exactly).
- Task reviewer (agent aeeca3e1739655c13): spec-compliance PASS,
  code-quality PASS. Independently confirmed (not just re-stated from the
  report): DataMapper::Create synchronously populates the auto-increment
  id before it's read for the causal-parent-id; isReplaying() gating has
  no gap; opId early-return is positioned before the cascade block;
  execute(SetCategory) and the cascade path never call each other, each
  logs exactly once; legAccounts is genuinely positionally aligned with
  action.legs; the divergence test (Step 10) actually proves replay
  reproduces the pinned rule version's outcome, not the edited one.
- Two Minor, non-blocking notes (parked, no fix needed): (M1)
  TransactionJournalRecord.causal_parent_id DB column (pre-existing,
  predates this task) stays unpopulated -- the causal link lives in
  LogEntry::causalParentId correctly, this raw column is just unused;
  (M2) rule.actionValue (SqlAnsiString<256>) vs CategoryRecord::name
  (SqlAnsiString<128>) cross-width string Where-comparison has no other
  precedent in the codebase to check against, but compiles clean under
  -Weverything -Werror and is verified correct at runtime by the passing
  cascade test.
- Ruling: both Minor notes are non-load-bearing and out of this task's
  scope -- parked as-is, no fix dispatched.

Task 12: complete

## Task 13: Rational overflow fuzz test + pre-decode-gap finding

- Implementer (haiku): DONE_WITH_CONCERNS. Commit `1948410`.
- MAJOR DETOUR: controller investigation of an apparent test failure
  (-5000+5000 reporting 495000) led through a long, fully-resolved
  false-lead chain: (1) ruled out ledger/Rational code defect via a
  standalone isolated repro (passed); (2) ruled out lld-link ICF via a
  controlled A/B relink with identical objects, one with /OPT:NOICF one
  without -- both produced the SAME wrong result, disproving ICF as the
  cause (an earlier CMakeLists.txt /OPT:NOICF change was added then fully
  reverted once disproven); (3) root-caused via debug-print injection
  (prints never appeared in the executed binary despite a real,
  content-changing source edit) to a stale/corrupted cache entry in the
  third-party, machine-local `fastcache-cc`/`fastcached` compiler-cache
  daemon (D:\caching\, external to this repo) silently serving old
  object bytes and reporting fake compile success. Confirmed
  conclusively: with FASTCACHE_ADDR cleared and a full clean rebuild,
  morph_tests passes 1077/1077 test cases (20,120/20,120 assertions) and
  ladder_ledger_tests passes 29/29 (80/80 assertions) -- including this
  task's own tests exactly as committed, no code changes needed. User
  restarted the FastCached service mid-investigation; confirmed the
  restart did NOT clear the bad entry (same stale hash, same wrong
  result, reproduced again after restart) -- the corruption lives in the
  persisted on-disk L2 store, not just in-memory L1. Left FASTCACHE_ADDR
  cleared in this build tree's CMakeCache for the remainder of this SDD
  run's verification builds; the daemon itself was left untouched
  (no destructive action taken on shared local infrastructure outside
  this repo's scope).
- Ruling: this detour is entirely a local-machine build-tooling issue,
  not a morph or ledger defect, and out of this PR's scope to fix --
  reported to the user, who is aware and will address the daemon
  separately. Filed as https://github.com/LASTRADA-Software/fastcached/issues/51.
- Task reviewer (agent ac17775ac62c3ee94), briefed with the above
  detour's conclusion so it wouldn't re-litigate build correctness:
  spec-compliance FAIL, code-quality "needs rework". Two Critical
  findings, both independently verified against the actual diff by the
  reviewer (not taken on the implementer's word):
  - C1: the pre-decode-gap test (`test_ledger_model.cpp`) never actually
    decodes wire JSON / calls `setWire` via glaze -- it constructs
    `Rational{Numerator{5}, Denominator{0}, DecimalPlaces{2}}` directly
    via the plain in-process constructor, which already clamps on its
    own. This doesn't exercise finding #002's actual claim (that the
    *wire/glaze decode path* clamps hostile input).
  - C2: finding #001's "roughly 9 billion rows" was never measured by
    the committed fuzz test -- the loop caps at count<10^8 with
    perLeg=10^9, so the running sum never approaches int64_t's range
    (max ~10^17 vs INT64_MAX~9.2x10^18); the break condition is
    structurally unreachable within the loop's own bound. "9 billion"
    is a hand-computed estimate (INT64_MAX/perLeg), never actually
    produced by running the test, and the test's own comment
    ("Document the measured N... once run") misrepresents this as
    empirical.
  - I1 (Important, parked/no fix required this round): the brief's own
    Step 2 template values (`b{Numerator{500000},...,DecimalPlaces{4}}`)
    were themselves confused about how `dp` works in this codebase's
    `Rational` (dp is a non-scaling display tag, never a multiplier --
    verified against rational.hpp directly) -- the brief's own worked
    example was arithmetically wrong before the implementer's silent
    fix to `5000`. Both values happen to produce a passing but
    less-meaningful test (equal-magnitude opposite-sign cancellation,
    not genuine cross-dp reduction, because dp never rescales anything
    in this design). Parked as a documentation/comment clarity issue,
    not a correctness defect -- no fix dispatched for I1 alone.
- Fix-loop round 1 dispatched next: resume same implementer, fix C1
  (round-trip the clamped leg through real glaze/glz::read_json wire
  decode) and C2 (either raise the fuzz loop's cap to actually reach the
  boundary and report the true measured count, or rewrite finding #001
  to present the number as a computed estimate, and correct the test's
  misleading "Document the measured N" comment to state what it truly
  verifies).

### Task 13 fix-loop rounds 1-2 (controller-authored, not re-dispatched to a subagent)

- Round 1 (commit c5994b5): C1 fixed (pre-decode-gap test now genuinely
  decodes {"num":5,"den":0,"dp":2} via glz::read_json, reaching
  Rational::setWire through the real glz::meta<Rational> wire-codec
  specialisation, not the plain in-process constructor). C2 fixed by
  replacing the brute-force O(N) loop (proven to take 6+ minutes at the
  corrected 10^10 cap -- an implementer's attempt was killed mid-run)
  with an O(log N) binary search over real Rational::operator+ calls via
  exponentiation-by-squaring, converging on the exact boundary
  9,223,372,037 in ~0.2s. Scoped re-review (agent a4994e90f0209c0e3)
  confirmed C1/C2 both resolved but found a NEW Critical: the doubling
  helper's `term = term + term` ran unconditionally including on the
  final, unneeded iteration, causing real signed-overflow UB for large
  probes independent of the boundary being measured.
- Round 2 (commit 4eebe69, controller-authored fix, not re-dispatched):
  fixed by breaking out of the loop once no remaining bit of n needs
  term doubled further. Scoped re-review (agent a84780db49af9ec9c)
  confirmed this specific bug resolved (hand-traced n=9223372037 and
  n=5, plus simulated n in [1,200000] and boundary-adjacent values) but
  found a SIBLING Critical still present: sumOfNLegs was still called
  unconditionally on every binary-search probe, including ones the
  closed-form oracle had already certified would overflow -- so its
  internal `result + term` accumulation still ran real, unchecked
  Rational addition on values already known to exceed int64_t's range.
- Round 3 (commit 207bac1, controller-authored fix, not re-dispatched):
  fixed by moving the wouldOverflow check to guard the sumOfNLegs call
  itself (not just its result) -- the function is now only ever invoked
  on probes already certified overflow-free. Final scoped re-review
  (agent a642192097d475cb3) confirmed the file is now PROVABLY free of
  signed-overflow UB: after this fix, sumOfNLegs's domain is restricted
  to n <= INT64_MAX/perLeg, and both term's max value (perLeg*2^33,
  ~93% of INT64_MAX) and result's max value (bound*perLeg, exactly
  <=INT64_MAX by construction of `bound`) stay safely in range for
  every possible call. No third instance of the bug, no other
  UB-shaped issue anywhere else in the file. Zero remaining findings.
- Ruling: these three controller-authored fix rounds (not dispatched to
  a fresh implementer subagent, given their small, surgical, and highly
  interdependent nature -- each fix was a few lines directly informed
  by the immediately preceding scoped review's exact finding) are
  within the SDD skill's fix-loop process (rounds 1-3 of up to 5 permit
  resuming/directly fixing before escalating model tier); each round got
  its own independent scoped re-review exactly as the process requires.
  All full-suite regression runs (morph_tests 1077/1077, 20139
  assertions; ladder_ledger_tests 29/29, 81 assertions) passed after
  every round.

Task 13: complete (2 fix rounds, both fully resolved and independently
re-verified; the earlier ICF/fastcache-cc detour is documented above
and was not a Task 13 defect).

## Task 14: UndoTransaction -- compensating action, never undoLast()

- Plan corrections before dispatch (commit d5ba12c): fixed a wrong API
  reference (Rational::operator-() const is a MEMBER unary negation, not
  the free binary operator-(lhs,rhs) also declared in rational.hpp);
  resolved the brief's vague "reuse that private implementation" into a
  concrete storeJournalImpl extraction mirroring Task 12's
  setCategoryImpl precedent exactly; resolved a genuinely novel
  key-resolution question (UndoTransaction only naturally carries
  journalId, but every keyed action derives its key from a ledgerId
  field) by raising it to the user rather than deciding silently --
  ruling: add a redundant ledgerId field to the action, keep
  ActionKeyTraits::key() a trivial field read, cross-check the journal
  really belongs to that ledger inside execute(); fixed the reversal's
  date to morph::time::Timestamp::now() (client-observable-date
  convention, matching StoreTransaction.date) instead of the
  server-audit-stamp morph::ladder::now() the brief had wrongly cited;
  wrote out the Step 1 test's full body (was a placeholder).
- Implementer: DONE. Commit `3907f62`. One reported, pre-cleared
  deviation: `Lightweight::BelongsTo::Value()` returns the raw FK scalar
  directly, not a nested record -- the brief's pseudocode used the wrong
  accessor shape at three call sites; fixed, matching budget_model.cpp's
  own existing precedent.
- Controller-independent verification: `cmake --build build/clangcl-release
  --target ladder_ledger_tests` (no-op, already current) +
  `ladder_ledger_tests.exe` direct run -- 100% pass, 91 assertions / 30
  test cases, 0 failures (matches implementer's report exactly).
- Task reviewer (agent a1bb27b65622ac646): spec-compliance PASS,
  code-quality PASS, zero findings. Independently confirmed (not just
  re-stated): execute(StoreTransaction) is verifiably byte-for-byte
  untouched (diff shows only pure insertions after its end); the
  zero-sum-skip reasoning in storeJournalImpl is mathematically sound
  (negating an already-zero-sum leg set is unconditionally zero-sum,
  Rational negation being linear/exact); causalParentId is minted from
  the UNDONE journal's own already-persisted row id, read before
  storeJournalImpl runs and never mutated by it; no stray/redundant
  SqlTransaction in execute(UndoTransaction); the new test verifies
  actual reversal leg values via direct row inspection, not just
  net-zero balance; the pre-cleared BelongsTo::Value() claim verified
  independently against the real Lightweight header.

Task 14: complete

## Task 15: CSV import -- content-hash cross-import dedup, opId ledger populated

- Plan corrections before dispatch (commit 9960726): four real gaps
  found and resolved -- (1) ImportOpId already existed from Task 11b,
  the brief wrongly said to define a new one; (2) ledger_imported_ops's
  real key is (owner_principal, op_id), not (ledgerId, opId) as the
  brief claimed -- confirmed against the actual entity/migration;
  (3) no account info anywhere in the brief's CSV format despite every
  transaction needing >=2 real-account legs -- raised to the user,
  ruled: add a required counterAccountId field + account_id CSV column,
  each row posts a two-leg entry against its own account and the
  chunk-wide counter-account; (4) test snippets hardcoded LedgerId{1}
  with no backing row -- fixed to create a real LedgerRecord first, per
  every other test's own convention. Also specified exact decimal-string
  parsing (never std::stod/atof) and scoped the opId-ledger table to be
  populated but not read back for an early-return (this task's own test
  doesn't need counted-replay semantics; content-hash dedup alone
  already gives correct behavior) -- recorded as a deliberate ruling,
  not a TODO.
- Implementer: DONE_WITH_CONCERNS. Commit `ba532c0`. Four further
  real, independently-verified deviations, all sound: (1) no
  whole-chunk SqlTransaction -- storeJournalImpl (Task 14) opens/commits
  its own transaction per call, and SqlTransaction's real implementation
  toggles SQL_ATTR_AUTOCOMMIT on the raw connection with no
  nesting/savepoint support, so nesting a second one would silently
  break rollback after row 1; committed per-row atomically instead;
  (2) a check-then-insert guard was needed on the ImportedOpRecord
  insert (the brief implied unconditional) since the real UNIQUE index
  on (owner_principal, op_id) would otherwise throw on a replayed opId;
  (3) the brief's own test CSV dates (`2026-01-01`, 10 chars) are
  rejected by DateTime::fromIso8601's real >=19-char requirement --
  fixed to full ISO-8601 timestamps in test data; (4) the brief's own
  test assertion was self-contradictory given its own no-early-return
  design -- replaced with a correct, stronger assertion set, flagged
  rather than silently kept. Added two extra, non-redundant tests
  (balance-check, malformed-row-rejection) beyond the brief's two.
- Controller-independent verification: `cmake --build build/clangcl-release
  --target ladder_ledger_tests` (no-op, already current) +
  `ladder_ledger_tests.exe` direct run -- 100% pass, 105 assertions / 34
  test cases, 0 failures (matches implementer's report exactly).
- Task reviewer (agent a51d6c7638046d3ef): spec-compliance PASS,
  code-quality PASS, zero findings. All four pre-cleared deviations
  independently re-verified against real source (not taken on the
  implementer's word): SqlTransaction's autocommit-toggling constructor
  read directly from vendored Lightweight source; the real UNIQUE index
  confirmed in schema.cpp; fromIso8601's exact 19-char minimum read
  directly from datetime.hpp; the replay test's actual traced behavior
  matches its corrected assertions exactly. parseAmount hand-traced for
  "-4.50", "12", "-0.05", "0.5" -- no octal/leading-zero bug (std::stoll
  defaults to base 10). Content-hash field set and non-cryptographic
  std::hash choice judged an acceptable, documented tradeoff for this
  rung's explicitly-scoped stress-test purpose. Zero-sum leg exactness
  confirmed via Rational::operator-()'s canonicalizing constructor
  (gcd(x,1)==1 always, no reduction possible) plus the diff's own test
  empirically confirming exact opposite balances.

Task 15: complete

## Task 16: Reports -- submit->poll job idiom, snapshot semantics, model-owned executor

- Plan corrections before dispatch (commit e711143): dispatched a
  dedicated research pass (background Explore agent) before writing
  this task's brief, since the original brief text flagged a genuine,
  unresolved uncertainty about the worker-pool seam. Findings, all
  load-bearing: (1) NO worker-pool-from-inside-a-model seam exists
  anywhere in this codebase -- exhaustively confirmed across
  bank/bookmarks/pastebin/polls; the design spec's own claim that
  rung 2 "establishes" one is not actually true (bookmarks' real
  background job lives entirely at the App/Bridge/RemoteServer layer,
  re-entering the model as a fresh client dispatch). Raised to the
  user; ruled: LedgerModel gets its own std::shared_ptr<IExecutor>
  member, a genuinely new local pattern -- filed as finding 003.
  (2) Confirmed the real raw-query API for WAL snapshot pinning
  (Lightweight::SqlStatement{connection}.ExecuteDirect(rawSql), a raw
  BEGIN DEFERRED needed first since SqlTransaction itself issues no
  BEGIN). (3) ReportJobRecord::jobId (string) vs ReportJobId (int64
  strong id) is a genuine type mismatch nothing exercised before this
  task -- resolved by storing the row's own stringified id.
  (4) Adopted the pooled-DataMapper convention (GlobalDataMapperPool)
  for this task's own new worker-thread code specifically, not
  retrofitted onto existing execute() methods. (5) Confirmed no
  deferred-executor test double exists -- tests genuinely spin a real
  thread pool with bounded polling, matching the brief's own already-
  correct test shape.
- Implementer (opus, given the architectural delicacy): DONE_WITH_CONCERNS.
  Commit `a479d31`. Self-discovered and clearly reported a significant
  finding: the test DB is NOT actually in WAL mode (no PRAGMA
  journal_mode=WAL anywhere reachable; Lightweight's own source
  explicitly declines WAL, relying on busy_timeout=60000 instead) --
  "WAL snapshot" is a misnomer for what BEGIN DEFERRED actually pins
  (a consistent read snapshot via a SHARED lock that DOES block
  writers, unlike true WAL). Mitigated by committing the read
  transaction before any write, on both success and exception paths.
  Four forced (non-discretionary) deviations from the brief's literal
  code, all verified real: ReportLine moved to namespace scope (glaze
  reflection needs linkage); (void) discards on [[nodiscard]]
  ExecuteDirect; computeReportJson/finishReportJob extracted as
  helpers to avoid duplicating job-row-write logic; outer catch(...)
  instead of catch(const std::exception&) so any throw still reaches
  a terminal job state. Added a stronger first test (decodes and
  verifies real aggregation, not just has_value()) plus two extra
  validation/not-found tests.
- Controller-independent verification: rebuilt cleanly, 120/120
  assertions (38 test cases) on first run, stable across further runs
  including [reports] subset re-run 3x, ~6s total suite runtime (not
  hung).
- Task reviewer (agent a485b8417f86fdbbe, opus for the threading
  rigor needed): spec-compliance PASS, code-quality PASS WITH ONE
  IMPORTANT FINDING. Independently re-verified every locking claim
  against real vendored Lightweight source line-by-line (not taken on
  the implementer's word) -- confirmed the not-actually-WAL finding is
  accurate and even stronger than claimed (Lightweight's own source
  comment explicitly declines WAL); confirmed the happy-path mitigation
  is real. Found a genuine gap the implementer's own mitigation missed:
  BEGIN DEFERRED sat outside the inner try (if it threw, nothing would
  commit) and a recovery COMMIT that itself threw (real SQLITE_BUSY-on-
  commit possibility) would replace the in-flight exception and
  propagate with the transaction still open -- DataMapperPool::Return
  performs no transaction cleanup, so either path leaks an open read
  lock onto the pooled connection, stalling the next unrelated caller
  for up to 60s. Confirmed via reading Pool.hpp directly.
- Fix round 1 (commit 38a84d6, controller-authored -- small, precise,
  well-understood fix, not re-dispatched): introduced WalSnapshotGuard,
  an RAII class whose constructor issues BEGIN DEFERRED and whose
  destructor issues COMMIT unconditionally inside its own catch(...).
  Rebuilt clean on first try, 120/120 assertions unchanged, [reports]
  subset re-run 3x stable.
- Scoped re-review (agent aea3fe8b70a5538d2, opus): original finding
  RESOLVED, with an unusually rigorous C++ semantics trace --
  confirmed a throwing constructor means the destructor never runs
  (correct: nothing to clean up if BEGIN never succeeded); confirmed
  the destructor is implicitly noexcept (a reference member is
  trivially destructible) and that both its own throw sources are
  written inline inside its own try block, so nothing can ever escape
  it; confirmed deleting copy/move is correct (a defaulted copy/move
  would produce two guards on the same connection, both issuing
  COMMIT -- a real double-COMMIT bug the deletion prevents). Two Minor
  notes: (1) no fault-injection test exists for the guard's exception
  path -- accepted, matches the earlier Failed-path scoping decision;
  (2) the new code's own comments violated CLAUDE.md's present-tense-
  only rule ("used to propagate", "had thrown") -- fixed immediately
  as fix round 2.
- Fix round 2 (commit 3146628, controller-authored, comment-only):
  rewrote both flagged comments to state only current behavior +
  rationale, no fix-history framing. Rebuilt clean, 120/120 assertions
  unchanged.

Task 16: complete (2 fix rounds -- one Important correctness fix, one
Minor documentation-style fix -- both independently re-verified).

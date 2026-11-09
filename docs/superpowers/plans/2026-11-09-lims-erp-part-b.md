# lims ERP Part B: Ledger and Billing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give lims a double-entry ledger and the billing that feeds it — Part B of `docs/superpowers/specs/2026-11-09-lims-erp-design.md` — and, on the way, build the first instance of hypotheses 1, 2, 3, 4, 5, 7 and 8 and the second instance of hypothesis 6 with morph as it stands, each with a test that measures the outcome and a README resolved decision that records it.

**Architecture:** Two new feature modules and one new shared module join `org`, `lab` and `app`: `posting` (shared; the `PostingRequest`/`PostingResult` contract only), `ledger` (accounts, periods, journal entries, exchange rates, reports; knows nothing of invoices) and `billing` (settings, clients, price lists, billable lines, invoices, the edit lease, the billing run; builds a `PostingRequest` and hands it to a port the app installs). `lab` writes an outbox row in `PublishSample`'s transaction; the app relays it with `morph::journal::OutboxRelay` into billing's idempotent consumer. Every cross-module call — posting, the dated rate, the event — is wired in `app`, the only module that sees every other. Money is `morph::units::Quantity<ErpUnit::money, 2>` over `morph::math::Rational`, with the currency a `CurrencyCode` field beside it; posters round half-up per line and add an explicit rounding line; the ledger validates and never rounds.

**Tech Stack:** C++23, CMake ≥ 3.25 (Ninja), morph (header-only), LASTRADA Lightweight ORM over SQLite, glaze, Catch2, Qt 6 (Core; `QTimer` in the server), morph ui/1 authoring API (`morph/ui/author.hpp`).

## Global Constraints

Copied from the design (`docs/superpowers/specs/2026-11-09-lims-erp-design.md`), `examples/IMPLEMENTATION.md`, `AGENTS.md` and the Part A plan. Every task's requirements include this section.

- **Principle (design):** "A piece of work lands on one side by two questions, both of which must be answered yes before anything moves into `include/morph`: 1. **Is it mechanism?** … The chart of accounts, pricing, invoice layout, stock valuation, approval thresholds and the tax rate are not; atomic work across models, durable reactions between models, leases, scheduled work, approval state and scoped authorization may be. … 2. **Has it been needed twice?** … Inside lims, two modules count as two places." "So a guarantee confirmed once is recorded as confirmed and built in lims; its second instance is built against a framework seam designed for both."
- **Dependency rule (design):** "Each module is its own static library with its own include directory, DTOs, entities, migrations and models. A module's target links `lims/org` and, where it posts or records postings, `lims/posting`; nothing else. Because a module's include path holds only its own headers and those two, including another module's header is a compile error, not a lint finding. A compile check (`tests/compile_checks/`) includes billing from inventory and must fail to build."
- **Atomic steps (design):** "Posting an invoice must assign its number, mark it posted and create its journal entry as one fact … **billing** owns the invoice, its number series and the mapping from an invoice to accounts (receivable, revenue, tax, as account codes configured per company) and builds a `posting::PostingRequest`; **ledger** owns validation and recording of a `PostingRequest` and knows nothing of invoices. The atomic step itself … is the open question: hypothesis 1 says morph has no way to express it, and the first implementation shows what happens when the two calls are made in sequence."
- **Reactions (design):** "A module that commits a fact other modules may react to — `lab.AnalysisPublished` … — writes an outbox row in the same transaction as the fact. The event's id is `<module>.<event>:<aggregate id>:<aggregate version>`, so a redelivery has the same id. `lims/app` wires each event to a consumer in another module … Consumers are idempotent by event id."
- **Shared database (design):** "All modules use one SQLite file through the Lightweight ORM (`IMPLEMENTATION.md` rule 4), with table names prefixed per module. No module reads another's tables."
- **Ledger (design, verbatim):** "Chart of accounts per company: asset, liability, equity, revenue and expense accounts, each with a code and a name. A journal entry has a date, a period, a source reference supplied by the poster, and two or more lines, each with an account, a side (debit or credit), an amount in transaction currency and its value in company currency. **Balancing.** An entry must sum to zero per transaction currency and in company currency, checked before commit. The ledger never rounds or auto-balances. Company-currency values are rounded by the **poster**, half-up to the company currency's decimals, line by line; where that leaves a company-currency difference, the poster adds an explicit exchange-rounding line to the company's configured rounding account. The ledger refuses any entry that still does not balance. Entries are immutable once posted. A correction is a reversal entry that names the original. Periods are monthly and open, closing or closed. Posting into a closed period is refused. Closing verifies that no draft entries remain and freezes the period's balances. Exchange rates are dated, per currency pair, entered by hand; a posting uses the rate valid on its date. Reports: trial balance, account ledger, balance per account as of a date. Money is `morph::units::Quantity` over `morph::math::Rational`, never integer minor units. One company currency is zero-decimal (JPY), so that case is always exercised."
- **Billing (design, verbatim):** "Price lists per company, versioned with effective dates: a price per analysis definition, optionally overridden per client. `lab.AnalysisPublished` creates a billable line at the price valid on the publishing date. A redelivered event bills nothing twice. An invoice collects one client's billable lines, in the client's currency. Lifecycle: draft → submitted → approved → posted. Approval is four-eyes and limited by the approver's value limit; an approval not acted on within its deadline escalates to a company-scoped approver. While one clerk edits a draft invoice, it is leased to them: a second clerk sees who holds it, and the lease lapses when the holder's connection closes or after a timeout. Posting assigns a **gap-free number** per company and fiscal year (`INV-2026-000123`). A posting that fails leaves no hole in the series and no partial state. Tax: one flat rate per company, computed on the invoice total and rounded half-up to the currency's decimals. No tax engine. The **billing run** is scheduled: on a configured day each month it drafts invoices for every client with unbilled lines. Its progress is visible while it runs, and a run interrupted by a restart resumes rather than duplicating invoices."
- **Out of scope (design):** "PDF and print-file rendering; a tax engine and statutory reports; payments, credit notes and supplier invoices; … bank and payment integration; electronic invoicing". Part D's screens are not built here; Task 9 adds one functional panel because a measurement needs a real screen.
- **Hypotheses Part B measures (design table, verbatim columns "Hypothesis" / "Refuted by"):**
  1. "**No atomic work across models.** Each `execute` takes its own pooled connection and commits alone, models are not ordered against each other, and an awaiting handler holds its own model's gate; there is no way to commit billing's state change and ledger's entry together. Gap-free numbering is part of it: a number assigned in billing's transaction is gap-free only if the entry commits with it." / "Both postings expressed with existing seams, and a failure injected between the halves leaves no partial state and no hole in either series". Part B builds instance 1 (invoice posting).
  2. "**Delivering events to a model consumer needs hand-rolled code per consumer.** `OutboxRelay` delivers at least once to an `IActionLog` sink, and `IReplayLedger` dedups by op id; neither delivers to another model." / "Both consumers receive each event exactly once in effect, across a restart, with only wiring code in `lims/app` and no per-consumer delivery loop or cursor". Instance 1: analysis published → billable line.
  3. "**No lease that reaches a model.** `IEnvelopeService::connectionClosed` and shared-instance attach counts exist, but no hook tells a model that the connection holding its lease closed." / "Both leases lapse on disconnect through an existing hook, without a timer of the model's own". Instance 1: draft invoice edit lease.
  4. "**No periodic trigger and no resume after restart.** … `TimeoutScheduler` is a detail type; nothing starts work on a schedule or resumes it after a restart." / "Both scheduled and resumable with public primitives and no per-module scheduler". Instances: billing run; approval escalation — **both are in Part B** (see Task 12).
  5. "**No long-running approval state.** `forms::flows` are wizards inside one session; approvals last days, across principals, with limits, deadlines and escalation." / "Both lifecycles expressed as ordinary actions sharing no code but the declared authorization". Instance 1: invoice approval.
  6. "**Authorization cannot see the payload.** `IAuthorizer::authorize(ctx, modelType, actionType)` decides before the payload is decoded, so a scope, a value limit or a separation-of-duties rule can only be checked inside each model, by hand." / "A declared form of rule checked once by the framework for every module, or shown to be domain logic the models must own". Part A built instance 1 (README decision 25); Part B builds instance 2 (billing's create/approve pair and the approver's value limit) and then designs the seam or dispositions it (Task 16).
  7. "**Effects leak at row and company granularity.** `fanOut` re-authorizes each watcher, but at model and action level only, and `record::privateKeys` hides keys from non-writers; a principal of company A can still learn that company B's record changed." / "A company-B change reaching no company-A connection, in a two-company remote test". Instance 1: invoices.
  8. "**Money at ERP magnitude and with runtime currency.** `Rational` saturates and logs unless `checked*` is used; a `Quantity` unit is a compile-time tag, and `UnitTraits::convert` takes no date, so a currency known only from the record and a dated rate do not fit `Quantity`." / "A trial balance over 10⁷ lines of up to 10⁹ minor units each (totals within int64) summing exactly, with an over-range sum refused rather than saturated; and runtime currencies handled with `Quantity` as it stands". Instance 1: trial balance; the invoice half of instance 2 (orders are Part C).
- **How a hypothesis task is built (user, for this plan):** each hypothesis task builds its first instance in lims with existing morph seams, plus a test that measures the outcome (confirmed / refuted / mis-framed), and records it as a README resolved decision. A framework seam is designed only when a second instance needs the same hand-rolled guarantee. A measuring test asserts the behaviour it measured; if the assertion that pins the outcome fails, the outcome is the other one — change the assertion to what was measured and use the README text for that outcome, never weaken the test until it passes.
- **Rule 1 (IMPLEMENTATION.md):** "All business logic, all invariants, and all persistence access live in plain, single-threaded model classes with typed actions." "Models must re-check their own preconditions and authorization (`Context::principal`) — the schema's `required` and the client gates are UX, not security." "Action failures are thrown as the app's typed error set."
- **Rule 2 (IMPLEMENTATION.md):** "Screens never say what to refresh. Every action that changes data declares its effects with `BRIDGE_REGISTER_EFFECTS` beside its registration, over records registered with `BRIDGE_REGISTER_RECORD` … The rung's bundle loads with no refusal and no warning." Work through `docs/KEEPING-SCREENS-CURRENT.md`'s checklist for every new or changed action.
- **Rule 3 (IMPLEMENTATION.md):** "The only plain type permitted in an action/result field is `std::string`"; "Entity identity: A per-entity strong id type … exposing `hasValue()`"; "Closed sets of states/options: `enum class` **with a `glz::meta`/`glz::enumerate` specialisation**". "Forbidden in any DTO field: `int`, `int64_t`, `double`, `float`, `bool`, raw enums." Numbers in DTOs are `Quantity` (`Amount`, `Rate`, `Count` from Task 1); dates are `morph::time::Timestamp`.
- **Rule 4 (IMPLEMENTATION.md):** "All persistence goes through the Lightweight ORM"; "Schema is owned by `LIGHTWEIGHT_SQL_MIGRATION` definitions … Migrations are the *only* DDL mechanism"; "Forbidden: direct `sqlite3_*` calls; hand-written SQL strings outside Lightweight's facilities"; the sanctioned escape tier is "Lightweight's own raw-query facility, invoked from inside the model, with a mandatory finding entry". Prefer the typed `DataMapper` API (`Create`/`Query`/`Update`/`Delete`, `.Where(FieldNameOf<…>, …).All()`) over raw SQL everywhere it can express the operation.
- **Rule 5 (IMPLEMENTATION.md):** "Every model is 100% unit tested … Model tests run the full backend-mode matrix (`Local` / `LocalSingleThread` / `Socket`)"; "every invariant named in the rung README … exists as a named test before the feature is called done."
- **AGENTS.md, comments:** "A comment … states **what the code does now and why**. … No history. … No issue numbers, no commit hashes." Public API keeps complete `@param`/`@tparam`/`@return`. Doxygen runs with `WARN_AS_ERROR = FAIL_ON_WARNINGS`.
- **AGENTS.md, verification:** "Ask whether the check would still pass if the feature did nothing. If it would, it is not evidence. Mutate the feature and confirm the check fails." Every measuring test names its mutation in a `// Mutation:` comment above it, and the step that adds it runs that mutation once. State results as measured or inferred.
- **AGENTS.md, filing bar:** defects in `include/morph`/`src` get an issue; `examples/` and CI configuration are fixed in place and not filed; "The *same* example defect in a second place — File it against `include/morph`." A filed issue states its verification status, real output, and what would change the verdict. Never apply a `triage: *` label by hand.
- **Design review (user):** a framework spec under `docs/spec/` gets one independent review subagent on the default model (never Fable) before it is implemented.
- **Lane rules (user):** every build/configure is wrapped in `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock`, builds at `-j10` at most; tests run under the same lock with `timeout 300`, or as `ctest -j6 -L ladder-lims`. Implementation lanes run on Sonnet; two or three lanes at a time, one compile at a time. Commit as the current git user, with the session's attribution trailer. Another session may be editing `examples/lims/screens/**`: rebase before touching a screen file and keep the change to the lines the task names.

## Build and test commands

The ladder build directory is the `linux-everything` preset's (`build/linux-everything`: clang, Release, `MORPH_BUILD_LADDER=ON`, `MORPH_LADDER_RUNGS=all`, Qt and Qt Quick on). Every command below is used verbatim in the tasks.

- Configure: `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --preset linux-everything`
- Build lims: `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --build build/linux-everything --target ladder_lims_tests ladder_lims_screen_tests ladder_lims_render_smoke ladder_lims_server morph_examples_client -j10`
- Model suite: `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock timeout 300 build/linux-everything/examples/lims/ladder_lims_tests`
- Screen suite: `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock env QT_QPA_PLATFORM=offscreen timeout 300 build/linux-everything/ladder_lims_screen_tests`
- Everything labelled for the rung: `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock ctest --test-dir build/linux-everything -j6 -L ladder-lims --output-on-failure`

A Catch2 tag or name after the binary narrows a run, e.g. `… ladder_lims_tests "[ledger]"`. Task 16, if it changes `include/morph`, also builds and runs the framework suite: `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --build build/linux-everything --target morph_tests -j10` and `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock timeout 300 build/linux-everything/tests/morph_tests "[guard]"` (the target and path are the framework suite's in `tests/CMakeLists.txt`; if they differ there, use what that file names).

## Conventions every task uses

Read once; the tasks rely on them without restating them.

- **Namespaces and ids.** Every module keeps the `lims` namespace (README decision 21). A new id is `LIMS_DEFINE_STRONG_ID(Name)` in `namespace lims`, then `LIMS_DEFINE_STRONG_ID_WIRE(Name)` and `LIMS_DEFINE_STRONG_ID_SHAPE_TAG(Name, "lims.<camelName>")` at global scope (`lims/org/core/strong_id.hpp`).
- **Rows to values.** A Rational column pair is `<name>_num`/`<name>_den` (`Bigint`); the decimals are the currency's (`decimalsOf`, Task 1) and are not stored. An enum column stores the underlying integer and is range-checked when read (`org_model.cpp`'s `toView` shows the pattern); a row outside its enumeration is a `ValidationError`.
- **A model action** starts `requirePrincipal();`, validates (`ValidationError` naming the action), asks `requireRoleSomewhere<Action>()` before looking up rows the payload names, looks them up (`NotFound`), then `requireAuthorized(action)` (Part A's order, README decision 25), then writes in one `Lightweight::SqlTransaction` (`SqlTransactionMode::ROLLBACK`, `Commit()` at the end).
- **A list** filters by `OrgModel::visibleCompanies(currentPrincipal())` (README decision 26) and sorts by id.
- **Declared authorization.** Every mutating action has an `Authorization<Action>` specialisation in its module's `authorization/` header, is added to `lims::app::declaredRequirements()` (and the array size raised), and to the `Mutating` tuple and the model list of `test_declared_authorization.cpp`'s coverage case. Actions with company scope use the helper Task 3 adds, `CompanyRule<Action, Role>`.
- **Registration** is at the bottom of the model header, in `// NOLINTBEGIN(misc-const-correctness)` / `// NOLINTEND`, as `org_model.hpp` does: `BRIDGE_REGISTER_MODEL`, then per action `BRIDGE_REGISTER_ACTION` (reads with `::morph::model::Loggable::No`) and, for a mutating one, `BRIDGE_REGISTER_EFFECTS` directly under it. Records are registered beside their view type with `BRIDGE_REGISTER_RECORD(Type, "<Module>_<Name>")`.
- **Tables** are prefixed per module (`org_`, `ledger_`, `billing_`; lab keeps `lims_`). Migration ids for Part B are `202611100000NN`, numbered in the order the tasks add them, so they apply after every Part A migration.
- **Each module header tree** that holds Lightweight records carries the one-key `.clang-tidy` that `org/include/.clang-tidy` holds (the `TableName` exemption), copied verbatim with "org" replaced by the module's name.
- **Tests** are `examples/lims/tests/test_<topic>.cpp`, tagged `[lims][<topic>]`, start with `DbFixture fixture;`, and set principals with `lims::test::ScopedPrincipal` and roles with `lims::test::grant` (`tests/lims_test_support.hpp`). A test that needs a fixed instant uses `morph::ladder::ScopedClockOverride` (`examples/common/clock.hpp`).

## File structure after Part B

```
examples/lims/
  CMakeLists.txt                     MODULES org posting lab ledger billing app; SHARED_MODULES org posting; probes
  org/include/lims/org/core/         references.hpp (ClientId, SampleId, AnalysisId, ResultId), money.hpp
  org/include/lims/org/dto/          limit_dto.hpp
  posting/include/lims/posting/      posting.hpp                      (INTERFACE module: DTOs only)
  lab/include/lims/lab/events/       lab_events.hpp                   lab.AnalysisPublished and lab's outbox
  lab/src/events/                    lab_events.cpp
  ledger/include/.clang-tidy
  ledger/include/lims/ledger/core/   types.hpp, balance.hpp
  ledger/include/lims/ledger/dto/    account_dto.hpp, entry_dto.hpp, rate_dto.hpp, report_dto.hpp
  ledger/include/lims/ledger/db/     ledger_entity.hpp
  ledger/include/lims/ledger/models/ ledger_model.hpp, ledger_report_model.hpp
  ledger/include/lims/ledger/authorization/ ledger_authorization.hpp
  ledger/src/{core,db,models,authorization}/…
  billing/include/.clang-tidy
  billing/include/lims/billing/core/ types.hpp, fault_hook.hpp, ledger_port.hpp, invoice_math.hpp
  billing/include/lims/billing/dto/  settings_dto.hpp, price_dto.hpp, line_dto.hpp, invoice_dto.hpp, lease_dto.hpp, run_dto.hpp
  billing/include/lims/billing/db/   billing_entity.hpp
  billing/include/lims/billing/models/ price_list_model.hpp, invoice_model.hpp, invoice_edit_model.hpp, billing_run_model.hpp
  billing/include/lims/billing/authorization/ billing_authorization.hpp
  billing/src/{core,db,models,authorization}/…
  app/include/lims/app/              declared_requirements.hpp, lims_authorizer.hpp, event_wiring.hpp, ledger_wiring.hpp, billing_scheduler.hpp
  app/src/                           event_wiring.cpp, ledger_wiring.cpp, billing_scheduler.cpp
  src/server/main.cpp                + the ledger port, the event relay and the billing schedule on one QTimer
  screens/lims_screens.cpp           lifecycle screen gains its sample's billable lines (Task 9)
  tests/                             test_money, test_posting_contract, test_value_limits, test_ledger_chart, test_ledger_entries,
                                     test_ledger_reports, test_price_lists, test_domain_events, test_invoices, test_billing_run,
                                     test_invoice_approval, test_invoice_lease, test_invoice_posting, test_two_company_effects,
                                     lims_server_rig.hpp
  tests/compile_checks/              + posting_includes_lab, lab_includes_posting, ledger_includes_lab, billing_includes_ledger,
                                       ledger_includes_billing
```

Module dependency after Part B: `org` → nothing; `posting` → `org`; `lab`, `ledger`, `billing` → `org`, `posting`; `app` → every module.

---

### Task 1: Money and the ids other modules refer to

**Files:**
- Create: `examples/lims/org/include/lims/org/core/money.hpp`, `examples/lims/org/include/lims/org/core/references.hpp`, `examples/lims/tests/test_money.cpp`
- Modify: `examples/lims/org/include/lims/org/core/errors.hpp` (add `OutOfRange`), `examples/lims/lab/include/lims/lab/core/types.hpp` (drop the four moved ids), `examples/IMPLEMENTATION.md` (the stale floor), `examples/lims/README.md` (decision 28)

**Interfaces:**
- Consumes: `lims::CurrencyCode` (`org/core/types.hpp`), `morph::units::Quantity`, `morph::units::UnitTraits`, `morph::math::checkedRoundToDecimalPlaces`, `morph::math::checkedAdd`, `morph::math::RoundingMode::HalfAwayFromZero`.
- Produces: `enum class lims::ErpUnit { money, rate, count }`; `lims::Amount = Quantity<ErpUnit::money, 2>`; `lims::Rate = Quantity<ErpUnit::rate, 6>`; `lims::Count = Quantity<ErpUnit::count, 0>`; `constexpr std::uint32_t lims::decimalsOf(CurrencyCode)`; `constexpr std::string_view lims::currencyName(CurrencyCode)`; `bool lims::fitsCurrency(Rational const&, CurrencyCode)`; `Rational lims::roundToCurrency(Rational const&, CurrencyCode)` (throws `OutOfRange`); `class lims::ExactSum { bool add(Rational const&); Rational total() const; }`; `class lims::OutOfRange : LimsError`; `lims::ClientId`, `lims::SampleId`, `lims::AnalysisId`, `lims::ResultId` now in `lims/org/core/references.hpp`, with their wire and shape tags unchanged.

Billing names a lab client, sample, result and analysis; it cannot include lab. The four ids move to org, unchanged in name, wire form and shape tag, so every journal entry that carries one still decodes.

- [ ] **Step 1: Write the failing test**

Create `examples/lims/tests/test_money.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Money: one money unit whose currency is a field beside it, and the half-up rounding every poster uses.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <morph/util/rational.hpp>

#include "lims/org/core/errors.hpp"
#include "lims/org/core/money.hpp"

namespace {

using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

/// @brief @p numerator / @p denominator at @p places.
/// @param numerator The numerator.
/// @param denominator The denominator.
/// @param places The decimal places.
/// @return The value.
[[nodiscard]] Rational ratio(std::int64_t numerator, std::int64_t denominator, std::uint32_t places) {
    return Rational{Numerator{numerator}, Denominator{denominator}, DecimalPlaces{places}};
}

}  // namespace

TEST_CASE("money: a currency's decimals, JPY's being none", "[lims][money]") {
    CHECK(lims::decimalsOf(lims::CurrencyCode::EUR) == 2U);
    CHECK(lims::decimalsOf(lims::CurrencyCode::USD) == 2U);
    CHECK(lims::decimalsOf(lims::CurrencyCode::GBP) == 2U);
    CHECK(lims::decimalsOf(lims::CurrencyCode::CHF) == 2U);
    CHECK(lims::decimalsOf(lims::CurrencyCode::JPY) == 0U);
    CHECK(lims::currencyName(lims::CurrencyCode::JPY) == "JPY");
}

// Mutation: round half-even in roundToCurrency (1.005 EUR becomes 1.00, 100.5 JPY becomes 100).
TEST_CASE("money: a poster rounds half-up to the currency's decimals", "[lims][money]") {
    CHECK(lims::roundToCurrency(ratio(1005, 1000, 3), lims::CurrencyCode::EUR) == ratio(101, 100, 2));
    CHECK(lims::roundToCurrency(ratio(1004, 1000, 3), lims::CurrencyCode::EUR) == ratio(100, 100, 2));
    CHECK(lims::roundToCurrency(ratio(1005, 10, 1), lims::CurrencyCode::JPY) == ratio(101, 1, 0));
    CHECK(lims::roundToCurrency(ratio(1004, 10, 1), lims::CurrencyCode::JPY) == ratio(100, 1, 0));
    // Away from zero below it: a negative half rounds to the larger magnitude.
    CHECK(lims::roundToCurrency(ratio(-1005, 1000, 3), lims::CurrencyCode::EUR) == ratio(-101, 100, 2));
    // A value that already fits is unchanged.
    CHECK(lims::roundToCurrency(ratio(1234, 100, 2), lims::CurrencyCode::EUR) == ratio(1234, 100, 2));
}

TEST_CASE("money: a value fits a currency when it is a whole number of its minor units", "[lims][money]") {
    CHECK(lims::fitsCurrency(ratio(12, 1, 0), lims::CurrencyCode::JPY));
    CHECK_FALSE(lims::fitsCurrency(ratio(25, 2, 1), lims::CurrencyCode::JPY));
    CHECK(lims::fitsCurrency(ratio(1234, 100, 2), lims::CurrencyCode::EUR));
    CHECK_FALSE(lims::fitsCurrency(ratio(12345, 1000, 3), lims::CurrencyCode::EUR));
    CHECK_FALSE(lims::fitsCurrency(ratio(1, 3, 2), lims::CurrencyCode::EUR));
}

// Mutation: let ExactSum::add use operator+ (the over-range add saturates and reports success).
TEST_CASE("money: an exact sum refuses a total it cannot hold, and keeps the last one it could", "[lims][money]") {
    lims::ExactSum sum;
    CHECK(sum.add(ratio(150, 100, 2)));
    CHECK(sum.add(ratio(250, 100, 2)));
    CHECK(sum.total() == ratio(400, 100, 2));

    lims::ExactSum huge;
    auto const near = std::numeric_limits<std::int64_t>::max() / 2 + 1;
    CHECK(huge.add(ratio(near, 1, 0)));
    CHECK_FALSE(huge.add(ratio(near, 1, 0)));
    CHECK(huge.total() == ratio(near, 1, 0));
}

TEST_CASE("money: rounding a value whose rounded numerator does not fit throws OutOfRange", "[lims][money]") {
    auto const tooBig = ratio(std::numeric_limits<std::int64_t>::max(), 3, 0);
    CHECK_THROWS_AS(lims::roundToCurrency(tooBig * ratio(100, 1, 0), lims::CurrencyCode::EUR), lims::OutOfRange);
}

TEST_CASE("money: the money units name themselves for the schema", "[lims][money]") {
    CHECK(lims::Amount::unitMeta().id == "money");
    CHECK(lims::Amount::declaredDecimals == 2U);
    CHECK(lims::Rate::unitMeta().id == "rate");
    CHECK(lims::Rate::declaredDecimals == 6U);
    CHECK(lims::Count::unitMeta().id == "count");
    CHECK(lims::Count::declaredDecimals == 0U);
}
```

The last `CHECK_THROWS_AS` multiplies with `operator*`, which saturates and logs; the saturated product rounded to two places is the value whose numerator does not fit. If the saturated product happens to fit after rounding on this toolchain (inferred, not measured), replace the case with `ratio(std::numeric_limits<std::int64_t>::max(), 3, 0)` rounded to `EUR` directly, whose rounded numerator `INT64_MAX / 3 * 100` does not fit.

- [ ] **Step 2: Run it and see it fail**

Run: Build lims. Expected: compile error, `lims/org/core/money.hpp` not found.

- [ ] **Step 3: Add `OutOfRange` to the error set**

In `examples/lims/org/include/lims/org/core/errors.hpp`, after `class Conflict`, add:

```cpp
/// @brief Thrown when an amount, a sum or a rounded value is outside what the books can hold exactly.
///
/// The ledger refuses rather than saturates: a balance clamped to the largest representable value is a wrong
/// balance that looks plausible.
class OutOfRange : public LimsError {
public:
    explicit OutOfRange(std::string message) : LimsError{std::move(message)} {}
};
```

- [ ] **Step 4: Write `money.hpp`**

Create `examples/lims/org/include/lims/org/core/money.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <morph/util/quantity.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "lims/org/core/errors.hpp"
#include "lims/org/core/types.hpp"

/// @file
/// Money for every module: an exact amount whose currency is a field beside it, a dimensionless rate, a count, and
/// the one rounding rule posters use.

namespace lims {

/// @brief The units of lims's business quantities.
///
/// One `money` unit for every currency rather than one unit per currency: a `Quantity`'s unit is a template
/// argument, and the currency of an invoice, a price list or a ledger line is known only from its record. The
/// currency travels as a `CurrencyCode` field beside the amount, and the model that combines two amounts checks that
/// they share it, since the type no longer can.
enum class ErpUnit : std::uint8_t {
    money,  ///< An amount in the currency named beside it.
    rate,   ///< A dimensionless ratio: an exchange rate or a tax rate.
    count,  ///< A whole number: a day of the month, a number of hours.
};

}  // namespace lims

/// @brief Unit metadata for `ErpUnit`. There are no relations: no two units convert.
template <>
struct morph::units::UnitTraits<lims::ErpUnit> {
    /// @brief Display metadata for @p unit.
    /// @param unit The unit.
    /// @return Its id, display text and default decimals.
    static constexpr morph::units::UnitMeta meta(lims::ErpUnit unit) noexcept {
        switch (unit) {
            case lims::ErpUnit::rate:
                return {.id = "rate", .display = "", .defaultDecimals = 6};
            case lims::ErpUnit::count:
                return {.id = "count", .display = "", .defaultDecimals = 0};
            case lims::ErpUnit::money:
            default:
                return {.id = "money", .display = "", .defaultDecimals = 2};
        }
    }
};

namespace lims {

/// @brief An amount of money in the currency a field beside it names. Declared at two decimals for the schema; a
///        JPY amount holds whole yen, which `fitsCurrency` checks.
using Amount = ::morph::units::Quantity<ErpUnit::money, 2>;

/// @brief An exchange rate (units of the target currency per unit of the source) or a tax rate (0.19 for 19 %).
using Rate = ::morph::units::Quantity<ErpUnit::rate, 6>;

/// @brief A whole number.
using Count = ::morph::units::Quantity<ErpUnit::count, 0>;

/// @brief The minor-unit decimals of @p currency.
/// @param currency The currency.
/// @return 0 for JPY, which has no minor unit; 2 for the others.
[[nodiscard]] constexpr std::uint32_t decimalsOf(CurrencyCode currency) noexcept {
    return currency == CurrencyCode::JPY ? 0U : 2U;
}

/// @brief The ISO 4217 code of @p currency.
/// @param currency The currency.
/// @return Its three-letter code.
[[nodiscard]] constexpr std::string_view currencyName(CurrencyCode currency) noexcept {
    switch (currency) {
        case CurrencyCode::USD:
            return "USD";
        case CurrencyCode::GBP:
            return "GBP";
        case CurrencyCode::CHF:
            return "CHF";
        case CurrencyCode::JPY:
            return "JPY";
        case CurrencyCode::EUR:
        default:
            return "EUR";
    }
}

/// @brief Whether @p value is a whole number of @p currency's minor units.
/// @param value The value.
/// @param currency The currency.
/// @return `true` when the value's canonical denominator divides `10^decimalsOf(currency)`.
[[nodiscard]] constexpr bool fitsCurrency(::morph::math::Rational const& value, CurrencyCode currency) noexcept {
    std::int64_t const scale = decimalsOf(currency) == 0U ? 1 : 100;
    return scale % value.denominator == 0;
}

/// @brief Rounds @p value half-up to @p currency's decimals: the rule every poster applies, line by line.
///
/// Half away from zero, which is half-up for the positive amounts invoices and postings carry; a reversal negates an
/// amount that is already rounded, so it never rounds a negative value.
/// @param value The value.
/// @param currency The currency.
/// @return The rounded value, tagged at the currency's decimals.
/// @throws OutOfRange when the rounded value's numerator does not fit an `int64`.
[[nodiscard]] inline ::morph::math::Rational roundToCurrency(::morph::math::Rational const& value,
                                                             CurrencyCode currency) {
    auto const rounded = ::morph::math::checkedRoundToDecimalPlaces(
        value, ::morph::math::DecimalPlaces{decimalsOf(currency)}, ::morph::math::RoundingMode::HalfAwayFromZero);
    if (!rounded.has_value()) {
        throw OutOfRange{"an amount in " + std::string{currencyName(currency)} + " is too large to round exactly"};
    }
    return *rounded;
}

/// @brief A running total that is exact or refuses: the summation every ledger total and report uses.
///
/// Sums `Rational` values, not `Quantity` ones: a `Quantity` sum records a provenance node per addition, which a
/// report over millions of lines cannot afford.
class ExactSum {
public:
    /// @brief Adds @p value, unless the exact total would not fit.
    /// @param value The value.
    /// @return `true` when added; `false` when the total would overflow, which leaves the total as it was.
    [[nodiscard]] bool add(::morph::math::Rational const& value) noexcept {
        auto const next = ::morph::math::checkedAdd(_total, value);
        if (!next.has_value()) {
            return false;
        }
        _total = *next;
        return true;
    }

    /// @brief The total so far.
    /// @return The exact total of every value added.
    [[nodiscard]] ::morph::math::Rational total() const noexcept { return _total; }

private:
    ::morph::math::Rational _total{0, ::morph::math::DecimalPlaces{0}};
};

}  // namespace lims
```

- [ ] **Step 5: Move the four referenced ids into org**

Create `examples/lims/org/include/lims/org/core/references.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lims/org/core/strong_id.hpp"

/// @file
/// The ids of records one module owns and another refers to: lab's clients, samples, analysis definitions and
/// results, which billing bills. Shared so a module that refers to one does not include its owner. The shape tags are
/// part of every journal entry that carries one, so they are spelled exactly as lab first wrote them.

namespace lims {

LIMS_DEFINE_STRONG_ID(ClientId);
LIMS_DEFINE_STRONG_ID(SampleId);
LIMS_DEFINE_STRONG_ID(AnalysisId);
LIMS_DEFINE_STRONG_ID(ResultId);

}  // namespace lims

LIMS_DEFINE_STRONG_ID_WIRE(ClientId);
LIMS_DEFINE_STRONG_ID_WIRE(SampleId);
LIMS_DEFINE_STRONG_ID_WIRE(AnalysisId);
LIMS_DEFINE_STRONG_ID_WIRE(ResultId);

LIMS_DEFINE_STRONG_ID_SHAPE_TAG(ClientId, "lims.clientId");
LIMS_DEFINE_STRONG_ID_SHAPE_TAG(SampleId, "lims.sampleId");
LIMS_DEFINE_STRONG_ID_SHAPE_TAG(AnalysisId, "lims.analysisId");
LIMS_DEFINE_STRONG_ID_SHAPE_TAG(ResultId, "lims.resultId");
```

In `examples/lims/lab/include/lims/lab/core/types.hpp`: add `#include "lims/org/core/references.hpp"` after the `strong_id.hpp` include, and delete the lines `LIMS_DEFINE_STRONG_ID(ClientId);`, `LIMS_DEFINE_STRONG_ID(SampleId);`, `LIMS_DEFINE_STRONG_ID(AnalysisId);`, `LIMS_DEFINE_STRONG_ID(ResultId);`, the four matching `LIMS_DEFINE_STRONG_ID_WIRE(...)` lines and the four matching `LIMS_DEFINE_STRONG_ID_SHAPE_TAG(...)` lines. Nothing else in lab changes.

- [ ] **Step 6: Correct `IMPLEMENTATION.md`'s money sentence**

In `examples/IMPLEMENTATION.md`, replace

```
modelled on `examples/forms/lab_units.hpp`): the enum, `UnitTraits`
metadata, the consteval algebra, and the exact entry-unit relations. Money
is a unit system too (currency units with per-currency `dp` — respecting
the `DecimalPlaces >= 1` floor and the documented JPY/KRW convention in
`docs/spec/util/quantity_type.md`).
```

with

```
modelled on `examples/forms/lab_units.hpp`): the enum, `UnitTraits`
metadata, the consteval algebra, and the exact entry-unit relations. Money
is a unit system too. Where the type fixes the currency, a unit per currency
declares its decimals, and zero is legal (`docs/spec/util/quantity_type.md`:
`Dec == 0` is what a zero-decimal currency such as JPY declares). Where the
currency is known only from the record, as in lims, one money unit carries
the amount and the currency is a field beside it (lims decision 28).
```

- [ ] **Step 7: Build and run**

Run: Build lims, then Model suite. Expected: `test_money.cpp`'s six cases pass, and every pre-existing case passes unchanged (the move changes no shape tag; `test_lims_payload_shape.cpp` is the check). Then run the mutation named above `"money: a poster rounds half-up…"` (change `HalfAwayFromZero` to `HalfEven` in `roundToCurrency`), see that case fail, and restore it.

- [ ] **Step 8: Record decision 28 in the README**

Append to `examples/lims/README.md`'s resolved decisions, after decision 27:

```markdown
### 28. Money is one unit whose currency is a field beside it

An amount is `lims::Amount`, a `Quantity` over the single `ErpUnit::money` unit and an exact `Rational`, and its
currency is a `CurrencyCode` field on the same record. A unit per currency would let the type refuse adding yen to
euros, but a `Quantity`'s unit is a template argument, and the currency of a price list, an invoice or a ledger line
is known only from its row. So the models check what the type cannot: amounts they combine share a currency, and an
amount fits its currency's minor unit (JPY has none). Posters round with `lims::roundToCurrency`, half away from zero
— half-up for the positive amounts they round. Totals are summed with `lims::ExactSum`, which refuses an overflow
instead of saturating, over `Rational` values rather than `Quantity` ones, because a `Quantity` records a provenance
node per addition. The ids billing refers to (client, sample, analysis, result) moved from lab to org, unchanged on
the wire and in the journal.
```

- [ ] **Step 9: Commit**

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/org/include/lims/org/core/money.hpp examples/lims/org/include/lims/org/core/references.hpp \
        examples/lims/org/include/lims/org/core/errors.hpp examples/lims/lab/include/lims/lab/core/types.hpp \
        examples/lims/tests/test_money.cpp examples/IMPLEMENTATION.md examples/lims/README.md
git commit -m "Give lims exact money with the currency beside the amount

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 2: The `posting` shared module

**Files:**
- Create: `examples/lims/posting/include/lims/posting/posting.hpp`, `examples/lims/tests/test_posting_contract.cpp`, `examples/lims/tests/compile_checks/posting_includes_lab.cpp`, `examples/lims/tests/compile_checks/lab_includes_posting.cpp`
- Modify: `examples/lims/CMakeLists.txt`, `examples/lims/README.md` (Modules table)

**Interfaces:**
- Consumes: `lims::Amount`, `lims::Rate`, `lims::CompanyId`, `lims::CurrencyCode`, `morph::time::Timestamp`.
- Produces: `lims::JournalEntryId`; `enum class lims::PostingSide { Debit, Credit }`; `struct lims::PostingLine { std::string accountCode; PostingSide side; Amount amount; Amount companyAmount; }`; `struct lims::PostingRequest { CompanyId company; Timestamp date; std::string sourceKey; std::string sourceReference; CurrencyCode currency; Rate rate; std::vector<PostingLine> lines; }`; `struct lims::PostingResult { JournalEntryId entryId; }`; the CMake target `ladder_lims_posting` (INTERFACE).

- [ ] **Step 1: Write the failing test**

Create `examples/lims/tests/test_posting_contract.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The posting contract: what a poster hands the ledger, and what it gets back. DTOs only.

#include <catch2/catch_test_macros.hpp>
#include <glaze/glaze.hpp>
#include <morph/util/rational.hpp>
#include <string>

#include "lims/posting/posting.hpp"

namespace {

using morph::math::DecimalPlaces;
using morph::math::Rational;

}  // namespace

// Mutation: drop `side` from PostingLine's reflection by renaming it (the round trip loses the credit side).
TEST_CASE("posting contract: a request round-trips through JSON with its zero-decimal amounts", "[lims][posting]") {
    lims::PostingRequest request{
        .company = lims::CompanyId{2},
        .date = {},
        .sourceKey = "billing.invoice:7",
        .sourceReference = "invoice 7",
        .currency = lims::CurrencyCode::JPY,
        .rate = lims::Rate{Rational{1, DecimalPlaces{6}}},
        .lines = {lims::PostingLine{.accountCode = "1200",
                                    .side = lims::PostingSide::Debit,
                                    .amount = lims::Amount{Rational{1100, DecimalPlaces{0}}},
                                    .companyAmount = lims::Amount{Rational{1100, DecimalPlaces{0}}}},
                  lims::PostingLine{.accountCode = "4000",
                                    .side = lims::PostingSide::Credit,
                                    .amount = lims::Amount{Rational{1100, DecimalPlaces{0}}},
                                    .companyAmount = lims::Amount{Rational{1100, DecimalPlaces{0}}}}},
    };
    auto const json = glz::write_json(request);
    REQUIRE(json.has_value());
    lims::PostingRequest decoded;
    REQUIRE_FALSE(glz::read_json(decoded, *json));
    REQUIRE(decoded.lines.size() == 2U);
    CHECK(decoded.company == lims::CompanyId{2});
    CHECK(decoded.sourceKey == "billing.invoice:7");
    CHECK(decoded.currency == lims::CurrencyCode::JPY);
    CHECK(decoded.lines[1].side == lims::PostingSide::Credit);
    CHECK(decoded.lines[1].accountCode == "4000");
    CHECK(*decoded.lines[0].amount == Rational{1100, DecimalPlaces{0}});
}

TEST_CASE("posting contract: a result names the entry", "[lims][posting]") {
    lims::PostingResult const result{.entryId = lims::JournalEntryId{9}};
    auto const json = glz::write_json(result);
    REQUIRE(json.has_value());
    CHECK(*json == R"({"entryId":9})");
}
```

- [ ] **Step 2: Run it and see it fail**

Run: Build lims. Expected: compile error, `lims/posting/posting.hpp` not found.

- [ ] **Step 3: Write the contract**

Create `examples/lims/posting/include/lims/posting/posting.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/util/datetime.hpp>
#include <string>
#include <vector>

#include "lims/org/core/money.hpp"
#include "lims/org/core/strong_id.hpp"
#include "lims/org/core/types.hpp"

/// @file
/// The posting contract: a poster (billing, and inventory later) maps its document to a `PostingRequest`; the ledger
/// validates and records it and answers a `PostingResult`. DTOs only. Neither side includes the other: the app hands
/// one to the other.

namespace lims {

LIMS_DEFINE_STRONG_ID(JournalEntryId);

/// @brief Which side of the entry a line is on.
enum class PostingSide : std::uint8_t {
    Debit,   ///< Debit.
    Credit,  ///< Credit.
};

/// @brief One line of a posting.
struct PostingLine {
    /// @brief The account's code in the company's chart, such as `1200`.
    std::string accountCode;
    /// @brief The side.
    PostingSide side = PostingSide::Debit;
    /// @brief The amount in the request's transaction currency; positive, or zero for an exchange-rounding line.
    Amount amount;
    /// @brief Its value in the company's currency, rounded by the poster to that currency's decimals.
    Amount companyAmount;
};

/// @brief One fact a poster asks the ledger to record.
struct PostingRequest {
    /// @brief The company whose books it goes in.
    CompanyId company;
    /// @brief The entry's date; it picks the period and the exchange rate.
    ::morph::time::Timestamp date;
    /// @brief The poster's unique key for the document, such as `billing.invoice:7`. The ledger records each key
    ///        once: a request repeated with the same key answers the entry the first one recorded.
    std::string sourceKey;
    /// @brief What the entry shows as its source, such as an invoice number.
    std::string sourceReference;
    /// @brief The transaction currency of every line's `amount`.
    CurrencyCode currency = CurrencyCode::EUR;
    /// @brief The rate the poster converted with: company-currency units per transaction-currency unit. One when the
    ///        currencies are the same; otherwise it must be the ledger's rate valid on `date`.
    Rate rate;
    /// @brief Two or more lines.
    std::vector<PostingLine> lines;
};

/// @brief What the ledger recorded.
struct PostingResult {
    /// @brief The journal entry.
    JournalEntryId entryId;
};

}  // namespace lims

LIMS_DEFINE_STRONG_ID_WIRE(JournalEntryId);
LIMS_DEFINE_STRONG_ID_SHAPE_TAG(JournalEntryId, "lims.journalEntryId");

/// @brief Reflects `PostingSide` as its enumerator name.
template <>
struct glz::meta<lims::PostingSide> {
    using enum lims::PostingSide;

    /// @brief The enumerator ⇄ name mapping glaze reads and writes.
    static constexpr auto value = glz::enumerate(Debit, Credit);
};
```

- [ ] **Step 4: Add the module to the rung and probe its boundary**

In `examples/lims/CMakeLists.txt`, change the call to:

```cmake
morph_add_rung(NAME lims
    MODULES org posting lab app
    SHARED_MODULES org posting
    APP_MODULE app)
```

and, below the three existing `_lims_boundary_probe` lines, add:

```cmake
    _lims_boundary_probe(posting_includes_lab posting fail lims/lab/models/sample_model.hpp)
    _lims_boundary_probe(lab_includes_posting lab pass lims/posting/posting.hpp)
```

Create `examples/lims/tests/compile_checks/posting_includes_lab.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Compiled as part of lims/posting, it includes a feature module's header and must fail to build: the posting
// contract is shared, so it may depend on org alone. Outside its test the macro is undefined and this is empty.
#ifdef LIMS_MODULE_BOUNDARY_PROBE
#include <lims/lab/models/sample_model.hpp>
#endif
```

and `examples/lims/tests/compile_checks/lab_includes_posting.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The control for posting_includes_lab: compiled as part of lims/lab, it includes the posting contract and must
// build, which shows a feature module reaches the second shared module. Outside its test the macro is undefined and
// this is empty.
#ifdef LIMS_MODULE_BOUNDARY_PROBE
#include <lims/posting/posting.hpp>
#endif
```

- [ ] **Step 5: Build and run**

Run: Configure, Build lims, Model suite `"[posting]"`, then Everything labelled for the rung. Expected: the two posting cases pass; `lims.module_boundary.posting_includes_lab` and `lims.module_boundary.lab_includes_posting` pass; nothing else changes.

- [ ] **Step 6: Update the Modules table and commit**

In `examples/lims/README.md`'s "Modules" table add the row `` | `posting/` (shared) | the posting contract: `PostingRequest` and `PostingResult`, DTOs only | `org` | `` after `org/`, and change `lab/`'s "Links" cell to `` `org`, `posting` ``.

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/posting examples/lims/CMakeLists.txt examples/lims/tests/compile_checks/posting_includes_lab.cpp \
        examples/lims/tests/compile_checks/lab_includes_posting.cpp examples/lims/tests/test_posting_contract.cpp \
        examples/lims/README.md
git commit -m "Add lims's posting contract as a shared module

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 3: Org — the business roles and value limits

**Files:**
- Create: `examples/lims/org/include/lims/org/dto/limit_dto.hpp`, `examples/lims/tests/test_value_limits.cpp`
- Modify: `examples/lims/org/include/lims/org/dto/role_dto.hpp` (three roles), `examples/lims/org/src/models/org_model.cpp` (`kMaxRole`, `SetValueLimit`, `ListValueLimits`, `valueLimit`, `findCompany`), `examples/lims/org/include/lims/org/models/org_model.hpp`, `examples/lims/org/include/lims/org/db/org_entity.hpp` (`ValueLimitRecord`), `examples/lims/org/src/db/org_schema.cpp` (migration), `examples/lims/org/include/lims/org/authorization/declared.hpp` (`CompanyRule`), `examples/lims/org/include/lims/org/authorization/org_authorization.hpp`, `examples/lims/app/include/lims/app/declared_requirements.hpp`, `examples/lims/tests/test_declared_authorization.cpp` (coverage tuple)

**Interfaces:**
- Consumes: `lims::Amount`, `lims::fitsCurrency`, `OrgModel::rolesIn`, `requireAuthorized`.
- Produces: `LimsRole::Accountant`, `LimsRole::BillingClerk`, `LimsRole::InvoiceApprover` (appended, so stored values of the first three are unchanged); `lims::ValueLimitId`; `struct SetValueLimit { std::string principal; CompanyId company; Amount limit; }`; `struct ValueLimitView { ValueLimitId id; std::string principal; CompanyId company; Amount limit; std::string setBy; Timestamp setAt; }`; `ListValueLimits` / `ListValueLimitsResult { std::vector<ValueLimitView> limits; }`; `static std::optional<Rational> OrgModel::valueLimit(std::string_view principal, CompanyId company)`; `static std::optional<CompanyView> OrgModel::findCompany(CompanyId)`; `template <typename Action, LimsRole Role> struct lims::CompanyRule` (role plus `scopeOf` reading `action.company`).

A value limit belongs to a principal at a company, in the company's base currency. Billing's approver uses it now and procurement's requisition approver in Part C, so it lives in org, beside the roles, rather than in either module.

- [ ] **Step 1: Write the failing test**

Create `examples/lims/tests/test_value_limits.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Value limits: up to what amount a principal may approve at a company, in its base currency.

#include <catch2/catch_test_macros.hpp>
#include <morph/util/rational.hpp>
#include <string>

#include "lims/org/core/errors.hpp"
#include "lims/org/dto/limit_dto.hpp"
#include "lims/org/models/org_model.hpp"
#include "lims_test_support.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;
using morph::math::DecimalPlaces;
using morph::math::Rational;

namespace {

/// @brief @p units whole units at two decimals.
/// @param units The units.
/// @return The amount.
[[nodiscard]] lims::Amount euros(std::int64_t units) { return lims::Amount{Rational{units, DecimalPlaces{2}}}; }

}  // namespace

TEST_CASE("value limits: the business roles are named and stored after the first three", "[lims][limits]") {
    CHECK(lims::roleName(lims::LimsRole::Accountant) == "accountant");
    CHECK(lims::roleName(lims::LimsRole::BillingClerk) == "billing-clerk");
    CHECK(lims::roleName(lims::LimsRole::InvoiceApprover) == "invoice-approver");
    CHECK(static_cast<int>(lims::LimsRole::Supervisor) == 2);
    CHECK(static_cast<int>(lims::LimsRole::InvoiceApprover) == 5);
}

// Mutation: let valueLimit ignore the company (bob's limit at the seeded company answers for Osaka too).
TEST_CASE("value limits: a supervisor sets a limit per principal and company, and setting it again replaces it",
          "[lims][limits]") {
    DbFixture fixture;
    lims::test::grant("alice", lims::LimsRole::Supervisor);
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;
    auto const osaka = org.execute(lims::CreateCompany{
        .name = "Osaka branch", .baseCurrency = lims::CurrencyCode::JPY, .fiscalYearStart = lims::Month::April});

    CHECK_FALSE(lims::OrgModel::valueLimit("bob", lims::test::seededCompany()).has_value());
    auto const first =
        org.execute(lims::SetValueLimit{.principal = "bob", .company = lims::test::seededCompany(), .limit = euros(500)});
    CHECK(first.setBy == "alice");
    org.execute(lims::SetValueLimit{.principal = "bob", .company = lims::test::seededCompany(), .limit = euros(800)});
    CHECK(lims::OrgModel::valueLimit("bob", lims::test::seededCompany()) == Rational{800, DecimalPlaces{2}});
    CHECK_FALSE(lims::OrgModel::valueLimit("bob", osaka.id).has_value());
    CHECK(org.execute(lims::ListValueLimits{}).limits.size() == 1U);
}

TEST_CASE("value limits: a limit must fit the company's currency and not be negative", "[lims][limits]") {
    DbFixture fixture;
    lims::test::grant("alice", lims::LimsRole::Supervisor);
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;
    auto const osaka = org.execute(lims::CreateCompany{
        .name = "Osaka branch", .baseCurrency = lims::CurrencyCode::JPY, .fiscalYearStart = lims::Month::April});
    CHECK_THROWS_AS(org.execute(lims::SetValueLimit{.principal = "bob",
                                                    .company = osaka.id,
                                                    .limit = lims::Amount{Rational{morph::math::Numerator{25},
                                                                                   morph::math::Denominator{2},
                                                                                   DecimalPlaces{1}}}}),
                    lims::ValidationError);
    CHECK_THROWS_AS(
        org.execute(lims::SetValueLimit{.principal = "bob", .company = osaka.id, .limit = euros(-1)}),
        lims::ValidationError);
    CHECK_THROWS_AS(org.execute(lims::SetValueLimit{.principal = "bob", .company = osaka.id, .limit = {}}),
                    lims::ValidationError);
}

TEST_CASE("value limits: only a supervisor of the company sets one", "[lims][limits][authorization]") {
    DbFixture fixture;
    lims::test::grant("alice", lims::LimsRole::Supervisor);
    lims::test::grant("carol", lims::LimsRole::InvoiceApprover);
    ScopedPrincipal const carol{"carol"};
    lims::OrgModel org;
    CHECK_THROWS_AS(org.execute(lims::SetValueLimit{
                        .principal = "carol", .company = lims::test::seededCompany(), .limit = euros(1)}),
                    lims::Forbidden);
}

TEST_CASE("value limits: findCompany answers a company's currency and fiscal-year start", "[lims][limits]") {
    DbFixture fixture;
    auto const seeded = lims::OrgModel::findCompany(lims::test::seededCompany());
    REQUIRE(seeded.has_value());
    CHECK(seeded->baseCurrency == lims::CurrencyCode::EUR);
    CHECK_FALSE(lims::OrgModel::findCompany(lims::CompanyId{999}).has_value());
    CHECK_FALSE(lims::OrgModel::findCompany(lims::CompanyId{}).has_value());
}
```

- [ ] **Step 2: Run it and see it fail**

Run: Build lims. Expected: compile error, `lims/org/dto/limit_dto.hpp` not found.

- [ ] **Step 3: Add the roles**

In `role_dto.hpp`, extend `LimsRole` (append only — the column stores the underlying integer):

```cpp
enum class LimsRole : std::uint8_t {
    Analyst,          ///< May capture results.
    Verifier,         ///< May verify somebody else's results.
    Supervisor,       ///< May grant and revoke roles, and set value limits.
    Accountant,       ///< May keep a company's ledger: accounts, periods, entries and rates.
    BillingClerk,     ///< May keep a company's billing: prices, clients, invoices and billing runs.
    InvoiceApprover,  ///< May approve somebody else's invoice up to their value limit.
};
```

add the three `case`s to `roleName` (`"accountant"`, `"billing-clerk"`, `"invoice-approver"`), and extend the `glz::enumerate` to `Analyst, Verifier, Supervisor, Accountant, BillingClerk, InvoiceApprover`. In `org_model.cpp` change `constexpr int kMaxRole = static_cast<int>(LimsRole::Supervisor);` to `constexpr int kMaxRole = static_cast<int>(LimsRole::InvoiceApprover);`.

- [ ] **Step 4: Write the DTOs, the record and the migration**

Create `examples/lims/org/include/lims/org/dto/limit_dto.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/records.hpp>
#include <morph/util/datetime.hpp>
#include <string>
#include <vector>

#include "lims/org/core/money.hpp"
#include "lims/org/core/strong_id.hpp"
#include "lims/org/core/types.hpp"

/// @file
/// Value limits: the amount, in a company's base currency, up to which a principal may approve there.

namespace lims {

LIMS_DEFINE_STRONG_ID(ValueLimitId);

/// @brief Sets a principal's value limit at a company, replacing any they had there.
struct SetValueLimit {
    /// @brief Whose limit.
    std::string principal;
    /// @brief The company.
    CompanyId company;
    /// @brief The limit, in the company's base currency.
    Amount limit;

    /// @brief Whether the request is well-formed.
    /// @return `true` when a principal of at most 64 bytes, a company and a limit are named.
    [[nodiscard]] bool validate() const noexcept {
        return !principal.empty() && principal.size() <= 64 && company.hasValue() && limit.hasValue();
    }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief One principal's limit at one company.
struct ValueLimitView {
    /// @brief Its id.
    ValueLimitId id;
    /// @brief Whose limit.
    std::string principal;
    /// @brief The company.
    CompanyId company;
    /// @brief The limit, in the company's base currency.
    Amount limit;
    /// @brief Who set it.
    std::string setBy;
    /// @brief When.
    ::morph::time::Timestamp setAt;
};

/// @brief Lists the value limits at the companies the caller can see.
struct ListValueLimits {
    /// @brief Always ready.
    /// @return `true`.
    [[nodiscard]] bool validate() const noexcept { return true; }
};

/// @brief The limits, by id.
struct ListValueLimitsResult {
    /// @brief The limits.
    std::vector<ValueLimitView> limits;
};

}  // namespace lims

LIMS_DEFINE_STRONG_ID_WIRE(ValueLimitId);
LIMS_DEFINE_STRONG_ID_SHAPE_TAG(ValueLimitId, "lims.valueLimitId");

BRIDGE_REGISTER_RECORD(lims::ValueLimitView, "Org_ValueLimit")
```

In `org_entity.hpp`, after `RoleAssignmentRecord`, add:

```cpp
/// @brief One principal's value limit at one company: at most one row per pair.
struct ValueLimitRecord {
    static constexpr std::string_view TableName = "org_value_limits";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"principal"}> principal;                     // 1
    Light::BelongsTo<&CompanyRecord::id, Light::SqlRealName{"company_id"}> company;                        // 2
    Light::Field<std::int64_t, Light::SqlRealName{"limit_num"}> limitNum{0};                               // 3
    Light::Field<std::int64_t, Light::SqlRealName{"limit_den"}> limitDen{1};                               // 4
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"set_by"}> setBy;                            // 5
    Light::Field<std::int64_t, Light::SqlRealName{"set_at"}> setAt{0};                                     // 6
};
```

In `org_schema.cpp`, inside the NOLINT block, add:

```cpp
LIGHTWEIGHT_SQL_MIGRATION(20261110000001, "Create org_value_limits") {
    plan.CreateTableIfNotExists("org_value_limits")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("principal", Varchar(64))
        .RequiredForeignKey("company_id", Bigint(),
                            SqlForeignKeyReferenceDefinition{.tableName = "org_companies", .columnName = "id"})
        .RequiredColumn("limit_num", Bigint())
        .RequiredColumn("limit_den", Bigint())
        .RequiredColumn("set_by", Varchar(64))
        .RequiredColumn("set_at", Bigint());
}
```

- [ ] **Step 5: Declare the rule, with the company-scope helper**

In `declared.hpp`, before `struct AuthorizationFacts`, add:

```cpp
/// @brief A rule needing @p Role at the company the action's `company` field names: the shape most ERP actions have.
/// @tparam Action An action with a `CompanyId company` member.
/// @tparam Role The role.
template <typename Action, LimsRole Role>
struct CompanyRule {
    /// @brief The role.
    static constexpr LimsRole role = Role;

    /// @brief The company the payload names.
    /// @param action The action.
    /// @return Its company.
    [[nodiscard]] static Scope scopeOf(Action const& action) { return Scope{action.company}; }
};
```

In `org_authorization.hpp` add `#include "lims/org/dto/limit_dto.hpp"` and:

```cpp
/// @brief A value limit is set by a supervisor at its company.
template <>
struct Authorization<SetValueLimit> : CompanyRule<SetValueLimit, LimsRole::Supervisor> {};
```

- [ ] **Step 6: Implement the model additions**

In `org_model.hpp` add `#include "lims/org/dto/limit_dto.hpp"`, and these members after `findSite`:

```cpp
    /// @brief Sets a principal's value limit at a company, replacing any they had there.
    /// @param action The principal, the company and the limit.
    /// @return The limit as stored.
    /// @throws ValidationError if the request is malformed, or the limit is negative or does not fit the company's
    ///         currency.
    /// @throws NotFound if the company does not exist.
    /// @throws Forbidden if the caller is not a supervisor at the company.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    ValueLimitView execute(const SetValueLimit& action);

    /// @brief Lists the value limits at the companies the caller can see.
    /// @param action Carries nothing.
    /// @return The limits, by id.
    ListValueLimitsResult execute(const ListValueLimits& action);

    /// @brief @p principal's value limit at @p company.
    /// @param principal The principal.
    /// @param company The company.
    /// @return The limit in the company's base currency; nothing when they have none.
    [[nodiscard]] static std::optional<::morph::math::Rational> valueLimit(std::string_view principal,
                                                                          CompanyId company);

    /// @brief The company with @p company's id, for a module that keeps books in its currency.
    /// @param company The company.
    /// @return It, or nothing when @p company is empty or names no company.
    [[nodiscard]] static std::optional<CompanyView> findCompany(CompanyId company);
```

and at the bottom, inside the NOLINT block:

```cpp
BRIDGE_REGISTER_ACTION(lims::OrgModel, lims::SetValueLimit, "SetValueLimit")
BRIDGE_REGISTER_EFFECTS(lims::SetValueLimit, ::morph::effects::changes<lims::ValueLimitView>)
BRIDGE_REGISTER_ACTION(lims::OrgModel, lims::ListValueLimits, "ListValueLimits", ::morph::model::Loggable::No)
```

In `org_model.cpp` add, in the anonymous namespace:

```cpp
/// @brief Maps a value-limit row to its view, its limit at @p currency's decimals.
/// @param row The row.
/// @param currency The company's base currency.
/// @return The view.
[[nodiscard]] ValueLimitView toView(db::ValueLimitRecord const& row, CurrencyCode currency) {
    return ValueLimitView{
        .id = ValueLimitId{static_cast<std::int64_t>(row.id.Value())},
        .principal = std::string{row.principal.Value().ToStringView()},
        .company = CompanyId{static_cast<std::int64_t>(row.company.Value())},
        .limit = Amount{::morph::math::Rational{::morph::math::Numerator{row.limitNum.Value()},
                                                ::morph::math::Denominator{row.limitDen.Value()},
                                                ::morph::math::DecimalPlaces{decimalsOf(currency)}}},
        .setBy = std::string{row.setBy.Value().ToStringView()},
        .setAt = timestampFromMillis(row.setAt.Value()),
    };
}
```

and the members:

```cpp
std::optional<CompanyView> OrgModel::findCompany(CompanyId company) {
    if (!company.hasValue()) {
        return std::nullopt;
    }
    Lightweight::DataMapper mapper;
    auto const rows = mapper.Query<db::CompanyRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::CompanyRecord::id>, "=", *company)
                          .All();
    if (rows.empty()) {
        return std::nullopt;
    }
    return toView(rows.front());
}

ValueLimitView OrgModel::execute(const SetValueLimit& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"SetValueLimit: a principal, a company and a limit are required"};
    }
    requireRoleSomewhere<SetValueLimit>();
    auto const company = findCompany(action.company);
    if (!company.has_value()) {
        throw NotFound{"SetValueLimit: company " + std::to_string(*action.company) + " does not exist"};
    }
    requireAuthorized(action);
    auto const& limit = *action.limit;
    if (limit.isNegative() || !fitsCurrency(limit, company->baseCurrency)) {
        throw ValidationError{"SetValueLimit: a limit is a non-negative amount in " +
                              std::string{currencyName(company->baseCurrency)}};
    }
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    auto rows = mapper.Query<db::ValueLimitRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::ValueLimitRecord::principal>, "=", action.principal)
                    .Where(::Lightweight::FieldNameOf<&db::ValueLimitRecord::company>, "=", *action.company)
                    .All();
    db::ValueLimitRecord row = rows.empty() ? db::ValueLimitRecord{} : rows.front();
    row.principal = Lightweight::SqlAnsiString<64>{action.principal};
    row.company = static_cast<std::uint64_t>(*action.company);
    row.limitNum = limit.numerator;
    row.limitDen = limit.denominator;
    row.setBy = Lightweight::SqlAnsiString<64>{::morph::session::current()->principal};
    row.setAt = nowMillis();
    if (rows.empty()) {
        mapper.Create(row);
    } else {
        mapper.Update(row);
    }
    sqlTxn.Commit();
    return toView(row, company->baseCurrency);
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ListValueLimitsResult OrgModel::execute(const ListValueLimits& action) {
    static_cast<void>(action);
    auto const visible = visibleCompanies(currentPrincipal());
    Lightweight::DataMapper mapper;
    ListValueLimitsResult result;
    for (auto const& row : mapper.Query<db::ValueLimitRecord>().All()) {
        CompanyId const company{static_cast<std::int64_t>(row.company.Value())};
        if (std::ranges::find(visible, company) == visible.end()) {
            continue;
        }
        auto const view = findCompany(company);
        result.limits.push_back(toView(row, view.has_value() ? view->baseCurrency : CurrencyCode::EUR));
    }
    std::ranges::sort(result.limits, {}, [](ValueLimitView const& limit) { return *limit.id; });
    return result;
}

std::optional<::morph::math::Rational> OrgModel::valueLimit(std::string_view principal, CompanyId company) {
    if (!company.hasValue()) {
        return std::nullopt;
    }
    Lightweight::DataMapper mapper;
    auto const rows = mapper.Query<db::ValueLimitRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::ValueLimitRecord::principal>, "=",
                                 std::string{principal})
                          .Where(::Lightweight::FieldNameOf<&db::ValueLimitRecord::company>, "=", *company)
                          .All();
    if (rows.empty()) {
        return std::nullopt;
    }
    return ::morph::math::Rational{::morph::math::Numerator{rows.front().limitNum.Value()},
                                   ::morph::math::Denominator{rows.front().limitDen.Value()},
                                   ::morph::math::DecimalPlaces{2}};
}
```

Add `#include "lims/org/core/money.hpp"` to `org_model.cpp`. `Rational`'s equality compares the value, not its decimals tag (inferred from `rational.hpp`'s exact `operator<=>`; if `==` also compares the tag, construct the expected value in the test with the same tag the model returns).

- [ ] **Step 7: Put the action in the edge table and the coverage case**

In `declared_requirements.hpp`, raise the array to `6` and add `requirementOf<SetValueLimit>()`. In `test_declared_authorization.cpp`'s coverage case, add `lims::SetValueLimit` to `Mutating`. In `"the edge's table lists every declared role"` add `CHECK(lims::app::requiredRole("SetValueLimit") == lims::LimsRole::Supervisor);`.

- [ ] **Step 8: Build and run**

Run: Build lims, Model suite `"[limits]"`, then Model suite `"[authorization]"`, then Screen suite. Expected: all pass (the org access screen lists roles by name, so the new roles need no screen change). Run the mutation above the second case and restore it.

- [ ] **Step 9: Commit**

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/org examples/lims/app/include/lims/app/declared_requirements.hpp \
        examples/lims/tests/test_value_limits.cpp examples/lims/tests/test_declared_authorization.cpp
git commit -m "Add lims's business roles and per-company value limits

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---
### Task 4: Ledger — the module, the chart of accounts and periods

**Files:**
- Create: `examples/lims/org/include/lims/org/core/calendar.hpp`, `examples/lims/ledger/include/.clang-tidy`, `examples/lims/ledger/include/lims/ledger/core/types.hpp`, `examples/lims/ledger/include/lims/ledger/dto/account_dto.hpp`, `examples/lims/ledger/include/lims/ledger/db/ledger_entity.hpp`, `examples/lims/ledger/include/lims/ledger/models/ledger_model.hpp`, `examples/lims/ledger/include/lims/ledger/authorization/ledger_authorization.hpp`, `examples/lims/ledger/src/db/ledger_schema.cpp`, `examples/lims/ledger/src/models/ledger_model.cpp`, `examples/lims/ledger/src/authorization/ledger_authorization.cpp`, `examples/lims/tests/test_ledger_chart.cpp`, `examples/lims/tests/compile_checks/ledger_includes_lab.cpp`
- Modify: `examples/lims/CMakeLists.txt`, `examples/lims/tests/lims_test_support.hpp` (`at`, `signedAs`, `limsAuthorizer`), `examples/lims/app/include/lims/app/declared_requirements.hpp`, `examples/lims/tests/test_declared_authorization.cpp`, `examples/lims/README.md` (Modules table)

**Interfaces:**
- Consumes: `lims::Month`, `lims::CompanyId`, `OrgModel::findCompany`, `OrgModel::visibleCompanies`, `CompanyRule`, `requireRoleSomewhere`, `requireAuthorized`.
- Produces: `struct lims::CalendarMonth { std::int32_t year; Month month; }`, `CalendarMonth lims::calendarMonthOf(Timestamp const&)`, `std::int64_t lims::monthStartMillis(CalendarMonth)`, `std::string lims::monthLabel(CalendarMonth)`, `std::int32_t lims::fiscalYearOf(Timestamp const&, Month fiscalYearStart)`, `std::uint32_t lims::dayOfMonth(Timestamp const&)`, `std::int64_t lims::millisOf(Timestamp const&)`; `lims::AccountId`, `lims::PeriodId`; `enum class AccountType { Asset, Liability, Equity, Revenue, Expense }`; `enum class PeriodState { Open, Closing, Closed }`; actions `CreateAccount { CompanyId company; std::string code; std::string name; AccountType type; }` → `AccountView`, `ListAccounts { CompanyId company; }` → `ListAccountsResult`, `OpenPeriod { CompanyId company; Timestamp month; }` → `PeriodView { PeriodId id; CompanyId company; Timestamp start; std::string label; PeriodState state; }`, `ListPeriods { CompanyId company; }` → `ListPeriodsResult`, `BeginClosePeriod { PeriodId periodId; }` → `PeriodView`; model `lims::LedgerModel` (unkeyed, `"LedgerModel"`); records `Ledger_Account`, `Ledger_Period`; target `ladder_lims_ledger`; test helpers `lims::test::at(y, m, d, h = 9)`, `lims::test::signedAs(issuer, principal)`, `lims::test::limsAuthorizer(secret)`.

Dates are UTC instants (`morph::time::Timestamp`); an entry's or a period's month is its UTC calendar month. A fiscal year is named by the calendar year it begins in: with an April start, 10 February 2027 is in fiscal year 2026.

- [ ] **Step 1: Add the test helpers**

Append to `examples/lims/tests/lims_test_support.hpp`, inside `namespace lims::test` (add `#include <chrono>`, `#include <cstdint>`, `#include <memory>`, `#include <morph/session/session_auth.hpp>`, `#include <morph/util/datetime.hpp>` and `#include "lims/app/lims_authorizer.hpp"`):

```cpp
/// @brief A UTC instant on a calendar day.
/// @param year The year.
/// @param month The month, 1 to 12.
/// @param day The day.
/// @param hour The hour.
/// @return The instant.
[[nodiscard]] inline morph::time::DateTime at(int year, unsigned month, unsigned day, int hour = 9) {
    return morph::time::DateTime{std::chrono::year{year}, std::chrono::month{month}, std::chrono::day{day},
                                 std::chrono::hours{hour}, std::chrono::minutes{0}, std::chrono::seconds{0}};
}

/// @brief A signed session for @p principal.
/// @param issuer The issuer.
/// @param principal The principal.
/// @return The context, with the token and the principal set.
[[nodiscard]] inline morph::session::Context signedAs(morph::session::TokenIssuer const& issuer,
                                                      std::string principal) {
    morph::session::Context ctx;
    ctx.principal = principal;
    ctx.token = issuer.issue(morph::session::SessionToken{
        .principal = std::move(principal), .issuedAtMs = 0, .expiresAtMs = 4102444800000, .roles = {}});
    return ctx;
}

/// @brief The server's authorizer, reading roles where production does.
/// @param secret The token secret.
/// @return The authorizer.
[[nodiscard]] inline std::shared_ptr<lims::auth::LimsAuthorizer> limsAuthorizer(std::string_view secret) {
    return std::make_shared<lims::auth::LimsAuthorizer>(
        std::string{secret}, [](std::string_view principal) { return lims::OrgModel::rolesAnywhere(principal); });
}
```

`test_declared_authorization.cpp` and `test_visibility.cpp` define their own `signedAs` in an anonymous namespace; leave them, since `lims::test::signedAs` does not collide with a file-local one.

- [ ] **Step 2: Write the failing test**

Create `examples/lims/tests/test_ledger_chart.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The ledger's chart of accounts and its monthly periods, per company.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <morph/session/session_auth.hpp>
#include <string>
#include <string_view>

#include "lims/ledger/models/ledger_model.hpp"
#include "lims/org/core/calendar.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/models/org_model.hpp"
#include "lims_test_support.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::testkit::awaitQt;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;

namespace {

constexpr std::string_view kSecret = "lims-ledger-chart-test-secret-at-least-32b";

/// @brief Creates account @p code of @p type at the seeded company, as the current principal.
/// @param ledger The model.
/// @param code The code.
/// @param type The type.
/// @return The account.
lims::AccountView account(lims::LedgerModel& ledger, std::string code, lims::AccountType type) {
    return ledger.execute(lims::CreateAccount{
        .company = lims::test::seededCompany(), .code = code, .name = "Account " + code, .type = type});
}

}  // namespace

TEST_CASE("calendar: months, labels and fiscal years are UTC calendar facts", "[lims][ledger]") {
    auto const feb = morph::time::Timestamp{lims::test::at(2027, 2, 10)};
    CHECK(lims::monthLabel(lims::calendarMonthOf(feb)) == "2027-02");
    CHECK(lims::dayOfMonth(feb) == 10U);
    CHECK(lims::fiscalYearOf(feb, lims::Month::April) == 2026);
    CHECK(lims::fiscalYearOf(feb, lims::Month::January) == 2027);
    CHECK(lims::fiscalYearOf(morph::time::Timestamp{lims::test::at(2026, 4, 1, 0)}, lims::Month::April) == 2026);
    CHECK(lims::monthStartMillis(lims::calendarMonthOf(feb)) ==
          lims::millisOf(morph::time::Timestamp{lims::test::at(2027, 2, 1, 0)}));
}

// Mutation: drop the per-company filter from ListAccounts (dave, an accountant at Osaka only, sees the seeded
// company's accounts).
TEST_CASE("ledger chart: an accountant keeps accounts per company, each code once", "[lims][ledger]") {
    DbFixture fixture;
    lims::test::grant("alice", lims::LimsRole::Accountant);
    lims::test::grant("alice", lims::LimsRole::Supervisor);
    lims::LedgerModel ledger;
    lims::CompanyId osaka;
    {
        ScopedPrincipal const alice{"alice"};
        osaka = lims::OrgModel{}
                    .execute(lims::CreateCompany{.name = "Osaka branch",
                                                 .baseCurrency = lims::CurrencyCode::JPY,
                                                 .fiscalYearStart = lims::Month::April})
                    .id;
        account(ledger, "1200", lims::AccountType::Asset);
        account(ledger, "2100", lims::AccountType::Liability);
        account(ledger, "3000", lims::AccountType::Equity);
        account(ledger, "4000", lims::AccountType::Revenue);
        account(ledger, "6000", lims::AccountType::Expense);
        CHECK_THROWS_AS(account(ledger, "1200", lims::AccountType::Asset), lims::Conflict);
        CHECK_THROWS_AS(account(ledger, "", lims::AccountType::Asset), lims::ValidationError);
        auto const listed = ledger.execute(lims::ListAccounts{.company = lims::test::seededCompany()});
        REQUIRE(listed.accounts.size() == 5U);
        CHECK(listed.accounts.front().code == "1200");
        CHECK(listed.accounts.back().type == lims::AccountType::Expense);
        lims::OrgModel{}.execute(lims::AssignRole{
            .principal = "dave", .role = lims::LimsRole::Accountant, .company = osaka, .site = {}});
    }
    ScopedPrincipal const dave{"dave"};
    CHECK(ledger.execute(lims::ListAccounts{.company = lims::test::seededCompany()}).accounts.empty());
    CHECK_THROWS_AS(account(ledger, "1300", lims::AccountType::Asset), lims::Forbidden);
}

// Mutation: let BeginClosePeriod accept a Closing period (the second begin succeeds).
TEST_CASE("ledger chart: a month is opened once and moves from open to closing", "[lims][ledger]") {
    DbFixture fixture;
    lims::test::grant("alice", lims::LimsRole::Accountant);
    ScopedPrincipal const alice{"alice"};
    lims::LedgerModel ledger;
    auto const opened = ledger.execute(lims::OpenPeriod{.company = lims::test::seededCompany(),
                                                        .month = morph::time::Timestamp{lims::test::at(2026, 11, 17)}});
    CHECK(opened.label == "2026-11");
    CHECK(opened.state == lims::PeriodState::Open);
    CHECK(opened.start == morph::time::Timestamp{lims::test::at(2026, 11, 1, 0)});
    CHECK_THROWS_AS(ledger.execute(lims::OpenPeriod{.company = lims::test::seededCompany(),
                                                    .month = morph::time::Timestamp{lims::test::at(2026, 11, 2)}}),
                    lims::Conflict);
    auto const closing = ledger.execute(lims::BeginClosePeriod{.periodId = opened.id});
    CHECK(closing.state == lims::PeriodState::Closing);
    CHECK_THROWS_AS(ledger.execute(lims::BeginClosePeriod{.periodId = opened.id}), lims::IllegalTransition);
    CHECK_THROWS_AS(ledger.execute(lims::BeginClosePeriod{.periodId = lims::PeriodId{999}}), lims::NotFound);
    CHECK(ledger.execute(lims::ListPeriods{.company = lims::test::seededCompany()}).periods.size() == 1U);
}

TEST_CASE("LedgerModel over the full backend-mode matrix: an account and a period", "[lims][ledger][matrix]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    lims::test::grant("alice", lims::LimsRole::Accountant);
    morph::session::TokenIssuer const issuer{std::string{kSecret}, morph::session::hmacSha256};
    BackendRig rig{mode, 1, lims::test::limsAuthorizer(kSecret)};
    rig.bridge(0).setDefaultSession(lims::test::signedAs(issuer, "alice"));
    auto ledger = rig.client<lims::LedgerModel>(0);
    auto const created = awaitQt(ledger.execute(lims::CreateAccount{.company = lims::test::seededCompany(),
                                                                    .code = "1200",
                                                                    .name = "Receivables",
                                                                    .type = lims::AccountType::Asset}));
    CHECK(created.code == "1200");
    auto const period = awaitQt(ledger.execute(lims::OpenPeriod{
        .company = lims::test::seededCompany(), .month = morph::time::Timestamp{lims::test::at(2026, 11, 1)}}));
    CHECK(period.label == "2026-11");
    CHECK(awaitQt(ledger.execute(lims::ListAccounts{.company = lims::test::seededCompany()})).accounts.size() == 1U);
}
```

- [ ] **Step 3: Run it and see it fail**

Run: Build lims. Expected: compile error, `lims/ledger/models/ledger_model.hpp` not found.

- [ ] **Step 4: The calendar helpers (org)**

Create `examples/lims/org/include/lims/org/core/calendar.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <compare>
#include <cstdint>
#include <format>
#include <morph/util/datetime.hpp>
#include <string>

#include "lims/org/core/types.hpp"

/// @file
/// The calendar facts the books need: a UTC instant's month, the label it is shown under, and the fiscal year it
/// falls in for a company whose fiscal year starts in a given month.

namespace lims {

/// @brief A calendar month: a period of the books.
struct CalendarMonth {
    /// @brief The year.
    std::int32_t year = 1970;
    /// @brief The month.
    Month month = Month::January;

    /// @brief Chronological order.
    /// @param other The other month.
    /// @return The ordering.
    auto operator<=>(CalendarMonth const& other) const = default;
};

/// @brief Milliseconds since the epoch of an engaged @p at; 0 for an empty one.
/// @param at The instant.
/// @return The milliseconds.
[[nodiscard]] inline std::int64_t millisOf(::morph::time::Timestamp const& at) {
    return at.hasValue() ? (*at).value.time_since_epoch().count() : 0;
}

/// @brief The UTC calendar day of @p at.
/// @param at The instant.
/// @return Its year, month and day.
[[nodiscard]] inline std::chrono::year_month_day calendarDayOf(::morph::time::Timestamp const& at) {
    auto const instant = std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{millisOf(at)}};
    return std::chrono::year_month_day{std::chrono::floor<std::chrono::days>(instant)};
}

/// @brief The month @p at falls in, in UTC.
/// @param at The instant.
/// @return The month.
[[nodiscard]] inline CalendarMonth calendarMonthOf(::morph::time::Timestamp const& at) {
    auto const day = calendarDayOf(at);
    return CalendarMonth{.year = static_cast<std::int32_t>(day.year()),
                         .month = static_cast<Month>(static_cast<unsigned>(day.month()) - 1U)};
}

/// @brief The day of the month @p at falls on, in UTC.
/// @param at The instant.
/// @return 1 to 31.
[[nodiscard]] inline std::uint32_t dayOfMonth(::morph::time::Timestamp const& at) {
    return static_cast<unsigned>(calendarDayOf(at).day());
}

/// @brief The first instant of @p month, in milliseconds since the epoch.
/// @param month The month.
/// @return Midnight UTC on its first day.
[[nodiscard]] inline std::int64_t monthStartMillis(CalendarMonth month) {
    auto const first = std::chrono::year_month_day{std::chrono::year{month.year},
                                                   std::chrono::month{static_cast<unsigned>(month.month) + 1U},
                                                   std::chrono::day{1}};
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::sys_days{first}.time_since_epoch())
        .count();
}

/// @brief The label a period is shown under.
/// @param month The month.
/// @return `YYYY-MM`.
[[nodiscard]] inline std::string monthLabel(CalendarMonth month) {
    return std::format("{:04}-{:02}", month.year, static_cast<unsigned>(month.month) + 1U);
}

/// @brief The fiscal year @p at falls in, named by the calendar year it begins in.
/// @param at The instant.
/// @param fiscalYearStart The month the company's fiscal year starts.
/// @return The year: with an April start, February 2027 is in fiscal year 2026.
[[nodiscard]] inline std::int32_t fiscalYearOf(::morph::time::Timestamp const& at, Month fiscalYearStart) {
    auto const month = calendarMonthOf(at);
    return month.month < fiscalYearStart ? month.year - 1 : month.year;
}

}  // namespace lims
```

- [ ] **Step 5: Ledger types, DTOs, records, migration**

Create `examples/lims/ledger/include/.clang-tidy` — `org/include/.clang-tidy` verbatim with its first comment line reading "The lims ledger module's header tree holds Lightweight records, …".

Create `examples/lims/ledger/include/lims/ledger/core/types.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <glaze/glaze.hpp>

#include "lims/org/core/strong_id.hpp"
#include "lims/posting/posting.hpp"

/// @file
/// The ledger's identities and closed sets.

namespace lims {

LIMS_DEFINE_STRONG_ID(AccountId);
LIMS_DEFINE_STRONG_ID(PeriodId);
LIMS_DEFINE_STRONG_ID(ExchangeRateId);

/// @brief What an account records.
enum class AccountType : std::uint8_t {
    Asset,      ///< What the company owns or is owed.
    Liability,  ///< What it owes.
    Equity,     ///< What its owners hold.
    Revenue,    ///< What it earns.
    Expense,    ///< What it spends.
};

/// @brief Where a month of the books stands.
enum class PeriodState : std::uint8_t {
    Open,     ///< Entries are drafted and posted.
    Closing,  ///< No new drafts; postings and the drafts already there may still be posted.
    Closed,   ///< Frozen: nothing more is recorded in it.
};

/// @brief Where a journal entry stands.
enum class EntryState : std::uint8_t {
    Draft,   ///< Being prepared by hand; not in any balance.
    Posted,  ///< In the books, immutable.
};

}  // namespace lims

LIMS_DEFINE_STRONG_ID_WIRE(AccountId);
LIMS_DEFINE_STRONG_ID_WIRE(PeriodId);
LIMS_DEFINE_STRONG_ID_WIRE(ExchangeRateId);
LIMS_DEFINE_STRONG_ID_SHAPE_TAG(AccountId, "lims.accountId");
LIMS_DEFINE_STRONG_ID_SHAPE_TAG(PeriodId, "lims.periodId");
LIMS_DEFINE_STRONG_ID_SHAPE_TAG(ExchangeRateId, "lims.exchangeRateId");

/// @brief Reflects `AccountType` as its enumerator name.
template <>
struct glz::meta<lims::AccountType> {
    using enum lims::AccountType;

    /// @brief The enumerator ⇄ name mapping glaze reads and writes.
    static constexpr auto value = glz::enumerate(Asset, Liability, Equity, Revenue, Expense);
};

/// @brief Reflects `PeriodState` as its enumerator name.
template <>
struct glz::meta<lims::PeriodState> {
    using enum lims::PeriodState;

    /// @brief The enumerator ⇄ name mapping glaze reads and writes.
    static constexpr auto value = glz::enumerate(Open, Closing, Closed);
};

/// @brief Reflects `EntryState` as its enumerator name.
template <>
struct glz::meta<lims::EntryState> {
    using enum lims::EntryState;

    /// @brief The enumerator ⇄ name mapping glaze reads and writes.
    static constexpr auto value = glz::enumerate(Draft, Posted);
};
```

Create `examples/lims/ledger/include/lims/ledger/dto/account_dto.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/records.hpp>
#include <morph/util/datetime.hpp>
#include <string>
#include <vector>

#include "lims/ledger/core/types.hpp"
#include "lims/org/core/types.hpp"

/// @file
/// The chart of accounts and the periods, as the wire sees them.

namespace lims {

/// @brief Adds an account to a company's chart.
struct CreateAccount {
    /// @brief The company.
    CompanyId company;
    /// @brief Its code, unique in the company: 1 to 16 letters and digits.
    std::string code;
    /// @brief Its name.
    std::string name;
    /// @brief What it records.
    AccountType type = AccountType::Asset;

    /// @brief Whether the account is well-formed.
    /// @return `true` when a company, a code of 1 to 16 letters and digits and a name of 1 to 128 bytes are named.
    [[nodiscard]] bool validate() const noexcept {
        bool const codeOk = !code.empty() && code.size() <= 16 && std::ranges::all_of(code, [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        });
        return company.hasValue() && codeOk && !name.empty() && name.size() <= 128;
    }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief One account.
struct AccountView {
    /// @brief Its id.
    AccountId id;
    /// @brief The company.
    CompanyId company;
    /// @brief Its code.
    std::string code;
    /// @brief Its name.
    std::string name;
    /// @brief What it records.
    AccountType type = AccountType::Asset;
};

/// @brief Lists a company's accounts.
struct ListAccounts {
    /// @brief The company.
    CompanyId company;

    /// @brief Whether a company is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return company.hasValue(); }
};

/// @brief The accounts, by code.
struct ListAccountsResult {
    /// @brief The accounts.
    std::vector<AccountView> accounts;
};

/// @brief Opens the month an instant falls in, for a company.
struct OpenPeriod {
    /// @brief The company.
    CompanyId company;
    /// @brief Any instant in the month.
    ::morph::time::Timestamp month;

    /// @brief Whether the request is well-formed.
    /// @return `true` when a company and an instant are named.
    [[nodiscard]] bool validate() const noexcept { return company.hasValue() && month.hasValue(); }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief One month of a company's books.
struct PeriodView {
    /// @brief Its id.
    PeriodId id;
    /// @brief The company.
    CompanyId company;
    /// @brief Midnight UTC on the month's first day.
    ::morph::time::Timestamp start;
    /// @brief `YYYY-MM`.
    std::string label;
    /// @brief Where it stands.
    PeriodState state = PeriodState::Open;
};

/// @brief Lists a company's periods.
struct ListPeriods {
    /// @brief The company.
    CompanyId company;

    /// @brief Whether a company is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return company.hasValue(); }
};

/// @brief The periods, by start.
struct ListPeriodsResult {
    /// @brief The periods.
    std::vector<PeriodView> periods;
};

/// @brief `Open → Closing`: no new drafts while the month is reviewed.
struct BeginClosePeriod {
    /// @brief The period.
    PeriodId periodId;

    /// @brief Whether a period is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return periodId.hasValue(); }
};

}  // namespace lims

BRIDGE_REGISTER_RECORD(lims::AccountView, "Ledger_Account")
BRIDGE_REGISTER_RECORD(lims::PeriodView, "Ledger_Period")
```

(add `#include <algorithm>` for `std::ranges::all_of`).

Create `examples/lims/ledger/include/lims/ledger/db/ledger_entity.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <cstdint>
#include <optional>
#include <string_view>

/// @file
/// The ledger's tables, prefixed `ledger_`. Companies are org's: a company id is stored as a plain column, since no
/// module reads another's tables.

namespace lims::db {

/// @brief One account of one company's chart.
struct AccountRecord {
    static constexpr std::string_view TableName = "ledger_accounts";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"company_id"}> companyId{0};                             // 1
    Light::Field<Light::SqlAnsiString<16>, Light::SqlRealName{"code"}> code;                               // 2
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"name"}> name;                              // 3
    /// @brief `AccountType` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"type"}> type{0};  // 4
};

/// @brief One month of one company's books.
struct PeriodRecord {
    static constexpr std::string_view TableName = "ledger_periods";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"company_id"}> companyId{0};                             // 1
    /// @brief Midnight UTC on the month's first day, in epoch milliseconds.
    Light::Field<std::int64_t, Light::SqlRealName{"start_ms"}> startMs{0};  // 2
    /// @brief `PeriodState` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"state"}> state{0};  // 3
};

}  // namespace lims::db
```

Create `examples/lims/ledger/src/db/ledger_schema.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <Lightweight/Lightweight.hpp>
#include <Lightweight/SqlMigration.hpp>

using namespace Lightweight::SqlColumnTypeDefinitions;
using Lightweight::SqlForeignKeyReferenceDefinition;

// The macro names a struct and a static object whose constructor registers the migration, so neither can be const,
// local to an anonymous namespace, or free of a throwing initializer.
// NOLINTBEGIN(bugprone-throwing-static-initialization,cert-err58-cpp,misc-use-internal-linkage,misc-use-anonymous-namespace,cppcoreguidelines-avoid-non-const-global-variables)
LIGHTWEIGHT_SQL_MIGRATION(20261110000002, "Create ledger_accounts and ledger_periods") {
    plan.CreateTableIfNotExists("ledger_accounts")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("company_id", Bigint())
        .RequiredColumn("code", Varchar(16))
        .RequiredColumn("name", Varchar(128))
        .RequiredColumn("type", Integer());
    plan.CreateTableIfNotExists("ledger_periods")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("company_id", Bigint())
        .RequiredColumn("start_ms", Bigint())
        .RequiredColumn("state", Integer());
}
// NOLINTEND(bugprone-throwing-static-initialization,cert-err58-cpp,misc-use-internal-linkage,misc-use-anonymous-namespace,cppcoreguidelines-avoid-non-const-global-variables)
```

The company id is a plain column, not a foreign key into `org_companies`: the ledger reaches companies through `OrgModel` only. (The SQLite file would accept the foreign key; leaving it out keeps "no module reads another's tables" true of the schema as well as the code.)

- [ ] **Step 6: The model**

Create `examples/lims/ledger/include/lims/ledger/models/ledger_model.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/registry.hpp>

#include "lims/ledger/dto/account_dto.hpp"

/// @file
/// `LedgerModel`: a company's chart of accounts, its periods, its journal entries and its exchange rates.

namespace lims {

/// @brief The books of every company.
///
/// Unkeyed, holding no connection: each `execute()` opens a `Lightweight::DataMapper` for its own duration. Knows
/// nothing of invoices or stock: a poster's document reaches it as a `PostingRequest`.
class LedgerModel {
public:
    /// @brief Adds an account to a company's chart.
    /// @param action The company, code, name and type.
    /// @return The account.
    /// @throws ValidationError if the account is malformed.
    /// @throws NotFound if the company does not exist.
    /// @throws Conflict if the company's chart already has the code.
    /// @throws Forbidden if the caller is not an accountant at the company.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    AccountView execute(const CreateAccount& action);

    /// @brief Lists a company's accounts, when the caller can see the company.
    /// @param action The company.
    /// @return Its accounts by code; none for a company the caller cannot see.
    /// @throws ValidationError if no company is named, or a row holds a type outside its enumeration.
    ListAccountsResult execute(const ListAccounts& action);

    /// @brief Opens the month an instant falls in.
    /// @param action The company and an instant in the month.
    /// @return The period, open.
    /// @throws ValidationError if the request is malformed.
    /// @throws NotFound if the company does not exist.
    /// @throws Conflict if the month is already a period of the company.
    /// @throws Forbidden if the caller is not an accountant at the company.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    PeriodView execute(const OpenPeriod& action);

    /// @brief Lists a company's periods, when the caller can see the company.
    /// @param action The company.
    /// @return Its periods by start.
    /// @throws ValidationError if no company is named, or a row holds a state outside its enumeration.
    ListPeriodsResult execute(const ListPeriods& action);

    /// @brief `Open → Closing`.
    /// @param action The period.
    /// @return The period, closing.
    /// @throws ValidationError if no period is named.
    /// @throws NotFound if it does not exist.
    /// @throws IllegalTransition if it is not open.
    /// @throws Forbidden if the caller is not an accountant at its company.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    PeriodView execute(const BeginClosePeriod& action);
};

}  // namespace lims

// The registration macros declare a non-const status object each.
// NOLINTBEGIN(misc-const-correctness)
BRIDGE_REGISTER_MODEL(lims::LedgerModel, "LedgerModel")
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::CreateAccount, "CreateAccount")
BRIDGE_REGISTER_EFFECTS(lims::CreateAccount, ::morph::effects::changes<lims::AccountView>)
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::ListAccounts, "ListAccounts", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::OpenPeriod, "OpenPeriod")
BRIDGE_REGISTER_EFFECTS(lims::OpenPeriod, ::morph::effects::changes<lims::PeriodView>)
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::ListPeriods, "ListPeriods", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::BeginClosePeriod, "BeginClosePeriod")
BRIDGE_REGISTER_EFFECTS(lims::BeginClosePeriod, ::morph::effects::updates<lims::PeriodView>)
// NOLINTEND(misc-const-correctness)
```

Create `examples/lims/ledger/include/lims/ledger/authorization/ledger_authorization.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lims/ledger/dto/account_dto.hpp"
#include "lims/org/authorization/declared.hpp"

/// @file
/// The ledger module's declared authorization: keeping a company's books is an accountant's, at that company.

namespace lims {

/// @brief An account is added by an accountant at its company.
template <>
struct Authorization<CreateAccount> : CompanyRule<CreateAccount, LimsRole::Accountant> {};

/// @brief A month is opened by an accountant at its company.
template <>
struct Authorization<OpenPeriod> : CompanyRule<OpenPeriod, LimsRole::Accountant> {};

/// @brief A month's close is begun by an accountant at the period's company.
template <>
struct Authorization<BeginClosePeriod> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::Accountant;

    /// @brief The company of the period the payload names.
    /// @param action The action.
    /// @return It; an empty company when there is no such period.
    [[nodiscard]] static Scope scopeOf(BeginClosePeriod const& action);
};

}  // namespace lims
```

Create `examples/lims/ledger/src/authorization/ledger_authorization.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "lims/ledger/authorization/ledger_authorization.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <cstdint>

#include "lims/ledger/db/ledger_entity.hpp"

namespace lims {

namespace {

/// @brief The company of period @p period, or an empty company.
/// @param period The period.
/// @return The scope.
[[nodiscard]] Scope periodCompany(PeriodId period) {
    if (!period.hasValue()) {
        return Scope{CompanyId{}};
    }
    Lightweight::DataMapper mapper;
    auto const rows = mapper.Query<db::PeriodRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::PeriodRecord::id>, "=", *period)
                          .All();
    return rows.empty() ? Scope{CompanyId{}} : Scope{CompanyId{rows.front().companyId.Value()}};
}

}  // namespace

Scope Authorization<BeginClosePeriod>::scopeOf(BeginClosePeriod const& action) {
    return periodCompany(action.periodId);
}

}  // namespace lims
```

Create `examples/lims/ledger/src/models/ledger_model.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "lims/ledger/models/ledger_model.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <algorithm>
#include <cstdint>
#include <string>

#include "lims/ledger/authorization/ledger_authorization.hpp"
#include "lims/ledger/db/ledger_entity.hpp"
#include "lims/org/core/calendar.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/core/model_support.hpp"
#include "lims/org/models/org_model.hpp"

namespace lims {

namespace {

constexpr int kMaxAccountType = static_cast<int>(AccountType::Expense);
constexpr int kMaxPeriodState = static_cast<int>(PeriodState::Closed);

/// @brief Maps an account row to its view.
/// @param row The row.
/// @return The view.
/// @throws ValidationError if the row stores a type outside its enumeration.
[[nodiscard]] AccountView toView(db::AccountRecord const& row) {
    int const type = row.type.Value();
    if (type < 0 || type > kMaxAccountType) {
        throw ValidationError{"account " + std::to_string(row.id.Value()) + " stores a type outside its range"};
    }
    return AccountView{.id = AccountId{static_cast<std::int64_t>(row.id.Value())},
                       .company = CompanyId{row.companyId.Value()},
                       .code = std::string{row.code.Value().ToStringView()},
                       .name = std::string{row.name.Value().ToStringView()},
                       .type = static_cast<AccountType>(type)};
}

/// @brief Maps a period row to its view.
/// @param row The row.
/// @return The view.
/// @throws ValidationError if the row stores a state outside its enumeration.
[[nodiscard]] PeriodView toView(db::PeriodRecord const& row) {
    int const state = row.state.Value();
    if (state < 0 || state > kMaxPeriodState) {
        throw ValidationError{"period " + std::to_string(row.id.Value()) + " stores a state outside its range"};
    }
    auto const start = timestampFromMillis(row.startMs.Value());
    return PeriodView{.id = PeriodId{static_cast<std::int64_t>(row.id.Value())},
                      .company = CompanyId{row.companyId.Value()},
                      .start = start,
                      .label = monthLabel(calendarMonthOf(start)),
                      .state = static_cast<PeriodState>(state)};
}

/// @brief Throws `NotFound` unless @p company exists.
/// @param where The action, for the message.
/// @param company The company.
void requireCompany(std::string_view where, CompanyId company) {
    if (!OrgModel::findCompany(company).has_value()) {
        throw NotFound{std::string{where} + ": company " + std::to_string(*company) + " does not exist"};
    }
}

/// @brief Whether the caller can see @p company.
/// @param company The company.
/// @return Whether a role of theirs reaches it.
[[nodiscard]] bool visible(CompanyId company) {
    auto const companies = OrgModel::visibleCompanies(currentPrincipal());
    return std::ranges::find(companies, company) != companies.end();
}

}  // namespace

AccountView LedgerModel::execute(const CreateAccount& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"CreateAccount: a company, a code of 1 to 16 letters and digits and a name are required"};
    }
    requireRoleSomewhere<CreateAccount>();
    requireCompany("CreateAccount", action.company);
    requireAuthorized(action);
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    if (!mapper.Query<db::AccountRecord>()
             .Where(::Lightweight::FieldNameOf<&db::AccountRecord::companyId>, "=", *action.company)
             .Where(::Lightweight::FieldNameOf<&db::AccountRecord::code>, "=", action.code)
             .All()
             .empty()) {
        throw Conflict{"CreateAccount: company " + std::to_string(*action.company) + " already has account " +
                       action.code};
    }
    db::AccountRecord row;
    row.companyId = *action.company;
    row.code = Lightweight::SqlAnsiString<16>{action.code};
    row.name = Lightweight::SqlAnsiString<128>{action.name};
    row.type = static_cast<int>(action.type);
    mapper.Create(row);
    sqlTxn.Commit();
    return toView(row);
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ListAccountsResult LedgerModel::execute(const ListAccounts& action) {
    if (!action.validate()) {
        throw ValidationError{"ListAccounts: a company is required"};
    }
    ListAccountsResult result;
    if (!visible(action.company)) {
        return result;
    }
    Lightweight::DataMapper mapper;
    for (auto const& row : mapper.Query<db::AccountRecord>()
                               .Where(::Lightweight::FieldNameOf<&db::AccountRecord::companyId>, "=", *action.company)
                               .All()) {
        result.accounts.push_back(toView(row));
    }
    std::ranges::sort(result.accounts, {}, &AccountView::code);
    return result;
}

PeriodView LedgerModel::execute(const OpenPeriod& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"OpenPeriod: a company and a month are required"};
    }
    requireRoleSomewhere<OpenPeriod>();
    requireCompany("OpenPeriod", action.company);
    requireAuthorized(action);
    auto const month = calendarMonthOf(action.month);
    std::int64_t const start = monthStartMillis(month);
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    if (!mapper.Query<db::PeriodRecord>()
             .Where(::Lightweight::FieldNameOf<&db::PeriodRecord::companyId>, "=", *action.company)
             .Where(::Lightweight::FieldNameOf<&db::PeriodRecord::startMs>, "=", start)
             .All()
             .empty()) {
        throw Conflict{"OpenPeriod: " + monthLabel(month) + " is already a period of company " +
                       std::to_string(*action.company)};
    }
    db::PeriodRecord row;
    row.companyId = *action.company;
    row.startMs = start;
    row.state = static_cast<int>(PeriodState::Open);
    mapper.Create(row);
    sqlTxn.Commit();
    return toView(row);
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ListPeriodsResult LedgerModel::execute(const ListPeriods& action) {
    if (!action.validate()) {
        throw ValidationError{"ListPeriods: a company is required"};
    }
    ListPeriodsResult result;
    if (!visible(action.company)) {
        return result;
    }
    Lightweight::DataMapper mapper;
    for (auto const& row : mapper.Query<db::PeriodRecord>()
                               .Where(::Lightweight::FieldNameOf<&db::PeriodRecord::companyId>, "=", *action.company)
                               .All()) {
        result.periods.push_back(toView(row));
    }
    std::ranges::sort(result.periods, {}, [](PeriodView const& period) { return period.start; });
    return result;
}

PeriodView LedgerModel::execute(const BeginClosePeriod& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"BeginClosePeriod: a period is required"};
    }
    requireRoleSomewhere<BeginClosePeriod>();
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    auto rows = mapper.Query<db::PeriodRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::PeriodRecord::id>, "=", *action.periodId)
                    .All();
    if (rows.empty()) {
        throw NotFound{"BeginClosePeriod: period " + std::to_string(*action.periodId) + " does not exist"};
    }
    requireAuthorized(action);
    auto& row = rows.front();
    if (row.state.Value() != static_cast<int>(PeriodState::Open)) {
        throw IllegalTransition{"BeginClosePeriod: period " + toView(row).label + " is not open"};
    }
    row.state = static_cast<int>(PeriodState::Closing);
    mapper.Update(row);
    sqlTxn.Commit();
    return toView(row);
}

}  // namespace lims
```

- [ ] **Step 7: Build the module, probe its boundary, declare its rules**

In `examples/lims/CMakeLists.txt` set `MODULES org posting lab ledger app` and add `_lims_boundary_probe(ledger_includes_lab ledger fail lims/lab/models/sample_model.hpp)`. Create `tests/compile_checks/ledger_includes_lab.cpp` in `posting_includes_lab.cpp`'s shape, its comment reading "Compiled as part of lims/ledger, it includes lab's header and must fail to build: the ledger knows no other feature module." In `declared_requirements.hpp` raise the array by three and add `requirementOf<CreateAccount>()`, `requirementOf<OpenPeriod>()`, `requirementOf<BeginClosePeriod>()` (include `lims/ledger/authorization/ledger_authorization.hpp`). In the coverage case add the three actions to `Mutating` and `"LedgerModel"` to the model list the dispatcher check walks. Add a `ledger/` row to the README's Modules table: `` | `ledger/` | accounts, periods, journal entries, exchange rates and the reports | `org`, `posting` | ``.

- [ ] **Step 8: Build and run**

Run: Configure, Build lims, Model suite `"[ledger]"`, Model suite `"[authorization]"`, Everything labelled for the rung. Expected: all pass, `lims.module_boundary.ledger_includes_lab` included. Run both mutations named in the test file and restore them.

- [ ] **Step 9: Commit**

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/ledger examples/lims/org/include/lims/org/core/calendar.hpp examples/lims/CMakeLists.txt \
        examples/lims/tests examples/lims/app/include/lims/app/declared_requirements.hpp examples/lims/README.md
git commit -m "Add lims's ledger module with a chart of accounts and monthly periods

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 5: Ledger — journal entries, balancing, reversal, recording a posting, closing a period

**Files:**
- Create: `examples/lims/ledger/include/lims/ledger/core/balance.hpp`, `examples/lims/ledger/src/core/balance.cpp`, `examples/lims/ledger/include/lims/ledger/dto/entry_dto.hpp`, `examples/lims/ledger/src/models/ledger_entries.cpp`, `examples/lims/tests/test_ledger_entries.cpp`
- Modify: `ledger_entity.hpp` (four records), `ledger_schema.cpp` (migration), `ledger_model.hpp` (actions, `record`), `ledger_authorization.hpp`/`.cpp`, `declared_requirements.hpp`, `test_declared_authorization.cpp`

**Interfaces:**
- Consumes: `PostingRequest`, `PostingLine`, `PostingResult`, `ExactSum`, `fitsCurrency`, `calendarMonthOf`, `monthStartMillis`, `morph::offline::IReplayLedger`.
- Produces: `std::optional<std::string> lims::entryProblem(std::span<PostingLine const> lines, CurrencyCode transaction, CurrencyCode company)`; `struct JournalEntryView { JournalEntryId id; CompanyId company; PeriodId period; Timestamp date; EntryState state; std::string sourceKey; std::string sourceReference; std::string memo; CurrencyCode currency; Rate rate; JournalEntryId reverses; JournalEntryId reversedBy; std::vector<PostingLine> lines; std::string createdBy; std::string postedBy; }`; actions `CreateDraftEntry { CompanyId company; Timestamp date; std::string memo; CurrencyCode currency; Rate rate; std::vector<PostingLine> lines; }`, `PostEntry { JournalEntryId entryId; }`, `ReverseEntry { JournalEntryId entryId; Timestamp date; }`, `GetEntry { JournalEntryId entryId; }`, `ListEntries { CompanyId company; }` → `ListEntriesResult`, `ClosePeriod { PeriodId periodId; }` → `PeriodView`; `static PostingResult LedgerModel::record(PostingRequest const& request)` (not an action); record `Ledger_Entry`.

`record()` is deliberately not a registered action. A posting is a fact another module's document produces, and only the app's port (Task 14) hands it over; a client that could send `RecordPosting` could put any entry in the books under an invoice's name. The ledger records each `sourceKey` once per company through an `IReplayLedger` over `ledger_posting_sources`, written in the entry's own transaction, so a poster that retries gets the first entry back.

- [ ] **Step 1: Write the failing test**

Create `examples/lims/tests/test_ledger_entries.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Journal entries: balanced per currency and in company currency, never rounded by the ledger, immutable once posted,
// corrected by reversal; a posting recorded once per source key; a period closed only without drafts.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <vector>

#include "lims/ledger/core/balance.hpp"
#include "lims/ledger/models/ledger_model.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/models/org_model.hpp"
#include "lims_test_support.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;
using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

namespace {

/// @brief @p minor minor units at @p places decimals.
/// @param minor The minor units.
/// @param places The currency's decimals.
/// @return The amount.
[[nodiscard]] lims::Amount money(std::int64_t minor, std::uint32_t places = 2) {
    std::int64_t const scale = places == 0U ? 1 : 100;
    return lims::Amount{Rational{Numerator{minor}, Denominator{scale}, DecimalPlaces{places}}};
}

/// @brief A line whose company amount equals its amount.
/// @param code The account.
/// @param side The side.
/// @param minor The amount in minor units.
/// @return The line.
[[nodiscard]] lims::PostingLine line(std::string code, lims::PostingSide side, std::int64_t minor) {
    return lims::PostingLine{.accountCode = std::move(code), .side = side, .amount = money(minor),
                             .companyAmount = money(minor)};
}

/// @brief The rate one.
/// @return It.
[[nodiscard]] lims::Rate one() { return lims::Rate{Rational{1, DecimalPlaces{6}}}; }

/// @brief The books every case starts from: alice an accountant at the seeded company (EUR), its chart and
///        November 2026 open.
/// @return November's period.
lims::PeriodView books() {
    lims::test::grant("alice", lims::LimsRole::Accountant);
    ScopedPrincipal const alice{"alice"};
    lims::LedgerModel ledger;
    for (auto const& [code, type] : std::vector<std::pair<std::string, lims::AccountType>>{
             {"1200", lims::AccountType::Asset},
             {"2200", lims::AccountType::Liability},
             {"4000", lims::AccountType::Revenue},
             {"6900", lims::AccountType::Expense}}) {
        ledger.execute(lims::CreateAccount{
            .company = lims::test::seededCompany(), .code = code, .name = "Account " + code, .type = type});
    }
    return ledger.execute(lims::OpenPeriod{.company = lims::test::seededCompany(),
                                           .month = morph::time::Timestamp{lims::test::at(2026, 11, 1)}});
}

/// @brief A posting of @p minor from receivables to revenue on 10 November 2026.
/// @param key The source key.
/// @param minor The amount.
/// @return The request.
[[nodiscard]] lims::PostingRequest posting(std::string key, std::int64_t minor) {
    return lims::PostingRequest{
        .company = lims::test::seededCompany(),
        .date = morph::time::Timestamp{lims::test::at(2026, 11, 10)},
        .sourceKey = key,
        .sourceReference = key,
        .currency = lims::CurrencyCode::EUR,
        .rate = one(),
        .lines = {line("1200", lims::PostingSide::Debit, minor), line("4000", lims::PostingSide::Credit, minor)}};
}

}  // namespace

// Mutation: skip the company-currency sum in entryProblem (the 1-cent company difference passes).
TEST_CASE("ledger entries: an entry balances per transaction currency and in company currency, unrounded",
          "[lims][ledger]") {
    std::vector<lims::PostingLine> balanced{line("1200", lims::PostingSide::Debit, 1000),
                                            line("4000", lims::PostingSide::Credit, 1000)};
    CHECK_FALSE(lims::entryProblem(balanced, lims::CurrencyCode::EUR, lims::CurrencyCode::EUR).has_value());

    std::vector<lims::PostingLine> oneLine{line("1200", lims::PostingSide::Debit, 1000)};
    CHECK(lims::entryProblem(oneLine, lims::CurrencyCode::EUR, lims::CurrencyCode::EUR).has_value());

    std::vector<lims::PostingLine> unbalanced{line("1200", lims::PostingSide::Debit, 1000),
                                              line("4000", lims::PostingSide::Credit, 999)};
    CHECK(lims::entryProblem(unbalanced, lims::CurrencyCode::EUR, lims::CurrencyCode::EUR).has_value());

    // USD lines in a EUR company: the USD amounts balance, the EUR values are one cent apart.
    std::vector<lims::PostingLine> companyOff{
        {.accountCode = "1200", .side = lims::PostingSide::Debit, .amount = money(1000), .companyAmount = money(921)},
        {.accountCode = "4000", .side = lims::PostingSide::Credit, .amount = money(1000), .companyAmount = money(920)}};
    auto const problem = lims::entryProblem(companyOff, lims::CurrencyCode::USD, lims::CurrencyCode::EUR);
    REQUIRE(problem.has_value());
    CHECK_THAT(*problem, Catch::Matchers::ContainsSubstring("company currency"));

    // An unrounded company value is refused, not rounded: the ledger never rounds.
    std::vector<lims::PostingLine> unrounded{
        {.accountCode = "1200",
         .side = lims::PostingSide::Debit,
         .amount = money(1000),
         .companyAmount = lims::Amount{Rational{Numerator{92051}, Denominator{10000}, DecimalPlaces{4}}}},
        {.accountCode = "4000",
         .side = lims::PostingSide::Credit,
         .amount = money(1000),
         .companyAmount = lims::Amount{Rational{Numerator{92051}, Denominator{10000}, DecimalPlaces{4}}}}};
    CHECK(lims::entryProblem(unrounded, lims::CurrencyCode::USD, lims::CurrencyCode::EUR).has_value());

    // JPY has no minor unit.
    std::vector<lims::PostingLine> fractionalYen{
        {.accountCode = "1200", .side = lims::PostingSide::Debit, .amount = money(1050), .companyAmount = money(1050)},
        {.accountCode = "4000", .side = lims::PostingSide::Credit, .amount = money(1050), .companyAmount = money(1050)}};
    CHECK(lims::entryProblem(fractionalYen, lims::CurrencyCode::JPY, lims::CurrencyCode::JPY).has_value());
}

// Mutation: let PostEntry update a posted entry (the second post succeeds).
TEST_CASE("ledger entries: a draft is posted once and is then immutable; a correction is a reversal naming it",
          "[lims][ledger]") {
    DbFixture fixture;
    auto const november = books();
    ScopedPrincipal const alice{"alice"};
    lims::LedgerModel ledger;
    auto const draft = ledger.execute(lims::CreateDraftEntry{
        .company = lims::test::seededCompany(),
        .date = morph::time::Timestamp{lims::test::at(2026, 11, 12)},
        .memo = "accrual",
        .currency = lims::CurrencyCode::EUR,
        .rate = one(),
        .lines = {line("6900", lims::PostingSide::Debit, 5000), line("2200", lims::PostingSide::Credit, 5000)}});
    CHECK(draft.state == lims::EntryState::Draft);
    CHECK(draft.period == november.id);
    auto const posted = ledger.execute(lims::PostEntry{.entryId = draft.id});
    CHECK(posted.state == lims::EntryState::Posted);
    CHECK(posted.postedBy == "alice");
    CHECK_THROWS_AS(ledger.execute(lims::PostEntry{.entryId = draft.id}), lims::IllegalTransition);

    auto const reversal = ledger.execute(
        lims::ReverseEntry{.entryId = draft.id, .date = morph::time::Timestamp{lims::test::at(2026, 11, 20)}});
    CHECK(reversal.state == lims::EntryState::Posted);
    CHECK(reversal.reverses == draft.id);
    REQUIRE(reversal.lines.size() == 2U);
    CHECK(reversal.lines[0].side == lims::PostingSide::Credit);
    CHECK(ledger.execute(lims::GetEntry{.entryId = draft.id}).reversedBy == reversal.id);
    CHECK_THROWS_AS(ledger.execute(lims::ReverseEntry{.entryId = draft.id,
                                                      .date = morph::time::Timestamp{lims::test::at(2026, 11, 21)}}),
                    lims::Conflict);
}

// Mutation: drop the replay-ledger lookup from record (the retried posting makes a second entry).
TEST_CASE("ledger entries: a posting is recorded once per source key, and a retry answers the first entry",
          "[lims][ledger]") {
    DbFixture fixture;
    books();
    auto const first = lims::LedgerModel::record(posting("billing.invoice:1", 11900));
    auto const retried = lims::LedgerModel::record(posting("billing.invoice:1", 11900));
    CHECK(retried.entryId == first.entryId);
    ScopedPrincipal const alice{"alice"};
    CHECK(lims::LedgerModel{}.execute(lims::ListEntries{.company = lims::test::seededCompany()}).entries.size() ==
          1U);
    CHECK_THROWS_AS(lims::LedgerModel::record(posting("billing.invoice:2", 0)), lims::ValidationError);
    auto unknown = posting("billing.invoice:3", 100);
    unknown.lines[0].accountCode = "9999";
    CHECK_THROWS_AS(lims::LedgerModel::record(unknown), lims::NotFound);
    auto noPeriod = posting("billing.invoice:4", 100);
    noPeriod.date = morph::time::Timestamp{lims::test::at(2026, 12, 1)};
    CHECK_THROWS_AS(lims::LedgerModel::record(noPeriod), lims::NotFound);
}

// Mutation: skip the draft check in ClosePeriod (the period closes with a draft in it).
TEST_CASE("ledger entries: a period closes only without drafts, and nothing is recorded in it afterwards",
          "[lims][ledger]") {
    DbFixture fixture;
    auto const november = books();
    lims::LedgerModel::record(posting("billing.invoice:1", 1000));
    ScopedPrincipal const alice{"alice"};
    lims::LedgerModel ledger;
    auto const draft = ledger.execute(lims::CreateDraftEntry{
        .company = lims::test::seededCompany(),
        .date = morph::time::Timestamp{lims::test::at(2026, 11, 12)},
        .memo = "accrual",
        .currency = lims::CurrencyCode::EUR,
        .rate = one(),
        .lines = {line("6900", lims::PostingSide::Debit, 500), line("2200", lims::PostingSide::Credit, 500)}});
    ledger.execute(lims::BeginClosePeriod{.periodId = november.id});
    // Closing: no new drafts, postings still recorded.
    CHECK_THROWS_AS(ledger.execute(lims::CreateDraftEntry{
                        .company = lims::test::seededCompany(),
                        .date = morph::time::Timestamp{lims::test::at(2026, 11, 13)},
                        .memo = "late",
                        .currency = lims::CurrencyCode::EUR,
                        .rate = one(),
                        .lines = {line("6900", lims::PostingSide::Debit, 1), line("2200", lims::PostingSide::Credit, 1)}}),
                    lims::Conflict);
    lims::LedgerModel::record(posting("billing.invoice:2", 2000));
    CHECK_THROWS_WITH(ledger.execute(lims::ClosePeriod{.periodId = november.id}),
                      Catch::Matchers::ContainsSubstring("draft"));
    ledger.execute(lims::PostEntry{.entryId = draft.id});
    auto const closed = ledger.execute(lims::ClosePeriod{.periodId = november.id});
    CHECK(closed.state == lims::PeriodState::Closed);
    CHECK_THROWS_AS(lims::LedgerModel::record(posting("billing.invoice:3", 3000)), lims::Conflict);
    CHECK_THROWS_AS(ledger.execute(lims::ReverseEntry{.entryId = draft.id,
                                                      .date = morph::time::Timestamp{lims::test::at(2026, 11, 30)}}),
                    lims::Conflict);
}
```

- [ ] **Step 2: Run it and see it fail**

Run: Build lims. Expected: compile error, `lims/ledger/core/balance.hpp` not found.

- [ ] **Step 3: The balance rule**

Create `examples/lims/ledger/include/lims/ledger/core/balance.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <span>
#include <string>

#include "lims/posting/posting.hpp"

/// @file
/// The one rule an entry must meet before it is recorded. Pure: the same lines always give the same answer.

namespace lims {

/// @brief Why @p lines cannot be recorded, if they cannot.
///
/// An entry needs two or more lines, each with a non-negative amount in @p transaction's minor units and a value in
/// @p company's minor units; the debits equal the credits in each; and when the currencies are the same, each line's
/// two amounts are equal. Nothing is rounded: a value with more decimals than its currency is refused.
/// @param lines The lines.
/// @param transaction The transaction currency.
/// @param company The company's currency.
/// @return The reason, in words; nothing when the lines may be recorded.
[[nodiscard]] std::optional<std::string> entryProblem(std::span<PostingLine const> lines, CurrencyCode transaction,
                                                      CurrencyCode company);

}  // namespace lims
```

Create `examples/lims/ledger/src/core/balance.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "lims/ledger/core/balance.hpp"

namespace lims {

std::optional<std::string> entryProblem(std::span<PostingLine const> lines, CurrencyCode transaction,
                                        CurrencyCode company) {
    if (lines.size() < 2U) {
        return std::string{"an entry needs two or more lines"};
    }
    ExactSum debit;
    ExactSum credit;
    ExactSum companyDebit;
    ExactSum companyCredit;
    for (PostingLine const& line : lines) {
        if (!line.amount.hasValue() || !line.companyAmount.hasValue()) {
            return std::string{"line on account " + line.accountCode + " has no amount"};
        }
        auto const& amount = *line.amount;
        auto const& value = *line.companyAmount;
        if (amount.isNegative() || value.isNegative()) {
            return std::string{"line on account " + line.accountCode + " is negative; its side says which way"};
        }
        if (!fitsCurrency(amount, transaction)) {
            return std::string{"line on account " + line.accountCode + " has more decimals than " +
                               std::string{currencyName(transaction)}};
        }
        if (!fitsCurrency(value, company)) {
            return std::string{"line on account " + line.accountCode + " is not rounded to company currency " +
                               std::string{currencyName(company)}};
        }
        if (transaction == company && amount != value) {
            return std::string{"line on account " + line.accountCode + " values its amount differently in its own "
                                                                       "currency"};
        }
        bool const isDebit = line.side == PostingSide::Debit;
        if (!(isDebit ? debit : credit).add(amount) || !(isDebit ? companyDebit : companyCredit).add(value)) {
            return std::string{"the entry is too large to total exactly"};
        }
    }
    if (debit.total() != credit.total()) {
        return std::string{"debits and credits differ in " + std::string{currencyName(transaction)}};
    }
    if (companyDebit.total() != companyCredit.total()) {
        return std::string{"debits and credits differ in company currency " + std::string{currencyName(company)}};
    }
    if (debit.total().isZero()) {
        return std::string{"an entry moves no amount"};
    }
    return std::nullopt;
}

}  // namespace lims
```

- [ ] **Step 4: DTOs, records, migration**

Create `examples/lims/ledger/include/lims/ledger/dto/entry_dto.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/records.hpp>
#include <morph/util/datetime.hpp>
#include <string>
#include <vector>

#include "lims/ledger/core/types.hpp"
#include "lims/posting/posting.hpp"

/// @file
/// Journal entries as the wire sees them. An entry's lines are `PostingLine`s: the same shape a poster sends.

namespace lims {

/// @brief One journal entry.
struct JournalEntryView {
    /// @brief Its id.
    JournalEntryId id;
    /// @brief The company.
    CompanyId company;
    /// @brief The period its date falls in.
    PeriodId period;
    /// @brief Its date.
    ::morph::time::Timestamp date;
    /// @brief Draft or posted.
    EntryState state = EntryState::Draft;
    /// @brief The poster's key; empty for an entry made by hand.
    std::string sourceKey;
    /// @brief What it shows as its source.
    std::string sourceReference;
    /// @brief A note.
    std::string memo;
    /// @brief The transaction currency.
    CurrencyCode currency = CurrencyCode::EUR;
    /// @brief The rate its company values were converted with.
    Rate rate;
    /// @brief The entry it reverses; empty unless it is a reversal.
    JournalEntryId reverses;
    /// @brief The entry that reverses it; empty unless it was reversed.
    JournalEntryId reversedBy;
    /// @brief Its lines, in order.
    std::vector<PostingLine> lines;
    /// @brief Who made it.
    std::string createdBy;
    /// @brief Who posted it; empty while a draft.
    std::string postedBy;
};

/// @brief Drafts an entry by hand.
struct CreateDraftEntry {
    /// @brief The company.
    CompanyId company;
    /// @brief Its date.
    ::morph::time::Timestamp date;
    /// @brief A note, at most 255 bytes.
    std::string memo;
    /// @brief The transaction currency.
    CurrencyCode currency = CurrencyCode::EUR;
    /// @brief The rate its company values were converted with.
    Rate rate;
    /// @brief Two or more lines.
    std::vector<PostingLine> lines;

    /// @brief Whether the draft is well-formed.
    /// @return `true` when a company, a date, a rate and two or more lines are named and the memo fits.
    [[nodiscard]] bool validate() const noexcept {
        return company.hasValue() && date.hasValue() && rate.hasValue() && lines.size() >= 2U && memo.size() <= 255;
    }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief Posts a draft: it enters the balances and can no longer change.
struct PostEntry {
    /// @brief The entry.
    JournalEntryId entryId;

    /// @brief Whether an entry is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return entryId.hasValue(); }
};

/// @brief Corrects a posted entry with a posted reversal on @p date that names it.
struct ReverseEntry {
    /// @brief The entry.
    JournalEntryId entryId;
    /// @brief The reversal's date.
    ::morph::time::Timestamp date;

    /// @brief Whether an entry and a date are named.
    /// @return `true` when they are.
    [[nodiscard]] bool validate() const noexcept { return entryId.hasValue() && date.hasValue(); }
};

/// @brief Reads one entry.
struct GetEntry {
    /// @brief The entry.
    JournalEntryId entryId;

    /// @brief Whether an entry is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return entryId.hasValue(); }
};

/// @brief Lists a company's entries.
struct ListEntries {
    /// @brief The company.
    CompanyId company;

    /// @brief Whether a company is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return company.hasValue(); }
};

/// @brief The entries, by id.
struct ListEntriesResult {
    /// @brief The entries.
    std::vector<JournalEntryView> entries;
};

/// @brief `Closing → Closed`: freezes the period's balances. Refused while a draft remains in it.
struct ClosePeriod {
    /// @brief The period.
    PeriodId periodId;

    /// @brief Whether a period is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return periodId.hasValue(); }
};

}  // namespace lims

BRIDGE_REGISTER_RECORD(lims::JournalEntryView, "Ledger_Entry")
```

Append to `ledger_entity.hpp`:

```cpp
/// @brief One journal entry's header.
struct EntryRecord {
    static constexpr std::string_view TableName = "ledger_entries";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"company_id"}> companyId{0};                             // 1
    Light::BelongsTo<&PeriodRecord::id, Light::SqlRealName{"period_id"}> period;                           // 2
    Light::Field<std::int64_t, Light::SqlRealName{"date_ms"}> dateMs{0};                                   // 3
    /// @brief `EntryState` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"state"}> state{0};                                                  // 4
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"source_key"}> sourceKey;                      // 5
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"source_reference"}> sourceReference;          // 6
    Light::Field<Light::SqlAnsiString<255>, Light::SqlRealName{"memo"}> memo;                                 // 7
    /// @brief `CurrencyCode` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"currency"}> currency{0};                                            // 8
    Light::Field<std::int64_t, Light::SqlRealName{"rate_num"}> rateNum{1};                                    // 9
    Light::Field<std::int64_t, Light::SqlRealName{"rate_den"}> rateDen{1};                                    // 10
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"reverses_id"}> reversesId;                  // 11
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"reversed_by_id"}> reversedById;             // 12
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"created_by"}> createdBy;                       // 13
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"posted_by"}> postedBy;                         // 14
};

/// @brief One line of one entry.
struct EntryLineRecord {
    static constexpr std::string_view TableName = "ledger_entry_lines";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&EntryRecord::id, Light::SqlRealName{"entry_id"}> entry;                              // 1
    Light::Field<int, Light::SqlRealName{"position"}> position{0};                                         // 2
    Light::BelongsTo<&AccountRecord::id, Light::SqlRealName{"account_id"}> account;                        // 3
    /// @brief `PostingSide` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"side"}> side{0};                             // 4
    Light::Field<std::int64_t, Light::SqlRealName{"amount_num"}> amountNum{0};         // 5
    Light::Field<std::int64_t, Light::SqlRealName{"amount_den"}> amountDen{1};         // 6
    Light::Field<std::int64_t, Light::SqlRealName{"company_num"}> companyNum{0};       // 7
    Light::Field<std::int64_t, Light::SqlRealName{"company_den"}> companyDen{1};       // 8
};

/// @brief A poster's source key, recorded once per company with the entry it produced.
struct PostingSourceRecord {
    static constexpr std::string_view TableName = "ledger_posting_sources";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<Light::SqlAnsiString<32>, Light::SqlRealName{"scope"}> scope;                             // 1
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"source_key"}> sourceKey;                   // 2
    Light::Field<Light::SqlAnsiString<32>, Light::SqlRealName{"entry"}> entry;                             // 3
};

/// @brief One account's frozen totals in one closed period, in company currency.
struct PeriodBalanceRecord {
    static constexpr std::string_view TableName = "ledger_period_balances";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&PeriodRecord::id, Light::SqlRealName{"period_id"}> period;                           // 1
    Light::BelongsTo<&AccountRecord::id, Light::SqlRealName{"account_id"}> account;                        // 2
    Light::Field<std::int64_t, Light::SqlRealName{"debit_num"}> debitNum{0};                               // 3
    Light::Field<std::int64_t, Light::SqlRealName{"debit_den"}> debitDen{1};                               // 4
    Light::Field<std::int64_t, Light::SqlRealName{"credit_num"}> creditNum{0};                             // 5
    Light::Field<std::int64_t, Light::SqlRealName{"credit_den"}> creditDen{1};                             // 6
};
```

Append to `ledger_schema.cpp`'s NOLINT block:

```cpp
LIGHTWEIGHT_SQL_MIGRATION(20261110000003, "Create ledger_entries, their lines, posting sources and period balances") {
    plan.CreateTableIfNotExists("ledger_entries")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("company_id", Bigint())
        .RequiredForeignKey("period_id", Bigint(),
                            SqlForeignKeyReferenceDefinition{.tableName = "ledger_periods", .columnName = "id"})
        .RequiredColumn("date_ms", Bigint())
        .RequiredColumn("state", Integer())
        .RequiredColumn("source_key", Varchar(128))
        .RequiredColumn("source_reference", Varchar(128))
        .RequiredColumn("memo", Varchar(255))
        .RequiredColumn("currency", Integer())
        .RequiredColumn("rate_num", Bigint())
        .RequiredColumn("rate_den", Bigint())
        .Column("reverses_id", Bigint())
        .Column("reversed_by_id", Bigint())
        .RequiredColumn("created_by", Varchar(64))
        .RequiredColumn("posted_by", Varchar(64));
    plan.CreateTableIfNotExists("ledger_entry_lines")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("entry_id", Bigint(),
                            SqlForeignKeyReferenceDefinition{.tableName = "ledger_entries", .columnName = "id"})
        .RequiredColumn("position", Integer())
        .RequiredForeignKey("account_id", Bigint(),
                            SqlForeignKeyReferenceDefinition{.tableName = "ledger_accounts", .columnName = "id"})
        .RequiredColumn("side", Integer())
        .RequiredColumn("amount_num", Bigint())
        .RequiredColumn("amount_den", Bigint())
        .RequiredColumn("company_num", Bigint())
        .RequiredColumn("company_den", Bigint());
    plan.CreateTableIfNotExists("ledger_posting_sources")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("scope", Varchar(32))
        .RequiredColumn("source_key", Varchar(128))
        .RequiredColumn("entry", Varchar(32));
    plan.CreateTableIfNotExists("ledger_period_balances")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("period_id", Bigint(),
                            SqlForeignKeyReferenceDefinition{.tableName = "ledger_periods", .columnName = "id"})
        .RequiredForeignKey("account_id", Bigint(),
                            SqlForeignKeyReferenceDefinition{.tableName = "ledger_accounts", .columnName = "id"})
        .RequiredColumn("debit_num", Bigint())
        .RequiredColumn("debit_den", Bigint())
        .RequiredColumn("credit_num", Bigint())
        .RequiredColumn("credit_den", Bigint());
}
```

- [ ] **Step 5: Declare the model's new surface and rules**

In `ledger_model.hpp` include `lims/ledger/dto/entry_dto.hpp` and `lims/posting/posting.hpp`, and add (each with the same Doxygen shape as Task 4's members — `@param action`, `@return`, every `@throws`: `ValidationError` for a malformed request or an entry `entryProblem` refuses, `NotFound` for a missing company, period, account or entry, `IllegalTransition` for a wrong state, `Conflict` for a closed or closing period as stated in the test, `Forbidden`, `EmptyPrincipalError`):

```cpp
    JournalEntryView execute(const CreateDraftEntry& action);
    JournalEntryView execute(const PostEntry& action);
    JournalEntryView execute(const ReverseEntry& action);
    JournalEntryView execute(const GetEntry& action);
    ListEntriesResult execute(const ListEntries& action);
    PeriodView execute(const ClosePeriod& action);

    /// @brief Records a poster's request as a posted entry, once per company and source key.
    ///
    /// Not an action: postings reach the ledger only from a module's document, through the app's wiring. Checks
    /// what a hand-made entry is checked for, and that the request's rate is the ledger's rate on its date (one when
    /// the currencies are the same).
    /// @param request The request.
    /// @return The entry; the first one recorded under the request's key when it is repeated.
    /// @throws ValidationError if the request is malformed, does not balance, or names a rate other than the dated
    ///         one.
    /// @throws NotFound if the company, the period of its date or an account does not exist.
    /// @throws Conflict if the period is closed.
    [[nodiscard]] static PostingResult record(PostingRequest const& request);
```

Registrations, inside the NOLINT block:

```cpp
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::CreateDraftEntry, "CreateDraftEntry")
BRIDGE_REGISTER_EFFECTS(lims::CreateDraftEntry, ::morph::effects::changes<lims::JournalEntryView>)
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::PostEntry, "PostEntry")
BRIDGE_REGISTER_EFFECTS(lims::PostEntry, ::morph::effects::updates<lims::JournalEntryView>)
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::ReverseEntry, "ReverseEntry")
BRIDGE_REGISTER_EFFECTS(lims::ReverseEntry, ::morph::effects::changes<lims::JournalEntryView>)
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::GetEntry, "GetEntry", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::ListEntries, "ListEntries", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::ClosePeriod, "ClosePeriod")
BRIDGE_REGISTER_EFFECTS(lims::ClosePeriod, ::morph::effects::updates<lims::PeriodView>)
```

`ReverseEntry` returns the reversal and changes the original's `reversedBy`, so it declares `changes<JournalEntryView>` (rows the reply does not carry change), not `updates`.

In `ledger_authorization.hpp` (include `entry_dto.hpp`):

```cpp
/// @brief A draft is made by an accountant at its company.
template <>
struct Authorization<CreateDraftEntry> : CompanyRule<CreateDraftEntry, LimsRole::Accountant> {};

/// @brief A draft is posted by an accountant at the entry's company.
template <>
struct Authorization<PostEntry> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::Accountant;

    /// @brief The company of the entry the payload names.
    /// @param action The action.
    /// @return It; an empty company when there is no such entry.
    [[nodiscard]] static Scope scopeOf(PostEntry const& action);
};

/// @brief An entry is reversed by an accountant at its company.
template <>
struct Authorization<ReverseEntry> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::Accountant;

    /// @brief The company of the entry the payload names.
    /// @param action The action.
    /// @return It; an empty company when there is no such entry.
    [[nodiscard]] static Scope scopeOf(ReverseEntry const& action);
};

/// @brief A period is closed by an accountant at its company.
template <>
struct Authorization<ClosePeriod> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::Accountant;

    /// @brief The company of the period the payload names.
    /// @param action The action.
    /// @return It; an empty company when there is no such period.
    [[nodiscard]] static Scope scopeOf(ClosePeriod const& action);
};
```

and in `ledger_authorization.cpp`, beside `periodCompany`:

```cpp
/// @brief The company of entry @p entry, or an empty company.
/// @param entry The entry.
/// @return The scope.
[[nodiscard]] Scope entryCompany(JournalEntryId entry) {
    if (!entry.hasValue()) {
        return Scope{CompanyId{}};
    }
    Lightweight::DataMapper mapper;
    auto const rows = mapper.Query<db::EntryRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::EntryRecord::id>, "=", *entry)
                          .All();
    return rows.empty() ? Scope{CompanyId{}} : Scope{CompanyId{rows.front().companyId.Value()}};
}
```

with `Authorization<PostEntry>::scopeOf` and `Authorization<ReverseEntry>::scopeOf` returning `entryCompany(action.entryId)` and `Authorization<ClosePeriod>::scopeOf` returning `periodCompany(action.periodId)`. Add the four to `declaredRequirements()` and the coverage tuple.

- [ ] **Step 6: Implement entries**

Create `examples/lims/ledger/src/models/ledger_entries.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <algorithm>
#include <cstdint>
#include <map>
#include <morph/offline/replay_ledger.hpp>
#include <morph/session/session.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "lims/ledger/authorization/ledger_authorization.hpp"
#include "lims/ledger/core/balance.hpp"
#include "lims/ledger/db/ledger_entity.hpp"
#include "lims/ledger/models/ledger_model.hpp"
#include "lims/org/core/calendar.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/core/model_support.hpp"
#include "lims/org/models/org_model.hpp"

namespace lims {

namespace {

using ::morph::math::DecimalPlaces;
using ::morph::math::Denominator;
using ::morph::math::Numerator;
using ::morph::math::Rational;

constexpr int kMaxEntryState = static_cast<int>(EntryState::Posted);
constexpr int kMaxCurrencyCode = static_cast<int>(CurrencyCode::JPY);

/// @brief The posting sources of one company, as an `IReplayLedger` over `ledger_posting_sources`.
///
/// Constructed over the caller's open mapper for one `record()`, so a source key is recorded in the same transaction
/// as the entry it produced. Response-replay: the payload is the entry's id, which a repeated request answers.
class PostingSources final : public ::morph::offline::IReplayLedger {
public:
    /// @param mapper The caller's mapper. Borrowed: it must outlive this instance.
    explicit PostingSources(Lightweight::DataMapper& mapper) : _mapper{mapper} {}

protected:
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- scope/opId, not interchangeable
    [[nodiscard]] std::optional<std::string> doLookup(std::string_view scope, std::string_view opId) const override {
        auto const rows = _mapper.Query<db::PostingSourceRecord>()
                              .Where(::Lightweight::FieldNameOf<&db::PostingSourceRecord::scope>, "=",
                                     std::string{scope})
                              .Where(::Lightweight::FieldNameOf<&db::PostingSourceRecord::sourceKey>, "=",
                                     std::string{opId})
                              .All();
        if (rows.empty()) {
            return std::nullopt;
        }
        return std::string{rows.front().entry.Value().ToStringView()};
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- scope/opId, not interchangeable
    void doRecord(std::string_view scope, std::string_view opId, std::string payload) override {
        db::PostingSourceRecord row;
        row.scope = Lightweight::SqlAnsiString<32>{std::string{scope}};
        row.sourceKey = Lightweight::SqlAnsiString<128>{std::string{opId}};
        row.entry = Lightweight::SqlAnsiString<32>{payload};
        _mapper.Create(row);
    }

private:
    // Borrowed for one record(); never reseated.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    Lightweight::DataMapper& _mapper;
};

/// @brief @p num / @p den at @p currency's decimals.
/// @param num The numerator.
/// @param den The denominator.
/// @param currency The currency.
/// @return The amount.
[[nodiscard]] Amount amountOf(std::int64_t num, std::int64_t den, CurrencyCode currency) {
    return Amount{Rational{Numerator{num}, Denominator{den}, DecimalPlaces{decimalsOf(currency)}}};
}

/// @brief The rows of entry @p id: its header and its lines in order.
struct LoadedEntry {
    db::EntryRecord header;
    std::vector<db::EntryLineRecord> lines;
};

/// @brief Loads entry @p id.
/// @param mapper The open mapper.
/// @param id The entry.
/// @return It, or nothing.
[[nodiscard]] std::optional<LoadedEntry> loadEntry(Lightweight::DataMapper& mapper, std::int64_t id) {
    auto headers =
        mapper.Query<db::EntryRecord>().Where(::Lightweight::FieldNameOf<&db::EntryRecord::id>, "=", id).All();
    if (headers.empty()) {
        return std::nullopt;
    }
    auto lines = mapper.Query<db::EntryLineRecord>()
                     .Where(::Lightweight::FieldNameOf<&db::EntryLineRecord::entry>, "=", id)
                     .All();
    std::ranges::sort(lines, {}, [](db::EntryLineRecord const& line) { return line.position.Value(); });
    return LoadedEntry{.header = std::move(headers.front()), .lines = std::move(lines)};
}

/// @brief The company's base currency.
/// @param company The company.
/// @return It.
/// @throws NotFound if the company does not exist.
[[nodiscard]] CurrencyCode companyCurrency(CompanyId company) {
    auto const found = OrgModel::findCompany(company);
    if (!found.has_value()) {
        throw NotFound{"company " + std::to_string(*company) + " does not exist"};
    }
    return found->baseCurrency;
}

/// @brief Maps an entry's rows to its view.
/// @param mapper The open mapper, for the account codes.
/// @param entry The rows.
/// @return The view.
/// @throws ValidationError if a row holds a state or currency outside its enumeration.
[[nodiscard]] JournalEntryView toView(Lightweight::DataMapper& mapper, LoadedEntry const& entry) {
    auto const& row = entry.header;
    int const state = row.state.Value();
    int const currency = row.currency.Value();
    if (state < 0 || state > kMaxEntryState || currency < 0 || currency > kMaxCurrencyCode) {
        throw ValidationError{"entry " + std::to_string(row.id.Value()) + " stores a state or currency out of range"};
    }
    CompanyId const company{row.companyId.Value()};
    CurrencyCode const transaction = static_cast<CurrencyCode>(currency);
    CurrencyCode const base = companyCurrency(company);
    JournalEntryView view{
        .id = JournalEntryId{static_cast<std::int64_t>(row.id.Value())},
        .company = company,
        .period = PeriodId{static_cast<std::int64_t>(row.period.Value())},
        .date = timestampFromMillis(row.dateMs.Value()),
        .state = static_cast<EntryState>(state),
        .sourceKey = std::string{row.sourceKey.Value().ToStringView()},
        .sourceReference = std::string{row.sourceReference.Value().ToStringView()},
        .memo = std::string{row.memo.Value().ToStringView()},
        .currency = transaction,
        .rate = Rate{Rational{Numerator{row.rateNum.Value()}, Denominator{row.rateDen.Value()}, DecimalPlaces{6}}},
        .reverses = JournalEntryId::fromOptional(row.reversesId.Value()),
        .reversedBy = JournalEntryId::fromOptional(row.reversedById.Value()),
        .lines = {},
        .createdBy = std::string{row.createdBy.Value().ToStringView()},
        .postedBy = std::string{row.postedBy.Value().ToStringView()},
    };
    for (auto const& line : entry.lines) {
        auto const accounts = mapper.Query<db::AccountRecord>()
                                  .Where(::Lightweight::FieldNameOf<&db::AccountRecord::id>, "=",
                                         static_cast<std::int64_t>(line.account.Value()))
                                  .All();
        view.lines.push_back(PostingLine{
            .accountCode = accounts.empty() ? std::string{} : std::string{accounts.front().code.Value().ToStringView()},
            .side = line.side.Value() == static_cast<int>(PostingSide::Credit) ? PostingSide::Credit
                                                                               : PostingSide::Debit,
            .amount = amountOf(line.amountNum.Value(), line.amountDen.Value(), transaction),
            .companyAmount = amountOf(line.companyNum.Value(), line.companyDen.Value(), base)});
    }
    return view;
}

/// @brief The period of @p company that @p date falls in.
/// @param mapper The open mapper.
/// @param company The company.
/// @param date The date.
/// @return Its row.
/// @throws NotFound if the month is not a period of the company.
[[nodiscard]] db::PeriodRecord periodOf(Lightweight::DataMapper& mapper, CompanyId company,
                                        ::morph::time::Timestamp const& date) {
    auto const month = calendarMonthOf(date);
    auto rows = mapper.Query<db::PeriodRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::PeriodRecord::companyId>, "=", *company)
                    .Where(::Lightweight::FieldNameOf<&db::PeriodRecord::startMs>, "=", monthStartMillis(month))
                    .All();
    if (rows.empty()) {
        throw NotFound{"company " + std::to_string(*company) + " has no period " + monthLabel(month)};
    }
    return rows.front();
}

/// @brief Throws `Conflict` when @p period does not take what @p allowed permits.
/// @param period The period.
/// @param allowsClosing Whether a closing period takes it.
/// @param what What is refused, for the message.
void requireRecordable(db::PeriodRecord const& period, bool allowsClosing, std::string_view what) {
    int const state = period.state.Value();
    std::string const label = monthLabel(calendarMonthOf(timestampFromMillis(period.startMs.Value())));
    if (state == static_cast<int>(PeriodState::Closed)) {
        throw Conflict{std::string{what} + ": period " + label + " is closed"};
    }
    if (state == static_cast<int>(PeriodState::Closing) && !allowsClosing) {
        throw Conflict{std::string{what} + ": period " + label + " is closing and takes no new drafts"};
    }
}

/// @brief The account id of each line's code in @p company's chart.
/// @param mapper The open mapper.
/// @param company The company.
/// @param lines The lines.
/// @return The ids, in line order.
/// @throws NotFound if a code is not in the chart.
[[nodiscard]] std::vector<std::uint64_t> accountIds(Lightweight::DataMapper& mapper, CompanyId company,
                                                    std::span<PostingLine const> lines) {
    std::vector<std::uint64_t> ids;
    for (PostingLine const& line : lines) {
        auto const rows = mapper.Query<db::AccountRecord>()
                              .Where(::Lightweight::FieldNameOf<&db::AccountRecord::companyId>, "=", *company)
                              .Where(::Lightweight::FieldNameOf<&db::AccountRecord::code>, "=", line.accountCode)
                              .All();
        if (rows.empty()) {
            throw NotFound{"company " + std::to_string(*company) + " has no account " + line.accountCode};
        }
        ids.push_back(rows.front().id.Value());
    }
    return ids;
}

/// @brief Writes an entry and its lines; the caller commits.
/// @param mapper The open mapper, inside the caller's transaction.
/// @param header The header, its id unset.
/// @param accounts The lines' account ids.
/// @param lines The lines.
/// @return The new entry's id.
std::int64_t writeEntry(Lightweight::DataMapper& mapper, db::EntryRecord& header,
                        std::vector<std::uint64_t> const& accounts, std::span<PostingLine const> lines) {
    mapper.Create(header);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        db::EntryLineRecord row;
        row.entry = header.id.Value();
        row.position = static_cast<int>(i);
        row.account = accounts[i];
        row.side = static_cast<int>(lines[i].side);
        row.amountNum = (*lines[i].amount).numerator;
        row.amountDen = (*lines[i].amount).denominator;
        row.companyNum = (*lines[i].companyAmount).numerator;
        row.companyDen = (*lines[i].companyAmount).denominator;
        mapper.Create(row);
    }
    return static_cast<std::int64_t>(header.id.Value());
}

/// @brief A header for @p company on @p date in @p period.
/// @return The header, its id unset.
[[nodiscard]] db::EntryRecord headerFor(CompanyId company, ::morph::time::Timestamp const& date,
                                        db::PeriodRecord const& period, EntryState state, CurrencyCode currency,
                                        Rational const& rate, std::string const& author) {
    db::EntryRecord header;
    header.companyId = *company;
    header.period = period.id.Value();
    header.dateMs = millisOf(date);
    header.state = static_cast<int>(state);
    header.currency = static_cast<int>(currency);
    header.rateNum = rate.numerator;
    header.rateDen = rate.denominator;
    header.createdBy = Lightweight::SqlAnsiString<64>{author};
    header.postedBy = Lightweight::SqlAnsiString<64>{state == EntryState::Posted ? author : std::string{}};
    return header;
}

/// @brief Throws `ValidationError` naming @p where when @p lines cannot be recorded.
void requireBalanced(std::string_view where, std::span<PostingLine const> lines, CurrencyCode transaction,
                     CurrencyCode company) {
    if (auto const problem = entryProblem(lines, transaction, company)) {
        throw ValidationError{std::string{where} + ": " + *problem};
    }
}

}  // namespace

JournalEntryView LedgerModel::execute(const CreateDraftEntry& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"CreateDraftEntry: a company, a date, a rate and two or more lines are required"};
    }
    requireRoleSomewhere<CreateDraftEntry>();
    CurrencyCode const base = companyCurrency(action.company);
    requireAuthorized(action);
    requireBalanced("CreateDraftEntry", action.lines, action.currency, base);
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    auto const period = periodOf(mapper, action.company, action.date);
    requireRecordable(period, false, "CreateDraftEntry");
    auto const accounts = accountIds(mapper, action.company, action.lines);
    auto header = headerFor(action.company, action.date, period, EntryState::Draft, action.currency, *action.rate,
                            ::morph::session::current()->principal);
    header.memo = Lightweight::SqlAnsiString<255>{action.memo};
    std::int64_t const id = writeEntry(mapper, header, accounts, action.lines);
    sqlTxn.Commit();
    return toView(mapper, *loadEntry(mapper, id));
}

JournalEntryView LedgerModel::execute(const PostEntry& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"PostEntry: an entry is required"};
    }
    requireRoleSomewhere<PostEntry>();
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    auto entry = loadEntry(mapper, *action.entryId);
    if (!entry.has_value()) {
        throw NotFound{"PostEntry: entry " + std::to_string(*action.entryId) + " does not exist"};
    }
    requireAuthorized(action);
    if (entry->header.state.Value() != static_cast<int>(EntryState::Draft)) {
        throw IllegalTransition{"PostEntry: entry " + std::to_string(*action.entryId) + " is already posted"};
    }
    auto const periods = mapper.Query<db::PeriodRecord>()
                             .Where(::Lightweight::FieldNameOf<&db::PeriodRecord::id>, "=",
                                    static_cast<std::int64_t>(entry->header.period.Value()))
                             .All();
    requireRecordable(periods.front(), true, "PostEntry");
    entry->header.state = static_cast<int>(EntryState::Posted);
    entry->header.postedBy = Lightweight::SqlAnsiString<64>{::morph::session::current()->principal};
    mapper.Update(entry->header);
    sqlTxn.Commit();
    return toView(mapper, *entry);
}

JournalEntryView LedgerModel::execute(const ReverseEntry& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"ReverseEntry: an entry and a date are required"};
    }
    requireRoleSomewhere<ReverseEntry>();
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    auto original = loadEntry(mapper, *action.entryId);
    if (!original.has_value()) {
        throw NotFound{"ReverseEntry: entry " + std::to_string(*action.entryId) + " does not exist"};
    }
    requireAuthorized(action);
    if (original->header.state.Value() != static_cast<int>(EntryState::Posted)) {
        throw IllegalTransition{"ReverseEntry: a draft is corrected by editing it away, not by reversal"};
    }
    if (original->header.reversedById.Value().has_value()) {
        throw Conflict{"ReverseEntry: entry " + std::to_string(*action.entryId) + " is already reversed"};
    }
    auto const view = toView(mapper, *original);
    std::vector<PostingLine> lines = view.lines;
    for (PostingLine& line : lines) {
        line.side = line.side == PostingSide::Debit ? PostingSide::Credit : PostingSide::Debit;
    }
    auto const period = periodOf(mapper, view.company, action.date);
    requireRecordable(period, true, "ReverseEntry");
    auto const accounts = accountIds(mapper, view.company, lines);
    auto header = headerFor(view.company, action.date, period, EntryState::Posted, view.currency, *view.rate,
                            ::morph::session::current()->principal);
    header.sourceReference = Lightweight::SqlAnsiString<128>{"reversal of entry " + std::to_string(*action.entryId)};
    header.reversesId = *action.entryId;
    std::int64_t const id = writeEntry(mapper, header, accounts, lines);
    original->header.reversedById = id;
    mapper.Update(original->header);
    sqlTxn.Commit();
    return toView(mapper, *loadEntry(mapper, id));
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
JournalEntryView LedgerModel::execute(const GetEntry& action) {
    if (!action.validate()) {
        throw ValidationError{"GetEntry: an entry is required"};
    }
    Lightweight::DataMapper mapper;
    auto const entry = loadEntry(mapper, *action.entryId);
    auto const visibleCompanies = OrgModel::visibleCompanies(currentPrincipal());
    if (!entry.has_value() ||
        std::ranges::find(visibleCompanies, CompanyId{entry->header.companyId.Value()}) == visibleCompanies.end()) {
        throw NotFound{"GetEntry: entry " + std::to_string(*action.entryId) + " does not exist"};
    }
    return toView(mapper, *entry);
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ListEntriesResult LedgerModel::execute(const ListEntries& action) {
    if (!action.validate()) {
        throw ValidationError{"ListEntries: a company is required"};
    }
    ListEntriesResult result;
    auto const visibleCompanies = OrgModel::visibleCompanies(currentPrincipal());
    if (std::ranges::find(visibleCompanies, action.company) == visibleCompanies.end()) {
        return result;
    }
    Lightweight::DataMapper mapper;
    for (auto const& header : mapper.Query<db::EntryRecord>()
                                  .Where(::Lightweight::FieldNameOf<&db::EntryRecord::companyId>, "=", *action.company)
                                  .All()) {
        result.entries.push_back(toView(mapper, *loadEntry(mapper, static_cast<std::int64_t>(header.id.Value()))));
    }
    std::ranges::sort(result.entries, {}, [](JournalEntryView const& entry) { return *entry.id; });
    return result;
}

PeriodView LedgerModel::execute(const ClosePeriod& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"ClosePeriod: a period is required"};
    }
    requireRoleSomewhere<ClosePeriod>();
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    auto periods = mapper.Query<db::PeriodRecord>()
                       .Where(::Lightweight::FieldNameOf<&db::PeriodRecord::id>, "=", *action.periodId)
                       .All();
    if (periods.empty()) {
        throw NotFound{"ClosePeriod: period " + std::to_string(*action.periodId) + " does not exist"};
    }
    requireAuthorized(action);
    auto& period = periods.front();
    if (period.state.Value() != static_cast<int>(PeriodState::Closing)) {
        throw IllegalTransition{"ClosePeriod: a period is closed from closing, after its close has begun"};
    }
    auto const entries = mapper.Query<db::EntryRecord>()
                             .Where(::Lightweight::FieldNameOf<&db::EntryRecord::period>, "=", *action.periodId)
                             .All();
    auto const drafts = std::ranges::count_if(
        entries, [](db::EntryRecord const& entry) { return entry.state.Value() == static_cast<int>(EntryState::Draft); });
    if (drafts > 0) {
        throw Conflict{"ClosePeriod: " + std::to_string(drafts) + " draft entries remain in the period"};
    }
    // Freezing: each account's totals in the period, in company currency, kept with the closed period.
    std::map<std::uint64_t, std::pair<ExactSum, ExactSum>> totals;
    for (auto const& entry : entries) {
        for (auto const& line : mapper.Query<db::EntryLineRecord>()
                                    .Where(::Lightweight::FieldNameOf<&db::EntryLineRecord::entry>, "=",
                                           static_cast<std::int64_t>(entry.id.Value()))
                                    .All()) {
            auto& [debit, credit] = totals[line.account.Value()];
            Rational const value{Numerator{line.companyNum.Value()}, Denominator{line.companyDen.Value()},
                                 DecimalPlaces{2}};
            if (!(line.side.Value() == static_cast<int>(PostingSide::Debit) ? debit : credit).add(value)) {
                throw OutOfRange{"ClosePeriod: an account's total is too large to freeze exactly"};
            }
        }
    }
    for (auto const& [account, sums] : totals) {
        db::PeriodBalanceRecord row;
        row.period = period.id.Value();
        row.account = account;
        row.debitNum = sums.first.total().numerator;
        row.debitDen = sums.first.total().denominator;
        row.creditNum = sums.second.total().numerator;
        row.creditDen = sums.second.total().denominator;
        mapper.Create(row);
    }
    period.state = static_cast<int>(PeriodState::Closed);
    mapper.Update(period);
    sqlTxn.Commit();
    auto const start = timestampFromMillis(period.startMs.Value());
    return PeriodView{.id = PeriodId{static_cast<std::int64_t>(period.id.Value())},
                      .company = CompanyId{period.companyId.Value()},
                      .start = start,
                      .label = monthLabel(calendarMonthOf(start)),
                      .state = PeriodState::Closed};
}

PostingResult LedgerModel::record(PostingRequest const& request) {
    if (!request.company.hasValue() || !request.date.hasValue() || request.sourceKey.empty() ||
        request.sourceKey.size() > 128 || request.sourceReference.size() > 128 || !request.rate.hasValue()) {
        throw ValidationError{"record: a company, a date, a source key and a rate are required"};
    }
    CurrencyCode const base = companyCurrency(request.company);
    requireBalanced("record", request.lines, request.currency, base);
    Rational const one{1, DecimalPlaces{6}};
    if (request.currency == base) {
        if (*request.rate != one) {
            throw ValidationError{"record: a posting in the company's own currency converts at one"};
        }
    } else {
        auto const dated = rateOn(request.company, request.currency, base, request.date);
        if (!dated.has_value() || *dated != *request.rate) {
            throw ValidationError{"record: the rate is not the ledger's " + std::string{currencyName(request.currency)} +
                                  "/" + std::string{currencyName(base)} + " rate on the posting's date"};
        }
    }
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    PostingSources sources{mapper};
    std::string const scope = std::to_string(*request.company);
    if (auto const known = sources.lookup(scope, request.sourceKey)) {
        return PostingResult{.entryId = JournalEntryId{std::stoll(*known)}};
    }
    auto const period = periodOf(mapper, request.company, request.date);
    requireRecordable(period, true, "record");
    auto const accounts = accountIds(mapper, request.company, request.lines);
    auto header = headerFor(request.company, request.date, period, EntryState::Posted, request.currency, *request.rate,
                            "posting");
    header.sourceKey = Lightweight::SqlAnsiString<128>{request.sourceKey};
    header.sourceReference = Lightweight::SqlAnsiString<128>{request.sourceReference};
    std::int64_t const id = writeEntry(mapper, header, accounts, request.lines);
    sources.record(scope, request.sourceKey, std::to_string(id));
    sqlTxn.Commit();
    return PostingResult{.entryId = JournalEntryId{id}};
}

}  // namespace lims
```

`record` calls `rateOn`, the dated rate Task 6 stores. Declare it in `ledger_model.hpp` now, with the Doxygen Task 6 Step 4 gives it, and define it for this task at the end of `ledger_entries.cpp` as the ledger with no rates yet answers:

```cpp
std::optional<::morph::math::Rational> LedgerModel::rateOn(CompanyId /*company*/, CurrencyCode /*from*/,
                                                           CurrencyCode /*to*/, ::morph::time::Timestamp const& /*on*/) {
    return std::nullopt;
}
```

Task 6 deletes this definition and adds the real one beside the rate table, so a foreign-currency posting is refused until then; this task's tests post in the company's own currency only.

The `"posting"` author on a recorded entry names the poster's channel; the person who caused the posting is the poster's own record (the invoice's `postedBy`, Task 14).

- [ ] **Step 7: Build and run**

Run: Build lims, Model suite `"[ledger]"`, Model suite `"[authorization]"`. Expected: all pass. Run the four mutations named in `test_ledger_entries.cpp` and restore each.

- [ ] **Step 8: Commit**

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/ledger examples/lims/tests examples/lims/app/include/lims/app/declared_requirements.hpp
git commit -m "Add lims journal entries: balanced, unrounded, reversed, recorded once per source

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---
### Task 6: Ledger — exchange rates, the reports, and money at ERP magnitude (hypothesis 8, first instance)

**Files:**
- Create: `examples/lims/ledger/include/lims/ledger/dto/rate_dto.hpp`, `examples/lims/ledger/include/lims/ledger/dto/report_dto.hpp`, `examples/lims/ledger/include/lims/ledger/models/ledger_report_model.hpp`, `examples/lims/ledger/src/models/ledger_rates.cpp`, `examples/lims/ledger/src/models/ledger_report_model.cpp`, `examples/lims/tests/test_ledger_reports.cpp`
- Modify: `ledger_entity.hpp` (`ExchangeRateRecord`), `ledger_schema.cpp`, `ledger_model.hpp` (rate actions, `rateOn`'s Doxygen), `ledger_entries.cpp` (delete the interim `rateOn`), `ledger_authorization.hpp`, `declared_requirements.hpp`, `test_declared_authorization.cpp`, `examples/lims/README.md` (decision 29)

**Interfaces:**
- Consumes: `ExactSum`, `OutOfRange`, `EntryRecord`, `EntryLineRecord`, `AccountRecord`, `LedgerModel::record`.
- Produces: `SetExchangeRate { CompanyId company; CurrencyCode from; CurrencyCode to; Timestamp validFrom; Rate rate; }` → `ExchangeRateView { ExchangeRateId id; CompanyId company; CurrencyCode from; CurrencyCode to; Timestamp validFrom; Rate rate; std::string setBy; }`; `ListExchangeRates { CompanyId company; }` → `ListExchangeRatesResult`; `static std::optional<Rational> LedgerModel::rateOn(CompanyId, CurrencyCode from, CurrencyCode to, Timestamp const& on)`; model `lims::LedgerReportModel` (`"LedgerReportModel"`) with `TrialBalance { CompanyId company; Timestamp asOf; }` → `TrialBalanceView { CompanyId company; Timestamp asOf; std::vector<TrialBalanceRow> rows; Amount totalDebit; Amount totalCredit; }`, `TrialBalanceRow { std::string accountCode; std::string accountName; AccountType type; Amount debit; Amount credit; }`, `AccountBalance { CompanyId company; std::string accountCode; Timestamp asOf; }` → `AccountBalanceView { std::string accountCode; Amount debit; Amount credit; Amount balance; }`, `AccountLedger { CompanyId company; std::string accountCode; Timestamp from; Timestamp to; }` → `AccountLedgerView { std::string accountCode; std::vector<AccountLedgerLine> lines; }` with `AccountLedgerLine { JournalEntryId entryId; Timestamp date; std::string sourceReference; PostingSide side; Amount companyAmount; Amount balance; }`; records `Ledger_ExchangeRate`, and `Ledger_TrialBalanceRow`, `Ledger_AccountBalance`, `Ledger_AccountLedgerLine` registered `keyless` and `derivedFrom<JournalEntryView>`.

A rate is entered per company and currency pair with the date it applies from; the rate valid on a date is the one with the latest `validFrom` not after it. Reports total posted entries only, in company currency, with `ExactSum`; a total that does not fit is `OutOfRange`, never a clamped number.

- [ ] **Step 1: Write the failing test**

Create `examples/lims/tests/test_ledger_reports.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Dated exchange rates, the three reports, and hypothesis 8's first half: totals at ERP magnitude are exact, and a
// total that does not fit is refused rather than saturated.

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <limits>
#include <morph/util/rational.hpp>
#include <string>
#include <vector>

#include "lims/ledger/models/ledger_model.hpp"
#include "lims/ledger/models/ledger_report_model.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/core/money.hpp"
#include "lims/org/models/org_model.hpp"
#include "lims_test_support.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;
using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

namespace {

/// @brief A whole-unit amount.
/// @param units The units.
/// @return The amount at two decimals.
[[nodiscard]] lims::Amount units(std::int64_t units) { return lims::Amount{Rational{units, DecimalPlaces{2}}}; }

/// @brief alice an accountant at the seeded company, accounts 1200 and 4000, November 2026 open.
void books() {
    lims::test::grant("alice", lims::LimsRole::Accountant);
    ScopedPrincipal const alice{"alice"};
    lims::LedgerModel ledger;
    ledger.execute(lims::CreateAccount{
        .company = lims::test::seededCompany(), .code = "1200", .name = "Receivables", .type = lims::AccountType::Asset});
    ledger.execute(lims::CreateAccount{
        .company = lims::test::seededCompany(), .code = "4000", .name = "Revenue", .type = lims::AccountType::Revenue});
    ledger.execute(lims::OpenPeriod{.company = lims::test::seededCompany(),
                                    .month = morph::time::Timestamp{lims::test::at(2026, 11, 1)}});
}

/// @brief Records @p amount from receivables to revenue on @p day of November 2026.
/// @param key The source key.
/// @param amount The amount.
/// @param day The day.
void post(std::string key, lims::Amount amount, unsigned day = 10) {
    lims::LedgerModel::record(lims::PostingRequest{
        .company = lims::test::seededCompany(),
        .date = morph::time::Timestamp{lims::test::at(2026, 11, day)},
        .sourceKey = key,
        .sourceReference = key,
        .currency = lims::CurrencyCode::EUR,
        .rate = lims::Rate{Rational{1, DecimalPlaces{6}}},
        .lines = {lims::PostingLine{.accountCode = "1200", .side = lims::PostingSide::Debit, .amount = amount,
                                    .companyAmount = amount},
                  lims::PostingLine{.accountCode = "4000", .side = lims::PostingSide::Credit, .amount = amount,
                                    .companyAmount = amount}}});
}

}  // namespace

// Mutation: pick the earliest valid rate instead of the latest (the 15 November posting converts at 1.05).
TEST_CASE("ledger rates: a posting uses the rate valid on its date", "[lims][ledger][rates]") {
    DbFixture fixture;
    books();
    ScopedPrincipal const alice{"alice"};
    lims::LedgerModel ledger;
    auto const rate = [](std::int64_t num, std::int64_t den) {
        return lims::Rate{Rational{Numerator{num}, Denominator{den}, DecimalPlaces{6}}};
    };
    ledger.execute(lims::SetExchangeRate{.company = lims::test::seededCompany(),
                                         .from = lims::CurrencyCode::USD,
                                         .to = lims::CurrencyCode::EUR,
                                         .validFrom = morph::time::Timestamp{lims::test::at(2026, 11, 1, 0)},
                                         .rate = rate(105, 100)});
    ledger.execute(lims::SetExchangeRate{.company = lims::test::seededCompany(),
                                         .from = lims::CurrencyCode::USD,
                                         .to = lims::CurrencyCode::EUR,
                                         .validFrom = morph::time::Timestamp{lims::test::at(2026, 11, 15, 0)},
                                         .rate = rate(92, 100)});
    auto const on = [](unsigned day) {
        return lims::LedgerModel::rateOn(lims::test::seededCompany(), lims::CurrencyCode::USD, lims::CurrencyCode::EUR,
                                         morph::time::Timestamp{lims::test::at(2026, 11, day)});
    };
    CHECK(on(14) == Rational{Numerator{105}, Denominator{100}, DecimalPlaces{6}});
    CHECK(on(15) == Rational{Numerator{92}, Denominator{100}, DecimalPlaces{6}});
    CHECK_FALSE(lims::LedgerModel::rateOn(lims::test::seededCompany(), lims::CurrencyCode::USD,
                                          lims::CurrencyCode::EUR,
                                          morph::time::Timestamp{lims::test::at(2026, 10, 31)})
                    .has_value());
    CHECK_THROWS_AS(ledger.execute(lims::SetExchangeRate{.company = lims::test::seededCompany(),
                                                         .from = lims::CurrencyCode::EUR,
                                                         .to = lims::CurrencyCode::EUR,
                                                         .validFrom = morph::time::Timestamp{lims::test::at(2026, 11, 1)},
                                                         .rate = rate(1, 1)}),
                    lims::ValidationError);

    // A USD posting on 20 November must state 0.92, and its company values are the poster's rounding.
    lims::PostingRequest usd{
        .company = lims::test::seededCompany(),
        .date = morph::time::Timestamp{lims::test::at(2026, 11, 20)},
        .sourceKey = "billing.invoice:1",
        .sourceReference = "invoice 1",
        .currency = lims::CurrencyCode::USD,
        .rate = rate(105, 100),
        .lines = {lims::PostingLine{.accountCode = "1200", .side = lims::PostingSide::Debit, .amount = units(100),
                                    .companyAmount = units(92)},
                  lims::PostingLine{.accountCode = "4000", .side = lims::PostingSide::Credit, .amount = units(100),
                                    .companyAmount = units(92)}}};
    CHECK_THROWS_AS(lims::LedgerModel::record(usd), lims::ValidationError);
    usd.rate = rate(92, 100);
    CHECK(lims::LedgerModel::record(usd).entryId.hasValue());
}

// Mutation: include drafts in TrialBalance (the draft's 7 units appear in the totals).
TEST_CASE("ledger reports: trial balance, account balance and account ledger agree, over posted entries only",
          "[lims][ledger][reports]") {
    DbFixture fixture;
    books();
    post("a", units(100), 3);
    post("b", units(250), 12);
    ScopedPrincipal const alice{"alice"};
    lims::LedgerModel{}.execute(lims::CreateDraftEntry{
        .company = lims::test::seededCompany(),
        .date = morph::time::Timestamp{lims::test::at(2026, 11, 13)},
        .memo = "draft",
        .currency = lims::CurrencyCode::EUR,
        .rate = lims::Rate{Rational{1, DecimalPlaces{6}}},
        .lines = {lims::PostingLine{.accountCode = "1200", .side = lims::PostingSide::Debit, .amount = units(7),
                                    .companyAmount = units(7)},
                  lims::PostingLine{.accountCode = "4000", .side = lims::PostingSide::Credit, .amount = units(7),
                                    .companyAmount = units(7)}}});
    lims::LedgerReportModel reports;
    auto const trial = reports.execute(lims::TrialBalance{
        .company = lims::test::seededCompany(), .asOf = morph::time::Timestamp{lims::test::at(2026, 11, 30)}});
    CHECK(*trial.totalDebit == Rational{350, DecimalPlaces{2}});
    CHECK(*trial.totalCredit == *trial.totalDebit);
    REQUIRE(trial.rows.size() == 2U);
    CHECK(trial.rows[0].accountCode == "1200");

    auto const asOfFifth = reports.execute(lims::AccountBalance{.company = lims::test::seededCompany(),
                                                                .accountCode = "1200",
                                                                .asOf = morph::time::Timestamp{lims::test::at(2026, 11, 5)}});
    CHECK(*asOfFifth.balance == Rational{100, DecimalPlaces{2}});

    auto const ledgerLines = reports.execute(lims::AccountLedger{.company = lims::test::seededCompany(),
                                                                 .accountCode = "4000",
                                                                 .from = morph::time::Timestamp{lims::test::at(2026, 11, 1, 0)},
                                                                 .to = morph::time::Timestamp{lims::test::at(2026, 11, 30)}});
    REQUIRE(ledgerLines.lines.size() == 2U);
    CHECK(ledgerLines.lines[1].side == lims::PostingSide::Credit);
    CHECK(*ledgerLines.lines[1].balance == Rational{-350, DecimalPlaces{2}});
}

// Hypothesis 8, first half. The summation every report uses, at the magnitude the design names: 10^7 lines of up to
// 10^9 minor units (a total of at most 10^16 minor units, inside int64), and an over-range total.
// Mutation: make ExactSum::add use operator+ (the over-range case reports a clamped total and add() returns true).
TEST_CASE("Hypothesis 8: ten million lines of up to 10^9 minor units total exactly, and an over-range total is refused",
          "[lims][ledger][h8]") {
    constexpr std::int64_t kLines = 10'000'000;
    constexpr std::uint64_t kMaxMinor = 1'000'000'000;
    lims::ExactSum sum;
    std::int64_t expectedMinor = 0;
    std::uint64_t state = 0x9E3779B97F4A7C15ULL;
    bool allAdded = true;
    auto const started = std::chrono::steady_clock::now();
    for (std::int64_t i = 0; i < kLines; ++i) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        auto const minor = static_cast<std::int64_t>((state >> 33U) % (kMaxMinor + 1U));
        expectedMinor += minor;
        allAdded = sum.add(Rational{Numerator{minor}, Denominator{100}, DecimalPlaces{2}}) && allAdded;
    }
    auto const elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    WARN("summed " << kLines << " lines exactly in " << elapsed.count() << " ms; total " << expectedMinor
                   << " minor units");
    REQUIRE(allAdded);
    CHECK(sum.total() == Rational{Numerator{expectedMinor}, Denominator{100}, DecimalPlaces{2}});

    // The control: operator+ saturates and reports success; ExactSum refuses.
    Rational const big{5'000'000'000'000'000'000, DecimalPlaces{0}};
    Rational const clamped = big + big;
    CHECK(clamped.numerator == std::numeric_limits<std::int64_t>::max());
    lims::ExactSum over;
    CHECK(over.add(big));
    CHECK_FALSE(over.add(big));
}

// Mutation: let TrialBalance clamp instead of throwing (it answers a total of INT64_MAX).
TEST_CASE("Hypothesis 8: a trial balance whose total does not fit is refused, not saturated", "[lims][ledger][h8]") {
    DbFixture fixture;
    books();
    post("huge-1", lims::Amount{Rational{5'000'000'000'000'000'000, DecimalPlaces{2}}});
    post("huge-2", lims::Amount{Rational{5'000'000'000'000'000'000, DecimalPlaces{2}}});
    ScopedPrincipal const alice{"alice"};
    CHECK_THROWS_AS(lims::LedgerReportModel{}.execute(lims::TrialBalance{
                        .company = lims::test::seededCompany(),
                        .asOf = morph::time::Timestamp{lims::test::at(2026, 11, 30)}}),
                    lims::OutOfRange);
}
```

`clamped.numerator == INT64_MAX` is inferred from `rational.hpp`'s description ("clamps to `+/-INT64_MAX` and logs"); if the measured clamp is another value, assert the measured value and keep the point the check makes: `operator+` reported success with a wrong total.

- [ ] **Step 2: Run it and see it fail**

Run: Build lims. Expected: compile error, `lims/ledger/models/ledger_report_model.hpp` not found.

- [ ] **Step 3: Rates — DTOs, record, migration, rules**

Create `rate_dto.hpp` with the structs in Interfaces. `SetExchangeRate::validate()` is `company.hasValue() && validFrom.hasValue() && rate.hasValue() && from != to`, `explicitSubmit = true`; `ListExchangeRates::validate()` is `company.hasValue()`. Register `BRIDGE_REGISTER_RECORD(lims::ExchangeRateView, "Ledger_ExchangeRate")`. Doxygen every member as `account_dto.hpp` does.

Append to `ledger_entity.hpp`:

```cpp
/// @brief One dated rate of one currency pair for one company.
struct ExchangeRateRecord {
    static constexpr std::string_view TableName = "ledger_exchange_rates";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"company_id"}> companyId{0};                             // 1
    Light::Field<int, Light::SqlRealName{"from_currency"}> fromCurrency{0};                                // 2
    Light::Field<int, Light::SqlRealName{"to_currency"}> toCurrency{0};                                    // 3
    Light::Field<std::int64_t, Light::SqlRealName{"valid_from_ms"}> validFromMs{0};                        // 4
    Light::Field<std::int64_t, Light::SqlRealName{"rate_num"}> rateNum{1};                                 // 5
    Light::Field<std::int64_t, Light::SqlRealName{"rate_den"}> rateDen{1};                                 // 6
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"set_by"}> setBy;                            // 7
};
```

and the migration `LIGHTWEIGHT_SQL_MIGRATION(20261110000004, "Create ledger_exchange_rates")` with those columns (all `RequiredColumn`; `Integer()` for the currencies, `Bigint()` otherwise, `Varchar(64)` for `set_by`). Add `template <> struct Authorization<SetExchangeRate> : CompanyRule<SetExchangeRate, LimsRole::Accountant> {};`, put it in `declaredRequirements()` and the coverage tuple.

- [ ] **Step 4: Rates — the model**

In `ledger_model.hpp` add `ExchangeRateView execute(const SetExchangeRate& action);` (throws `ValidationError` for a malformed rate, a non-positive one, or `from == to`; `NotFound`; `Conflict` if the pair already has a rate from that instant; `Forbidden`; `EmptyPrincipalError`), `ListExchangeRatesResult execute(const ListExchangeRates& action);`, and this Doxygen on `rateOn`:

```cpp
    /// @brief The rate from @p from to @p to valid on @p on: the one with the latest start not after it.
    /// @param company The company whose rates are asked.
    /// @param from The source currency.
    /// @param to The target currency.
    /// @param on The date.
    /// @return Units of @p to per unit of @p from; nothing when no rate is valid yet.
    [[nodiscard]] static std::optional<::morph::math::Rational> rateOn(CompanyId company, CurrencyCode from,
                                                                      CurrencyCode to,
                                                                      ::morph::time::Timestamp const& on);
```

with registrations `BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::SetExchangeRate, "SetExchangeRate")`, `BRIDGE_REGISTER_EFFECTS(lims::SetExchangeRate, ::morph::effects::changes<lims::ExchangeRateView>)`, `BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::ListExchangeRates, "ListExchangeRates", ::morph::model::Loggable::No)`. Delete the interim `rateOn` from `ledger_entries.cpp`, and create `ledger/src/models/ledger_rates.cpp` holding `SetExchangeRate`, `ListExchangeRates` (the conventions' list shape) and:

```cpp
std::optional<::morph::math::Rational> LedgerModel::rateOn(CompanyId company, CurrencyCode from, CurrencyCode to,
                                                           ::morph::time::Timestamp const& on) {
    if (!company.hasValue() || !on.hasValue()) {
        return std::nullopt;
    }
    Lightweight::DataMapper mapper;
    std::optional<db::ExchangeRateRecord> best;
    for (auto const& row : mapper.Query<db::ExchangeRateRecord>()
                               .Where(::Lightweight::FieldNameOf<&db::ExchangeRateRecord::companyId>, "=", *company)
                               .Where(::Lightweight::FieldNameOf<&db::ExchangeRateRecord::fromCurrency>, "=",
                                      static_cast<int>(from))
                               .Where(::Lightweight::FieldNameOf<&db::ExchangeRateRecord::toCurrency>, "=",
                                      static_cast<int>(to))
                               .All()) {
        if (row.validFromMs.Value() <= millisOf(on) &&
            (!best.has_value() || row.validFromMs.Value() > best->validFromMs.Value())) {
            best = row;
        }
    }
    if (!best.has_value()) {
        return std::nullopt;
    }
    return ::morph::math::Rational{::morph::math::Numerator{best->rateNum.Value()},
                                   ::morph::math::Denominator{best->rateDen.Value()}, ::morph::math::DecimalPlaces{6}};
}
```

`SetExchangeRate::execute` follows the conventions' action shape: validate, `requireRoleSomewhere`, the company via `OrgModel::findCompany` (`NotFound`), `requireAuthorized`, refuse a rate that is zero or negative (`ValidationError`) or a second row for the same pair and `validFromMs` (`Conflict`), create the row, answer the view.

- [ ] **Step 5: Reports**

Create `report_dto.hpp` with the structs in Interfaces (every query's `validate()` requires a company and its dates engaged; `AccountBalance`/`AccountLedger` also a non-empty code), and at the bottom:

```cpp
BRIDGE_REGISTER_RECORD(lims::TrialBalanceRow, "Ledger_TrialBalanceRow", ::morph::record::keyless,
                       ::morph::record::derivedFrom<lims::JournalEntryView>)
BRIDGE_REGISTER_RECORD(lims::AccountBalanceView, "Ledger_AccountBalance", ::morph::record::keyless,
                       ::morph::record::derivedFrom<lims::JournalEntryView>)
BRIDGE_REGISTER_RECORD(lims::AccountLedgerLine, "Ledger_AccountLedgerLine", ::morph::record::keyless,
                       ::morph::record::derivedFrom<lims::JournalEntryView>)
```

A report is computed from entries, so every action that changes `JournalEntryView` changes it too (`docs/KEEPING-SCREENS-CURRENT.md`, checklist item 4).

Create `ledger_report_model.hpp` (`class LedgerReportModel` with the three `execute` overloads, each documented with `@throws ValidationError` for a malformed query, `NotFound` for an unknown account, `OutOfRange` for a total that does not fit; registered `"LedgerReportModel"`, each action `Loggable::No`). Create `ledger_report_model.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "lims/ledger/models/ledger_report_model.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "lims/ledger/db/ledger_entity.hpp"
#include "lims/org/core/calendar.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/core/model_support.hpp"
#include "lims/org/models/org_model.hpp"

namespace lims {

namespace {

using ::morph::math::DecimalPlaces;
using ::morph::math::Denominator;
using ::morph::math::Numerator;
using ::morph::math::Rational;

/// @brief One posted line, in company currency, with what the reports need of its entry.
struct PostedLine {
    std::int64_t entryId = 0;
    std::int64_t dateMs = 0;
    std::string sourceReference;
    std::uint64_t account = 0;
    PostingSide side = PostingSide::Debit;
    Rational value;
};

/// @brief Every posted line of @p company dated from @p fromMs to @p toMs inclusive, by date then entry.
/// @param mapper The open mapper.
/// @param company The company.
/// @param fromMs The first instant.
/// @param toMs The last instant.
/// @return The lines.
[[nodiscard]] std::vector<PostedLine> postedLines(Lightweight::DataMapper& mapper, CompanyId company,
                                                  std::int64_t fromMs, std::int64_t toMs) {
    std::vector<PostedLine> lines;
    for (auto const& entry : mapper.Query<db::EntryRecord>()
                                 .Where(::Lightweight::FieldNameOf<&db::EntryRecord::companyId>, "=", *company)
                                 .All()) {
        if (entry.state.Value() != static_cast<int>(EntryState::Posted) || entry.dateMs.Value() < fromMs ||
            entry.dateMs.Value() > toMs) {
            continue;
        }
        for (auto const& line : mapper.Query<db::EntryLineRecord>()
                                    .Where(::Lightweight::FieldNameOf<&db::EntryLineRecord::entry>, "=",
                                           static_cast<std::int64_t>(entry.id.Value()))
                                    .All()) {
            lines.push_back(PostedLine{
                .entryId = static_cast<std::int64_t>(entry.id.Value()),
                .dateMs = entry.dateMs.Value(),
                .sourceReference = std::string{entry.sourceReference.Value().ToStringView()},
                .account = line.account.Value(),
                .side = line.side.Value() == static_cast<int>(PostingSide::Credit) ? PostingSide::Credit
                                                                                   : PostingSide::Debit,
                .value = Rational{Numerator{line.companyNum.Value()}, Denominator{line.companyDen.Value()},
                                  DecimalPlaces{2}}});
        }
    }
    std::ranges::sort(lines, {}, [](PostedLine const& line) { return std::pair{line.dateMs, line.entryId}; });
    return lines;
}

/// @brief Adds @p value to @p sum or throws `OutOfRange` naming @p what.
void addExact(ExactSum& sum, Rational const& value, std::string_view what) {
    if (!sum.add(value)) {
        throw OutOfRange{std::string{what} + " is too large to total exactly"};
    }
}

/// @brief @p value as an amount at @p currency's decimals.
[[nodiscard]] Amount asAmount(Rational value, CurrencyCode currency) {
    value.decimalPlaces = DecimalPlaces{decimalsOf(currency)};
    return Amount{value};
}

/// @brief The company's currency, or `NotFound`.
[[nodiscard]] CurrencyCode baseOf(CompanyId company) {
    auto const found = OrgModel::findCompany(company);
    if (!found.has_value()) {
        throw NotFound{"company " + std::to_string(*company) + " does not exist"};
    }
    return found->baseCurrency;
}

/// @brief The row of account @p code of @p company, or `NotFound`.
[[nodiscard]] db::AccountRecord accountOf(Lightweight::DataMapper& mapper, CompanyId company, std::string const& code) {
    auto rows = mapper.Query<db::AccountRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::AccountRecord::companyId>, "=", *company)
                    .Where(::Lightweight::FieldNameOf<&db::AccountRecord::code>, "=", code)
                    .All();
    if (rows.empty()) {
        throw NotFound{"company " + std::to_string(*company) + " has no account " + code};
    }
    return rows.front();
}

/// @brief Whether the caller can see @p company.
[[nodiscard]] bool visible(CompanyId company) {
    auto const companies = OrgModel::visibleCompanies(currentPrincipal());
    return std::ranges::find(companies, company) != companies.end();
}

}  // namespace

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
TrialBalanceView LedgerReportModel::execute(const TrialBalance& action) {
    if (!action.validate()) {
        throw ValidationError{"TrialBalance: a company and a date are required"};
    }
    CurrencyCode const base = baseOf(action.company);
    TrialBalanceView view{.company = action.company, .asOf = action.asOf, .rows = {}, .totalDebit = {}, .totalCredit = {}};
    if (!visible(action.company)) {
        return view;
    }
    Lightweight::DataMapper mapper;
    std::map<std::uint64_t, std::pair<ExactSum, ExactSum>> sums;
    ExactSum totalDebit;
    ExactSum totalCredit;
    for (PostedLine const& line : postedLines(mapper, action.company, 0, millisOf(action.asOf))) {
        auto& [debit, credit] = sums[line.account];
        bool const isDebit = line.side == PostingSide::Debit;
        addExact(isDebit ? debit : credit, line.value, "an account's total");
        addExact(isDebit ? totalDebit : totalCredit, line.value, "the trial balance");
    }
    for (auto const& account : mapper.Query<db::AccountRecord>()
                                   .Where(::Lightweight::FieldNameOf<&db::AccountRecord::companyId>, "=",
                                          *action.company)
                                   .All()) {
        auto const found = sums.find(account.id.Value());
        if (found == sums.end()) {
            continue;
        }
        view.rows.push_back(TrialBalanceRow{.accountCode = std::string{account.code.Value().ToStringView()},
                                            .accountName = std::string{account.name.Value().ToStringView()},
                                            .type = static_cast<AccountType>(account.type.Value()),
                                            .debit = asAmount(found->second.first.total(), base),
                                            .credit = asAmount(found->second.second.total(), base)});
    }
    std::ranges::sort(view.rows, {}, &TrialBalanceRow::accountCode);
    view.totalDebit = asAmount(totalDebit.total(), base);
    view.totalCredit = asAmount(totalCredit.total(), base);
    return view;
}
```

and `AccountBalance` (the account's debit and credit totals to `asOf`, `balance = checkedSub(debit, credit)` — `OutOfRange` when it does not fit) and `AccountLedger` (the account's lines from `from` to `to`, each with the running `balance` debit-positive, starting from the balance before `from`) in the same file, built from `postedLines`, `accountOf`, `addExact` and `asAmount` as `TrialBalance` is. An invisible company answers an empty view. `AccountType` read from a row is range-checked as `ledger_model.cpp` does; reuse that check by moving `toView(db::AccountRecord)`'s range test into a free `accountTypeOf(int)` in `ledger_entity.hpp`'s namespace if both files need it.

- [ ] **Step 6: Build and run, and record the measured time**

Run: Build lims, Model suite `"[ledger]"`. Expected: all pass. The `WARN` line prints the measured summation time; copy it into decision 29. Run the four mutations named in the file and restore each. Then run Everything labelled for the rung.

- [ ] **Step 7: Record decision 29 in the README**

Append, filling the two bracketed measurements from Step 6's output (the `WARN` line and the clamped value):

```markdown
### 29. The ledger validates and never rounds, and its totals are exact or refused (hypothesis 8, first instance)

A journal entry is recorded only if it has two or more lines whose debits equal their credits in the transaction
currency and in company currency, each amount a whole number of its currency's minor units (`lims::entryProblem`).
The ledger rounds nothing: a poster converts and rounds, half-up, line by line, and adds an explicit rounding line
where that leaves a difference. A posting reaches the ledger only through `LedgerModel::record`, which is not an
action — no client can post an entry under a document's name — and is recorded once per company and source key
through an `IReplayLedger` over `ledger_posting_sources`, in the entry's own transaction, so a retry answers the first
entry. Entries are immutable once posted; a correction is a posted reversal naming the original. A period is open,
closing (no new drafts; postings and posting of existing drafts continue) or closed (nothing more is recorded; each
account's totals are frozen in `ledger_period_balances`), and closes only with no draft left in it. An exchange rate
is entered per company and currency pair from a date; the rate on a date is the latest one starting on or before it,
and a foreign-currency posting must state exactly that rate.

**Hypothesis 8, first half (magnitude), is refuted for the summation.** Measured in `tests/test_ledger_reports.cpp`:
10⁷ lines of up to 10⁹ minor units each total exactly through `lims::ExactSum` (`checkedAdd`) in [measured] ms, and a
trial balance whose total passes `int64` throws `OutOfRange` instead of answering a number. The hypothesis' premise
holds: `operator+` on the same values saturates to [measured clamp] and reports success, so exactness depends on
every total in lims using `ExactSum`, which reports and models do by construction and nothing in morph enforces. Not
measured: a report over 10⁷ rows in the database — the rows are loaded into memory, which is hypothesis 10's subject
in Part C.
```

- [ ] **Step 8: Commit**

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/ledger examples/lims/tests examples/lims/app/include/lims/app/declared_requirements.hpp \
        examples/lims/README.md
git commit -m "Add lims exchange rates and ledger reports, and measure exact totals at ERP magnitude

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 7: Billing — the module, settings, billing clients and price lists

**Files:**
- Create: `examples/lims/billing/include/.clang-tidy`, `examples/lims/billing/include/lims/billing/core/types.hpp`, `examples/lims/billing/include/lims/billing/dto/settings_dto.hpp`, `examples/lims/billing/include/lims/billing/dto/price_dto.hpp`, `examples/lims/billing/include/lims/billing/db/billing_entity.hpp`, `examples/lims/billing/include/lims/billing/models/price_list_model.hpp`, `examples/lims/billing/include/lims/billing/authorization/billing_authorization.hpp`, `examples/lims/billing/src/db/billing_schema.cpp`, `examples/lims/billing/src/models/price_list_model.cpp`, `examples/lims/billing/src/authorization/billing_authorization.cpp`, `examples/lims/tests/test_price_lists.cpp`, `examples/lims/tests/compile_checks/billing_includes_ledger.cpp`, `examples/lims/tests/compile_checks/ledger_includes_billing.cpp`
- Modify: `examples/lims/CMakeLists.txt`, `declared_requirements.hpp`, `test_declared_authorization.cpp`, `examples/lims/README.md` (Modules table, decision 30)

**Interfaces:**
- Consumes: `Amount`, `Rate`, `Count`, `fitsCurrency`, `ClientId`, `AnalysisId`, `CompanyRule`, `OrgModel::findCompany`.
- Produces: ids `BillingClientId`, `PriceListId`, `PriceId`, `BillableLineId`, `InvoiceId`, `BillingRunId`; enums `BillableLineState { Unbilled, Unpriced, Invoiced }`, `InvoiceState { Draft, Submitted, Approved, Posted }`, `ApprovalLevel { Site, Company }`, `BillingRunState { Running, Finished }` (all in `billing/core/types.hpp`, with `glz::enumerate`); `SetBillingSettings { CompanyId company; std::string receivableAccount, revenueAccount, taxAccount, roundingAccount; Rate taxRate; Count approvalHours; Count billingDay; }` → `BillingSettingsView` (same fields); `GetBillingSettings { CompanyId company; }`; `RegisterBillingClient { ClientId client; CompanyId company; std::string name; CurrencyCode currency; }` → `BillingClientView { BillingClientId id; ClientId client; CompanyId company; std::string name; CurrencyCode currency; }`; `ListBillingClients {}`; `CreatePriceList { CompanyId company; std::string name; CurrencyCode currency; Timestamp validFrom; }` → `PriceListView { PriceListId id; CompanyId company; std::string name; CurrencyCode currency; Timestamp validFrom; std::vector<PriceView> prices; }`; `SetPrice { PriceListId priceList; AnalysisId analysis; ClientId client; Amount amount; }` → `PriceView { PriceId id; AnalysisId analysis; ClientId client; Amount amount; }`; `ListPriceLists { CompanyId company; }`; `QuotePrice { CompanyId company; ClientId client; AnalysisId analysis; Timestamp on; }` → `PriceQuoteView { PriceListId priceList; Amount amount; CurrencyCode currency; }`; statics `PriceListModel::settingsOf(CompanyId)`, `PriceListModel::billingClient(ClientId, CompanyId)`, `PriceListModel::priceOn(CompanyId, ClientId, AnalysisId, Timestamp const&) -> std::optional<PriceQuoteView>`; model `lims::PriceListModel` (`"PriceListModel"`); records `Billing_Settings` (keyed by `company`), `Billing_Client`, `Billing_PriceList`, `Billing_Price`; target `ladder_lims_billing`.

Pricing rule: a client is billed in the currency it is registered with at a company. The price of an analysis on a date is read from that company's price list in the client's currency with the latest `validFrom` not after the date — the list in force — and within it the client's own price if it has one, otherwise the list's price. A list in force with no price for the analysis prices nothing: a new list version replaces the old one whole, so an older list's price never leaks through.

- [ ] **Step 1: Write the failing test**

Create `examples/lims/tests/test_price_lists.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Billing settings, billing clients and price lists with effective dates and per-client prices.

#include <catch2/catch_test_macros.hpp>
#include <morph/util/rational.hpp>
#include <string>

#include "lims/billing/models/price_list_model.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/models/org_model.hpp"
#include "lims_test_support.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;
using morph::math::DecimalPlaces;
using morph::math::Rational;

namespace {

[[nodiscard]] lims::Amount euros(std::int64_t units) { return lims::Amount{Rational{units, DecimalPlaces{2}}}; }

[[nodiscard]] morph::time::Timestamp day(unsigned month, unsigned dayOfMonth) {
    return morph::time::Timestamp{lims::test::at(2026, month, dayOfMonth, 0)};
}

}  // namespace

// Mutation: fall back to an older list when the list in force has no price (the nitrate price of the October list
// answers in December).
TEST_CASE("price lists: the list in force on a date prices an analysis, a client's own price first",
          "[lims][billing][prices]") {
    DbFixture fixture;
    lims::test::grant("bea", lims::LimsRole::BillingClerk);
    ScopedPrincipal const bea{"bea"};
    lims::PriceListModel prices;
    lims::ClientId const waterworks{1};
    lims::ClientId const brewery{2};
    lims::AnalysisId const nitrate{1};
    lims::AnalysisId const turbidity{2};
    prices.execute(lims::RegisterBillingClient{
        .client = waterworks, .company = lims::test::seededCompany(), .name = "Waterworks", .currency = lims::CurrencyCode::EUR});
    prices.execute(lims::RegisterBillingClient{
        .client = brewery, .company = lims::test::seededCompany(), .name = "Brewery", .currency = lims::CurrencyCode::EUR});
    auto const october = prices.execute(lims::CreatePriceList{
        .company = lims::test::seededCompany(), .name = "2026 Q4", .currency = lims::CurrencyCode::EUR, .validFrom = day(10, 1)});
    prices.execute(lims::SetPrice{.priceList = october.id, .analysis = nitrate, .client = {}, .amount = euros(40)});
    prices.execute(lims::SetPrice{.priceList = october.id, .analysis = nitrate, .client = brewery, .amount = euros(35)});
    auto const december = prices.execute(lims::CreatePriceList{
        .company = lims::test::seededCompany(), .name = "December", .currency = lims::CurrencyCode::EUR, .validFrom = day(12, 1)});
    prices.execute(lims::SetPrice{.priceList = december.id, .analysis = turbidity, .client = {}, .amount = euros(20)});

    auto const quote = [](lims::ClientId client, lims::AnalysisId analysis, morph::time::Timestamp on) {
        return lims::PriceListModel::priceOn(lims::test::seededCompany(), client, analysis, on);
    };
    REQUIRE(quote(waterworks, nitrate, day(11, 10)).has_value());
    CHECK(*quote(waterworks, nitrate, day(11, 10))->amount == Rational{40, DecimalPlaces{2}});
    CHECK(*quote(brewery, nitrate, day(11, 10))->amount == Rational{35, DecimalPlaces{2}});
    CHECK_FALSE(quote(waterworks, nitrate, day(12, 2)).has_value());
    CHECK(*quote(waterworks, turbidity, day(12, 2))->amount == Rational{20, DecimalPlaces{2}});
    CHECK_FALSE(quote(waterworks, nitrate, day(9, 30)).has_value());
    CHECK_FALSE(quote(lims::ClientId{3}, nitrate, day(11, 10)).has_value());
}

TEST_CASE("price lists: a JPY list holds whole yen, and a client is registered once per company",
          "[lims][billing][prices]") {
    DbFixture fixture;
    lims::test::grant("bea", lims::LimsRole::BillingClerk);
    ScopedPrincipal const bea{"bea"};
    lims::PriceListModel prices;
    auto const yen = prices.execute(lims::CreatePriceList{
        .company = lims::test::seededCompany(), .name = "Yen", .currency = lims::CurrencyCode::JPY, .validFrom = day(1, 1)});
    CHECK_THROWS_AS(prices.execute(lims::SetPrice{
                        .priceList = yen.id,
                        .analysis = lims::AnalysisId{1},
                        .client = {},
                        .amount = lims::Amount{Rational{morph::math::Numerator{25}, morph::math::Denominator{2},
                                                        DecimalPlaces{1}}}}),
                    lims::ValidationError);
    CHECK(prices.execute(lims::SetPrice{.priceList = yen.id,
                                        .analysis = lims::AnalysisId{1},
                                        .client = {},
                                        .amount = lims::Amount{Rational{4500, DecimalPlaces{0}}}})
              .id.hasValue());
    prices.execute(lims::RegisterBillingClient{
        .client = lims::ClientId{1}, .company = lims::test::seededCompany(), .name = "W", .currency = lims::CurrencyCode::JPY});
    CHECK_THROWS_AS(prices.execute(lims::RegisterBillingClient{.client = lims::ClientId{1},
                                                               .company = lims::test::seededCompany(),
                                                               .name = "W",
                                                               .currency = lims::CurrencyCode::EUR}),
                    lims::Conflict);
}

// Mutation: accept a billing day of 29 (the settings save).
TEST_CASE("price lists: a supervisor sets a company's billing settings, within their ranges",
          "[lims][billing][prices]") {
    DbFixture fixture;
    lims::test::grant("sam", lims::LimsRole::Supervisor);
    ScopedPrincipal const sam{"sam"};
    lims::PriceListModel prices;
    lims::SetBillingSettings settings{.company = lims::test::seededCompany(),
                                      .receivableAccount = "1200",
                                      .revenueAccount = "4000",
                                      .taxAccount = "2200",
                                      .roundingAccount = "6900",
                                      .taxRate = lims::Rate{Rational{morph::math::Numerator{19}, morph::math::Denominator{100}, DecimalPlaces{6}}},
                                      .approvalHours = lims::Count{Rational{48, DecimalPlaces{0}}},
                                      .billingDay = lims::Count{Rational{25, DecimalPlaces{0}}}};
    prices.execute(settings);
    auto const stored = lims::PriceListModel::settingsOf(lims::test::seededCompany());
    REQUIRE(stored.has_value());
    CHECK(stored->revenueAccount == "4000");
    settings.billingDay = lims::Count{Rational{29, DecimalPlaces{0}}};
    CHECK_THROWS_AS(prices.execute(settings), lims::ValidationError);
    settings.billingDay = lims::Count{Rational{1, DecimalPlaces{0}}};
    settings.taxRate = lims::Rate{Rational{2, DecimalPlaces{6}}};
    CHECK_THROWS_AS(prices.execute(settings), lims::ValidationError);
}
```

- [ ] **Step 2: Run it and see it fail**

Run: Build lims. Expected: compile error, `lims/billing/models/price_list_model.hpp` not found.

- [ ] **Step 3: Types, DTOs, records, migration**

Create `billing/include/.clang-tidy` (the `TableName` exemption, as Task 4). Create `billing/core/types.hpp` with the six ids (`LIMS_DEFINE_STRONG_ID`, wire, shape tags `lims.billingClientId`, `lims.priceListId`, `lims.priceId`, `lims.billableLineId`, `lims.invoiceId`, `lims.billingRunId`) and the four enums from Interfaces, each enumerator documented and reflected with `glz::enumerate`, in `types.hpp`'s style from Task 4. Create `settings_dto.hpp` and `price_dto.hpp` with the structs in Interfaces. Validation:

- `SetBillingSettings::validate()`: company engaged; each account code 1–16 letters and digits (the `CreateAccount` rule); `taxRate`, `approvalHours`, `billingDay` engaged. The model then refuses a tax rate outside `[0, 1]`, approval hours below 1 or above 720, a billing day outside 1–28 (every month has it), each as `ValidationError`.
- `RegisterBillingClient::validate()`: client and company engaged, name 1–128 bytes.
- `CreatePriceList::validate()`: company and `validFrom` engaged, name 1–128 bytes.
- `SetPrice::validate()`: price list, analysis and amount engaged (client optional). The model refuses a negative amount or one that does not fit the list's currency.
- Queries require their company (`QuotePrice` also client, analysis, `on`).

Register `BRIDGE_REGISTER_RECORD(lims::BillingSettingsView, "Billing_Settings", ::morph::record::key<&lims::BillingSettingsView::company>)`, `BRIDGE_REGISTER_RECORD(lims::BillingClientView, "Billing_Client")`, `BRIDGE_REGISTER_RECORD(lims::PriceListView, "Billing_PriceList")`, `BRIDGE_REGISTER_RECORD(lims::PriceView, "Billing_Price")`.

Create `billing_entity.hpp` (namespace `lims::db`, `TableName`s `billing_settings`, `billing_clients`, `billing_price_lists`, `billing_prices`) with: `BillingSettingsRecord` (id, `company_id`, the four account codes as `SqlAnsiString<16>`, `tax_rate_num`/`_den`, `approval_hours`, `billing_day` as `std::int64_t`); `BillingClientRecord` (id, `client_id`, `company_id`, `name` `SqlAnsiString<128>`, `currency` int); `PriceListRecord` (id, `company_id`, `name`, `currency`, `valid_from_ms`); `PriceRecord` (id, `BelongsTo<&PriceListRecord::id> price_list_id`, `analysis_id`, `client_id` `std::optional<std::int64_t>`, `amount_num`, `amount_den`). Company, client and analysis ids are plain columns: they are other modules' rows. Create `billing/src/db/billing_schema.cpp` with `LIGHTWEIGHT_SQL_MIGRATION(20261110000005, "Create billing settings, clients, price lists and prices")`, one `CreateTableIfNotExists` per record in `ledger_schema.cpp`'s style, `client_id` of `billing_prices` a nullable `.Column`.

- [ ] **Step 4: Rules and the model**

`billing_authorization.hpp`:

```cpp
/// @brief A company's billing settings are a supervisor's.
template <>
struct Authorization<SetBillingSettings> : CompanyRule<SetBillingSettings, LimsRole::Supervisor> {};

/// @brief A client is registered for billing by a billing clerk at the company.
template <>
struct Authorization<RegisterBillingClient> : CompanyRule<RegisterBillingClient, LimsRole::BillingClerk> {};

/// @brief A price list is created by a billing clerk at its company.
template <>
struct Authorization<CreatePriceList> : CompanyRule<CreatePriceList, LimsRole::BillingClerk> {};

/// @brief A price is set by a billing clerk at the price list's company.
template <>
struct Authorization<SetPrice> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::BillingClerk;

    /// @brief The company of the price list the payload names.
    /// @param action The action.
    /// @return It; an empty company when there is no such list.
    [[nodiscard]] static Scope scopeOf(SetPrice const& action);
};
```

with `scopeOf` in `billing_authorization.cpp` reading `PriceListRecord` as `ledger_authorization.cpp`'s `periodCompany` does.

`price_list_model.hpp` declares `class PriceListModel` with the seven `execute` overloads (Doxygen in Task 4's shape, each `@throws` as the validation above says, plus `NotFound` for a missing company, price list or settings, `Conflict` for a second registration of a client at a company or a second price for the same analysis and client in a list, `Forbidden`, `EmptyPrincipalError`) and the three statics:

```cpp
    /// @brief The company's billing settings.
    /// @param company The company.
    /// @return Them, or nothing when the company has none.
    [[nodiscard]] static std::optional<BillingSettingsView> settingsOf(CompanyId company);

    /// @brief How @p client is billed at @p company.
    /// @param client The lab's client.
    /// @param company The company.
    /// @return The registration, or nothing when the client is not registered there.
    [[nodiscard]] static std::optional<BillingClientView> billingClient(ClientId client, CompanyId company);

    /// @brief The price of @p analysis for @p client at @p company on @p on: from the list in force in the client's
    ///        currency, the client's own price first.
    /// @param company The company.
    /// @param client The client.
    /// @param analysis The analysis definition.
    /// @param on The date.
    /// @return The price, or nothing when the client is not registered, no list is in force, or it has no price for
    ///         the analysis.
    [[nodiscard]] static std::optional<PriceQuoteView> priceOn(CompanyId company, ClientId client,
                                                               AnalysisId analysis, ::morph::time::Timestamp const& on);
```

Registrations: `"SetBillingSettings"` (`changes<BillingSettingsView>`), `"GetBillingSettings"` (read), `"RegisterBillingClient"` (`changes<BillingClientView>`), `"ListBillingClients"` (read), `"CreatePriceList"` (`changes<PriceListView>`), `"SetPrice"` (`changes<PriceView>`, `changes<PriceListView>` — a list's view holds its prices), `"ListPriceLists"` (read), `"QuotePrice"` (read).

`priceOn` in `price_list_model.cpp`:

```cpp
std::optional<PriceQuoteView> PriceListModel::priceOn(CompanyId company, ClientId client, AnalysisId analysis,
                                                      ::morph::time::Timestamp const& on) {
    auto const billed = billingClient(client, company);
    if (!billed.has_value() || !analysis.hasValue() || !on.hasValue()) {
        return std::nullopt;
    }
    Lightweight::DataMapper mapper;
    std::optional<db::PriceListRecord> inForce;
    for (auto const& list : mapper.Query<db::PriceListRecord>()
                                .Where(::Lightweight::FieldNameOf<&db::PriceListRecord::companyId>, "=", *company)
                                .Where(::Lightweight::FieldNameOf<&db::PriceListRecord::currency>, "=",
                                       static_cast<int>(billed->currency))
                                .All()) {
        if (list.validFromMs.Value() <= millisOf(on) &&
            (!inForce.has_value() || list.validFromMs.Value() > inForce->validFromMs.Value())) {
            inForce = list;
        }
    }
    if (!inForce.has_value()) {
        return std::nullopt;
    }
    std::optional<db::PriceRecord> general;
    for (auto const& price : mapper.Query<db::PriceRecord>()
                                 .Where(::Lightweight::FieldNameOf<&db::PriceRecord::priceList>, "=",
                                        static_cast<std::int64_t>(inForce->id.Value()))
                                 .Where(::Lightweight::FieldNameOf<&db::PriceRecord::analysisId>, "=", *analysis)
                                 .All()) {
        if (price.clientId.Value() == client.value) {
            general = price;
            break;
        }
        if (!price.clientId.Value().has_value()) {
            general = price;
        }
    }
    if (!general.has_value()) {
        return std::nullopt;
    }
    return PriceQuoteView{
        .priceList = PriceListId{static_cast<std::int64_t>(inForce->id.Value())},
        .amount = Amount{::morph::math::Rational{::morph::math::Numerator{general->amountNum.Value()},
                                                 ::morph::math::Denominator{general->amountDen.Value()},
                                                 ::morph::math::DecimalPlaces{decimalsOf(billed->currency)}}},
        .currency = billed->currency};
}
```

The remaining actions follow the conventions' action and list shapes; lists filter by `visibleCompanies`. `ListPriceLists` answers each list with its prices.

- [ ] **Step 5: The module, its probes, its rules in the edge table**

`examples/lims/CMakeLists.txt`: `MODULES org posting lab ledger billing app`; add `_lims_boundary_probe(billing_includes_ledger billing fail lims/ledger/models/ledger_model.hpp)` and `_lims_boundary_probe(ledger_includes_billing ledger fail lims/billing/models/price_list_model.hpp)`, with the two probe files in Task 2's shape (comments: "billing reaches the ledger only through the posting contract and the app's port" / "the ledger knows nothing of invoices"). Add the four declared actions to `declaredRequirements()` and the coverage tuple, `"PriceListModel"` to its model list, and a `billing/` row to the README's Modules table: `` | `billing/` | billing settings, clients, price lists, billable lines, invoices, the edit lease, billing runs | `org`, `posting` | ``.

- [ ] **Step 6: Build and run**

Run: Configure, Build lims, Model suite `"[billing]"`, Model suite `"[authorization]"`, Everything labelled for the rung. Expected: all pass, the two new boundary probes included. Run both named mutations and restore them.

- [ ] **Step 7: Record decision 30 and commit**

Append to the README:

```markdown
### 30. A price is read from the list in force, and a new list replaces the old one whole

A client is billed at a company in the currency it is registered with there (`RegisterBillingClient`); lims's client
record has no company or currency, since clients are reference data the whole lab shares (decision 22). A price list
belongs to a company, has a currency and a date it applies from; the list in force on a date is the latest that
applies by then, in the client's currency. In it, the client's own price for the analysis wins over the list's. A list
in force with no price for an analysis prices nothing — an older list is never consulted, so retiring a price is
making a list without it. A company's billing settings (its receivable, revenue, tax and rounding accounts, its flat
tax rate, how many hours an approval may wait, and the day of the month the billing run drafts invoices, 1 to 28 so
that every month has it) are a supervisor's.
```

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/billing examples/lims/CMakeLists.txt examples/lims/tests \
        examples/lims/app/include/lims/app/declared_requirements.hpp examples/lims/README.md
git commit -m "Add lims's billing module with settings, billing clients and dated price lists

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---
### Task 8: Domain events — `lab.AnalysisPublished` becomes a billable line (hypothesis 2, first instance)

**Files:**
- Create: `examples/lims/lab/include/lims/lab/events/lab_events.hpp`, `examples/lims/lab/include/lims/lab/events/lab_outbox_store.hpp`, `examples/lims/lab/src/events/lab_events.cpp`, `examples/lims/billing/include/lims/billing/dto/line_dto.hpp`, `examples/lims/billing/include/lims/billing/models/invoice_model.hpp`, `examples/lims/billing/src/models/invoice_model.cpp`, `examples/lims/app/include/lims/app/event_wiring.hpp`, `examples/lims/app/src/event_wiring.cpp`, `examples/lims/tests/billing_test_support.hpp`, `examples/lims/tests/test_domain_events.cpp`
- Modify: `examples/lims/lab/include/lims/lab/db/lims_entity.hpp` (`LabOutboxRecord`), `examples/lims/lab/src/db/schema.cpp` (migration), `examples/lims/lab/src/models/sample_model.cpp` (`writeState`), `billing_entity.hpp` (`BillableLineRecord`, `ConsumedEventRecord`), `billing_schema.cpp`, `examples/lims/src/server/main.cpp` (the relay on a timer), `examples/lims/README.md` (decision 31)

**Interfaces:**
- Consumes: `morph::journal::OutboxRelay`, `morph::journal::LogEntry`, `morph::journal::IActionLog`, `morph::offline::IReplayLedger`, `PriceListModel::priceOn`, `OrgModel::findSite`.
- Produces: `struct lims::AnalysisPublished { ResultId result; SampleId sample; AnalysisId analysis; ClientId client; SiteId site; Timestamp publishedAt; }`; `inline constexpr std::string_view lims::kAnalysisPublished = "lab.AnalysisPublished"`; `std::vector<morph::journal::LogEntry> lims::drainLabOutbox()`; `void lims::markLabOutboxRelayed(std::span<morph::journal::LogEntry const>)`; `void lims::writeAnalysisPublished(Lightweight::DataMapper&, db::SampleRecord const&, std::string const& principal)` (lab-internal); `struct lims::BillableEvent { std::string eventId; ResultId result; SampleId sample; AnalysisId analysis; ClientId client; SiteId site; CompanyId company; Timestamp publishedAt; }`; `BillableLineView { BillableLineId id; CompanyId company; SiteId site; ClientId client; SampleId sample; ResultId result; AnalysisId analysis; Timestamp publishedAt; BillableLineState state; Amount amount; CurrencyCode currency; InvoiceId invoice; std::string eventId; }`; `ListBillableLines { CompanyId company; ClientId client; SampleId sample; }` → `ListBillableLinesResult`; `static BillableLineView InvoiceModel::consume(BillableEvent const&)`; model `lims::InvoiceModel` (`"InvoiceModel"`); record `Billing_Line`; `class lims::app::BillingEventSink final : public morph::journal::IActionLog`; `std::size_t lims::app::relayLabEvents()`; test helpers `lims::test::LabFixture { ClientId client; AnalysisId nitrate; SampleId sample; std::vector<ResultId> results; }`, `LabFixture lims::test::readyToPublish(std::string reference = "WW-1")`, `LabFixture lims::test::publishedSample(std::string reference = "WW-1")`, `void lims::test::billingFor(ClientId, CurrencyCode = EUR)`.

The event id is `lab.AnalysisPublished:<result id>:<sample version after publishing>`: one event per published result, and a redelivery carries the same id. The payload is `AnalysisPublished` as JSON; billing never sees that type — the app decodes it and hands billing its own `BillableEvent`, so neither module includes the other.

A line whose client is not registered for billing at the sample's company, or for which no price is in force, is recorded `Unpriced` with no amount, not refused: a consumer that refuses an event stops the relay at that event, and every later event waits behind it (measured below).

- [ ] **Step 1: The shared test fixture**

Create `examples/lims/tests/billing_test_support.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/util/rational.hpp>
#include <string>
#include <vector>

#include "lims/billing/models/price_list_model.hpp"
#include "lims/lab/models/analysis_catalog_model.hpp"
#include "lims/lab/models/sample_model.hpp"
#include "lims_test_support.hpp"

/// @file
/// A sample with verified results, ready to publish or published, and a client set up to be billed: what every
/// billing suite starts from.

namespace lims::test {

/// @brief What `readyToPublish` made.
struct LabFixture {
    /// @brief The lab's client.
    ClientId client;
    /// @brief The nitrate analysis definition.
    AnalysisId nitrate;
    /// @brief The sample, at the seeded site.
    SampleId sample;
    /// @brief Its results, all verified.
    std::vector<ResultId> results;
};

/// @brief A sample at the seeded site with one nitrate reading captured by `alice` and verified by `carol`, in
///        `ToBeVerified`. Grants carol the verifier role at the seeded company.
/// @param reference The sample's reference.
/// @return The ids.
[[nodiscard]] inline LabFixture readyToPublish(std::string reference = "WW-1") {
    grant("carol", LimsRole::Verifier);
    LabFixture lab;
    ResultId result;
    {
        ScopedPrincipal const alice{"alice"};
        AnalysisCatalogModel catalog;
        auto const nitrate =
            catalog.execute(DefineAnalysis{.name = "Nitrate " + reference, .canonicalUnit = "mg_per_L", .decimalPlaces = 3});
        lab.nitrate = nitrate.analysisId;
        SampleModel bench;
        lab.client = bench.execute(RegisterClient{.name = "Waterworks Ltd"}).clientId;
        lab.sample = bench.execute(RegisterSample{.siteId = seededSite(), .clientId = lab.client, .reference = reference}).id;
        bench.execute(ReceiveSample{});
        bench.execute(StartWork{});
        result = bench
                     .execute(CaptureConcentration{
                         .analysisVersionId = nitrate.versionId,
                         .value = Concentration{::morph::math::Rational{::morph::math::Numerator{12},
                                                                        ::morph::math::Denominator{5},
                                                                        ::morph::math::DecimalPlaces{3}}}})
                     .id;
        bench.execute(SubmitForVerification{});
    }
    {
        ScopedPrincipal const carol{"carol"};
        SampleModel verifier;
        verifier.execute(OpenSample{.sampleId = lab.sample});
        verifier.execute(VerifyResult{.resultId = result});
    }
    lab.results.push_back(result);
    return lab;
}

/// @brief `readyToPublish`, then published by `alice`.
/// @param reference The sample's reference.
/// @return The ids.
[[nodiscard]] inline LabFixture publishedSample(std::string reference = "WW-1") {
    auto lab = readyToPublish(std::move(reference));
    ScopedPrincipal const alice{"alice"};
    SampleModel bench;
    bench.execute(OpenSample{.sampleId = lab.sample});
    bench.execute(PublishSample{});
    return lab;
}

/// @brief Registers @p client for billing at the seeded company in @p currency, with a price list in force since
///        1 January 2026 pricing every analysis id from 1 to 20 at 40 units (4000 yen in JPY). Done by `bea`, a
///        billing clerk there.
/// @param client The lab's client.
/// @param currency The client's currency.
inline void billingFor(ClientId client, CurrencyCode currency = CurrencyCode::EUR) {
    grant("bea", LimsRole::BillingClerk);
    ScopedPrincipal const bea{"bea"};
    PriceListModel prices;
    prices.execute(RegisterBillingClient{.client = client, .company = seededCompany(), .name = "Waterworks Ltd", .currency = currency});
    auto const list = prices.execute(CreatePriceList{.company = seededCompany(),
                                                     .name = std::string{currencyName(currency)} + " list",
                                                     .currency = currency,
                                                     .validFrom = ::morph::time::Timestamp{at(2026, 1, 1, 0)}});
    std::int64_t const price = currency == CurrencyCode::JPY ? 4000 : 40;
    for (std::int64_t analysis = 1; analysis <= 20; ++analysis) {
        prices.execute(SetPrice{.priceList = list.id,
                                .analysis = AnalysisId{analysis},
                                .client = {},
                                .amount = Amount{::morph::math::Rational{price, ::morph::math::DecimalPlaces{decimalsOf(currency)}}}});
    }
}

}  // namespace lims::test
```

If `CaptureConcentration`'s result type names its id other than `.id`, or `SampleModel` requires the capture on an `OpenSample`-attached handler, follow `test_declared_authorization.cpp`'s `twoSites()`, which this mirrors.

- [ ] **Step 2: Write the failing test**

Create `examples/lims/tests/test_domain_events.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Hypothesis 2, first instance: lab's AnalysisPublished reaches billing's consumer through lab's outbox, morph's
// OutboxRelay and an IReplayLedger in billing, and bills each published result once, across redelivery and restart.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <memory>
#include <morph/journal/outbox.hpp>
#include <span>
#include <stdexcept>
#include <string>

#include "billing_test_support.hpp"
#include "lims/app/event_wiring.hpp"
#include "lims/billing/models/invoice_model.hpp"
#include "lims/lab/db/lims_entity.hpp"
#include "lims/lab/events/lab_events.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;

namespace {

/// @brief The lines billing holds for @p sample, as bea sees them.
/// @param sample The sample.
/// @return The lines.
[[nodiscard]] std::vector<lims::BillableLineView> linesOf(lims::SampleId sample) {
    ScopedPrincipal const bea{"bea"};
    return lims::InvoiceModel{}.execute(lims::ListBillableLines{.company = {}, .client = {}, .sample = sample}).lines;
}

}  // namespace

// Mutation: drop the writeAnalysisPublished call from writeState (no row is written; the size check fails).
TEST_CASE("domain events: publishing a sample writes one outbox row per result, with a stable event id",
          "[lims][events]") {
    DbFixture fixture;
    auto const ready = lims::test::readyToPublish();
    CHECK(lims::drainLabOutbox().empty());
    {
        ScopedPrincipal const alice{"alice"};
        lims::SampleModel bench;
        bench.execute(lims::OpenSample{.sampleId = ready.sample});
        auto const published = bench.execute(lims::PublishSample{});
        auto const rows = lims::drainLabOutbox();
        REQUIRE(rows.size() == 1U);
        CHECK(rows[0].actionType == lims::kAnalysisPublished);
        CHECK(rows[0].idempotencyKey == "lab.AnalysisPublished:" + std::to_string(*ready.results[0]) + ":" +
                                            std::to_string(*published.version));
        CHECK(rows[0].principal == "alice");
    }
    // Draining is not consuming: the row stays until it is marked relayed.
    CHECK(lims::drainLabOutbox().size() == 1U);
}

TEST_CASE("domain events: a refused publish writes no outbox row", "[lims][events]") {
    DbFixture fixture;
    auto lab = lims::test::readyToPublish();
    ScopedPrincipal const alice{"alice"};
    lims::SampleModel bench;
    bench.execute(lims::OpenSample{.sampleId = lab.sample});
    bench.execute(lims::ReturnForRework{});
    CHECK_THROWS_AS(bench.execute(lims::PublishSample{}), lims::IllegalTransition);
    CHECK(lims::drainLabOutbox().empty());
}

// Hypothesis 2. Mutation: drop the replay-ledger lookup from InvoiceModel::consume (the redelivered batch bills the
// result twice).
TEST_CASE("Hypothesis 2: an event bills once, across a relay that dies before marking it and a restart",
          "[lims][events][h2]") {
    DbFixture fixture;
    auto const lab = lims::test::publishedSample();
    lims::test::billingFor(lab.client);

    // A relay that delivers and then dies before it marks the batch: what a crash between the two looks like.
    {
        morph::journal::OutboxRelay dying;
        dying.drainOutbox = [] { return lims::drainLabOutbox(); };
        dying.markRelayed = [](std::span<morph::journal::LogEntry const> /*rows*/) {
            throw std::runtime_error{"process killed before marking the batch"};
        };
        dying.sink = std::make_shared<lims::app::BillingEventSink>();
        CHECK_THROWS_WITH(dying.relay(), Catch::Matchers::ContainsSubstring("process killed"));
    }
    auto const afterCrash = linesOf(lab.sample);
    REQUIRE(afterCrash.size() == 1U);
    CHECK(afterCrash[0].state == lims::BillableLineState::Unbilled);
    CHECK(*afterCrash[0].amount == morph::math::Rational{40, morph::math::DecimalPlaces{2}});
    CHECK(lims::drainLabOutbox().size() == 1U);

    // The restart: a fresh relay over the same database redelivers the event, and billing recognises it.
    CHECK(lims::app::relayLabEvents() == 1U);
    auto const afterRestart = linesOf(lab.sample);
    REQUIRE(afterRestart.size() == 1U);
    CHECK(afterRestart[0].id == afterCrash[0].id);
    CHECK(lims::drainLabOutbox().empty());
    CHECK(lims::app::relayLabEvents() == 0U);
}

TEST_CASE("domain events: a line with no price in force is recorded unpriced, not refused", "[lims][events]") {
    DbFixture fixture;
    auto const lab = lims::test::publishedSample();
    CHECK(lims::app::relayLabEvents() == 1U);
    lims::test::grant("bea", lims::LimsRole::BillingClerk);
    auto const lines = linesOf(lab.sample);
    REQUIRE(lines.size() == 1U);
    CHECK(lines[0].state == lims::BillableLineState::Unpriced);
    CHECK_FALSE(lines[0].amount.hasValue());
}

// What a consumer that refuses an event does to the events after it.
TEST_CASE("Hypothesis 2: an event the consumer refuses holds back every event after it", "[lims][events][h2]") {
    DbFixture fixture;
    {
        Lightweight::DataMapper mapper;
        lims::db::LabOutboxRecord poison;
        poison.eventId = Lightweight::SqlAnsiString<128>{"lab.AnalysisPublished:999:1"};
        poison.eventType = Lightweight::SqlAnsiString<64>{std::string{lims::kAnalysisPublished}};
        poison.entityKey = Lightweight::SqlAnsiString<64>{"999"};
        poison.payload = Lightweight::SqlDynamicAnsiString<4096>{"{\"result\":999}"};
        poison.principal = Lightweight::SqlAnsiString<64>{"alice"};
        mapper.Create(poison);
    }
    auto const lab = lims::test::publishedSample();
    lims::test::billingFor(lab.client);
    CHECK_THROWS(lims::app::relayLabEvents());
    CHECK(lims::drainLabOutbox().size() == 2U);
    CHECK(linesOf(lab.sample).empty());
}
```

The poison event has no site, so the app cannot find its company and the consumer refuses it (`ValidationError` from `BillingEventSink::append`). `OutboxRelay::relay` appends in drain order and rethrows, so the good event behind it is never delivered and nothing is marked; the drained order is by outbox id, which puts the poison first.

- [ ] **Step 3: Run it and see it fail**

Run: Build lims. Expected: compile error, `lims/app/event_wiring.hpp` not found.

- [ ] **Step 4: Lab writes the event with the fact**

In `lims_entity.hpp` add:

```cpp
/// @brief One event lab committed and has not yet seen relayed: written in the transaction of the fact it reports,
///        deleted once the relay has delivered it.
struct LabOutboxRecord {
    static constexpr std::string_view TableName = "lims_outbox";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    /// @brief `<module>.<event>:<aggregate id>:<aggregate version>`; the same for a redelivery.
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"event_id"}> eventId;            // 1
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"event_type"}> eventType;         // 2
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"entity_key"}> entityKey;         // 3
    Light::Field<Light::SqlDynamicAnsiString<4096>, Light::SqlRealName{"payload"}> payload;     // 4
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"principal"}> principal;          // 5
    Light::Field<std::int64_t, Light::SqlRealName{"recorded_at"}> recordedAt{0};                // 6
};
```

and in `schema.cpp`:

```cpp
LIGHTWEIGHT_SQL_MIGRATION(20261110000006, "Create lims_outbox") {
    plan.CreateTableIfNotExists("lims_outbox")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("event_id", Varchar(128))
        .RequiredColumn("event_type", Varchar(64))
        .RequiredColumn("entity_key", Varchar(64))
        .RequiredColumn("payload", Varchar(4096))
        .RequiredColumn("principal", Varchar(64))
        .RequiredColumn("recorded_at", Bigint());
}
```

Create `examples/lims/lab/include/lims/lab/events/lab_events.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/journal/action_log.hpp>
#include <morph/util/datetime.hpp>
#include <span>
#include <string_view>
#include <vector>

#include "lims/org/core/references.hpp"
#include "lims/org/core/types.hpp"

/// @file
/// The facts lab tells other modules about, and its outbox: rows written in the transaction of the fact, read and
/// marked by a relay the app runs. ORM-free, so the screens and the app can include it.

namespace lims {

/// @brief The event type of a published result.
inline constexpr std::string_view kAnalysisPublished = "lab.AnalysisPublished";

/// @brief A result was published to its client.
struct AnalysisPublished {
    /// @brief The result.
    ResultId result;
    /// @brief Its sample.
    SampleId sample;
    /// @brief The analysis definition it was captured under.
    AnalysisId analysis;
    /// @brief The sample's client.
    ClientId client;
    /// @brief The sample's site.
    SiteId site;
    /// @brief When the sample was published.
    ::morph::time::Timestamp publishedAt;
};

/// @brief Every event lab committed and no relay has marked, oldest first, as journal entries: `actionType` is the
///        event type, `idempotencyKey` the event id, `payload` the event as JSON.
/// @return The entries; reading them changes nothing.
[[nodiscard]] std::vector<::morph::journal::LogEntry> drainLabOutbox();

/// @brief Deletes the outbox rows of @p rows, by event id: they were delivered.
/// @param rows Entries `drainLabOutbox` returned.
void markLabOutboxRelayed(std::span<::morph::journal::LogEntry const> rows);

}  // namespace lims
```

Create `examples/lims/lab/include/lims/lab/events/lab_outbox_store.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <string>

#include "lims/lab/db/lims_entity.hpp"

/// @file
/// Writing lab's events, inside the caller's transaction. Lab-internal: it names the ORM, which model headers do not.

namespace lims {

/// @brief Writes one `lab.AnalysisPublished` row per result of @p sample, through the caller's open mapper, so the
///        rows commit with the publication or not at all.
/// @param mapper The caller's mapper, inside its transaction.
/// @param sample The sample, as it is being published (its version already moved).
/// @param principal Who published it.
void writeAnalysisPublished(Lightweight::DataMapper& mapper, db::SampleRecord const& sample,
                            std::string const& principal);

}  // namespace lims
```

Create `examples/lims/lab/src/events/lab_events.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "lims/lab/events/lab_events.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <string>

#include "lims/lab/events/lab_outbox_store.hpp"
#include "lims/org/core/model_support.hpp"

namespace lims {

void writeAnalysisPublished(Lightweight::DataMapper& mapper, db::SampleRecord const& sample,
                            std::string const& principal) {
    auto const sampleId = static_cast<std::int64_t>(sample.id.Value());
    std::int64_t const now = nowMillis();
    for (auto const& result : mapper.Query<db::ResultRecord>()
                                  .Where(::Lightweight::FieldNameOf<&db::ResultRecord::sample>, "=", sampleId)
                                  .All()) {
        auto const versions = mapper.Query<db::AnalysisVersionRecord>()
                                  .Where(::Lightweight::FieldNameOf<&db::AnalysisVersionRecord::id>, "=",
                                         static_cast<std::int64_t>(result.analysisVersion.Value()))
                                  .All();
        AnalysisPublished const event{
            .result = ResultId{static_cast<std::int64_t>(result.id.Value())},
            .sample = SampleId{sampleId},
            .analysis = versions.empty() ? AnalysisId{}
                                         : AnalysisId{static_cast<std::int64_t>(versions.front().analysis.Value())},
            .client = ClientId{static_cast<std::int64_t>(sample.client.Value())},
            .site = SiteId::fromOptional(sample.siteId.Value()),
            .publishedAt = timestampFromMillis(now)};
        db::LabOutboxRecord row;
        row.eventId = Lightweight::SqlAnsiString<128>{std::string{kAnalysisPublished} + ":" +
                                                      std::to_string(*event.result) + ":" +
                                                      std::to_string(sample.version.Value())};
        row.eventType = Lightweight::SqlAnsiString<64>{std::string{kAnalysisPublished}};
        row.entityKey = Lightweight::SqlAnsiString<64>{std::to_string(sampleId)};
        row.payload = Lightweight::SqlDynamicAnsiString<4096>{glz::write_json(event).value_or(std::string{})};
        row.principal = Lightweight::SqlAnsiString<64>{principal};
        row.recordedAt = now;
        mapper.Create(row);
    }
}

std::vector<::morph::journal::LogEntry> drainLabOutbox() {
    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::LabOutboxRecord>().All();
    std::ranges::sort(rows, {}, [](db::LabOutboxRecord const& row) { return row.id.Value(); });
    std::vector<::morph::journal::LogEntry> entries;
    for (auto const& row : rows) {
        // journal-stamp-exempt: an event row carries its own type and payload; it was not produced by executing an
        // action, so there is no action shape to fingerprint.
        ::morph::journal::LogEntry entry;
        entry.modelType = "SampleModel";
        entry.entityKey = std::string{row.entityKey.Value()};
        entry.actionType = std::string{row.eventType.Value()};
        entry.payload = std::string{row.payload.Value()};
        entry.principal = std::string{row.principal.Value()};
        entry.timestampMs = row.recordedAt.Value();
        entry.idempotencyKey = std::string{row.eventId.Value()};
        entries.push_back(std::move(entry));
    }
    return entries;
}

void markLabOutboxRelayed(std::span<::morph::journal::LogEntry const> rows) {
    Lightweight::DataMapper mapper;
    for (auto const& entry : rows) {
        for (auto const& row : mapper.Query<db::LabOutboxRecord>()
                                   .Where(::Lightweight::FieldNameOf<&db::LabOutboxRecord::eventId>, "=",
                                          entry.idempotencyKey)
                                   .All()) {
            mapper.Delete(row);
        }
    }
}

}  // namespace lims
```

(`std::string{row.payload.Value()}` uses the strong string's explicit conversion, as `bookmarks/src/app/app.cpp`'s relay does; add `#include <algorithm>`.) `bookmarks` marks relayed rows with a raw `DELETE`; `DataMapper::Delete` expresses it, so it is used here (rule 4). If `Delete` is not available on this Lightweight pin (not verified by reading), use `Lightweight::SqlStatement` with `DELETE FROM lims_outbox WHERE event_id = ?` exactly as bookmarks does and add the rule-4 finding line to the README's findings.

In `sample_model.cpp`'s `SampleModel::writeState`, before `sqlTxn.Commit();`, add:

```cpp
    // The events go in the publication's own transaction: a published sample whose results were never announced,
    // or an announcement of a publication that rolled back, would each bill wrongly.
    if (target == SampleState::Published) {
        writeAnalysisPublished(mapper, row, ::morph::session::current()->principal);
    }
```

with `#include "lims/lab/events/lab_outbox_store.hpp"`.

- [ ] **Step 5: Billing's lines and its consumer**

Create `examples/lims/billing/include/lims/billing/dto/line_dto.hpp` with `BillableEvent` (documented "What billing is told when a result is published; not an action — only the app's event wiring hands one over"), `BillableLineView`, `ListBillableLines` (`validate()` always `true`; an empty filter field matches every line) and `ListBillableLinesResult`, and `BRIDGE_REGISTER_RECORD(lims::BillableLineView, "Billing_Line")`.

Append to `billing_entity.hpp`:

```cpp
/// @brief One published result, to be billed.
struct BillableLineRecord {
    static constexpr std::string_view TableName = "billing_lines";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"company_id"}> companyId{0};                             // 1
    Light::Field<std::int64_t, Light::SqlRealName{"site_id"}> siteId{0};                                   // 2
    Light::Field<std::int64_t, Light::SqlRealName{"client_id"}> clientId{0};                               // 3
    Light::Field<std::int64_t, Light::SqlRealName{"sample_id"}> sampleId{0};                               // 4
    Light::Field<std::int64_t, Light::SqlRealName{"result_id"}> resultId{0};                               // 5
    Light::Field<std::int64_t, Light::SqlRealName{"analysis_id"}> analysisId{0};                           // 6
    Light::Field<std::int64_t, Light::SqlRealName{"published_at"}> publishedAt{0};                         // 7
    /// @brief `BillableLineState` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"state"}> state{0};                                               // 8
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"amount_num"}> amountNum;                 // 9
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"amount_den"}> amountDen;                 // 10
    /// @brief `CurrencyCode` as its underlying integer; meaningful only when priced.
    Light::Field<int, Light::SqlRealName{"currency"}> currency{0};                                         // 11
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"invoice_id"}> invoiceId;                 // 12
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"event_id"}> eventId;                       // 13
};

/// @brief An event billing has consumed, with the line it made: one row per event id.
struct ConsumedEventRecord {
    static constexpr std::string_view TableName = "billing_consumed_events";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<Light::SqlAnsiString<32>, Light::SqlRealName{"scope"}> scope;                             // 1
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"event_id"}> eventId;                       // 2
    Light::Field<Light::SqlAnsiString<32>, Light::SqlRealName{"line"}> line;                               // 3
};
```

and migration `LIGHTWEIGHT_SQL_MIGRATION(20261110000007, "Create billing_lines and billing_consumed_events")` (nullable `.Column` for the three optionals).

Create `invoice_model.hpp`: `class InvoiceModel` with `ListBillableLinesResult execute(const ListBillableLines& action);` and

```cpp
    /// @brief Turns a published result into a billable line, once per event id.
    ///
    /// Not an action: only the app's event wiring calls it. Priced at the price in force on the publishing date
    /// (`PriceListModel::priceOn`); recorded `Unpriced` with no amount when there is none. The event id is recorded
    /// in the line's own transaction through an `IReplayLedger`, so a redelivered event answers the line it made.
    /// @param event The event.
    /// @return The line.
    /// @throws ValidationError if the event names no event id, company, site, client, sample, result or analysis.
    [[nodiscard]] static BillableLineView consume(BillableEvent const& event);
```

registered `BRIDGE_REGISTER_MODEL(lims::InvoiceModel, "InvoiceModel")` and `BRIDGE_REGISTER_ACTION(lims::InvoiceModel, lims::ListBillableLines, "ListBillableLines", ::morph::model::Loggable::No)`.

In `invoice_model.cpp`:

```cpp
namespace {

/// @brief The events billing consumed, as an `IReplayLedger` over `billing_consumed_events`, constructed over the
///        consumer's open mapper so an event id is recorded in the transaction of the line it made. The payload is
///        the line's id.
class ConsumedEvents final : public ::morph::offline::IReplayLedger {
public:
    /// @param mapper The consumer's mapper. Borrowed: it must outlive this instance.
    explicit ConsumedEvents(Lightweight::DataMapper& mapper) : _mapper{mapper} {}

protected:
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- scope/opId, not interchangeable
    [[nodiscard]] std::optional<std::string> doLookup(std::string_view scope, std::string_view opId) const override {
        auto const rows =
            _mapper.Query<db::ConsumedEventRecord>()
                .Where(::Lightweight::FieldNameOf<&db::ConsumedEventRecord::scope>, "=", std::string{scope})
                .Where(::Lightweight::FieldNameOf<&db::ConsumedEventRecord::eventId>, "=", std::string{opId})
                .All();
        if (rows.empty()) {
            return std::nullopt;
        }
        return std::string{rows.front().line.Value().ToStringView()};
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- scope/opId, not interchangeable
    void doRecord(std::string_view scope, std::string_view opId, std::string payload) override {
        db::ConsumedEventRecord row;
        row.scope = Lightweight::SqlAnsiString<32>{std::string{scope}};
        row.eventId = Lightweight::SqlAnsiString<128>{std::string{opId}};
        row.line = Lightweight::SqlAnsiString<32>{payload};
        _mapper.Create(row);
    }

private:
    // Borrowed for one consume(); never reseated.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    Lightweight::DataMapper& _mapper;
};

/// @brief The scope billing's consumed events are recorded in.
constexpr std::string_view kLineEvents = "billing.lines";

}  // namespace

BillableLineView InvoiceModel::consume(BillableEvent const& event) {
    if (event.eventId.empty() || event.eventId.size() > 128 || !event.company.hasValue() || !event.site.hasValue() ||
        !event.client.hasValue() || !event.sample.hasValue() || !event.result.hasValue() ||
        !event.analysis.hasValue() || !event.publishedAt.hasValue()) {
        throw ValidationError{"consume: event '" + event.eventId + "' does not name everything a line needs"};
    }
    auto const price = PriceListModel::priceOn(event.company, event.client, event.analysis, event.publishedAt);
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    ConsumedEvents consumed{mapper};
    if (auto const line = consumed.lookup(kLineEvents, event.eventId)) {
        return lineView(mapper, std::stoll(*line));
    }
    db::BillableLineRecord row;
    row.companyId = *event.company;
    row.siteId = *event.site;
    row.clientId = *event.client;
    row.sampleId = *event.sample;
    row.resultId = *event.result;
    row.analysisId = *event.analysis;
    row.publishedAt = millisOf(event.publishedAt);
    row.eventId = Lightweight::SqlAnsiString<128>{event.eventId};
    if (price.has_value()) {
        row.state = static_cast<int>(BillableLineState::Unbilled);
        row.amountNum = (*price->amount).numerator;
        row.amountDen = (*price->amount).denominator;
        row.currency = static_cast<int>(price->currency);
    } else {
        row.state = static_cast<int>(BillableLineState::Unpriced);
    }
    mapper.Create(row);
    consumed.record(kLineEvents, event.eventId, std::to_string(row.id.Value()));
    sqlTxn.Commit();
    return lineView(mapper, static_cast<std::int64_t>(row.id.Value()));
}
```

`lineView(mapper, id)` (anonymous namespace) loads the row and maps it, range-checking `state` and `currency` as Task 4's `toView` does and building `amount` only when both optionals are engaged. `ListBillableLines` answers the lines of the companies the caller can see, filtered by each engaged field of the query, sorted by id.

- [ ] **Step 6: The app's wiring**

Create `examples/lims/app/include/lims/app/event_wiring.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <morph/journal/action_log.hpp>
#include <mutex>
#include <string_view>
#include <vector>

/// @file
/// The one place lab's events meet billing: a journal sink that hands each `lab.AnalysisPublished` to billing's
/// consumer, and one relay pass over lab's outbox into it.

namespace lims::app {

/// @brief A `morph::journal::IActionLog` that delivers lab's events to billing instead of storing them.
///
/// `OutboxRelay` delivers to an action log; billing is a model. This adapter is what joins them: it decodes lab's
/// event, finds the company of its site, and calls `InvoiceModel::consume`. Delivery is at least once (the relay's
/// contract) and billing's consumer makes it once in effect.
class BillingEventSink final : public ::morph::journal::IActionLog {
public:
    /// @brief Delivers @p entry to billing.
    /// @param entry An entry `drainLabOutbox` produced.
    /// @throws ValidationError if the entry is not a `lab.AnalysisPublished` billing can bill: an unknown type, a
    ///         payload that does not decode, or a site that does not exist.
    void append(::morph::journal::LogEntry entry) override;

    /// @brief Nothing to flush: every `append` committed before it returned.
    void flush() override {}

    /// @brief The entries delivered so far through this sink, for @p entityKey or all.
    /// @param entityKey A sample id, or empty for every entry.
    /// @return The entries, in delivery order.
    [[nodiscard]] std::vector<::morph::journal::LogEntry> entries(std::string_view entityKey = {}) const override;

private:
    mutable std::mutex _mutex;
    std::vector<::morph::journal::LogEntry> _delivered;
};

/// @brief One relay pass: every event in lab's outbox to billing, then marked relayed.
/// @return How many events were relayed.
/// @throws std::exception from billing's consumer; the batch is then left unmarked and the next pass retries it.
std::size_t relayLabEvents();

}  // namespace lims::app
```

Create `examples/lims/app/src/event_wiring.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "lims/app/event_wiring.hpp"

#include <glaze/glaze.hpp>
#include <memory>
#include <morph/journal/outbox.hpp>
#include <string>

#include "lims/billing/models/invoice_model.hpp"
#include "lims/lab/events/lab_events.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/models/org_model.hpp"

namespace lims::app {

void BillingEventSink::append(::morph::journal::LogEntry entry) {
    if (entry.actionType != kAnalysisPublished) {
        throw ValidationError{"billing does not consume " + entry.actionType};
    }
    AnalysisPublished published;
    if (auto const error = glz::read<glz::opts{.error_on_unknown_keys = false}>(published, entry.payload)) {
        throw ValidationError{"event " + entry.idempotencyKey + " does not decode"};
    }
    auto const site = OrgModel::findSite(published.site);
    if (!site.has_value()) {
        throw ValidationError{"event " + entry.idempotencyKey + " names a site that does not exist"};
    }
    static_cast<void>(InvoiceModel::consume(BillableEvent{.eventId = entry.idempotencyKey,
                                                          .result = published.result,
                                                          .sample = published.sample,
                                                          .analysis = published.analysis,
                                                          .client = published.client,
                                                          .site = published.site,
                                                          .company = site->companyId,
                                                          .publishedAt = published.publishedAt}));
    std::scoped_lock const lock{_mutex};
    _delivered.push_back(std::move(entry));
}

std::vector<::morph::journal::LogEntry> BillingEventSink::entries(std::string_view entityKey) const {
    std::scoped_lock const lock{_mutex};
    std::vector<::morph::journal::LogEntry> matching;
    for (auto const& entry : _delivered) {
        if (entityKey.empty() || entry.entityKey == entityKey) {
            matching.push_back(entry);
        }
    }
    return matching;
}

std::size_t relayLabEvents() {
    ::morph::journal::OutboxRelay relay;
    relay.drainOutbox = [] { return drainLabOutbox(); };
    relay.markRelayed = [](std::span<::morph::journal::LogEntry const> rows) { markLabOutboxRelayed(rows); };
    relay.sink = std::make_shared<BillingEventSink>();
    return relay.relay().relayed;
}

}  // namespace lims::app
```

If `glz::read` with that option spelling does not compile against the pinned glaze, use `glz::read_json(published, entry.payload)` — lab writes the payload, so it has no unknown keys.

In `src/server/main.cpp`, after the server is listening, relay on a timer (include `<QTimer>` and `"lims/app/event_wiring.hpp"`):

```cpp
        // Lab's events reach billing through this relay; a pass that fails is logged and retried on the next tick,
        // since the batch stays unmarked.
        QTimer relayTimer;
        QObject::connect(&relayTimer, &QTimer::timeout, [] {
            try {
                static_cast<void>(lims::app::relayLabEvents());
            } catch (std::exception const& error) {
                std::cerr << "lims-server: event relay: " << error.what() << '\n';
            }
        });
        relayTimer.start(1000);
```

- [ ] **Step 7: Build, run, and measure what each consumer needed**

Run: Build lims, Model suite `"[events]"`, then Everything labelled for the rung. Expected: the five cases pass, existing suites unchanged. Run the h2 mutation and restore it. Then measure the code a consumer needed, and keep the output for decision 31:

```bash
cd /home/yaraslau/repo/morph
wc -l examples/lims/app/src/event_wiring.cpp examples/lims/app/include/lims/app/event_wiring.hpp
awk '/class ConsumedEvents/,/^};/' examples/lims/billing/src/models/invoice_model.cpp | wc -l
awk '/^std::vector<::morph::journal::LogEntry> drainLabOutbox/,/^}/' examples/lims/lab/src/events/lab_events.cpp | wc -l
```

- [ ] **Step 8: Record decision 31**

Append to the README, filling in the measured line counts:

```markdown
### 31. Lab's events reach billing through an outbox, morph's relay and an adapter per consumer (hypothesis 2, first instance)

`PublishSample` writes one `lab.AnalysisPublished` row per result into `lims_outbox`, in the transaction that
publishes, with the event id `lab.AnalysisPublished:<result>:<sample version>`. `lims::app::relayLabEvents` runs one
`morph::journal::OutboxRelay` pass: `drainOutbox` and `markRelayed` are lab's, and the sink is
`lims::app::BillingEventSink`, which decodes the event, finds its site's company and calls `InvoiceModel::consume`.
Billing records each event id in `billing_consumed_events` through an `IReplayLedger`, in the line's transaction, so
a redelivery answers the line it made. A result with no price in force is billed `Unpriced` rather than refused. The
server relays once a second on a `QTimer`.

**Hypothesis 2 is confirmed for this instance.** Measured in `tests/test_domain_events.cpp`: a relay that delivers and
dies before marking leaves one line and the event still in the outbox; a fresh relay redelivers it and billing answers
the same line, so the event bills once in effect across the crash and the restart. The delivery loop is morph's
(`OutboxRelay`), and the cursor is the outbox's own rows, but each consumer needed code of its own that is not wiring:
an `IActionLog` that is not a log, because the relay delivers only to an action log ([measured] lines in
`app/src/event_wiring.cpp` and its header), and an `IReplayLedger` over a table of its own ([measured] lines), because
the ledger's storage is per consumer by design. Lab's side needed a drain and a mark of its own ([measured] lines) as
bookmarks' outbox did. A consumer that refuses an event stops the relay at it: `OutboxRelay` delivers a batch in
order and marks it only whole, so every later event waits behind a poison one — measured in the same file. Inventory's
consumer of `lab.ResultCaptured` in Part C is the second instance; if it needs the same adapter and ledger, they are
the framework seam to design.
```

- [ ] **Step 9: Commit**

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/lab examples/lims/billing examples/lims/app examples/lims/src/server/main.cpp \
        examples/lims/tests examples/lims/README.md
git commit -m "Bill each published lims result once, through lab's outbox and morph's relay

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 9: Does billing's reaction to lab reach a screen? (the cross-module effects candidate)

**Files:**
- Create: `examples/lims/screens/tests/test_lims_billing_effects.cpp`
- Modify: `examples/lims/screens/lims_screens.cpp` (the lifecycle screen gains its sample's billable lines), `examples/lims/README.md` (decision 32)

**Interfaces:**
- Consumes: `InvoiceModel`, `ListBillableLines`, `lims::app::relayLabEvents`, `lims::test::readyToPublish`, `lims::test::billingFor`, `ScreenHarness`.
- Produces: on `lims.lifecycle`, the text `billableCount` (`"<n> billable lines"`) and the button `refreshBilling`; a measured verdict on README decision 26's finding.

README decision 26 inferred from reading `include/morph/core/effects.hpp` that a module's action cannot declare effects on another module's records, so a screen showing billing's lines does not refresh when a lab action makes one; it was never reproduced, because no Part A screen showed the other module's records. This task puts billing's lines on the sample's own screen — the lifecycle screen, where a published sample's charges belong — publishes from it, and measures. Rebase before editing `lims_screens.cpp`; another session may be changing `screens/`.

- [ ] **Step 1: Write the test**

Create `examples/lims/screens/tests/test_lims_billing_effects.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The cross-module effects question on a real screen: the lifecycle screen shows its sample's billable lines; a
// publish made on it creates a line in billing, through lab's event; does the screen show it?

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <string>
#include <testkit/db_fixture.hpp>
#include <testkit/screen_harness.hpp>

#include "billing_test_support.hpp"
#include "lims/app/event_wiring.hpp"
#include "lims/billing/models/invoice_model.hpp"
#include "lims_screens.hpp"

namespace {

using morph::client::LocalApp;
using morph::client::LocalSetup;
using morph::examples::testing::RigMode;
using morph::examples::testing::ScreenHarness;
using morph::ladder::testkit::DbFixture;
using morph::ui::ParamMap;
using morph::ui::Value;

LocalApp limsApp() { return LocalApp{.name = "lims", .bundle = &lims::screens::bundle, .setUp = {}}; }

}  // namespace

// The measured outcome: the line exists in billing, and the screen does not show it until it is refetched.
// If the CHECK_FALSE below fails, the candidate is refuted: the screen refreshed by itself.
TEST_CASE("lims billing effects: a line billing makes from a publish does not reach the screen that published",
          "[lims-screens][billing]") {
    DbFixture fixture;
    auto const lab = lims::test::readyToPublish();
    lims::test::billingFor(lab.client);
    // alice publishes from the screen; a billing role at the seeded company lets her list its lines (decision 26).
    lims::test::grant("alice", lims::LimsRole::BillingClerk);
    ScreenHarness harness{limsApp(), std::string{lims::screens::kLifecycle}, RigMode::Local,
                          ParamMap{{"sampleId", Value{*lab.sample}}}, LocalSetup{.db = {}, .user = "alice"}};
    REQUIRE(harness.waitForText("billableCount", "0 billable lines"));

    harness.click("publish");
    REQUIRE(harness.waitFor([&harness] {
        auto const header = harness.text("header");
        return header && header->find("Published") != std::string::npos;
    }));
    // What the server's timer does once a second.
    REQUIRE(lims::app::relayLabEvents() == 1U);
    {
        lims::test::ScopedPrincipal const alice{"alice"};
        REQUIRE(lims::InvoiceModel{}.execute(lims::ListBillableLines{.company = {}, .client = {}, .sample = lab.sample})
                    .lines.size() == 1U);
    }

    bool const refreshedByItself = harness.waitFor(
        [&harness] { return harness.text("billableCount") == std::optional<std::string>{"1 billable lines"}; },
        std::chrono::milliseconds{1500});
    CHECK_FALSE(refreshedByItself);

    // The control: the same panel shows the line once it is asked again, so the panel can show it.
    harness.click("refreshBilling");
    CHECK(harness.waitForText("billableCount", "1 billable lines"));
    for (auto const& problem : harness.problems()) {
        UNSCOPED_INFO(problem);
    }
    CHECK(harness.problems().empty());
}
```

- [ ] **Step 2: Give the lifecycle screen its sample's billable lines**

In `examples/lims/screens/lims_screens.cpp` add `#include "lims/billing/dto/line_dto.hpp"` and `#include "lims/billing/models/invoice_model.hpp"`. In `lifecycleScreen()`, after `auto const view = current.value();`:

```cpp
        // What the sample has been billed. Billing makes these lines when the sample is published, through lab's
        // event; this screen refreshes them from declared effects only.
        auto const billing = screen.model<InvoiceModel>("billing");
        auto const lines = screen.query<ListBillableLines>("billableLines", billing,
                                                           {{&ListBillableLines::sample, sampleId}});
        auto const lineRows = lines.value().at(&ListBillableLinesResult::lines);
```

and add a last child to the returned `ui::column`, after the `error` text:

```cpp
             ui::panel({ui::text({.text = ui::format("{0} billable lines", {ui::count(lineRows)})})
                            .testId("billableCount"),
                        ui::button({.label = "Refresh billing", .onClick = ui::refetch(lines)})
                            .testId("refreshBilling")},
                       {.title = ui::t("lims.lifecycle.billing", "Billing"), .padding = 2})
```

Add `lines.error()` to the `failure` coalesce. The `Body` initializer follows the mutation `open`'s `.body = {{&OpenSample::sampleId, sampleId}}`; if `query`'s third parameter does not accept that braced form, write `ui::Body<ListBillableLines>{{&ListBillableLines::sample, sampleId}}`.

- [ ] **Step 3: Build and run**

Run: Build lims, then Screen suite `"[billing]"`, then the whole Screen suite (the bundle must still load without warnings: `lims_bundle_loads_without_warnings`). Expected (inferred from decision 26's reading): the case passes — the line exists, the panel stays at 0 until refetched. If `CHECK_FALSE(refreshedByItself)` fails, the candidate is refuted: change it to `CHECK(refreshedByItself)` and use the refuted text in Step 4.

- [ ] **Step 4: Record decision 32, and file the framework gap if confirmed**

Append to the README (the confirmed text; if refuted, replace the bold paragraph with "**Refuted.** The screen showed the line without a refetch: [what carried it, from reading the harness's event path]" and file nothing):

```markdown
### 32. A module's reaction to another module's action does not reach the screen

The sample lifecycle screen shows what billing has made of its sample (`ListBillableLines` through `InvoiceModel`).
Publishing from that screen makes a billable line, through lab's event and billing's consumer (decision 31).

**Confirmed: the line does not reach the screen until it is refetched.** Measured in
`screens/tests/test_lims_billing_effects.cpp`: after the publish and a relay pass, billing holds the line and the
panel still shows none; its refresh button shows it. `PublishSample` declares lab's effects only, and cannot declare
billing's `Billing_Line` (lab does not include billing, and a second declaration from `app` is refused as
"declares different effects in two registrations", `include/morph/core/effects.hpp`); and billing's consumer is not an
action, so nothing derives its changes. This is the second place decision 26's finding appears — org's role changes
not reaching lab's sample list was the first — so it is filed against `include/morph` as a missing framework seam: a
way for the code that composes modules to state that one module's action, or a consumer it runs, changes another
module's records.
```

If confirmed, file the issue. Collect the real output first:

```bash
cd /home/yaraslau/repo/morph
flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock \
  env QT_QPA_PLATFORM=offscreen timeout 300 build/linux-everything/ladder_lims_screen_tests "[billing]" -s 2>&1 \
  | tail -40 > /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/plan_b_effects_output.txt
gh label list --limit 100 | grep -i 'area'
```

Then `gh issue create` with title "Effects: no way to declare that one module's action changes another module's records", labels `enhancement` and the `area:` label `gh label list` shows for core/effects (no `triage:` label), and a body containing: what was measured and on which commit (`git rev-parse HEAD`), the test's name and the output file's relevant lines, the two places (org→lab inferred in decision 26, billing←lab measured here), what was not verified (the remote path: a second connection watching `Billing_Line` while a third publishes — the server derives changes from the executed action's declared effects, so it is inferred to behave the same), and what would close it (a declared cross-module effect, or a consumer's changes fanned out, reaching this screen without a refetch).

- [ ] **Step 5: Commit**

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/screens/lims_screens.cpp examples/lims/screens/tests/test_lims_billing_effects.cpp \
        examples/lims/README.md
git commit -m "Measure whether billing's reaction to a publish reaches the lims lifecycle screen

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---
### Task 10: Invoices — drafts, totals, tax, and the invoice's runtime currency (hypothesis 8, the invoice half of instance 2)

**Files:**
- Create: `examples/lims/billing/include/lims/billing/dto/invoice_dto.hpp`, `examples/lims/billing/include/lims/billing/core/invoice_math.hpp`, `examples/lims/billing/src/core/invoice_math.cpp`, `examples/lims/billing/include/lims/billing/db/billing_store.hpp`, `examples/lims/billing/src/models/invoice_drafts.cpp`, `examples/lims/tests/test_invoices.cpp`
- Modify: `billing_entity.hpp` (`InvoiceRecord`), `billing_schema.cpp`, `invoice_model.hpp`, `billing_authorization.hpp`, `declared_requirements.hpp`, `test_declared_authorization.cpp`, `examples/lims/README.md` (decision 33)

**Interfaces:**
- Consumes: `BillableLineRecord`, `PriceListModel::billingClient`, `PriceListModel::settingsOf`, `roundToCurrency`, `ExactSum`.
- Produces: `struct lims::InvoiceTotals { Rational net; Rational tax; Rational gross; }`; `InvoiceTotals lims::invoiceTotals(std::span<Rational const> lineAmounts, Rational const& taxRate, CurrencyCode currency)` (throws `OutOfRange`); `InvoiceView { InvoiceId id; CompanyId company; SiteId site; ClientId client; CurrencyCode currency; InvoiceState state; std::string number; std::vector<BillableLineView> lines; Amount net; Amount tax; Amount gross; std::string reference; std::string createdBy; std::string submittedBy; std::string approvedBy; std::string postedBy; Timestamp submittedAt; Timestamp approvalDeadline; ApprovalLevel level; JournalEntryId entry; }`; `CreateInvoice { CompanyId company; SiteId site; ClientId client; }` → `InvoiceView`; `GetInvoice { InvoiceId invoiceId; }`; `ListInvoices { CompanyId company; }` → `ListInvoicesResult { std::vector<InvoiceView> invoices; }`; in `billing/db/billing_store.hpp` (ORM-level, billing-internal): `std::optional<std::int64_t> lims::draftInvoice(Lightweight::DataMapper&, CompanyId, SiteId, ClientId, std::string const& author)` and `InvoiceView lims::invoiceView(Lightweight::DataMapper&, std::int64_t invoiceId)`; record `Billing_Invoice`.

An invoice collects one client's unbilled lines at one site of one company, in the currency the client is billed in there. The site is the invoice's because approval is judged at it (Task 12). Tax is the company's flat rate on the net total, rounded half-up once; the gross is net plus tax.

- [ ] **Step 1: Write the failing test**

Create `examples/lims/tests/test_invoices.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Draft invoices and their totals, and the invoice half of hypothesis 8's second instance: the currency is the
// record's, not the type's.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <morph/forms/forms.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <vector>

#include "billing_test_support.hpp"
#include "lims/app/event_wiring.hpp"
#include "lims/billing/core/invoice_math.hpp"
#include "lims/billing/models/invoice_model.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;
using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

namespace {

[[nodiscard]] Rational exact(std::int64_t num, std::int64_t den, std::uint32_t places) {
    return Rational{Numerator{num}, Denominator{den}, DecimalPlaces{places}};
}

/// @brief Settings for the seeded company with @p taxRate, set by sam.
void settings(Rational taxRate) {
    lims::test::grant("sam", lims::LimsRole::Supervisor);
    ScopedPrincipal const sam{"sam"};
    lims::PriceListModel{}.execute(lims::SetBillingSettings{.company = lims::test::seededCompany(),
                                                            .receivableAccount = "1200",
                                                            .revenueAccount = "4000",
                                                            .taxAccount = "2200",
                                                            .roundingAccount = "6900",
                                                            .taxRate = lims::Rate{taxRate},
                                                            .approvalHours = lims::Count{Rational{48, DecimalPlaces{0}}},
                                                            .billingDay = lims::Count{Rational{25, DecimalPlaces{0}}}});
}

}  // namespace

// Mutation: compute tax per line and sum it (three 0.05 lines at 50 % tax 0.09, not 0.08).
TEST_CASE("invoices: tax is the flat rate on the net total, rounded half-up once", "[lims][billing][invoices]") {
    auto const eur = lims::invoiceTotals(std::vector<Rational>{exact(1005, 100, 2)}, exact(19, 100, 6),
                                         lims::CurrencyCode::EUR);
    CHECK(eur.net == exact(1005, 100, 2));
    CHECK(eur.tax == exact(191, 100, 2));
    CHECK(eur.gross == exact(1196, 100, 2));

    auto const yen = lims::invoiceTotals(std::vector<Rational>{exact(1005, 1, 0)}, exact(10, 100, 6),
                                         lims::CurrencyCode::JPY);
    CHECK(yen.tax == exact(101, 1, 0));
    CHECK(yen.gross == exact(1106, 1, 0));

    auto const once = lims::invoiceTotals(std::vector<Rational>{exact(5, 100, 2), exact(5, 100, 2), exact(5, 100, 2)},
                                          exact(1, 2, 6), lims::CurrencyCode::EUR);
    CHECK(once.tax == exact(8, 100, 2));
}

// Mutation: let CreateInvoice take lines of every site (the field-lab line joins the seeded site's invoice).
TEST_CASE("invoices: a draft collects one client's unbilled lines at one site, in the client's currency",
          "[lims][billing][invoices]") {
    DbFixture fixture;
    settings(exact(19, 100, 6));
    auto const lab = lims::test::publishedSample();
    lims::test::billingFor(lab.client);
    lims::app::relayLabEvents();
    ScopedPrincipal const bea{"bea"};
    lims::InvoiceModel invoices;
    auto const draft = invoices.execute(
        lims::CreateInvoice{.company = lims::test::seededCompany(), .site = lims::test::seededSite(), .client = lab.client});
    CHECK(draft.state == lims::InvoiceState::Draft);
    CHECK(draft.currency == lims::CurrencyCode::EUR);
    REQUIRE(draft.lines.size() == 1U);
    CHECK(draft.lines[0].state == lims::BillableLineState::Invoiced);
    CHECK(*draft.net == exact(40, 1, 2));
    CHECK(*draft.tax == exact(76, 10, 2));
    CHECK(*draft.gross == exact(476, 10, 2));
    CHECK(draft.createdBy == "bea");
    CHECK(draft.number.empty());
    // The lines are spent: a second draft finds none.
    CHECK_THROWS_AS(invoices.execute(lims::CreateInvoice{
                        .company = lims::test::seededCompany(), .site = lims::test::seededSite(), .client = lab.client}),
                    lims::Conflict);
}

TEST_CASE("invoices: a company without billing settings cannot invoice", "[lims][billing][invoices]") {
    DbFixture fixture;
    auto const lab = lims::test::publishedSample();
    lims::test::billingFor(lab.client);
    lims::app::relayLabEvents();
    ScopedPrincipal const bea{"bea"};
    CHECK_THROWS_WITH(lims::InvoiceModel{}.execute(lims::CreateInvoice{
                          .company = lims::test::seededCompany(), .site = lims::test::seededSite(), .client = lab.client}),
                      Catch::Matchers::ContainsSubstring("billing settings"));
}

// Hypothesis 8, the invoice half of instance 2: the currency is known only from the record.
TEST_CASE("Hypothesis 8: an amount's currency is a field the type does not see", "[lims][billing][h8]") {
    // The type adds a yen amount to a euro amount without complaint: both are the one money unit.
    STATIC_REQUIRE(requires(lims::Amount yen, lims::Amount euro) { yen + euro; });
    // The schema a JPY price list's price form is served with advertises two decimals, because the declared
    // precision is the type's; the model refuses fractional yen (test_price_lists.cpp) where the form allows it.
    std::string const schema = morph::forms::schemaJson<lims::SetPrice>();
    CHECK_THAT(schema, Catch::Matchers::ContainsSubstring(R"("x-decimalPlaces":2)"));
}
```

If `schemaJson` is spelled differently in `include/morph/forms/forms.hpp` (its file comment names `schemaJson<A>()`), use that file's spelling; if the key is written with spaces, match `"x-decimalPlaces"` and `2` with two `ContainsSubstring` checks.

- [ ] **Step 2: Run it and see it fail**

Run: Build lims. Expected: compile error, `lims/billing/core/invoice_math.hpp` not found.

- [ ] **Step 3: The totals**

Create `invoice_math.hpp` (declares `InvoiceTotals` and `invoiceTotals` with full Doxygen: `@param lineAmounts` the lines' amounts, `@param taxRate` the company's rate, `@param currency` the invoice's currency, `@return` net, tax and gross, `@throws OutOfRange` when a total does not fit) and `invoice_math.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "lims/billing/core/invoice_math.hpp"

#include "lims/org/core/errors.hpp"

namespace lims {

InvoiceTotals invoiceTotals(std::span<::morph::math::Rational const> lineAmounts,
                            ::morph::math::Rational const& taxRate, CurrencyCode currency) {
    ExactSum net;
    for (auto const& amount : lineAmounts) {
        if (!net.add(amount)) {
            throw OutOfRange{"an invoice's net total is too large to total exactly"};
        }
    }
    auto const product = ::morph::math::checkedMul(net.total(), taxRate);
    if (!product.has_value()) {
        throw OutOfRange{"an invoice's tax is too large to compute exactly"};
    }
    auto const tax = roundToCurrency(*product, currency);
    auto const gross = ::morph::math::checkedAdd(net.total(), tax);
    if (!gross.has_value()) {
        throw OutOfRange{"an invoice's gross total is too large to total exactly"};
    }
    auto netValue = net.total();
    netValue.decimalPlaces = ::morph::math::DecimalPlaces{decimalsOf(currency)};
    auto grossValue = *gross;
    grossValue.decimalPlaces = netValue.decimalPlaces;
    return InvoiceTotals{.net = netValue, .tax = tax, .gross = grossValue};
}

}  // namespace lims
```

- [ ] **Step 4: DTOs, record, migration, drafting**

Create `invoice_dto.hpp` with `InvoiceView` and the three actions from Interfaces; `CreateInvoice::validate()` requires the three ids and `explicitSubmit = true`; `GetInvoice` requires its id; `ListInvoices` its company. `BRIDGE_REGISTER_RECORD(lims::InvoiceView, "Billing_Invoice")`.

`InvoiceRecord` (`billing_invoices`): `id`; `company_id`, `site_id`, `client_id` (`std::int64_t`); `currency`, `state` (`int`); `number` `SqlAnsiString<32>`; `net_num`/`_den`, `tax_num`/`_den`, `gross_num`/`_den`; `reference` `SqlAnsiString<128>`; `created_by`, `submitted_by`, `approved_by`, `posted_by` `SqlAnsiString<64>`; `created_at`, `submitted_at`, `deadline_at` `std::int64_t`; `entry_id` `std::optional<std::int64_t>`. Migration `20261110000008 "Create billing_invoices"`.

Create `billing_store.hpp` (`#include <Lightweight/DataMapper/DataMapper.hpp>`; billing-internal, never included by a model header) declaring `draftInvoice` and `invoiceView` with Doxygen:

```cpp
/// @brief Drafts an invoice over every unbilled line of @p client at @p site, in the caller's transaction, and marks
///        the lines invoiced.
/// @param mapper The caller's mapper, inside its transaction.
/// @param company The company.
/// @param site The site.
/// @param client The client.
/// @param author Who drafts it.
/// @return The invoice's id; nothing when the client has no unbilled line there.
/// @throws NotFound if the company has no billing settings or the client is not billed there.
/// @throws OutOfRange if a total does not fit.
[[nodiscard]] std::optional<std::int64_t> draftInvoice(Lightweight::DataMapper& mapper, CompanyId company, SiteId site,
                                                       ClientId client, std::string const& author);

/// @brief The invoice @p invoiceId with its lines, through @p mapper.
/// @param mapper An open mapper.
/// @param invoiceId The invoice.
/// @return Its view; `level` is `Company` once its approval deadline has passed.
/// @throws NotFound if it does not exist.
/// @throws ValidationError if a row holds a state or currency outside its enumeration.
[[nodiscard]] InvoiceView invoiceView(Lightweight::DataMapper& mapper, std::int64_t invoiceId);
```

Implement both in `invoice_drafts.cpp`. `draftInvoice`: read `PriceListModel::settingsOf(company)` (`NotFound` "company N has no billing settings") and `PriceListModel::billingClient(client, company)` (`NotFound`); select the `Unbilled` lines with that company, site and client; none → `std::nullopt`; every line's currency must equal the client's, else `ValidationError` naming the line (the type cannot keep currencies apart, so the model does); `invoiceTotals` over their amounts at the settings' tax rate; create the `InvoiceRecord` (state `Draft`, `created_by`, `created_at = nowMillis()`), then set each line `Invoiced` with its `invoice_id`. `invoiceView` maps the row (range-checking `state` and `currency`), loads its lines by `invoice_id`, and sets `level = deadline_at > 0 && nowMillis() >= deadline_at ? ApprovalLevel::Company : ApprovalLevel::Site`. `InvoiceModel::execute(CreateInvoice)`: the conventions' action shape, then `draftInvoice` inside one transaction; `std::nullopt` → `Conflict` "client N has no unbilled lines at site M". `GetInvoice` answers `NotFound` for an invoice of a company the caller cannot see; `ListInvoices` answers the visible company's invoices by id.

Declare `template <> struct Authorization<CreateInvoice> : CompanyRule<CreateInvoice, LimsRole::BillingClerk> {};`, register `"CreateInvoice"` with `BRIDGE_REGISTER_EFFECTS(lims::CreateInvoice, ::morph::effects::changes<lims::InvoiceView>, ::morph::effects::changes<lims::BillableLineView>)`, `"GetInvoice"` and `"ListInvoices"` as reads, add `CreateInvoice` to the edge table and the coverage tuple, `"InvoiceModel"` to its model list.

- [ ] **Step 5: Build and run**

Run: Build lims, Model suite `"[invoices]"`, Model suite `"[h8]"`, Model suite `"[authorization]"`. Expected: all pass. Run the two mutations named in the file and restore them.

- [ ] **Step 6: Record decision 33**

```markdown
### 33. An invoice is one client's lines at one site, in the client's currency; the type does not know which (hypothesis 8, invoice half of instance 2)

`CreateInvoice` collects every unbilled line of a client at a site and marks them invoiced; tax is the company's flat
rate on the net total, rounded half-up once, and the gross is net plus tax. The site is the invoice's because its
approval is judged there (decision 35).

**Hypothesis 8's runtime-currency half is confirmed for invoices.** Measured in `tests/test_invoices.cpp`: with the
currency known only from the record, the amount is a `Quantity` over the one money unit, so the type adds yen to
euros (`STATIC_REQUIRE`), and the served schema of a JPY price's form still advertises `x-decimalPlaces: 2` — the
declared precision is the type's. What keeps currencies apart is model code: `draftInvoice` refuses a line in another
currency and `fitsCurrency` refuses fractional yen. `Quantity` holds the amount exactly; it cannot hold its currency.
Purchase orders in a supplier's currency (Part C) are the second instance; a form that follows a record's currency is
Part D's (`morph::forms::InstanceConstraints` serves per-instance decimals and is the existing seam to try there).
```

- [ ] **Step 7: Commit**

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/billing examples/lims/tests examples/lims/app/include/lims/app/declared_requirements.hpp \
        examples/lims/README.md
git commit -m "Draft lims invoices with exact totals and a flat tax in the client's currency

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 11: The scheduled billing run, its progress, and resuming it after a restart (hypothesis 4, first instance)

**Files:**
- Create: `examples/lims/billing/include/lims/billing/core/fault_hook.hpp`, `examples/lims/billing/src/core/fault_hook.cpp`, `examples/lims/billing/include/lims/billing/dto/run_dto.hpp`, `examples/lims/billing/include/lims/billing/models/billing_run_model.hpp`, `examples/lims/billing/src/models/billing_run_model.cpp`, `examples/lims/app/include/lims/app/billing_scheduler.hpp`, `examples/lims/app/src/billing_scheduler.cpp`, `examples/lims/tests/test_billing_run.cpp`
- Modify: `billing_entity.hpp` (`BillingRunRecord`, `BillingRunItemRecord`), `billing_schema.cpp`, `invoice_drafts.cpp` (a fault point), `billing_authorization.hpp`/`.cpp`, `declared_requirements.hpp`, `test_declared_authorization.cpp`, `src/server/main.cpp`, `examples/lims/README.md` (decision 34)

**Interfaces:**
- Consumes: `draftInvoice`, `PriceListModel::settingsOf`, `dayOfMonth`, `monthLabel`, `morph::session::detail::ScopedContext`.
- Produces: `void lims::setFaultHook(std::function<void(std::string_view)>)`, `void lims::faultPoint(std::string_view)`; `StartBillingRun { CompanyId company; }` → `BillingRunView { BillingRunId id; CompanyId company; std::string label; BillingRunState state; Count total; Count done; Timestamp startedAt; std::string startedBy; }`; `AdvanceBillingRun { BillingRunId runId; Count batch; }` → `BillingRunView`; `ListBillingRuns { CompanyId company; }` → `ListBillingRunsResult`; statics `BillingRunModel::dueCompanies(Timestamp const& now) -> std::vector<CompanyId>` and `BillingRunModel::runningRuns() -> std::vector<BillingRunId>`; model `lims::BillingRunModel` (`"BillingRunModel"`); record `Billing_Run`; `class lims::app::BillingScheduler { explicit BillingScheduler(std::string principal); void tick(Timestamp const& now); }`.

A run belongs to a company and a calendar month (`label`, `YYYY-MM`): at most one per pair. Starting it writes one item per (client, site) with unbilled lines — the run's cursor. Advancing drafts one item's invoice and marks the item done in one transaction, so an interruption anywhere leaves each item either wholly done or wholly pending, and a resumed run drafts only what is pending. Progress is `done` of `total`.

- [ ] **Step 1: Write the failing test**

Create `examples/lims/tests/test_billing_run.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Hypothesis 4, first instance: the billing run starts on its day, shows its progress, and a run interrupted
// mid-way resumes on the next tick without drafting any invoice twice.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <map>
#include <morph/util/rational.hpp>
#include <stdexcept>
#include <string>

#include "billing_test_support.hpp"
#include "clock.hpp"
#include "lims/app/billing_scheduler.hpp"
#include "lims/app/event_wiring.hpp"
#include "lims/billing/core/fault_hook.hpp"
#include "lims/billing/models/billing_run_model.hpp"
#include "lims/billing/models/invoice_model.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::ScopedClockOverride;
using morph::ladder::testkit::DbFixture;
using morph::math::DecimalPlaces;
using morph::math::Rational;

namespace {

/// @brief Clears the fault hook when the case ends, however it ends.
struct FaultHookGuard {
    FaultHookGuard() = default;
    FaultHookGuard(FaultHookGuard const&) = delete;
    FaultHookGuard& operator=(FaultHookGuard const&) = delete;
    FaultHookGuard(FaultHookGuard&&) = delete;
    FaultHookGuard& operator=(FaultHookGuard&&) = delete;
    ~FaultHookGuard() { lims::setFaultHook({}); }
};

/// @brief Three clients, each with one published, billable result at the seeded site; billing settings with billing
///        day 25; the scheduler principal a billing clerk. The lines are relayed.
void threeClientsToBill() {
    lims::test::grant("sam", lims::LimsRole::Supervisor);
    {
        ScopedPrincipal const sam{"sam"};
        lims::PriceListModel{}.execute(lims::SetBillingSettings{
            .company = lims::test::seededCompany(),
            .receivableAccount = "1200",
            .revenueAccount = "4000",
            .taxAccount = "2200",
            .roundingAccount = "6900",
            .taxRate = lims::Rate{Rational{morph::math::Numerator{19}, morph::math::Denominator{100}, DecimalPlaces{6}}},
            .approvalHours = lims::Count{Rational{48, DecimalPlaces{0}}},
            .billingDay = lims::Count{Rational{25, DecimalPlaces{0}}}});
    }
    for (std::string const reference : {"A-1", "B-1", "C-1"}) {
        auto const lab = lims::test::publishedSample(reference);
        lims::test::billingFor(lab.client);
    }
    lims::app::relayLabEvents();
    lims::test::grant("lims-scheduler", lims::LimsRole::BillingClerk);
}

/// @brief The company's runs, as bea sees them.
[[nodiscard]] std::vector<lims::BillingRunView> runs() {
    ScopedPrincipal const bea{"bea"};
    return lims::BillingRunModel{}.execute(lims::ListBillingRuns{.company = lims::test::seededCompany()}).runs;
}

/// @brief How many invoices each client has.
[[nodiscard]] std::map<std::int64_t, int> invoicesPerClient() {
    ScopedPrincipal const bea{"bea"};
    std::map<std::int64_t, int> counts;
    for (auto const& invoice :
         lims::InvoiceModel{}.execute(lims::ListInvoices{.company = lims::test::seededCompany()}).invoices) {
        ++counts[*invoice.client];
    }
    return counts;
}

}  // namespace

// Mutation: let dueCompanies ignore an existing run for the month (the second tick starts a second run, and
// StartBillingRun refuses it with Conflict, which tick() propagates).
TEST_CASE("Hypothesis 4: the billing run starts on its day of the month, once", "[lims][billing][run][h4]") {
    DbFixture fixture;
    threeClientsToBill();
    lims::app::BillingScheduler scheduler{"lims-scheduler"};
    {
        ScopedClockOverride const clock{lims::test::at(2026, 11, 24)};
        scheduler.tick(morph::time::Timestamp{lims::test::at(2026, 11, 24)});
        CHECK(runs().empty());
    }
    ScopedClockOverride const clock{lims::test::at(2026, 11, 25)};
    scheduler.tick(morph::time::Timestamp{lims::test::at(2026, 11, 25)});
    REQUIRE(runs().size() == 1U);
    CHECK(runs()[0].label == "2026-11");
    CHECK(runs()[0].state == lims::BillingRunState::Finished);
    scheduler.tick(morph::time::Timestamp{lims::test::at(2026, 11, 25, 15)});
    CHECK(runs().size() == 1U);
    CHECK(invoicesPerClient().size() == 3U);
}

TEST_CASE("billing run: progress is visible item by item", "[lims][billing][run]") {
    DbFixture fixture;
    threeClientsToBill();
    ScopedClockOverride const clock{lims::test::at(2026, 11, 25)};
    ScopedPrincipal const scheduler{"lims-scheduler"};
    lims::BillingRunModel model;
    auto run = model.execute(lims::StartBillingRun{.company = lims::test::seededCompany()});
    CHECK(*run.total == Rational{3, DecimalPlaces{0}});
    CHECK(*run.done == Rational{0, DecimalPlaces{0}});
    run = model.execute(lims::AdvanceBillingRun{.runId = run.id, .batch = lims::Count{Rational{1, DecimalPlaces{0}}}});
    CHECK(*run.done == Rational{1, DecimalPlaces{0}});
    CHECK(run.state == lims::BillingRunState::Running);
    run = model.execute(lims::AdvanceBillingRun{.runId = run.id, .batch = lims::Count{Rational{5, DecimalPlaces{0}}}});
    CHECK(run.state == lims::BillingRunState::Finished);
    CHECK_THROWS_AS(model.execute(lims::StartBillingRun{.company = lims::test::seededCompany()}), lims::Conflict);
}

// Hypothesis 4. Mutation: draft the item's invoice and mark the item done in two transactions (the interrupted
// item's invoice commits while the item stays pending; the resume drafts it again or finds the lines spent, so the
// per-client counts or the done count go wrong).
TEST_CASE("Hypothesis 4: a run interrupted mid-way resumes and drafts each client's invoice once",
          "[lims][billing][run][h4]") {
    DbFixture fixture;
    threeClientsToBill();
    ScopedClockOverride const clock{lims::test::at(2026, 11, 25)};
    FaultHookGuard const guard;
    int drafted = 0;
    lims::setFaultHook([&drafted](std::string_view point) {
        if (point == "run.afterInvoice" && ++drafted == 2) {
            throw std::runtime_error{"server stopped during the billing run"};
        }
    });
    {
        lims::app::BillingScheduler scheduler{"lims-scheduler"};
        CHECK_THROWS_WITH(scheduler.tick(morph::time::Timestamp{lims::test::at(2026, 11, 25)}),
                          Catch::Matchers::ContainsSubstring("server stopped"));
    }
    REQUIRE(runs().size() == 1U);
    CHECK(runs()[0].state == lims::BillingRunState::Running);
    CHECK(*runs()[0].done == Rational{1, DecimalPlaces{0}});
    CHECK(invoicesPerClient().size() == 1U);

    // The restart: the hook is gone, a new scheduler ticks, the run resumes where it stood.
    lims::setFaultHook({});
    lims::app::BillingScheduler restarted{"lims-scheduler"};
    restarted.tick(morph::time::Timestamp{lims::test::at(2026, 11, 25, 10)});
    REQUIRE(runs().size() == 1U);
    CHECK(runs()[0].state == lims::BillingRunState::Finished);
    CHECK(*runs()[0].done == Rational{3, DecimalPlaces{0}});
    auto const perClient = invoicesPerClient();
    CHECK(perClient.size() == 3U);
    for (auto const& [client, count] : perClient) {
        CAPTURE(client);
        CHECK(count == 1);
    }
}
```

- [ ] **Step 2: Run it and see it fail**

Run: Build lims. Expected: compile error, `lims/app/billing_scheduler.hpp` not found.

- [ ] **Step 3: The fault hook**

Create `fault_hook.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>
#include <string_view>

/// @file
/// Named points inside billing's transactions where a test injects a failure. Empty in production, where
/// `faultPoint` does nothing.

namespace lims {

/// @brief Installs @p hook, called with each point's name; an empty hook removes it. Set by a test before it acts
///        and cleared after, never while another thread is in billing.
/// @param hook The hook; it fails the point by throwing.
void setFaultHook(std::function<void(std::string_view)> hook);

/// @brief Runs the installed hook for @p point, if any.
/// @param point The point's name, such as `run.afterInvoice`.
void faultPoint(std::string_view point);

}  // namespace lims
```

and `fault_hook.cpp` holding the hook in a function-local `static std::function<void(std::string_view)>`.

In `draftInvoice`, after the invoice row is created and its lines are marked, call `faultPoint("run.afterInvoice");` — inside the caller's transaction, so a failure there rolls back the invoice with the item.

- [ ] **Step 4: Runs — DTOs, records, model**

`run_dto.hpp`: the structs from Interfaces; `StartBillingRun::validate()` requires the company; `AdvanceBillingRun::validate()` the run and a batch; `BRIDGE_REGISTER_RECORD(lims::BillingRunView, "Billing_Run")`.

Records: `BillingRunRecord` (`billing_runs`: `id`, `company_id`, `label` `SqlAnsiString<7>`, `state` int, `started_at`, `started_by` `SqlAnsiString<64>`) and `BillingRunItemRecord` (`billing_run_items`: `id`, `BelongsTo<&BillingRunRecord::id> run_id`, `client_id`, `site_id`, `state` int — 0 pending, 1 done — and `invoice_id` `std::optional<std::int64_t>`). Migration `20261110000009 "Create billing_runs and billing_run_items"`.

`billing_run_model.cpp`:

```cpp
BillingRunView BillingRunModel::execute(const StartBillingRun& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"StartBillingRun: a company is required"};
    }
    requireRoleSomewhere<StartBillingRun>();
    if (!PriceListModel::settingsOf(action.company).has_value()) {
        throw NotFound{"StartBillingRun: company " + std::to_string(*action.company) + " has no billing settings"};
    }
    requireAuthorized(action);
    std::string const label = monthLabel(calendarMonthOf(::morph::ladder::now()));
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    if (!mapper.Query<db::BillingRunRecord>()
             .Where(::Lightweight::FieldNameOf<&db::BillingRunRecord::companyId>, "=", *action.company)
             .Where(::Lightweight::FieldNameOf<&db::BillingRunRecord::label>, "=", label)
             .All()
             .empty()) {
        throw Conflict{"StartBillingRun: company " + std::to_string(*action.company) + " already ran " + label};
    }
    db::BillingRunRecord run;
    run.companyId = *action.company;
    run.label = Lightweight::SqlAnsiString<7>{label};
    run.state = static_cast<int>(BillingRunState::Running);
    run.startedAt = nowMillis();
    run.startedBy = Lightweight::SqlAnsiString<64>{::morph::session::current()->principal};
    mapper.Create(run);
    std::set<std::pair<std::int64_t, std::int64_t>> pairs;
    for (auto const& line : mapper.Query<db::BillableLineRecord>()
                                .Where(::Lightweight::FieldNameOf<&db::BillableLineRecord::companyId>, "=",
                                       *action.company)
                                .Where(::Lightweight::FieldNameOf<&db::BillableLineRecord::state>, "=",
                                       static_cast<int>(BillableLineState::Unbilled))
                                .All()) {
        pairs.emplace(line.clientId.Value(), line.siteId.Value());
    }
    for (auto const& [client, site] : pairs) {
        db::BillingRunItemRecord item;
        item.run = run.id.Value();
        item.clientId = client;
        item.siteId = site;
        mapper.Create(item);
    }
    sqlTxn.Commit();
    return runView(mapper, static_cast<std::int64_t>(run.id.Value()));
}

BillingRunView BillingRunModel::execute(const AdvanceBillingRun& action) {
    requirePrincipal();
    if (!action.validate() || (*action.batch).numerator < 1) {
        throw ValidationError{"AdvanceBillingRun: a run and a batch of at least one are required"};
    }
    requireRoleSomewhere<AdvanceBillingRun>();
    Lightweight::DataMapper mapper;
    auto runs = mapper.Query<db::BillingRunRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::BillingRunRecord::id>, "=", *action.runId)
                    .All();
    if (runs.empty()) {
        throw NotFound{"AdvanceBillingRun: run " + std::to_string(*action.runId) + " does not exist"};
    }
    requireAuthorized(action);
    std::string const author = ::morph::session::current()->principal;
    CompanyId const company{runs.front().companyId.Value()};
    std::int64_t remaining = (*action.batch).numerator / (*action.batch).denominator;
    for (auto item : mapper.Query<db::BillingRunItemRecord>()
                         .Where(::Lightweight::FieldNameOf<&db::BillingRunItemRecord::run>, "=", *action.runId)
                         .Where(::Lightweight::FieldNameOf<&db::BillingRunItemRecord::state>, "=", 0)
                         .All()) {
        if (remaining-- == 0) {
            break;
        }
        // The item's invoice and the item's being done are one fact: a run resumed after a failure here drafts
        // the item again from scratch, and one resumed after the commit skips it.
        Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
        auto const invoice = draftInvoice(mapper, company, SiteId{item.siteId.Value()}, ClientId{item.clientId.Value()},
                                          author);
        item.state = 1;
        item.invoiceId = invoice;
        mapper.Update(item);
        sqlTxn.Commit();
    }
    bool const pending = !mapper.Query<db::BillingRunItemRecord>()
                              .Where(::Lightweight::FieldNameOf<&db::BillingRunItemRecord::run>, "=", *action.runId)
                              .Where(::Lightweight::FieldNameOf<&db::BillingRunItemRecord::state>, "=", 0)
                              .All()
                              .empty();
    if (!pending) {
        auto& run = runs.front();
        run.state = static_cast<int>(BillingRunState::Finished);
        mapper.Update(run);
    }
    return runView(mapper, *action.runId);
}
```

`runView(mapper, id)` (anonymous namespace) maps the run with `total` = its items and `done` = its done items, as `Count`s. `dueCompanies(now)`: every company with billing settings whose `billing_day <= dayOfMonth(now)` and no run labelled `monthLabel(calendarMonthOf(now))`. `runningRuns()`: every run in `Running`. `ListBillingRuns` is the conventions' list. Header Doxygen as Task 4's (each `@throws`, including `NotFound` for a company without settings and `Conflict` for a second run in a month). Registrations: `"StartBillingRun"` and `"AdvanceBillingRun"` with `BRIDGE_REGISTER_EFFECTS(…, ::morph::effects::changes<lims::BillingRunView>, ::morph::effects::changes<lims::InvoiceView>, ::morph::effects::changes<lims::BillableLineView>)`, `"ListBillingRuns"` as a read. Rules: `Authorization<StartBillingRun> : CompanyRule<StartBillingRun, LimsRole::BillingClerk>`, and `Authorization<AdvanceBillingRun>` with `role = BillingClerk` and `scopeOf` reading the run's company in `billing_authorization.cpp`. Add both to the edge table and the coverage tuple, `"BillingRunModel"` to its model list.

- [ ] **Step 5: The scheduler, in the app**

Create `examples/lims/app/include/lims/app/billing_scheduler.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/util/datetime.hpp>
#include <string>

/// @file
/// The billing schedule: on each tick, start the run of every company whose billing day has come, and advance every
/// run that is not finished. The server ticks it once at start-up, which resumes a run a restart interrupted, and
/// once a minute after.

namespace lims::app {

/// @brief Starts and advances billing runs, as a service principal.
class BillingScheduler {
public:
    /// @param principal The principal runs are started and advanced as; it needs the billing-clerk role at each
    ///        company, granted like anyone's, so what the schedule may do is in the role assignments.
    explicit BillingScheduler(std::string principal) : _principal{std::move(principal)} {}

    /// @brief One pass of the schedule at @p now.
    /// @param now The instant; companies whose billing day is on or before its day of the month are due.
    /// @throws std::exception from a run; runs after the failing one wait for the next tick.
    void tick(::morph::time::Timestamp const& now);

private:
    std::string _principal;
};

}  // namespace lims::app
```

and `billing_scheduler.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "lims/app/billing_scheduler.hpp"

#include <morph/session/session.hpp>
#include <morph/util/rational.hpp>

#include "lims/billing/models/billing_run_model.hpp"

namespace lims::app {

void BillingScheduler::tick(::morph::time::Timestamp const& now) {
    // morph has no public way to act as a principal outside a dispatch, so the schedule installs the session the
    // models read through session::detail, as the lims tests do.
    ::morph::session::Context ctx;
    ctx.principal = _principal;
    ::morph::session::detail::ScopedContext const scope{ctx};
    BillingRunModel runs;
    for (CompanyId const company : BillingRunModel::dueCompanies(now)) {
        static_cast<void>(runs.execute(StartBillingRun{.company = company}));
    }
    Count const batch{::morph::math::Rational{50, ::morph::math::DecimalPlaces{0}}};
    for (BillingRunId const run : BillingRunModel::runningRuns()) {
        while (runs.execute(AdvanceBillingRun{.runId = run, .batch = batch}).state == BillingRunState::Running) {
        }
    }
}

}  // namespace lims::app
```

In `src/server/main.cpp` (include `"lims/app/billing_scheduler.hpp"` and `"clock.hpp"`), after the relay timer:

```cpp
        // The billing schedule: once now, which resumes a run a restart interrupted, then once a minute.
        lims::app::BillingScheduler scheduler{environment("LIMS_SCHEDULER_PRINCIPAL", "lims-scheduler")};
        auto const tickSchedule = [&scheduler] {
            try {
                scheduler.tick(morph::ladder::now());
            } catch (std::exception const& error) {
                std::cerr << "lims-server: billing schedule: " << error.what() << '\n';
            }
        };
        tickSchedule();
        QTimer scheduleTimer;
        QObject::connect(&scheduleTimer, &QTimer::timeout, tickSchedule);
        scheduleTimer.start(60'000);
```

and add `LIMS_SCHEDULER_PRINCIPAL (default lims-scheduler)` to the file's environment comment.

- [ ] **Step 6: Build and run**

Run: Build lims, Model suite `"[run]"`, Model suite `"[authorization]"`, Everything labelled for the rung. Expected: all pass. Run the two mutations and restore them.

- [ ] **Step 7: Record decision 34**

```markdown
### 34. The billing run is a cursor in billing's tables, driven by a timer in the server (hypothesis 4, first instance)

A run belongs to a company and a month; starting it records one item per client and site with unbilled lines, and
advancing it drafts one item's invoice and marks the item done in one transaction. A run interrupted anywhere leaves
every item wholly done or wholly pending, and resuming drafts only what is pending, so no invoice is drafted twice.
Progress is the run's done items of its total. `lims::app::BillingScheduler::tick` starts every company's run on or
after its billing day and advances every unfinished run; the server ticks it at start-up and once a minute, as the
principal `LIMS_SCHEDULER_PRINCIPAL` names, which holds the billing-clerk role like any clerk.

**Hypothesis 4 is confirmed for this instance.** Measured in `tests/test_billing_run.cpp`: the run starts on its day
and once; a run failed after its first invoice shows 1 of 3 done, and a fresh scheduler after the "restart" finishes
it with one invoice per client. None of it used a morph primitive for the schedule or the resume: the trigger is a
`QTimer` in the server, the resume is a cursor table of billing's own, and the schedule acts as its principal through
`morph::session::detail::ScopedContext`, a detail type, because nothing public runs a model action as a principal
outside a dispatch. `TimeoutScheduler`, the one timer morph has, is `morph::async::detail`. Approval escalation is the
second instance (decision 35).
```

- [ ] **Step 8: Commit**

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/billing examples/lims/app examples/lims/src/server/main.cpp examples/lims/tests \
        examples/lims/README.md
git commit -m "Schedule the lims billing run and resume it after a restart without duplicate invoices

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---
### Task 12: Invoice approval — four eyes, value limits, a deadline that escalates (hypotheses 5 and 6, and 4's second instance)

**Files:**
- Create: `examples/lims/billing/include/lims/billing/core/ledger_port.hpp`, `examples/lims/billing/src/core/ledger_port.cpp`, `examples/lims/billing/src/models/invoice_approval.cpp`, `examples/lims/tests/test_invoice_approval.cpp`
- Modify: `invoice_dto.hpp` (four actions), `invoice_model.hpp`, `billing_authorization.hpp`/`.cpp`, `declared_requirements.hpp`, `test_declared_authorization.cpp`, `examples/lims/README.md` (decisions 35 and 36)

**Interfaces:**
- Consumes: `Authorization`, `requireAuthorized`, `authorizationVerdict`, `AuthorizationFacts`, `OrgModel::rolesIn`, `OrgModel::valueLimit`, `OrgModel::findSite`, `OrgModel::findCompany`, `PriceListModel::settingsOf`, `invoiceView`, `roundToCurrency`, and the ledger's dated rate through `lims::ledgerPort().rateOn` (declared here, wired in Task 14; until then `ledgerPort().rateOn` is empty and a foreign-currency invoice cannot be approved — see Step 4).
- Produces: `SubmitInvoice { InvoiceId invoiceId; }`, `ApproveInvoice { InvoiceId invoiceId; }`, `ReturnInvoice { InvoiceId invoiceId; }` → `InvoiceView`; `ListPendingApprovals {}` → `ListInvoicesResult`; `Authorization<ApproveInvoice>` with `role = InvoiceApprover`, `scopeOf` (the invoice's site before its deadline, its company after), `amountOf` (gross in company currency), `limitOf` (the principal's value limit at the scope's company), `separatedFrom = typeId<CreateInvoice>`, `sameActorRefusal`, `firstPerformerOf` (the invoice's creator); `Authorization<ReturnInvoice>` (role and scope only); `Authorization<SubmitInvoice>` (billing clerk at the invoice's company); `lims::LedgerPort { std::function<PostingResult(PostingRequest const&)> post; std::function<std::optional<Rational>(CompanyId, CurrencyCode, CurrencyCode, Timestamp const&)> rateOn; }`, `void lims::setLedgerPort(LedgerPort)`, `LedgerPort const& lims::ledgerPort()` in `billing/core/ledger_port.hpp`.

Lifecycle: `Draft → Submitted` (a billing clerk; the deadline is now plus the company's approval hours), `Submitted → Approved` (an invoice approver who did not create it, whose value limit covers its gross in company currency, holding the role at the invoice's site — or, once the deadline has passed, at its company), `Submitted → Draft` (`ReturnInvoice`, by an approver who could approve it, limit aside). Escalation is the deadline the rule reads: nothing runs at the deadline, the scope the rule asks for changes, and `ListPendingApprovals` shows an invoice to whoever could approve it now.

- [ ] **Step 1: The ledger port (billing's half; the app fills it in Task 14)**

Create `examples/lims/billing/include/lims/billing/core/ledger_port.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>
#include <morph/util/datetime.hpp>
#include <morph/util/rational.hpp>
#include <optional>

#include "lims/posting/posting.hpp"

/// @file
/// What billing needs of the ledger, without including it: posting a request, and the dated rate between two
/// currencies. The app installs both at start-up, before the first dispatch, and never changes them after.

namespace lims {

/// @brief The ledger as billing reaches it.
struct LedgerPort {
    /// @brief Records a posting; answers its entry, or throws the ledger's refusal.
    std::function<PostingResult(PostingRequest const&)> post;
    /// @brief The rate from the first currency to the second valid on a date at a company, or nothing.
    std::function<std::optional<::morph::math::Rational>(CompanyId, CurrencyCode, CurrencyCode,
                                                         ::morph::time::Timestamp const&)>
        rateOn;
};

/// @brief Installs @p port. Called once at start-up (and by tests), before any billing action runs.
/// @param port The port.
void setLedgerPort(LedgerPort port);

/// @brief The installed port; both members empty until one is installed.
/// @return It.
[[nodiscard]] LedgerPort const& ledgerPort();

}  // namespace lims
```

and `billing/src/core/ledger_port.cpp` holding it in a function-local static.

- [ ] **Step 2: Write the failing test**

Create `examples/lims/tests/test_invoice_approval.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Invoice approval. Hypothesis 5: an approval that lasts days and passes between principals is ordinary actions and
// the declared authorization. Hypothesis 6, second instance: billing's create/approve pair and the approver's value
// limit, which the server edge cannot see. Hypothesis 4, second instance: escalation at a deadline, with nothing
// scheduled.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <morph/session/session_auth.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <string_view>

#include "billing_test_support.hpp"
#include "clock.hpp"
#include "lims/app/event_wiring.hpp"
#include "lims/billing/models/invoice_model.hpp"
#include "lims/org/dto/limit_dto.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::ScopedClockOverride;
using morph::ladder::testkit::awaitQt;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::math::DecimalPlaces;
using morph::math::Rational;

namespace {

constexpr std::string_view kSecret = "lims-invoice-approval-test-secret-32-bytes";

/// @brief A drafted EUR invoice at the seeded site for one published sample (gross 47.60), created by bea, with
///        48 approval hours. paula approves at the seeded site with a limit of 100; carl approves at the company with
///        a limit of 1000; tom approves at the site with a limit of 10; bea is also an approver with a limit of
///        1000 at the company. Created at 09:00 on 10 November 2026.
/// @return The invoice.
lims::InvoiceView draftedInvoice() {
    ScopedClockOverride const clock{lims::test::at(2026, 11, 10)};
    lims::test::grant("sam", lims::LimsRole::Supervisor);
    {
        ScopedPrincipal const sam{"sam"};
        lims::PriceListModel{}.execute(lims::SetBillingSettings{
            .company = lims::test::seededCompany(),
            .receivableAccount = "1200",
            .revenueAccount = "4000",
            .taxAccount = "2200",
            .roundingAccount = "6900",
            .taxRate = lims::Rate{Rational{morph::math::Numerator{19}, morph::math::Denominator{100}, DecimalPlaces{6}}},
            .approvalHours = lims::Count{Rational{48, DecimalPlaces{0}}},
            .billingDay = lims::Count{Rational{25, DecimalPlaces{0}}}});
        lims::OrgModel org;
        auto const limit = [&org](std::string principal, std::int64_t units) {
            org.execute(lims::SetValueLimit{.principal = std::move(principal),
                                            .company = lims::test::seededCompany(),
                                            .limit = lims::Amount{Rational{units, DecimalPlaces{2}}}});
        };
        limit("paula", 100);
        limit("carl", 1000);
        limit("tom", 10);
        limit("bea", 1000);
    }
    lims::test::grant("paula", lims::LimsRole::InvoiceApprover, lims::Scope{lims::test::seededSite()});
    lims::test::grant("tom", lims::LimsRole::InvoiceApprover, lims::Scope{lims::test::seededSite()});
    lims::test::grant("carl", lims::LimsRole::InvoiceApprover);
    lims::test::grant("bea", lims::LimsRole::InvoiceApprover);
    auto const lab = lims::test::publishedSample();
    lims::test::billingFor(lab.client);
    lims::app::relayLabEvents();
    ScopedPrincipal const bea{"bea"};
    return lims::InvoiceModel{}.execute(
        lims::CreateInvoice{.company = lims::test::seededCompany(), .site = lims::test::seededSite(), .client = lab.client});
}

/// @brief Submits @p invoice as bea at 09:00 on 10 November.
void submit(lims::InvoiceView const& invoice) {
    ScopedClockOverride const clock{lims::test::at(2026, 11, 10)};
    ScopedPrincipal const bea{"bea"};
    lims::InvoiceModel{}.execute(lims::SubmitInvoice{.invoiceId = invoice.id});
}

}  // namespace

// Mutation: drop the separation-of-duties members from Authorization<ApproveInvoice> (bea approves her own invoice).
TEST_CASE("invoice approval: four eyes and a value limit, judged in the model", "[lims][billing][approval]") {
    DbFixture fixture;
    auto const invoice = draftedInvoice();
    submit(invoice);
    ScopedClockOverride const clock{lims::test::at(2026, 11, 11)};
    lims::InvoiceModel invoices;
    {
        ScopedPrincipal const bea{"bea"};
        CHECK_THROWS_WITH(invoices.execute(lims::ApproveInvoice{.invoiceId = invoice.id}),
                          Catch::Matchers::ContainsSubstring("created this invoice"));
    }
    {
        ScopedPrincipal const tom{"tom"};
        CHECK_THROWS_WITH(invoices.execute(lims::ApproveInvoice{.invoiceId = invoice.id}),
                          Catch::Matchers::ContainsSubstring("value limit"));
    }
    {
        ScopedPrincipal const nobody{"nora"};
        CHECK_THROWS_AS(invoices.execute(lims::ApproveInvoice{.invoiceId = invoice.id}), lims::Forbidden);
    }
    ScopedPrincipal const paula{"paula"};
    auto const approved = invoices.execute(lims::ApproveInvoice{.invoiceId = invoice.id});
    CHECK(approved.state == lims::InvoiceState::Approved);
    CHECK(approved.approvedBy == "paula");
    CHECK_THROWS_AS(invoices.execute(lims::ApproveInvoice{.invoiceId = invoice.id}), lims::IllegalTransition);
}

// Hypothesis 4, second instance. Mutation: judge the scope at the site whatever the time (paula still approves after
// the deadline, and carl's pending list stays as it was).
TEST_CASE("Hypothesis 4: an approval past its deadline escalates to a company-scoped approver, with nothing scheduled",
          "[lims][billing][approval][h4]") {
    DbFixture fixture;
    auto const invoice = draftedInvoice();
    submit(invoice);
    lims::InvoiceModel invoices;
    auto const pendingFor = [&invoices](std::string principal) {
        ScopedPrincipal const as{std::move(principal)};
        return invoices.execute(lims::ListPendingApprovals{}).invoices.size();
    };
    {
        ScopedClockOverride const before{lims::test::at(2026, 11, 12, 8)};
        CHECK(pendingFor("paula") == 1U);
        CHECK(pendingFor("carl") == 1U);  // a company role holds at the site
        CHECK(pendingFor("tom") == 0U);   // over his limit
        CHECK(pendingFor("bea") == 0U);   // she created it
        ScopedPrincipal const bea{"bea"};
        CHECK(invoices.execute(lims::GetInvoice{.invoiceId = invoice.id}).level == lims::ApprovalLevel::Site);
    }
    ScopedClockOverride const after{lims::test::at(2026, 11, 12, 10)};
    CHECK(pendingFor("paula") == 0U);
    CHECK(pendingFor("carl") == 1U);
    {
        ScopedPrincipal const paula{"paula"};
        CHECK_THROWS_WITH(invoices.execute(lims::ApproveInvoice{.invoiceId = invoice.id}),
                          Catch::Matchers::ContainsSubstring("at company"));
    }
    ScopedPrincipal const carl{"carl"};
    CHECK(invoices.execute(lims::GetInvoice{.invoiceId = invoice.id}).level == lims::ApprovalLevel::Company);
    CHECK(invoices.execute(lims::ApproveInvoice{.invoiceId = invoice.id}).state == lims::InvoiceState::Approved);
}

// Hypothesis 5. Mutation: keep the approval state in the model instance instead of the row (the approval made over a
// second rig, after the first is gone, finds the invoice still a draft).
TEST_CASE("Hypothesis 5: an approval lasts across days, principals and restarts as ordinary actions",
          "[lims][billing][approval][h5]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto const invoice = draftedInvoice();
    morph::session::TokenIssuer const issuer{std::string{kSecret}, morph::session::hmacSha256};
    {
        ScopedClockOverride const monday{lims::test::at(2026, 11, 10)};
        BackendRig rig{mode, 1, lims::test::limsAuthorizer(kSecret)};
        rig.bridge(0).setDefaultSession(lims::test::signedAs(issuer, "bea"));
        auto clerk = rig.client<lims::InvoiceModel>(0);
        CHECK(awaitQt(clerk.execute(lims::SubmitInvoice{.invoiceId = invoice.id})).state ==
              lims::InvoiceState::Submitted);
    }
    // The server restarted; another principal, a day later, on another connection.
    ScopedClockOverride const tuesday{lims::test::at(2026, 11, 11, 15)};
    BackendRig rig{mode, 1, lims::test::limsAuthorizer(kSecret)};
    rig.bridge(0).setDefaultSession(lims::test::signedAs(issuer, "paula"));
    auto approver = rig.client<lims::InvoiceModel>(0);
    auto const approved = awaitQt(approver.execute(lims::ApproveInvoice{.invoiceId = invoice.id}));
    CHECK(approved.state == lims::InvoiceState::Approved);
    CHECK(approved.submittedBy == "bea");
    CHECK(approved.approvedBy == "paula");
}

// Hypothesis 6, second instance: the edge sees (principal, model, action) and allows; the model sees the invoice.
// Mutation: drop requireAuthorized from ApproveInvoice (bea's self-approval succeeds over every backend).
TEST_CASE("Hypothesis 6: the edge lets an invoice's creator through to approve it, and the model refuses",
          "[lims][billing][approval][h6]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto const invoice = draftedInvoice();
    submit(invoice);
    morph::session::TokenIssuer const issuer{std::string{kSecret}, morph::session::hmacSha256};
    auto const authorizer = lims::test::limsAuthorizer(kSecret);
    CHECK(authorizer->authorize(lims::test::signedAs(issuer, "bea"), "InvoiceModel", "ApproveInvoice"));
    CHECK(authorizer->authorize(lims::test::signedAs(issuer, "tom"), "InvoiceModel", "ApproveInvoice"));
    ScopedClockOverride const clock{lims::test::at(2026, 11, 11)};
    BackendRig rig{mode, 1, authorizer};
    rig.bridge(0).setDefaultSession(lims::test::signedAs(issuer, "bea"));
    auto client = rig.client<lims::InvoiceModel>(0);
    CHECK_THROWS_WITH(awaitQt(client.execute(lims::ApproveInvoice{.invoiceId = invoice.id})),
                      Catch::Matchers::ContainsSubstring("created this invoice"));
    rig.bridge(0).setDefaultSession(lims::test::signedAs(issuer, "tom"));
    CHECK_THROWS_WITH(awaitQt(client.execute(lims::ApproveInvoice{.invoiceId = invoice.id})),
                      Catch::Matchers::ContainsSubstring("value limit"));
}

TEST_CASE("invoice approval: an approver returns an invoice to draft, and a draft is submitted again",
          "[lims][billing][approval]") {
    DbFixture fixture;
    auto const invoice = draftedInvoice();
    submit(invoice);
    ScopedClockOverride const clock{lims::test::at(2026, 11, 11)};
    lims::InvoiceModel invoices;
    {
        ScopedPrincipal const tom{"tom"};  // over his limit, but returning needs none
        CHECK(invoices.execute(lims::ReturnInvoice{.invoiceId = invoice.id}).state == lims::InvoiceState::Draft);
    }
    ScopedPrincipal const bea{"bea"};
    auto const again = invoices.execute(lims::SubmitInvoice{.invoiceId = invoice.id});
    CHECK(again.state == lims::InvoiceState::Submitted);
    CHECK(again.approvalDeadline == morph::time::Timestamp{lims::test::at(2026, 11, 13)});
}
```

- [ ] **Step 3: Run it and see it fail**

Run: Build lims. Expected: compile error, `lims::SubmitInvoice` is not declared.

- [ ] **Step 4: The rules**

In `billing_authorization.hpp`:

```cpp
/// @brief An invoice is submitted by a billing clerk at its company.
template <>
struct Authorization<SubmitInvoice> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::BillingClerk;

    /// @brief The company of the invoice the payload names.
    /// @param action The action.
    /// @return It; an empty company when there is no such invoice.
    [[nodiscard]] static Scope scopeOf(SubmitInvoice const& action);
};

/// @brief An invoice is approved by an invoice approver who did not create it and whose value limit covers it: at
///        the invoice's site while its approval is within its deadline, at its company after. The scope moving
///        from site to company at the deadline is the escalation.
template <>
struct Authorization<ApproveInvoice> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::InvoiceApprover;

    /// @brief The pair's first half: the registered id of `CreateInvoice`.
    static constexpr std::string_view separatedFrom = ::morph::model::ActionTraits<CreateInvoice>::typeId();

    /// @brief The refusal when the approver created the invoice.
    static constexpr std::string_view sameActorRefusal = "created this invoice and cannot also approve it";

    /// @brief The invoice's site before its deadline, its company from the deadline on.
    /// @param action The action.
    /// @return The scope; an empty company when there is no such invoice.
    [[nodiscard]] static Scope scopeOf(ApproveInvoice const& action);

    /// @brief The invoice's gross in its company's currency: converted at the rate on its submission date and rounded
    ///        half-up when the currencies differ.
    /// @param action The action.
    /// @return The amount; zero when there is no such invoice.
    /// @throws ValidationError when the currencies differ and no rate is valid on the submission date.
    [[nodiscard]] static ::morph::math::Rational amountOf(ApproveInvoice const& action);

    /// @brief @p principal's value limit at the company of @p scope.
    /// @param principal The principal.
    /// @param scope The scope `scopeOf` gave.
    /// @return The limit, or nothing.
    [[nodiscard]] static std::optional<::morph::math::Rational> limitOf(std::string_view principal,
                                                                       Scope const& scope);

    /// @brief Who created the invoice.
    /// @param action The action.
    /// @return The creator; empty when there is no such invoice.
    [[nodiscard]] static std::optional<std::string> firstPerformerOf(ApproveInvoice const& action);
};

/// @brief An invoice is returned to draft by an invoice approver in the scope its approval is judged in.
template <>
struct Authorization<ReturnInvoice> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::InvoiceApprover;

    /// @brief As `Authorization<ApproveInvoice>::scopeOf`.
    /// @param action The action.
    /// @return The scope.
    [[nodiscard]] static Scope scopeOf(ReturnInvoice const& action);
};
```

In `billing_authorization.cpp`, beside a helper `invoiceRow(InvoiceId) -> std::optional<db::InvoiceRecord>`:

```cpp
/// @brief The scope an invoice's approval is judged in now.
/// @param invoice The invoice.
/// @return Its site before the deadline, its company from the deadline on; an empty company without an invoice.
[[nodiscard]] Scope approvalScope(InvoiceId invoice) {
    auto const row = invoiceRow(invoice);
    if (!row.has_value()) {
        return Scope{CompanyId{}};
    }
    bool const escalated = row->deadlineAt.Value() > 0 && nowMillis() >= row->deadlineAt.Value();
    return escalated ? Scope{CompanyId{row->companyId.Value()}} : Scope{SiteId{row->siteId.Value()}};
}

Scope Authorization<ApproveInvoice>::scopeOf(ApproveInvoice const& action) { return approvalScope(action.invoiceId); }

Scope Authorization<ReturnInvoice>::scopeOf(ReturnInvoice const& action) { return approvalScope(action.invoiceId); }

::morph::math::Rational Authorization<ApproveInvoice>::amountOf(ApproveInvoice const& action) {
    auto const row = invoiceRow(action.invoiceId);
    if (!row.has_value()) {
        return ::morph::math::Rational{0, ::morph::math::DecimalPlaces{0}};
    }
    CompanyId const company{row->companyId.Value()};
    auto const currency = static_cast<CurrencyCode>(row->currency.Value());
    auto const base = OrgModel::findCompany(company)->baseCurrency;
    ::morph::math::Rational const gross{::morph::math::Numerator{row->grossNum.Value()},
                                        ::morph::math::Denominator{row->grossDen.Value()},
                                        ::morph::math::DecimalPlaces{decimalsOf(currency)}};
    if (currency == base) {
        return gross;
    }
    auto const& port = ledgerPort();
    auto const rate = port.rateOn ? port.rateOn(company, currency, base, timestampFromMillis(row->submittedAt.Value()))
                                  : std::nullopt;
    if (!rate.has_value()) {
        throw ValidationError{"ApproveInvoice: no " + std::string{currencyName(currency)} + "/" +
                              std::string{currencyName(base)} + " rate on the invoice's submission date"};
    }
    auto const converted = ::morph::math::checkedMul(gross, *rate);
    if (!converted.has_value()) {
        throw OutOfRange{"ApproveInvoice: the invoice's gross is too large to convert exactly"};
    }
    return roundToCurrency(*converted, base);
}

std::optional<::morph::math::Rational> Authorization<ApproveInvoice>::limitOf(std::string_view principal,
                                                                             Scope const& scope) {
    if (auto const* company = std::get_if<CompanyId>(&scope)) {
        return OrgModel::valueLimit(principal, *company);
    }
    auto const site = OrgModel::findSite(std::get<SiteId>(scope));
    return site.has_value() ? OrgModel::valueLimit(principal, site->companyId) : std::nullopt;
}

std::optional<std::string> Authorization<ApproveInvoice>::firstPerformerOf(ApproveInvoice const& action) {
    auto const row = invoiceRow(action.invoiceId);
    return row.has_value() ? std::optional<std::string>{std::string{row->createdBy.Value().ToStringView()}}
                           : std::nullopt;
}
```

and `Authorization<SubmitInvoice>::scopeOf` returning the invoice's company. Put `SubmitInvoice`, `ApproveInvoice` and `ReturnInvoice` in the edge table and the coverage tuple.

- [ ] **Step 5: The actions**

Add the DTOs (`validate()` requires the id; `ListPendingApprovals::validate()` is `true`) and register in `invoice_model.hpp`: `"SubmitInvoice"`, `"ApproveInvoice"`, `"ReturnInvoice"`, each `BRIDGE_REGISTER_EFFECTS(…, ::morph::effects::updates<lims::InvoiceView>)`; `"ListPendingApprovals"` a read. Create `invoice_approval.cpp`:

```cpp
InvoiceView InvoiceModel::execute(const SubmitInvoice& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"SubmitInvoice: an invoice is required"};
    }
    requireRoleSomewhere<SubmitInvoice>();
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    auto row = invoiceRecord(mapper, action.invoiceId, "SubmitInvoice");
    requireAuthorized(action);
    if (row.state.Value() != static_cast<int>(InvoiceState::Draft)) {
        throw IllegalTransition{"SubmitInvoice: only a draft is submitted"};
    }
    auto const settings = PriceListModel::settingsOf(CompanyId{row.companyId.Value()});
    std::int64_t const hours = (*settings->approvalHours).numerator / (*settings->approvalHours).denominator;
    std::int64_t const now = nowMillis();
    row.state = static_cast<int>(InvoiceState::Submitted);
    row.submittedBy = Lightweight::SqlAnsiString<64>{::morph::session::current()->principal};
    row.submittedAt = now;
    row.deadlineAt = now + hours * 3'600'000;
    mapper.Update(row);
    sqlTxn.Commit();
    return invoiceView(mapper, static_cast<std::int64_t>(row.id.Value()));
}

InvoiceView InvoiceModel::execute(const ApproveInvoice& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"ApproveInvoice: an invoice is required"};
    }
    requireRoleSomewhere<ApproveInvoice>();
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    auto row = invoiceRecord(mapper, action.invoiceId, "ApproveInvoice");
    if (row.state.Value() != static_cast<int>(InvoiceState::Submitted)) {
        throw IllegalTransition{"ApproveInvoice: only a submitted invoice is approved"};
    }
    requireAuthorized(action);
    row.state = static_cast<int>(InvoiceState::Approved);
    row.approvedBy = Lightweight::SqlAnsiString<64>{::morph::session::current()->principal};
    mapper.Update(row);
    sqlTxn.Commit();
    return invoiceView(mapper, static_cast<std::int64_t>(row.id.Value()));
}
```

`ReturnInvoice` is `ApproveInvoice`'s shape setting `state = Draft` and clearing `submitted_by`, `submitted_at`, `deadline_at`. The state check comes before `requireAuthorized` in approval so that approving an approved invoice is an `IllegalTransition` for everyone, not a refusal that names who created it. `invoiceRecord(mapper, id, where)` (anonymous namespace) loads the row or throws `NotFound`. `ListPendingApprovals`:

```cpp
// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ListInvoicesResult InvoiceModel::execute(const ListPendingApprovals& action) {
    static_cast<void>(action);
    std::string const principal = currentPrincipal();
    ListInvoicesResult result;
    if (principal.empty()) {
        return result;
    }
    Lightweight::DataMapper mapper;
    for (auto const& row : mapper.Query<db::InvoiceRecord>()
                               .Where(::Lightweight::FieldNameOf<&db::InvoiceRecord::state>, "=",
                                      static_cast<int>(InvoiceState::Submitted))
                               .All()) {
        ApproveInvoice const approve{.invoiceId = InvoiceId{static_cast<std::int64_t>(row.id.Value())}};
        using Rule = Authorization<ApproveInvoice>;
        Scope const scope = Rule::scopeOf(approve);
        AuthorizationFacts facts{.principal = principal,
                                 .required = Rule::role,
                                 .held = OrgModel::rolesIn(principal, scope),
                                 .firstPerformer = Rule::firstPerformerOf(approve),
                                 .amount = std::nullopt,
                                 .limit = Rule::limitOf(principal, scope)};
        try {
            facts.amount = Rule::amountOf(approve);
        } catch (ValidationError const&) {
            continue;  // no rate yet: nobody can approve it, so it is nobody's to act on
        }
        if (authorizationVerdict(facts) == Verdict::Allowed) {
            result.invoices.push_back(invoiceView(mapper, static_cast<std::int64_t>(row.id.Value())));
        }
    }
    return result;
}
```

The pending list asks the same declared rule `ApproveInvoice` is judged by; it is the one place the rule is evaluated without the action, and `authorizationVerdict` is what makes that a call rather than a copy.

- [ ] **Step 6: Build and run**

Run: Build lims, Model suite `"[approval]"`, Model suite `"[authorization]"`. Expected: all pass, the h5 and h6 cases in all three modes. Run the four mutations named in the file and restore each.

- [ ] **Step 7: Measure the second instance of hypothesis 6 as code**

Record what the second instance needed that the first already had, for Task 16:

```bash
cd /home/yaraslau/repo/morph
grep -rn 'requireAuthorized(action)' examples/lims/*/src | wc -l
grep -rn 'requireRoleSomewhere<' examples/lims/*/src | wc -l
grep -c 'requirementOf<' examples/lims/app/include/lims/app/declared_requirements.hpp
grep -n 'std::tuple<' -A12 examples/lims/tests/test_declared_authorization.cpp | head -20
git diff --stat HEAD~12 -- examples/lims/billing/include/lims/billing/authorization examples/lims/billing/src/authorization
```

Keep the output in `/tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/plan_b_h6_measure.txt`.

- [ ] **Step 8: Record decisions 35 and 36**

```markdown
### 35. Invoice approval is ordinary actions on the invoice's row, and escalation is its deadline (hypotheses 5 and 4)

`SubmitInvoice`, `ApproveInvoice` and `ReturnInvoice` move an invoice between draft, submitted and approved; the
state, who submitted and approved it, and the approval deadline are columns of its row. The deadline is the submission
time plus the company's approval hours. Before it, the approver's role is judged at the invoice's site (a role at the
company holds there too); from it, only at the company: an approver at the site no longer can, and
`ListPendingApprovals` shows the invoice to whoever could approve it now, through the same declared rule.

**Hypothesis 5 is refuted for this instance.** Measured in `tests/test_invoice_approval.cpp`: an invoice submitted
by one principal on one rig is approved by another a day later on a new rig, in `Local`, `LocalSingleThread` and
`Socket` mode; four eyes and the value limit hold. The lifecycle needed no state outside the row and no code shared
with lab's verification but the declared authorization (`lims::Authorization`, `requireAuthorized`,
`authorizationVerdict`). Requisition approval in Part C is the second instance.

**Hypothesis 4's second instance needed no scheduled work.** Escalation is a deadline the rule reads when an approver
acts or lists, so nothing has to run at the deadline: measured in the same file with the clock before and after it.
What is inferred, not measured: a screen listing pending approvals is not told when the deadline passes, since no
action ran and no effect was declared; it shows the escalation on its next fetch. So the scheduled work hypothesis 4
names occurred once in Part B (the billing run, decision 34) and no framework seam is designed from it; Part C's
requisition escalation is expected to take this shape, and its stock jobs are out of the design's scope.

### 36. Billing's create/approve pair and the approver's value limit are declared, and checked in the model (hypothesis 6, second instance)

`Authorization<ApproveInvoice>` declares the invoice approver role in a scope its payload's invoice decides (site or,
past the deadline, company), a value limit (the invoice's gross in company currency against the approver's limit at
that company, `SetValueLimit`), and a separation-of-duties pair with `CreateInvoice`.

**Hypothesis 6 is confirmed for its second instance.** Measured in `tests/test_invoice_approval.cpp`:
`LimsAuthorizer` allows both the invoice's creator and an approver over their limit (it sees the principal, the model
and the action), and the model refuses both, in every mode. The same hand-rolled guarantee recurred: each declared
action's handler calls `requireRoleSomewhere` and `requireAuthorized` itself ([measured] calls across lab, org, ledger
and billing), the edge's table restates every declaration by hand ([measured] entries), and the coverage test lists
every mutating action by hand because nothing enumerates a model's actions publicly. Task 16 decides the seam.
```

- [ ] **Step 9: Commit**

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/billing examples/lims/tests examples/lims/app/include/lims/app/declared_requirements.hpp \
        examples/lims/README.md
git commit -m "Approve lims invoices with four eyes, value limits and deadline escalation

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 13: The draft invoice's edit lease, and its lapse on disconnect (hypothesis 3, first instance)

**Files:**
- Create: `examples/lims/billing/include/lims/billing/dto/lease_dto.hpp`, `examples/lims/billing/include/lims/billing/models/invoice_edit_model.hpp`, `examples/lims/billing/src/models/invoice_edit_model.cpp`, `examples/lims/tests/lims_server_rig.hpp`, `examples/lims/tests/test_invoice_lease.cpp`
- Modify: `billing_entity.hpp` (`InvoiceLeaseRecord`), `billing_schema.cpp`, `invoice_model.hpp` (`GetInvoiceLease`), `billing_authorization.hpp`/`.cpp`, `declared_requirements.hpp`, `test_declared_authorization.cpp`, `examples/lims/README.md` (decision 37)

**Interfaces:**
- Consumes: `invoiceView`, `morph::backend::RemoteServer` (`openConnection`, `setConnectionWriter`, `handle(msg, reply, cid)`, `handleInline(msg, cid)`, `closeConnection`), `morph::backend::IEnvelopeService`, `morph::backend::ConnectionFacts`, `morph::wire::Envelope`, `morph::wire::WatchBody`, `morph::wire::kWatchKind`.
- Produces: `LeaseInvoice { InvoiceId invoiceId; }` → `InvoiceLeaseView { InvoiceId invoice; std::string holder; Timestamp expiresAt; }`; `ReleaseInvoice {}` → `InvoiceLeaseView`; `RemoveInvoiceLine { BillableLineId line; }` → `InvoiceView`; `SetInvoiceReference { std::string reference; }` → `InvoiceView`; `GetInvoiceLease { InvoiceId invoiceId; }` (on `InvoiceModel`) → `InvoiceLeaseView` (empty holder when free); model `lims::InvoiceEditModel` (`"InvoiceEditModel"`, one private instance per editor); record `Billing_Lease` (keyed by `invoice`); `class lims::test::ServerRig` in `tests/lims_server_rig.hpp`.

A lease is a row: the invoice, the holder, a token naming the editing instance, and an expiry fifteen minutes after the last edit. `LeaseInvoice` takes a free or expired lease, or renews its own; another holder's live lease is refused in words naming them. Each edit requires the instance's own live lease and renews it. When the instance is destroyed — its screen unmounts locally, or the server reclaims it when its connection closes — it deletes its lease by token. Expiry needs no timer: an expired lease is free to the next `LeaseInvoice`.

- [ ] **Step 1: The server rig**

Create `examples/lims/tests/lims_server_rig.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <morph/core/connection_writer.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session_auth.hpp>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "lims_test_support.hpp"

/// @file
/// A lims server driven envelope by envelope: connections that open, register models, execute, watch records and
/// close, with every event the server writes to each kept. What a test of disconnects and fan-out needs that
/// `BackendRig` does not give: closing one connection while others stay, and the events each received.

namespace lims::test {

/// @brief Negotiates server-sent events for the connection its kind arrives on, as the UI service's `ui-hello` does.
class EventsHello final : public ::morph::backend::IEnvelopeService {
public:
    using IEnvelopeService::handle;

    /// @brief The kind it serves.
    static constexpr std::string_view kKind = "lims-test-hello";

    /// @brief Whether it serves @p kind.
    /// @param kind The envelope's kind.
    /// @return `true` for `lims-test-hello`.
    [[nodiscard]] bool serves(std::string_view kind) const override { return kind == kKind; }

    /// @brief Answers `ok`.
    /// @param request The envelope.
    /// @return The reply.
    [[nodiscard]] std::string handle(::morph::wire::Envelope const& request,
                                     ::morph::session::IAuthorizer const& /*authorizer*/) override {
        return ::morph::wire::encode(::morph::wire::makeOk(request.callId));
    }

    /// @brief Answers `ok` and records that the connection takes events.
    /// @param request The envelope.
    /// @param authorizer The authorizer.
    /// @param facts The connection's facts.
    /// @return The reply.
    [[nodiscard]] std::string handle(::morph::wire::Envelope const& request,
                                     ::morph::session::IAuthorizer const& authorizer,
                                     ::morph::backend::ConnectionFacts& facts) override {
        facts.events = true;
        return handle(request, authorizer);
    }
};

/// @brief One lims server, its connections and what each was sent.
class ServerRig {
public:
    /// @param secret The token secret the server's `LimsAuthorizer` verifies with.
    explicit ServerRig(std::string_view secret)
        : _issuer{std::string{secret}, ::morph::session::hmacSha256}, _server{[&] {
              ::morph::backend::ServerConfig config;
              config.services.push_back(std::make_shared<EventsHello>());
              config.coalesceWindow = std::chrono::milliseconds{0};
              return std::make_shared<::morph::backend::RemoteServer>(_pool, limsAuthorizer(secret), config);
          }()} {}

    /// @brief Opens a connection that takes events and keeps what it is sent.
    /// @return The connection.
    ::morph::backend::ConnectionId open() {
        auto const cid = _server->openConnection();
        _server->setConnectionWriter(cid, {.write = [this, cid](std::string message) {
                                               std::scoped_lock const lock{_mutex};
                                               _sent[cid].push_back(std::move(message));
                                           },
                                           .backlog = [] { return std::size_t{0}; }});
        ::morph::wire::Envelope hello;
        hello.kind = std::string{EventsHello::kKind};
        hello.callId = next();
        static_cast<void>(_server->handleInline(::morph::wire::encode(hello), cid));
        return cid;
    }

    /// @brief Registers a private instance of @p typeId on @p cid as @p principal.
    /// @return The instance id.
    std::uint64_t registerModel(::morph::backend::ConnectionId cid, std::string_view typeId,
                                std::string const& principal) {
        ::morph::wire::Envelope env;
        env.kind = "register";
        env.callId = next();
        env.typeId = std::string{typeId};
        env.session = signedAs(_issuer, principal);
        auto const reply = request(cid, std::move(env));
        return reply.modelId;
    }

    /// @brief Executes @p actionType with @p body on instance @p modelId of @p modelType, on @p cid, as @p principal.
    /// @return The reply envelope (`ok` or `err`).
    ::morph::wire::Envelope execute(::morph::backend::ConnectionId cid, std::uint64_t modelId,
                                    std::string_view modelType, std::string_view actionType, std::string body,
                                    std::string const& principal) {
        ::morph::wire::Envelope env;
        env.kind = "execute";
        env.callId = next();
        env.modelId = modelId;
        env.modelType = std::string{modelType};
        env.actionType = std::string{actionType};
        env.body = std::move(body);
        env.session = signedAs(_issuer, principal);
        return request(cid, std::move(env));
    }

    /// @brief Watches @p records on @p cid as @p principal.
    /// @return The reply.
    ::morph::wire::Envelope watch(::morph::backend::ConnectionId cid, std::vector<std::string> records,
                                  std::string const& principal) {
        ::morph::wire::Envelope env;
        env.kind = std::string{::morph::wire::kWatchKind};
        env.callId = next();
        env.session = signedAs(_issuer, principal);
        env.body = glz::write_json(::morph::wire::WatchBody{.records = std::move(records), .instances = {}, .replace = false})
                       .value_or(std::string{});
        return ::morph::wire::decode(_server->handleInline(::morph::wire::encode(env), cid));
    }

    /// @brief Everything the server wrote to @p cid that was not a reply: its events.
    /// @return The encoded envelopes.
    [[nodiscard]] std::vector<std::string> sent(::morph::backend::ConnectionId cid) const {
        std::scoped_lock const lock{_mutex};
        auto const found = _sent.find(cid);
        return found == _sent.end() ? std::vector<std::string>{} : found->second;
    }

    /// @brief Closes @p cid, as the transport does when the socket goes.
    void close(::morph::backend::ConnectionId cid) { _server->closeConnection(cid); }

    /// @brief Polls @p pred until it holds or @p budget passes.
    /// @return Whether it held.
    template <class Pred>
    [[nodiscard]] static bool eventually(Pred pred, std::chrono::milliseconds budget = std::chrono::milliseconds{3000}) {
        auto const deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            if (pred()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        return pred();
    }

private:
    [[nodiscard]] std::uint64_t next() { return ++_callId; }

    ::morph::wire::Envelope request(::morph::backend::ConnectionId cid, ::morph::wire::Envelope env) {
        auto promise = std::make_shared<std::promise<std::string>>();
        auto answer = promise->get_future();
        _server->handle(::morph::wire::encode(env), [promise](std::string reply) { promise->set_value(std::move(reply)); },
                        cid);
        return ::morph::wire::decode(answer.get());
    }

    ::morph::exec::ThreadPoolExecutor _pool{4};
    ::morph::session::TokenIssuer _issuer;
    std::shared_ptr<::morph::backend::RemoteServer> _server;
    mutable std::mutex _mutex;
    std::map<::morph::backend::ConnectionId, std::vector<std::string>> _sent;
    std::uint64_t _callId = 0;
};

}  // namespace lims::test
```

The shapes used here are read from `tests/test_push_watch.cpp` (its `Rig`, `FanHello`, `executeOn`) and `include/morph/core/remote.hpp`; the field names `ServerConfig::coalesceWindow`, `ConnectionWriter::write`/`backlog`, `ConnectionFacts::events` and `Envelope::modelId` are from those headers. Not verified by reading: that `register` is authorised by `LimsAuthorizer` without a role (its `authorizeRegister` is `IAuthorizer`'s default) — if `register` answers `err "unauthorized"`, read `session.hpp`'s `authorizeRegister` and give `LimsAuthorizer` the override `SigningAuthorizer` needs. `_pool` is declared before `_server` so the server goes first.

- [ ] **Step 2: Write the failing test**

Create `examples/lims/tests/test_invoice_lease.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Hypothesis 3, first instance: a draft invoice is leased to the clerk editing it; a second clerk is told who holds
// it; the lease lapses after a timeout and when the holder's connection closes.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <glaze/glaze.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "billing_test_support.hpp"
#include "clock.hpp"
#include "lims/app/event_wiring.hpp"
#include "lims/billing/models/invoice_edit_model.hpp"
#include "lims/billing/models/invoice_model.hpp"
#include "lims_server_rig.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::ScopedClockOverride;
using morph::ladder::testkit::DbFixture;

namespace {

constexpr std::string_view kSecret = "lims-invoice-lease-test-secret-at-least-32";

/// @brief A draft invoice, and two billing clerks, alice and bob, at the seeded company.
/// @return The invoice.
lims::InvoiceView draft() {
    lims::test::grant("sam", lims::LimsRole::Supervisor);
    {
        ScopedPrincipal const sam{"sam"};
        lims::PriceListModel{}.execute(lims::SetBillingSettings{
            .company = lims::test::seededCompany(),
            .receivableAccount = "1200",
            .revenueAccount = "4000",
            .taxAccount = "2200",
            .roundingAccount = "6900",
            .taxRate = lims::Rate{morph::math::Rational{0, morph::math::DecimalPlaces{6}}},
            .approvalHours = lims::Count{morph::math::Rational{48, morph::math::DecimalPlaces{0}}},
            .billingDay = lims::Count{morph::math::Rational{25, morph::math::DecimalPlaces{0}}}});
    }
    auto const lab = lims::test::publishedSample();
    lims::test::billingFor(lab.client);
    lims::app::relayLabEvents();
    lims::test::grant("alice", lims::LimsRole::BillingClerk);
    lims::test::grant("bob", lims::LimsRole::BillingClerk);
    ScopedPrincipal const alice{"alice"};
    return lims::InvoiceModel{}.execute(
        lims::CreateInvoice{.company = lims::test::seededCompany(), .site = lims::test::seededSite(), .client = lab.client});
}

/// @brief Who holds @p invoice's lease, as bob sees it.
[[nodiscard]] std::string holderOf(lims::InvoiceId invoice) {
    ScopedPrincipal const bob{"bob"};
    return lims::InvoiceModel{}.execute(lims::GetInvoiceLease{.invoiceId = invoice}).holder;
}

}  // namespace

// Mutation: skip the holder check in LeaseInvoice (bob takes alice's live lease).
TEST_CASE("invoice lease: a second clerk is told who holds the draft, and takes it once it expires",
          "[lims][billing][lease]") {
    DbFixture fixture;
    auto const invoice = draft();
    ScopedClockOverride const nine{lims::test::at(2026, 11, 10, 9)};
    lims::InvoiceEditModel aliceEditor;
    lims::InvoiceEditModel bobEditor;
    {
        ScopedPrincipal const alice{"alice"};
        auto const lease = aliceEditor.execute(lims::LeaseInvoice{.invoiceId = invoice.id});
        CHECK(lease.holder == "alice");
        aliceEditor.execute(lims::SetInvoiceReference{.reference = "PO 4711"});
    }
    {
        ScopedPrincipal const bob{"bob"};
        CHECK_THROWS_WITH(bobEditor.execute(lims::LeaseInvoice{.invoiceId = invoice.id}),
                          Catch::Matchers::ContainsSubstring("being edited by 'alice'"));
        CHECK_THROWS_AS(bobEditor.execute(lims::SetInvoiceReference{.reference = "mine"}), lims::Conflict);
    }
    CHECK(holderOf(invoice.id) == "alice");
    ScopedClockOverride const later{lims::test::at(2026, 11, 10, 10)};
    ScopedPrincipal const bob{"bob"};
    CHECK(bobEditor.execute(lims::LeaseInvoice{.invoiceId = invoice.id}).holder == "bob");
    CHECK(bobEditor.execute(lims::SetInvoiceReference{.reference = "PO 4712"}).reference == "PO 4712");
}

TEST_CASE("invoice lease: an editor that goes away releases its lease", "[lims][billing][lease]") {
    DbFixture fixture;
    auto const invoice = draft();
    {
        ScopedPrincipal const alice{"alice"};
        lims::InvoiceEditModel editor;
        editor.execute(lims::LeaseInvoice{.invoiceId = invoice.id});
        CHECK(holderOf(invoice.id) == "alice");
    }
    CHECK(holderOf(invoice.id).empty());
}

// Hypothesis 3. Mutation: remove the release from ~InvoiceEditModel (bob's lease is refused until the timeout).
TEST_CASE("Hypothesis 3: the lease lapses when the holder's connection closes", "[lims][billing][lease][h3]") {
    DbFixture fixture;
    auto const invoice = draft();
    lims::test::ServerRig rig{kSecret};
    auto const body = glz::write_json(lims::LeaseInvoice{.invoiceId = invoice.id}).value_or(std::string{});
    auto const aliceConnection = rig.open();
    auto const aliceEditor = rig.registerModel(aliceConnection, "InvoiceEditModel", "alice");
    REQUIRE(rig.execute(aliceConnection, aliceEditor, "InvoiceEditModel", "LeaseInvoice", body, "alice").kind == "ok");

    auto const bobConnection = rig.open();
    auto const bobEditor = rig.registerModel(bobConnection, "InvoiceEditModel", "bob");
    auto const refused = rig.execute(bobConnection, bobEditor, "InvoiceEditModel", "LeaseInvoice", body, "bob");
    CHECK(refused.kind == "err");
    CHECK_THAT(refused.message, Catch::Matchers::ContainsSubstring("being edited by 'alice'"));

    rig.close(aliceConnection);
    bool const lapsed = lims::test::ServerRig::eventually([&] {
        return rig.execute(bobConnection, bobEditor, "InvoiceEditModel", "LeaseInvoice", body, "bob").kind == "ok";
    });
    CHECK(lapsed);
    CHECK(holderOf(invoice.id) == "bob");
}
```

- [ ] **Step 3: Run it and see it fail**

Run: Build lims. Expected: compile error, `lims/billing/models/invoice_edit_model.hpp` not found.

- [ ] **Step 4: The lease**

`lease_dto.hpp`: the structs from Interfaces (`LeaseInvoice` requires its id; `RemoveInvoiceLine` its line; `SetInvoiceReference` a reference of at most 128 bytes; `ReleaseInvoice` always ready); `BRIDGE_REGISTER_RECORD(lims::InvoiceLeaseView, "Billing_Lease", ::morph::record::key<&lims::InvoiceLeaseView::invoice>)`. `InvoiceLeaseRecord` (`billing_invoice_leases`: `id`, `invoice_id`, `holder` `SqlAnsiString<64>`, `token` `SqlAnsiString<32>`, `expires_at`), migration `20261110000010 "Create billing_invoice_leases"`.

`invoice_edit_model.hpp`:

```cpp
/// @brief One clerk's editing of one draft invoice: the lease and the edits it allows.
///
/// One private instance per editor (a screen registers its own). The instance names its lease with a token of its
/// own, so a second instance of the same principal is another editor. Destroying the instance releases its lease:
/// that is what happens when its screen unmounts, and when the server reclaims the instance because the connection
/// that registered it closed.
class InvoiceEditModel {
public:
    InvoiceEditModel();
    ~InvoiceEditModel();
    InvoiceEditModel(InvoiceEditModel const&) = delete;
    InvoiceEditModel& operator=(InvoiceEditModel const&) = delete;
    InvoiceEditModel(InvoiceEditModel&&) = delete;
    InvoiceEditModel& operator=(InvoiceEditModel&&) = delete;

    /// @brief Leases a draft invoice to the caller, or renews this editor's lease.
    /// @param action The invoice.
    /// @return The lease.
    /// @throws ValidationError if no invoice is named.
    /// @throws NotFound if it does not exist.
    /// @throws IllegalTransition if it is not a draft.
    /// @throws Conflict if another editor's lease on it is live, naming its holder.
    /// @throws Forbidden if the caller is not a billing clerk at its company.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    InvoiceLeaseView execute(const LeaseInvoice& action);

    /// @brief Gives this editor's lease up.
    /// @param action Carries nothing.
    /// @return The lease as it was, its holder now empty.
    InvoiceLeaseView execute(const ReleaseInvoice& action);

    /// @brief Takes a line off the leased draft and returns it to unbilled; renews the lease.
    /// @param action The line.
    /// @return The invoice with its totals recomputed.
    /// @throws Conflict if this editor holds no live lease on the line's invoice.
    /// @throws NotFound if the line is not on an invoice.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    InvoiceView execute(const RemoveInvoiceLine& action);

    /// @brief Sets the leased draft's reference; renews the lease.
    /// @param action The reference.
    /// @return The invoice.
    /// @throws Conflict if this editor holds no live lease.
    /// @throws ValidationError if the reference is longer than 128 bytes.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    InvoiceView execute(const SetInvoiceReference& action);

private:
    std::string _token;
    std::optional<std::int64_t> _invoice;
};
```

registered as `"InvoiceEditModel"` with `"LeaseInvoice"`, `"ReleaseInvoice"` (`changes<InvoiceLeaseView>` each), `"RemoveInvoiceLine"` (`updates<InvoiceView>`, `changes<BillableLineView>`) and `"SetInvoiceReference"` (`updates<InvoiceView>`). `GetInvoiceLease` on `InvoiceModel` is a read. In `invoice_edit_model.cpp`:

```cpp
namespace {

/// @brief How long a lease lives after its last edit.
constexpr std::int64_t kLeaseMillis = 15 * 60 * 1000;

/// @brief A token naming one editing instance: 128 random bits as hex.
[[nodiscard]] std::string newToken() {
    std::random_device device;
    std::uniform_int_distribution<std::uint64_t> bits;
    return std::format("{:016x}{:016x}", bits(device), bits(device));
}

}  // namespace

InvoiceEditModel::InvoiceEditModel() : _token{newToken()} {}

InvoiceEditModel::~InvoiceEditModel() {
    if (!_invoice.has_value()) {
        return;
    }
    // A destructor runs wherever the last reference drops — a screen's unmount, or the server reclaiming a closed
    // connection's instances — so a failure here is logged, never thrown.
    try {
        Lightweight::DataMapper mapper;
        for (auto const& row : mapper.Query<db::InvoiceLeaseRecord>()
                                   .Where(::Lightweight::FieldNameOf<&db::InvoiceLeaseRecord::token>, "=", _token)
                                   .All()) {
            mapper.Delete(row);
        }
    } catch (std::exception const& error) {
        ::morph::log::logError("[lims] releasing invoice lease " + _token + " failed: " + error.what());
    }
}

InvoiceLeaseView InvoiceEditModel::execute(const LeaseInvoice& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"LeaseInvoice: an invoice is required"};
    }
    requireRoleSomewhere<LeaseInvoice>();
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    auto const invoice = invoiceView(mapper, *action.invoiceId);
    requireAuthorized(action);
    if (invoice.state != InvoiceState::Draft) {
        throw IllegalTransition{"LeaseInvoice: only a draft is edited"};
    }
    std::int64_t const now = nowMillis();
    std::string const principal = ::morph::session::current()->principal;
    auto rows = mapper.Query<db::InvoiceLeaseRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::InvoiceLeaseRecord::invoiceId>, "=", *action.invoiceId)
                    .All();
    db::InvoiceLeaseRecord row = rows.empty() ? db::InvoiceLeaseRecord{} : rows.front();
    if (!rows.empty() && row.expiresAt.Value() > now && row.token.Value().ToStringView() != _token) {
        throw Conflict{"LeaseInvoice: invoice " + std::to_string(*action.invoiceId) + " is being edited by '" +
                       std::string{row.holder.Value().ToStringView()} + "'"};
    }
    row.invoiceId = *action.invoiceId;
    row.holder = Lightweight::SqlAnsiString<64>{principal};
    row.token = Lightweight::SqlAnsiString<32>{_token};
    row.expiresAt = now + kLeaseMillis;
    if (rows.empty()) {
        mapper.Create(row);
    } else {
        mapper.Update(row);
    }
    sqlTxn.Commit();
    _invoice = *action.invoiceId;
    return InvoiceLeaseView{.invoice = action.invoiceId, .holder = principal, .expiresAt = timestampFromMillis(row.expiresAt.Value())};
}
```

The edits share one helper, `requireOwnLease(mapper, invoiceId)`: the lease row of the invoice must carry this instance's token and be unexpired, else `Conflict` "this editor does not hold invoice N's lease"; it then sets `expires_at` to now plus fifteen minutes. `RemoveInvoiceLine` finds the line's invoice, requires the lease, sets the line `Unbilled` with no invoice, recomputes the invoice's totals with `invoiceTotals` and the settings' tax rate, and answers `invoiceView`. `ReleaseInvoice` deletes this token's row and clears `_invoice`. Declare `Authorization<LeaseInvoice>` (billing clerk at the invoice's company, `scopeOf` reading the invoice) and add it to the edge table and the coverage tuple; the edits are refused without this instance's lease, which only `LeaseInvoice` grants, so they declare no role of their own — list `"RemoveInvoiceLine"`, `"SetInvoiceReference"` and `"ReleaseInvoice"` in `openMutatingActions()` (raising its size) with that reason in its doc comment, and add the three to the coverage tuple.

- [ ] **Step 5: Build and run**

Run: Build lims, Model suite `"[lease]"`, Model suite `"[authorization]"`. Expected (the h3 case inferred from `RemoteServer::closeConnection`'s documentation: "A *private* instance … is erased outright"): all pass. If `CHECK(lapsed)` fails, the destructor did not run on close, or ran without a database connection: the hypothesis is confirmed instead — change the check to `CHECK_FALSE(lapsed)`, assert that bob's lease succeeds only after the clock passes the expiry, and use the confirmed text in Step 6. Run the two mutations and restore them.

- [ ] **Step 6: Record decision 37**

The refuted text (the measured outcome if Step 5 passed as written):

```markdown
### 37. A draft invoice is leased to its editor, and the lease lapses with the editor's instance (hypothesis 3, first instance)

`InvoiceEditModel` is one private instance per editor. `LeaseInvoice` writes a lease row — the invoice, the holder,
the instance's token, an expiry fifteen minutes after the last edit — and refuses another editor's live lease in words
naming its holder; `GetInvoiceLease` shows the holder to anyone. Each edit requires the instance's own live lease and
renews it. An expired lease is free to the next `LeaseInvoice`, so the timeout needs no timer.

**Hypothesis 3 is refuted for this instance, through an existing hook that is not a lease hook.** Measured in
`tests/test_invoice_lease.cpp` over a real `RemoteServer`: closing the holder's connection frees the lease, and the
second clerk takes it without waiting for the timeout. The hook is the instance's destructor: `closeConnection`
erases a private instance, and `~InvoiceEditModel` deletes its lease by token. What that costs, stated plainly: the
release does database work in a destructor, on whatever thread drops the last reference (the server strand when a
connection closes), and a failure there can only be logged; a crash that kills the process releases nothing, and the
timeout is then the only lapse. Lot reservation in Part C is the second instance.
```

If confirmed instead: "**Hypothesis 3 is confirmed for this instance.** Measured in `tests/test_invoice_lease.cpp`: closing the holder's connection [what was observed]; the lease lapsed only at its timeout. No hook told the model that its connection closed."

- [ ] **Step 7: Commit**

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/billing examples/lims/tests examples/lims/app/include/lims/app/declared_requirements.hpp \
        examples/lims/README.md
git commit -m "Lease lims draft invoices to their editor and measure the lapse on disconnect

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---
### Task 14: Posting an invoice — two commits in sequence, under failure injection (hypothesis 1, first instance)

**Files:**
- Create: `examples/lims/ledger/include/lims/ledger/db/ledger_store.hpp`, `examples/lims/billing/src/models/invoice_posting.cpp`, `examples/lims/app/include/lims/app/ledger_wiring.hpp`, `examples/lims/app/src/ledger_wiring.cpp`, `examples/lims/tests/test_invoice_posting.cpp`
- Modify: `ledger_entries.cpp` (`record` over `recordPostingIn`), `invoice_math.hpp`/`.cpp` (`postingFor`), `billing_store.hpp`, `invoice_drafts.cpp` (`assignNumberAndPost`), `billing_entity.hpp` (`NumberSeriesRecord`), `billing_schema.cpp`, `invoice_dto.hpp` (`PostInvoice`), `invoice_model.hpp`, `billing_authorization.hpp`/`.cpp`, `declared_requirements.hpp`, `test_declared_authorization.cpp`, `billing_test_support.hpp` (`books`, `billingSettings`, `approvedInvoice`), `src/server/main.cpp`, `examples/lims/README.md` (decision 38)

**Interfaces:**
- Consumes: `LedgerPort`, `ledgerPort()`, `LedgerModel::record`, `LedgerModel::rateOn`, `fiscalYearOf`, `roundToCurrency`, `faultPoint`.
- Produces: `PostingResult lims::recordPostingIn(Lightweight::DataMapper&, PostingRequest const&)` (ledger-internal, in `ledger/db/ledger_store.hpp`; `record` is it inside its own transaction); `struct lims::InvoiceToPost { CompanyId company; CurrencyCode companyCurrency; CurrencyCode currency; Rational rate; Timestamp date; std::string sourceKey; std::string sourceReference; Rational net; Rational tax; Rational gross; std::string receivableAccount; std::string revenueAccount; std::string taxAccount; std::string roundingAccount; }`; `PostingRequest lims::postingFor(InvoiceToPost const&)`; `std::string lims::assignNumberAndPost(Lightweight::DataMapper&, std::int64_t invoiceId, JournalEntryId entry, std::string const& author)` (billing-internal); `PostInvoice { InvoiceId invoiceId; }` → `InvoiceView`; `void lims::app::installLedgerPort()`; test helpers `lims::test::books()`, `lims::test::billingSettings()`, `lims::InvoiceView lims::test::approvedInvoice(std::string reference = "WW-1")`.

The posting is the design's "two calls in sequence": the ledger records the entry and commits (its source key is `billing.invoice:<id>`, recorded once); then billing assigns the next number of the company's fiscal-year series, marks the invoice posted with the entry's id, and commits. The number is assigned in the second commit, so a failure before it leaves no hole in the series; what it can leave is an entry for an invoice that is not posted — the partial state hypothesis 1 predicts. A retry converges: the ledger answers the entry it recorded, and billing commits. A last case runs both halves in one SQLite transaction below the models, to show the gap is morph's, not the database's.

- [ ] **Step 1: The fixtures**

Append to `billing_test_support.hpp`:

```cpp
/// @brief The seeded company's books: accounts 1200, 2200, 4000, 6900 and November 2026 open, made by `acc`, an
///        accountant there; and the ledger port wired as the server wires it.
inline void books() {
    grant("acc", LimsRole::Accountant);
    ScopedPrincipal const acc{"acc"};
    LedgerModel ledger;
    for (auto const& [code, type] : std::vector<std::pair<std::string, AccountType>>{
             {"1200", AccountType::Asset}, {"2200", AccountType::Liability}, {"4000", AccountType::Revenue},
             {"6900", AccountType::Expense}}) {
        ledger.execute(CreateAccount{.company = seededCompany(), .code = code, .name = "Account " + code, .type = type});
    }
    ledger.execute(OpenPeriod{.company = seededCompany(), .month = ::morph::time::Timestamp{at(2026, 11, 1)}});
    app::installLedgerPort();
}

/// @brief The seeded company's billing settings, 19 % tax, 48 approval hours, billing day 25, set by `sam`.
inline void billingSettings() {
    grant("sam", LimsRole::Supervisor);
    ScopedPrincipal const sam{"sam"};
    PriceListModel{}.execute(SetBillingSettings{
        .company = seededCompany(),
        .receivableAccount = "1200",
        .revenueAccount = "4000",
        .taxAccount = "2200",
        .roundingAccount = "6900",
        .taxRate = Rate{::morph::math::Rational{::morph::math::Numerator{19}, ::morph::math::Denominator{100},
                                                ::morph::math::DecimalPlaces{6}}},
        .approvalHours = Count{::morph::math::Rational{48, ::morph::math::DecimalPlaces{0}}},
        .billingDay = Count{::morph::math::Rational{25, ::morph::math::DecimalPlaces{0}}}});
}

/// @brief An approved EUR invoice for one published sample: drafted and submitted by `bea`, approved by `carl`, an
///        approver at the company with a limit of 1000. Needs `billingSettings()` first.
/// @param reference The sample's reference, distinct per invoice.
/// @return The invoice.
inline InvoiceView approvedInvoice(std::string reference = "WW-1") {
    {
        ScopedPrincipal const sam{"sam"};
        OrgModel{}.execute(SetValueLimit{.principal = "carl", .company = seededCompany(),
                                         .limit = Amount{::morph::math::Rational{1000, ::morph::math::DecimalPlaces{2}}}});
    }
    grant("carl", LimsRole::InvoiceApprover);
    auto const lab = publishedSample(std::move(reference));
    billingFor(lab.client);
    app::relayLabEvents();
    InvoiceId id;
    {
        ScopedPrincipal const bea{"bea"};
        InvoiceModel invoices;
        id = invoices.execute(CreateInvoice{.company = seededCompany(), .site = seededSite(), .client = lab.client}).id;
        invoices.execute(SubmitInvoice{.invoiceId = id});
    }
    ScopedPrincipal const carl{"carl"};
    return InvoiceModel{}.execute(ApproveInvoice{.invoiceId = id});
}
```

(add the includes: `lims/ledger/models/ledger_model.hpp`, `lims/billing/models/invoice_model.hpp`, `lims/org/dto/limit_dto.hpp`, `lims/app/event_wiring.hpp`, `lims/app/ledger_wiring.hpp`).

- [ ] **Step 2: Write the failing test**

Create `examples/lims/tests/test_invoice_posting.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Hypothesis 1, first instance: posting an invoice is billing's state change and the ledger's entry, committed by
// two calls in sequence. A failure injected between them; a ledger refusal; numbers under concurrent posting; and the
// same two halves in one SQLite transaction, below the models.

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <morph/session/session_auth.hpp>
#include <set>
#include <stdexcept>
#include <string>

#include "billing_test_support.hpp"
#include "clock.hpp"
#include "lims/billing/core/fault_hook.hpp"
#include "lims/billing/core/invoice_math.hpp"
#include "lims/billing/db/billing_store.hpp"
#include "lims/ledger/db/ledger_store.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::ScopedClockOverride;
using morph::ladder::testkit::awaitQt;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

namespace {

constexpr std::string_view kSecret = "lims-invoice-posting-test-secret-at-least";

struct FaultHookGuard {
    FaultHookGuard() = default;
    FaultHookGuard(FaultHookGuard const&) = delete;
    FaultHookGuard& operator=(FaultHookGuard const&) = delete;
    FaultHookGuard(FaultHookGuard&&) = delete;
    FaultHookGuard& operator=(FaultHookGuard&&) = delete;
    ~FaultHookGuard() { lims::setFaultHook({}); }
};

/// @brief The seeded company's entries, as acc sees them.
[[nodiscard]] std::vector<lims::JournalEntryView> entries() {
    ScopedPrincipal const acc{"acc"};
    return lims::LedgerModel{}.execute(lims::ListEntries{.company = lims::test::seededCompany()}).entries;
}

/// @brief Posts @p invoice as bea.
lims::InvoiceView post(lims::InvoiceView const& invoice) {
    ScopedPrincipal const bea{"bea"};
    return lims::InvoiceModel{}.execute(lims::PostInvoice{.invoiceId = invoice.id});
}

[[nodiscard]] Rational exact(std::int64_t num, std::int64_t den, std::uint32_t places) {
    return Rational{Numerator{num}, Denominator{den}, DecimalPlaces{places}};
}

}  // namespace

// Mutation: drop the rounding line from postingFor (the USD invoice's company values differ by a cent and the ledger
// refuses the entry).
TEST_CASE("invoice posting: the poster rounds each line half-up and books the difference to the rounding account",
          "[lims][billing][posting]") {
    lims::InvoiceToPost const usd{.company = lims::CompanyId{1},
                                  .companyCurrency = lims::CurrencyCode::EUR,
                                  .currency = lims::CurrencyCode::USD,
                                  .rate = exact(92, 100, 6),
                                  .date = morph::time::Timestamp{lims::test::at(2026, 11, 20)},
                                  .sourceKey = "billing.invoice:9",
                                  .sourceReference = "invoice 9",
                                  .net = exact(1005, 100, 2),
                                  .tax = exact(191, 100, 2),
                                  .gross = exact(1196, 100, 2),
                                  .receivableAccount = "1200",
                                  .revenueAccount = "4000",
                                  .taxAccount = "2200",
                                  .roundingAccount = "6900"};
    auto const request = lims::postingFor(usd);
    REQUIRE(request.lines.size() == 4U);
    // 11.96 × 0.92 = 11.0032 → 11.00; 10.05 × 0.92 = 9.246 → 9.25; 1.91 × 0.92 = 1.7572 → 1.76; 11.00 − 11.01 = −0.01.
    CHECK(*request.lines[0].companyAmount == exact(1100, 100, 2));
    CHECK(*request.lines[1].companyAmount == exact(925, 100, 2));
    CHECK(*request.lines[2].companyAmount == exact(176, 100, 2));
    CHECK(request.lines[3].accountCode == "6900");
    CHECK(request.lines[3].side == lims::PostingSide::Debit);
    CHECK((*request.lines[3].amount).isZero());
    CHECK(*request.lines[3].companyAmount == exact(1, 100, 2));
    CHECK_FALSE(lims::entryProblem(request.lines, lims::CurrencyCode::USD, lims::CurrencyCode::EUR).has_value());

    auto same = usd;
    same.currency = lims::CurrencyCode::EUR;
    same.rate = exact(1, 1, 6);
    CHECK(lims::postingFor(same).lines.size() == 3U);
}

// Mutation: skip the series update in assignNumberAndPost (the second invoice is numbered 000001 too).
TEST_CASE("invoice posting: an approved invoice gets the next number of its company's fiscal year, and its entry",
          "[lims][billing][posting]") {
    DbFixture fixture;
    ScopedClockOverride const clock{lims::test::at(2026, 11, 20)};
    lims::test::books();
    lims::test::billingSettings();
    auto const first = lims::test::approvedInvoice("WW-1");
    auto const second = lims::test::approvedInvoice("WW-2");
    auto const posted = post(first);
    CHECK(posted.state == lims::InvoiceState::Posted);
    CHECK(posted.number == "INV-2026-000001");
    CHECK(posted.postedBy == "bea");
    REQUIRE(entries().size() == 1U);
    CHECK(entries()[0].id == posted.entry);
    CHECK(entries()[0].sourceKey == "billing.invoice:" + std::to_string(*first.id));
    CHECK(post(second).number == "INV-2026-000002");
    CHECK_THROWS_AS(post(first), lims::IllegalTransition);
}

// Hypothesis 1. Mutation: assign the number before calling the ledger (the failure leaves INV-2026-000001 assigned
// to an invoice with no entry, and the retry's number check fails).
TEST_CASE("Hypothesis 1: a failure between the ledger's commit and billing's leaves an entry for an unposted invoice",
          "[lims][billing][posting][h1]") {
    auto const point = GENERATE(std::string{"post.afterLedger"}, std::string{"post.beforeCommit"});
    CAPTURE(point);
    DbFixture fixture;
    ScopedClockOverride const clock{lims::test::at(2026, 11, 20)};
    lims::test::books();
    lims::test::billingSettings();
    auto const invoice = lims::test::approvedInvoice();
    FaultHookGuard const guard;
    lims::setFaultHook([&point](std::string_view at) {
        if (at == point) {
            throw std::runtime_error{"server stopped while posting"};
        }
    });
    CHECK_THROWS_WITH(post(invoice), Catch::Matchers::ContainsSubstring("server stopped"));

    // The partial state: the ledger holds the entry, billing does not hold the posting.
    REQUIRE(entries().size() == 1U);
    CHECK(entries()[0].sourceKey == "billing.invoice:" + std::to_string(*invoice.id));
    {
        ScopedPrincipal const bea{"bea"};
        auto const now = lims::InvoiceModel{}.execute(lims::GetInvoice{.invoiceId = invoice.id});
        CHECK(now.state == lims::InvoiceState::Approved);
        CHECK(now.number.empty());
        CHECK_FALSE(now.entry.hasValue());
    }

    // No hole: the number was never assigned. A retry converges on the one entry.
    lims::setFaultHook({});
    auto const retried = post(invoice);
    CHECK(retried.number == "INV-2026-000001");
    REQUIRE(entries().size() == 1U);
    CHECK(retried.entry == entries()[0].id);
}

TEST_CASE("invoice posting: a posting the ledger refuses changes nothing in billing and leaves no hole",
          "[lims][billing][posting]") {
    DbFixture fixture;
    ScopedClockOverride const clock{lims::test::at(2026, 11, 20)};
    lims::test::books();
    lims::test::billingSettings();
    auto const invoice = lims::test::approvedInvoice();
    {
        ScopedPrincipal const acc{"acc"};
        lims::LedgerModel ledger;
        auto const november = ledger.execute(lims::ListPeriods{.company = lims::test::seededCompany()}).periods.at(0);
        ledger.execute(lims::BeginClosePeriod{.periodId = november.id});
        ledger.execute(lims::ClosePeriod{.periodId = november.id});
    }
    CHECK_THROWS_AS(post(invoice), lims::Conflict);
    CHECK(entries().empty());
    {
        ScopedPrincipal const acc{"acc"};
        lims::LedgerModel{}.execute(lims::OpenPeriod{.company = lims::test::seededCompany(),
                                                     .month = morph::time::Timestamp{lims::test::at(2026, 12, 1)}});
    }
    ScopedClockOverride const december{lims::test::at(2026, 12, 2)};
    CHECK(post(invoice).number == "INV-2026-000001");
}

// Mutation: read the series' next number outside the posting's transaction (two concurrent posts take the same
// number, or one fails on the database lock).
TEST_CASE("invoice posting: two clerks posting at once get consecutive numbers", "[lims][billing][posting][socket-only]") {
    DbFixture fixture;
    ScopedClockOverride const clock{lims::test::at(2026, 11, 20)};
    lims::test::books();
    lims::test::billingSettings();
    auto const first = lims::test::approvedInvoice("WW-1");
    auto const second = lims::test::approvedInvoice("WW-2");
    lims::test::grant("bob", lims::LimsRole::BillingClerk);
    morph::session::TokenIssuer const issuer{std::string{kSecret}, morph::session::hmacSha256};
    BackendRig rig{Mode::Socket, 2, lims::test::limsAuthorizer(kSecret)};
    rig.bridge(0).setDefaultSession(lims::test::signedAs(issuer, "bea"));
    rig.bridge(1).setDefaultSession(lims::test::signedAs(issuer, "bob"));
    auto bea = rig.client<lims::InvoiceModel>(0);
    auto bob = rig.client<lims::InvoiceModel>(1);
    auto beaPosting = bea.execute(lims::PostInvoice{.invoiceId = first.id});
    auto bobPosting = bob.execute(lims::PostInvoice{.invoiceId = second.id});
    std::set<std::string> const numbers{awaitQt(std::move(beaPosting)).number, awaitQt(std::move(bobPosting)).number};
    CHECK(numbers == std::set<std::string>{"INV-2026-000001", "INV-2026-000002"});
}

// What one SQLite transaction does with both halves, below the models: the database can make them one fact.
TEST_CASE("Hypothesis 1: one database transaction holds both halves, when both are called below their models",
          "[lims][billing][posting][h1]") {
    DbFixture fixture;
    ScopedClockOverride const clock{lims::test::at(2026, 11, 20)};
    lims::test::books();
    lims::test::billingSettings();
    auto const invoice = lims::test::approvedInvoice();
    lims::InvoiceToPost const toPost{.company = lims::test::seededCompany(),
                                     .companyCurrency = lims::CurrencyCode::EUR,
                                     .currency = lims::CurrencyCode::EUR,
                                     .rate = exact(1, 1, 6),
                                     .date = morph::time::Timestamp{lims::test::at(2026, 11, 20)},
                                     .sourceKey = "billing.invoice:" + std::to_string(*invoice.id),
                                     .sourceReference = "invoice " + std::to_string(*invoice.id),
                                     .net = *invoice.net,
                                     .tax = *invoice.tax,
                                     .gross = *invoice.gross,
                                     .receivableAccount = "1200",
                                     .revenueAccount = "4000",
                                     .taxAccount = "2200",
                                     .roundingAccount = "6900"};
    {
        Lightweight::DataMapper mapper;
        Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
        auto const posted = lims::recordPostingIn(mapper, lims::postingFor(toPost));
        static_cast<void>(lims::assignNumberAndPost(mapper, *invoice.id, posted.entryId, "bea"));
        // Dropped without Commit(): the failure between the halves.
    }
    CHECK(entries().empty());
    {
        ScopedPrincipal const bea{"bea"};
        CHECK(lims::InvoiceModel{}.execute(lims::GetInvoice{.invoiceId = invoice.id}).number.empty());
    }
    {
        Lightweight::DataMapper mapper;
        Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
        auto const posted = lims::recordPostingIn(mapper, lims::postingFor(toPost));
        CHECK(lims::assignNumberAndPost(mapper, *invoice.id, posted.entryId, "bea") == "INV-2026-000001");
        sqlTxn.Commit();
    }
    CHECK(entries().size() == 1U);
}
```

Add `#include <catch2/generators/catch_generators.hpp>` for `GENERATE`.

- [ ] **Step 3: Run it and see it fail**

Run: Build lims. Expected: compile error, `lims/ledger/db/ledger_store.hpp` not found.

- [ ] **Step 4: The ledger's half below its model**

Create `ledger_store.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>

#include "lims/posting/posting.hpp"

/// @file
/// Recording a posting through a caller's open mapper. Ledger-internal: `LedgerModel::record` is it inside a
/// transaction of its own, and the hypothesis 1 suite calls it to put both halves of a posting in one transaction.

namespace lims {

/// @brief Records @p request as a posted entry through @p mapper, once per company and source key; the caller
///        commits.
/// @param mapper The caller's mapper, inside its transaction.
/// @param request The request.
/// @return The entry; the first one recorded under the request's key when it is repeated.
/// @throws ValidationError, NotFound, Conflict as `LedgerModel::record` documents.
[[nodiscard]] PostingResult recordPostingIn(Lightweight::DataMapper& mapper, PostingRequest const& request);

}  // namespace lims
```

In `ledger_entries.cpp`, move `record`'s body after its validation into `recordPostingIn(mapper, request)` (everything from `PostingSources sources{mapper};` to `sources.record(...)`, returning the result; the validation and the rate check move with it, since the probe calls it directly), and make `record`:

```cpp
PostingResult LedgerModel::record(PostingRequest const& request) {
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    auto const result = recordPostingIn(mapper, request);
    sqlTxn.Commit();
    return result;
}
```

- [ ] **Step 5: Billing's half, the mapping, the action**

`postingFor` in `invoice_math.cpp`:

```cpp
PostingRequest postingFor(InvoiceToPost const& invoice) {
    auto const toCompany = [&invoice](::morph::math::Rational const& amount) {
        auto const converted = ::morph::math::checkedMul(amount, invoice.rate);
        if (!converted.has_value()) {
            throw OutOfRange{"an invoice amount is too large to convert exactly"};
        }
        return roundToCurrency(*converted, invoice.companyCurrency);
    };
    auto const line = [&invoice, &toCompany](std::string account, PostingSide side, ::morph::math::Rational const& amount) {
        return PostingLine{.accountCode = std::move(account), .side = side, .amount = Amount{amount},
                           .companyAmount = Amount{toCompany(amount)}};
    };
    PostingRequest request{.company = invoice.company,
                           .date = invoice.date,
                           .sourceKey = invoice.sourceKey,
                           .sourceReference = invoice.sourceReference,
                           .currency = invoice.currency,
                           .rate = Rate{invoice.rate},
                           .lines = {line(invoice.receivableAccount, PostingSide::Debit, invoice.gross),
                                     line(invoice.revenueAccount, PostingSide::Credit, invoice.net),
                                     line(invoice.taxAccount, PostingSide::Credit, invoice.tax)}};
    // Each line is rounded on its own, so the company-currency sides can differ by the rounding; the difference is
    // booked, explicitly, to the rounding account. The ledger rounds nothing and would refuse the entry without it.
    auto const debit = *request.lines[0].companyAmount;
    auto const credit = *::morph::math::checkedAdd(*request.lines[1].companyAmount, *request.lines[2].companyAmount);
    if (debit != credit) {
        bool const debitShort = debit < credit;
        auto const difference = *::morph::math::checkedSub(debitShort ? credit : debit, debitShort ? debit : credit);
        request.lines.push_back(PostingLine{
            .accountCode = invoice.roundingAccount,
            .side = debitShort ? PostingSide::Debit : PostingSide::Credit,
            .amount = Amount{::morph::math::Rational{0, ::morph::math::DecimalPlaces{decimalsOf(invoice.currency)}}},
            .companyAmount = Amount{difference}});
    }
    return request;
}
```

(`InvoiceToPost`, `postingFor` declared in `invoice_math.hpp` with full Doxygen: `@param invoice` what the posting is made from, `@return` the request, `@throws OutOfRange`.)

`NumberSeriesRecord` (`billing_number_series`: `id`, `company_id`, `fiscal_year` `std::int64_t`, `next_number` `std::int64_t`), migration `20261110000011 "Create billing_number_series"`. `assignNumberAndPost` in `invoice_drafts.cpp`:

```cpp
std::string assignNumberAndPost(Lightweight::DataMapper& mapper, std::int64_t invoiceId, JournalEntryId entry,
                                std::string const& author) {
    auto invoices = mapper.Query<db::InvoiceRecord>()
                        .Where(::Lightweight::FieldNameOf<&db::InvoiceRecord::id>, "=", invoiceId)
                        .All();
    if (invoices.empty()) {
        throw NotFound{"invoice " + std::to_string(invoiceId) + " does not exist"};
    }
    auto& invoice = invoices.front();
    // The invoice's row is written first, so this transaction holds the database's write lock before it reads the
    // series: a second posting waits for it instead of reading the same next number.
    invoice.state = static_cast<int>(InvoiceState::Posted);
    invoice.postedBy = Lightweight::SqlAnsiString<64>{author};
    invoice.entryId = *entry;
    mapper.Update(invoice);
    CompanyId const company{invoice.companyId.Value()};
    auto const fiscalYear = fiscalYearOf(::morph::ladder::now(), OrgModel::findCompany(company)->fiscalYearStart);
    auto series = mapper.Query<db::NumberSeriesRecord>()
                      .Where(::Lightweight::FieldNameOf<&db::NumberSeriesRecord::companyId>, "=", *company)
                      .Where(::Lightweight::FieldNameOf<&db::NumberSeriesRecord::fiscalYear>, "=",
                             static_cast<std::int64_t>(fiscalYear))
                      .All();
    db::NumberSeriesRecord row = series.empty() ? db::NumberSeriesRecord{} : series.front();
    std::int64_t const number = series.empty() ? 1 : row.nextNumber.Value();
    row.companyId = *company;
    row.fiscalYear = fiscalYear;
    row.nextNumber = number + 1;
    if (series.empty()) {
        mapper.Create(row);
    } else {
        mapper.Update(row);
    }
    std::string const formatted = std::format("INV-{}-{:06}", fiscalYear, number);
    invoice.number = Lightweight::SqlAnsiString<32>{formatted};
    mapper.Update(invoice);
    faultPoint("post.beforeCommit");
    return formatted;
}
```

Not verified by reading: whether Lightweight opens SQLite transactions `DEFERRED` with a busy timeout. If the concurrency case fails with "database is locked", read `examples/common/testkit/db_busy_fixture.hpp` and `docs/LIGHTWEIGHT-CONSTRAINTS.md` for what the ladder already knows about SQLite locking, and record the outcome in decision 38 as a measured finding (the two posts then serialise only by retrying, which is the model's to do).

`InvoiceModel::execute(const PostInvoice&)` in `invoice_posting.cpp`:

```cpp
InvoiceView InvoiceModel::execute(const PostInvoice& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"PostInvoice: an invoice is required"};
    }
    requireRoleSomewhere<PostInvoice>();
    Lightweight::DataMapper mapper;
    auto const invoice = invoiceView(mapper, *action.invoiceId);
    requireAuthorized(action);
    if (invoice.state != InvoiceState::Approved) {
        throw IllegalTransition{"PostInvoice: only an approved invoice is posted"};
    }
    auto const& port = ledgerPort();
    if (!port.post || !port.rateOn) {
        throw ValidationError{"PostInvoice: no ledger is wired to post to"};
    }
    auto const company = OrgModel::findCompany(invoice.company);
    auto const settings = PriceListModel::settingsOf(invoice.company);
    auto const now = ::morph::ladder::now();
    ::morph::math::Rational rate{1, ::morph::math::DecimalPlaces{6}};
    if (invoice.currency != company->baseCurrency) {
        auto const dated = port.rateOn(invoice.company, invoice.currency, company->baseCurrency, now);
        if (!dated.has_value()) {
            throw ValidationError{"PostInvoice: no rate for the invoice's currency today"};
        }
        rate = *dated;
    }
    // The first call: the ledger records the entry and commits alone.
    PostingResult const posted = port.post(postingFor(InvoiceToPost{
        .company = invoice.company,
        .companyCurrency = company->baseCurrency,
        .currency = invoice.currency,
        .rate = rate,
        .date = now,
        .sourceKey = "billing.invoice:" + std::to_string(*invoice.id),
        .sourceReference = "invoice " + std::to_string(*invoice.id),
        .net = *invoice.net,
        .tax = *invoice.tax,
        .gross = *invoice.gross,
        .receivableAccount = settings->receivableAccount,
        .revenueAccount = settings->revenueAccount,
        .taxAccount = settings->taxAccount,
        .roundingAccount = settings->roundingAccount}));
    faultPoint("post.afterLedger");
    // The second call: billing numbers and posts the invoice and commits alone. Between the two, an entry exists
    // for an invoice that is not posted; that window is what hypothesis 1 names.
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    static_cast<void>(assignNumberAndPost(mapper, *invoice.id, posted.entryId, ::morph::session::current()->principal));
    sqlTxn.Commit();
    return invoiceView(mapper, *invoice.id);
}
```

Register `"PostInvoice"` with `BRIDGE_REGISTER_EFFECTS(lims::PostInvoice, ::morph::effects::updates<lims::InvoiceView>)` — the entry it makes in the ledger is the ledger's `Ledger_Entry`, which billing cannot name (decision 32's gap, a third place). Declare `Authorization<PostInvoice>` (billing clerk at the invoice's company), add it to the edge table and the coverage tuple.

- [ ] **Step 6: The app's port**

`ledger_wiring.hpp` declares `void installLedgerPort();` ("Gives billing the ledger: posting through `LedgerModel::record`, rates through `LedgerModel::rateOn`. Called once at start-up, before the first dispatch.") and `ledger_wiring.cpp`:

```cpp
void installLedgerPort() {
    setLedgerPort(LedgerPort{
        .post = [](PostingRequest const& request) { return LedgerModel::record(request); },
        .rateOn = [](CompanyId company, CurrencyCode from, CurrencyCode to, ::morph::time::Timestamp const& on) {
            return LedgerModel::rateOn(company, from, to, on);
        }});
}
```

In `src/server/main.cpp`, call `lims::app::installLedgerPort();` right after `lims::db::setup(...)`.

- [ ] **Step 7: Build and run**

Run: Build lims, Model suite `"[posting]"`, Everything labelled for the rung. Expected: all pass. Run the four mutations and restore each.

- [ ] **Step 8: Record decision 38**

```markdown
### 38. Posting an invoice is two commits in sequence, and the window between them is real (hypothesis 1, first instance)

`PostInvoice` builds the invoice's `PostingRequest` (`lims::postingFor`: receivable debit, revenue and tax credits,
each converted at the dated rate and rounded half-up on its own, and a line to the company's rounding account for the
difference) and hands it to the ledger through the port the app installs (`lims::app::installLedgerPort`). The ledger
records it once per source key and commits. Then billing assigns the next number of the company's fiscal-year series
(`INV-<fiscal year>-<six digits>`, the fiscal year named by the calendar year it begins in), marks the invoice posted
with its entry, and commits. The number is assigned in billing's commit, so no failure leaves a hole in the series.

**Hypothesis 1 is confirmed for this instance.** Measured in `tests/test_invoice_posting.cpp`: a failure injected
after the ledger's commit, or inside billing's transaction before its commit, leaves the ledger holding an entry for
an invoice that is approved, unnumbered and unposted — the partial state the design calls a ledger defect. A retry
converges (the ledger answers the entry it recorded; billing numbers and posts), and two clerks posting at once get
consecutive numbers. The same two halves called below their models, through one `Lightweight::DataMapper` and one
`SqlTransaction`, commit together or not at all: the database can make them one fact, and what cannot is morph's
`execute` — each model action takes its own connection and commits alone, and nothing composes two models' actions
into one transaction while keeping each model's authorization, effects and journal. Goods receipt in Part C is the
second instance, and the framework seam is designed against both.
```

- [ ] **Step 9: Commit**

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/ledger examples/lims/billing examples/lims/app examples/lims/src/server/main.cpp \
        examples/lims/tests examples/lims/README.md
git commit -m "Post lims invoices to the ledger in two commits and measure the window between them

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 15: Does a company-B change reach a company-A connection? (hypothesis 7, first instance)

**Files:**
- Create: `examples/lims/tests/test_two_company_effects.cpp`
- Modify: `examples/lims/README.md` (decision 39)

**Interfaces:**
- Consumes: `lims::test::ServerRig`, `InvoiceModel::consume`, `CreateInvoice`, `PriceListModel`, `OrgModel`.
- Produces: a measured verdict on hypothesis 7 for invoices.

- [ ] **Step 1: Write the test**

Create `examples/lims/tests/test_two_company_effects.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Hypothesis 7, first instance: two companies' clerks watch invoices on one server; a company-B invoice is created.
// Does the company-A connection hear of it?

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glaze/glaze.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <string_view>

#include "billing_test_support.hpp"
#include "lims/billing/models/invoice_model.hpp"
#include "lims_server_rig.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;
using morph::math::DecimalPlaces;
using morph::math::Rational;

namespace {

constexpr std::string_view kSecret = "lims-two-company-effects-secret-at-least-32";

/// @brief Whether any event sent to a connection names @p record.
[[nodiscard]] bool names(std::vector<std::string> const& sent, std::string_view record) {
    return std::ranges::any_of(sent, [record](std::string const& message) {
        return message.find(std::string{"\""} + std::string{record} + "\"") != std::string::npos;
    });
}

/// @brief A company with one site, billing settings, a JPY billing client 1 and one unbilled line at its site, all
///        set up by sam; olga a billing clerk there.
/// @return The company and its site.
std::pair<lims::CompanyId, lims::SiteId> osaka() {
    lims::test::grant("sam", lims::LimsRole::Supervisor);
    ScopedPrincipal const sam{"sam"};
    lims::OrgModel org;
    auto const company = org.execute(lims::CreateCompany{
        .name = "Osaka branch", .baseCurrency = lims::CurrencyCode::JPY, .fiscalYearStart = lims::Month::April});
    auto const site = org.execute(lims::CreateSite{.companyId = company.id, .name = "Osaka lab"});
    org.execute(lims::AssignRole{.principal = "olga", .role = lims::LimsRole::BillingClerk, .company = company.id, .site = {}});
    org.execute(lims::AssignRole{.principal = "sam", .role = lims::LimsRole::BillingClerk, .company = company.id, .site = {}});
    lims::PriceListModel prices;
    prices.execute(lims::SetBillingSettings{.company = company.id,
                                            .receivableAccount = "1200",
                                            .revenueAccount = "4000",
                                            .taxAccount = "2200",
                                            .roundingAccount = "6900",
                                            .taxRate = lims::Rate{Rational{0, DecimalPlaces{6}}},
                                            .approvalHours = lims::Count{Rational{48, DecimalPlaces{0}}},
                                            .billingDay = lims::Count{Rational{25, DecimalPlaces{0}}}});
    prices.execute(lims::RegisterBillingClient{
        .client = lims::ClientId{1}, .company = company.id, .name = "Osaka water", .currency = lims::CurrencyCode::JPY});
    auto const list = prices.execute(lims::CreatePriceList{.company = company.id,
                                                           .name = "Yen",
                                                           .currency = lims::CurrencyCode::JPY,
                                                           .validFrom = morph::time::Timestamp{lims::test::at(2026, 1, 1, 0)}});
    prices.execute(lims::SetPrice{
        .priceList = list.id, .analysis = lims::AnalysisId{1}, .client = {}, .amount = lims::Amount{Rational{4000, DecimalPlaces{0}}}});
    static_cast<void>(lims::InvoiceModel::consume(lims::BillableEvent{.eventId = "test:osaka:1",
                                                                      .result = lims::ResultId{901},
                                                                      .sample = lims::SampleId{901},
                                                                      .analysis = lims::AnalysisId{1},
                                                                      .client = lims::ClientId{1},
                                                                      .site = site.id,
                                                                      .company = company.id,
                                                                      .publishedAt = morph::time::Timestamp{lims::test::at(2026, 11, 10)}}));
    return {company.id, site.id};
}

}  // namespace

// Hypothesis 7. The measured outcome is asserted; if CHECK(leaked) fails, the hypothesis is refuted for invoices.
TEST_CASE("Hypothesis 7: a company-B invoice change reaches a company-A connection watching invoices",
          "[lims][billing][h7]") {
    DbFixture fixture;
    auto const [companyB, siteB] = osaka();
    lims::test::grant("alice", lims::LimsRole::BillingClerk);  // company A, the seeded company
    lims::test::ServerRig rig{kSecret};
    auto const alice = rig.open();
    auto const olga = rig.open();
    auto const aliceModel = rig.registerModel(alice, "InvoiceModel", "alice");
    auto const olgaModel = rig.registerModel(olga, "InvoiceModel", "olga");
    REQUIRE(rig.watch(alice, {"Billing_Invoice"}, "alice").kind == "ok");
    REQUIRE(rig.watch(olga, {"Billing_Invoice"}, "olga").kind == "ok");
    static_cast<void>(aliceModel);

    auto const body = glz::write_json(lims::CreateInvoice{.company = companyB, .site = siteB, .client = lims::ClientId{1}})
                          .value_or(std::string{});
    auto const created = rig.execute(olga, olgaModel, "InvoiceModel", "CreateInvoice", body, "olga");
    REQUIRE(created.kind == "ok");

    // The control: the writer's own connection hears of it, so the watch and the fan-out work.
    REQUIRE(lims::test::ServerRig::eventually([&] { return names(rig.sent(olga), "Billing_Invoice"); }));
    bool const leaked = lims::test::ServerRig::eventually([&] { return names(rig.sent(alice), "Billing_Invoice"); },
                                                          std::chrono::milliseconds{1000});
    for (auto const& message : rig.sent(alice)) {
        UNSCOPED_INFO("sent to company A: " << message);
    }
    CHECK(leaked);
}
```

The `UNSCOPED_INFO` lines print what company A was sent when the case fails; run it once with `-s` to see them when it passes, for decision 39 (whether the event carries company B's invoice key).

- [ ] **Step 2: Run it**

Run: Build lims, then `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock timeout 300 build/linux-everything/examples/lims/ladder_lims_tests "[h7]" -s`. Expected (inferred from `RemoteServer::fanOut`'s documentation: watchers are re-authorised "by the rule that admitted them", at model and action level): the case passes, and company A's event names `Billing_Invoice`. Note from the `-s` output whether it carries `"keys"` with company B's invoice id. If `CHECK(leaked)` fails, the hypothesis is refuted for invoices: assert `CHECK_FALSE(leaked)` and record what stopped it.

- [ ] **Step 3: Record decision 39 and commit**

```markdown
### 39. An invoice change in one company reaches another company's watching clerk (hypothesis 7, first instance)

**Hypothesis 7 is confirmed for invoices.** Measured in `tests/test_two_company_effects.cpp` over a real
`RemoteServer`: a clerk of the second company creates an invoice; the first company's clerk, watching `Billing_Invoice`
on another connection, is sent a `changes` event naming the record [with / without] the new invoice's key. The server
re-authorises a watcher by its principal, the model and the action that admitted the watch — not by the row's company,
which only the model knows — so the first company learns that the second's invoices changed, and when. Their lists
themselves stay scoped (decision 26): the refetch the event triggers answers only the first company's invoices.
`record::privateKeys` would hide the key, not the event. Stock in Part C is the second instance.
```

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/tests/test_two_company_effects.cpp examples/lims/README.md
git commit -m "Measure whether a lims invoice change in one company reaches another company's watcher

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---
### Task 16: Hypothesis 6's second instance — design the framework seam, or disposition the rule as domain logic

**Files (seam branch):**
- Create: `docs/spec/core/action_guards.md`, `include/morph/core/action_guard.hpp`, `tests/test_action_guard.cpp`
- Modify: `include/morph/core/registry.hpp` (`runHandler`, the Task runner, `ActionEntry`, `actionFacts`), `include/morph/core/bridge.hpp` (`localOp`, `localTaskOp`), `tests/CMakeLists.txt` (the new test file, if the suite lists its sources), `docs/spec/README.md` (the spec's index line), `docs/spec/core/registry.md` and `docs/spec/core/bridge.md` (the guard's place in each path), `examples/IMPLEMENTATION.md` (rule 1), `examples/lims/org/include/lims/org/authorization/declared.hpp` (the lims guard), every lims model `.cpp` that calls `requireRoleSomewhere`/`requireAuthorized` (the calls go), `examples/lims/app/include/lims/app/declared_requirements.hpp` and `lims_authorizer.hpp` (the edge table goes), `examples/lims/app/src/billing_scheduler.cpp`, the lims suites that call a declared action's `execute` directly to test a refusal, `examples/lims/tests/test_declared_authorization.cpp` (coverage by enumeration), `examples/lims/README.md` (decision 40, and decision 25's last paragraph)

**Files (disposition branch):** `examples/lims/README.md` (decision 40) only.

**Interfaces (seam branch):**
- Consumes: `morph::model::detail::ActionDispatcher::prepareAction`, `runHandler`, `Bridge`'s `localOp`/`localTaskOp`, `ActionValidator`, `lims::requireRoleSomewhere`, `lims::requireAuthorized`, `lims::DeclaresRule`.
- Produces: `template <typename Action> struct morph::model::ActionGuard;` (customisation point: `static void check(Action const&)`), `template <typename Action> concept morph::model::Guarded`, `template <typename Action> void morph::model::detail::guardAction(Action const&)`, `template <typename Model, typename Action> ActionTraits<Action>::Result morph::model::executeChecked(Model&, Action const&)` (validate, guard, execute — for a caller that runs a model directly), `struct morph::model::ActionFact { std::string actionType; bool guarded; }` and `std::vector<ActionFact> morph::model::actionFacts(std::string_view modelType)`; in lims, one constrained specialisation `template <typename Action> requires lims::DeclaresRule<Action> struct morph::model::ActionGuard<Action>`.

- [ ] **Step 1: Decide from what was measured**

Read decision 36 and `/tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/plan_b_h6_measure.txt`. The seam branch applies when both hold: (a) every declared action's handler calls `requireRoleSomewhere` and `requireAuthorized` itself, in four modules, so a forgotten call is an unguarded action that compiles and passes every test that does not name it; (b) the edge restates every declaration in `declaredRequirements()` because `IAuthorizer::authorize` sees no payload and nothing enumerates a model's actions. Both are mechanism — bookmarks or a CRM would need a check that runs after decoding on every dispatch path unchanged — and both are now needed by two modules (lab's pair, billing's pair and limit). The roles, scopes, limits and pairs stay lims's: the framework runs a declared check, it does not know what the check decides. If (a) or (b) does not hold, take the disposition branch (Step 9) and skip Steps 2–8.

- [ ] **Step 2: Write the spec**

Create `docs/spec/core/action_guards.md`. It states, in present tense and without history (`AGENTS.md`):

- **What a guard is.** A check an application declares per action type, by specialising `morph::model::ActionGuard<Action>` with `static void check(Action const&)`. The framework runs it on every dispatch path, on the decoded action, after the gates `prepareAction` applies (`reconcileDeclaredPrecision`, `enforceQuantityBounds`, `recomputeAll`, `ActionValidator<Action>::ready`) and before `Model::execute`. A guard that throws refuses the action; the exception is the execute's failure and is journaled as `Outcome::Failed` when the action is loggable, exactly as a throw from `execute` is, because a refused attempt is audit-worthy (lims decision 4).
- **Where it runs.** `ActionDispatcher`'s `runHandler` and its Task runner (the server path, so every remote topology); `Bridge`'s `localOp` and `localTaskOp` (the in-process path). `session::current()` is the execute's session on both, so a guard reads the principal the model would. On the server it runs on the model's strand, so it may read the model's store as `execute` would.
- **Why not `IAuthorizer`.** `authorize(ctx, modelType, actionType)` decides before the payload is decoded and is shared by every model; a guard is per action, sees the payload, and runs where the model runs. The authorizer keeps authentication and model-level gates; a guard is what reads the payload.
- **Direct calls.** Calling `model.execute(action)` directly runs no guard. `morph::model::executeChecked(model, action)` runs the validator, the guard and `execute` in that order, for server-side code that runs a model outside a dispatch (a scheduler) and for unit tests of the guard's outcome. `IMPLEMENTATION.md` rule 1's "models must re-check their own authorization" is met by a guard declared beside the action.
- **Enumeration.** `morph::model::actionFacts(modelType)` lists a registered model's actions with whether each is guarded, so an application can prove every mutating action is guarded or deliberately open without listing them by hand.
- **What it does not do.** It does not order guards against each other, compose checks across models, or know roles, scopes, limits or pairs.
- **Its two consumers.** lims's lab (capture/verify pair) and billing (create/approve pair with a value limit), both through one constrained specialisation over `lims::DeclaresRule`.
- **Alternatives considered,** each with why not: a payload-receiving `IAuthorizer` overload (it runs on the server strand before the model's strand, so it cannot read the model's rows in order with the execute); a registration macro with a function pointer (a trait composes with a constrained partial specialisation, which is what lets one lims declaration cover every module's actions); leaving the call in each handler (the measured recurrence).

Add the file to `docs/spec/README.md`'s index, and one paragraph each to `docs/spec/core/registry.md` (the server path) and `docs/spec/core/bridge.md` (the local path) pointing to it.

- [ ] **Step 3: One independent review**

Dispatch one review subagent on the default model (not Fable), giving it `docs/spec/core/action_guards.md`, `include/morph/core/registry.hpp`, `include/morph/core/bridge.hpp`, `include/morph/session/session.hpp`, and lims's `org/include/lims/org/authorization/declared.hpp` and README decisions 25 and 36 — the code and the evidence, not this plan's conclusion. Ask it whether the seam is mechanism, whether the two call sites cover every dispatch path (it should look for a third), whether journaling a refusal is right — lims decision 4 journals refused transitions but not unauthenticated attempts, and the lims guard's `requirePrincipal` would now throw inside the journaled `try` — and what the spec leaves undecided. Address every finding in the spec before Step 4; if it shows the rule is domain logic that cannot be run by the framework without knowing the domain, take the disposition branch instead and record its reasoning there.

- [ ] **Step 4: Write the failing framework test**

Create `tests/test_action_guard.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

// Action guards: a declared per-action check the framework runs on the decoded action, on every dispatch path,
// before Model::execute.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <morph/core/action_guard.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/session/session.hpp>
#include <stdexcept>
#include <string>

#include "test_support.hpp"

namespace guard_test {

struct Withdraw {
    int amount{};
};
struct Deposit {
    int amount{};
};
struct Balance {
    int value{};
};

/// Runs every action it is handed; counts what reached it.
struct Account {
    int executed = 0;
    Balance execute(Withdraw const& action) {
        ++executed;
        return Balance{.value = -action.amount};
    }
    Balance execute(Deposit const& action) {
        ++executed;
        return Balance{.value = action.amount};
    }
};

}  // namespace guard_test

/// A withdrawal above 100 is refused, and only for a principal other than "teller".
template <>
struct morph::model::ActionGuard<guard_test::Withdraw> {
    static void check(guard_test::Withdraw const& action) {
        auto const* ctx = morph::session::current();
        if (action.amount > 100 && (ctx == nullptr || ctx->principal != "teller")) {
            throw std::runtime_error{"withdrawal over the limit"};
        }
    }
};

BRIDGE_REGISTER_MODEL(guard_test::Account, "T_GuardAccount")
BRIDGE_REGISTER_ACTION(guard_test::Account, guard_test::Withdraw, "T_GuardWithdraw")
BRIDGE_REGISTER_ACTION(guard_test::Account, guard_test::Deposit, "T_GuardDeposit")

static_assert(morph::model::Guarded<guard_test::Withdraw>);
static_assert(!morph::model::Guarded<guard_test::Deposit>);

// Mutation: remove the guardAction call from runHandler (the large withdrawal executes on the server path).
TEST_CASE("action guard: the dispatcher runs it on the decoded action before execute", "[guard]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto const server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::exec::MainThreadExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server), owner};
    morph::bridge::BridgeHandler<guard_test::Account> account{bridge, &owner};
    CHECK_THROWS_WITH(morph::testing::await(owner, account.execute(guard_test::Withdraw{.amount = 500})),
                      Catch::Matchers::ContainsSubstring("over the limit"));
    CHECK(morph::testing::await(owner, account.execute(guard_test::Withdraw{.amount = 50})).value == -50);
    CHECK(morph::testing::await(owner, account.execute(guard_test::Deposit{.amount = 500})).value == 500);
}

// Mutation: remove the guardAction call from localOp (the large withdrawal executes on the local path).
TEST_CASE("action guard: the in-process bridge runs it too", "[guard]") {
    morph::exec::MainThreadExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(owner), owner};
    morph::bridge::BridgeHandler<guard_test::Account> account{bridge, &owner};
    CHECK_THROWS_WITH(morph::testing::await(owner, account.execute(guard_test::Withdraw{.amount = 500})),
                      Catch::Matchers::ContainsSubstring("over the limit"));
    morph::session::Context teller;
    teller.principal = "teller";
    bridge.setDefaultSession(teller);
    CHECK(morph::testing::await(owner, account.execute(guard_test::Withdraw{.amount = 500})).value == -500);
}

TEST_CASE("action guard: executeChecked runs it for a direct caller", "[guard]") {
    guard_test::Account account;
    CHECK_THROWS_WITH(morph::model::executeChecked(account, guard_test::Withdraw{.amount = 500}),
                      Catch::Matchers::ContainsSubstring("over the limit"));
    CHECK(account.executed == 0);
    CHECK(morph::model::executeChecked(account, guard_test::Deposit{.amount = 1}).value == 1);
    CHECK(account.executed == 1);
}

TEST_CASE("action guard: a model's actions are listed with whether each is guarded", "[guard]") {
    auto const facts = morph::model::actionFacts("T_GuardAccount");
    REQUIRE(facts.size() == 2U);
    for (auto const& fact : facts) {
        CAPTURE(fact.actionType);
        CHECK(fact.guarded == (fact.actionType == "T_GuardWithdraw"));
    }
}
```

The helpers `morph::testing::await`, `MainThreadExecutor` and the `SimulatedRemoteBackend` construction are the framework suite's (`tests/test_support.hpp`, `tests/test_push_watch.cpp`); read `test_support.hpp` and use its spelling for awaiting a `Completion` on an owner executor where it differs from `await(owner, …)`.

- [ ] **Step 5: Implement the seam**

Create `include/morph/core/action_guard.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <concepts>
#include <string>
#include <string_view>
#include <vector>

#include "registry.hpp"

/// @file
/// Action guards: a check an application declares per action type, run by the framework on the decoded action on
/// every dispatch path, after the action's validator and before `Model::execute`. See
/// `docs/spec/core/action_guards.md`.

namespace morph::model {

/// @brief Customisation point: specialise with `static void check(Action const&)` to guard @p Action.
///
/// `check` throws to refuse the action. It runs on the model's strand with the execute's session current, so it may
/// read the principal and the model's own store as `execute` would.
/// @tparam Action The action type.
template <typename Action>
struct ActionGuard;

/// @brief Whether @p Action declares a guard.
/// @tparam Action The action type.
template <typename Action>
concept Guarded = requires(Action const& action) {
    { ActionGuard<Action>::check(action) } -> std::same_as<void>;
};

namespace detail {

/// @brief Runs @p Action's guard, if it declares one.
/// @tparam Action The action type.
/// @param action The decoded, validated action.
template <typename Action>
void guardAction(Action const& action) {
    if constexpr (Guarded<Action>) {
        ActionGuard<Action>::check(action);
    }
}

}  // namespace detail

/// @brief Runs @p action on @p model as a dispatch would: the validator, the guard, then `execute`.
///
/// For a caller that holds a model directly — server-side code acting outside a dispatch, or a unit test of a
/// guard's outcome. A plain `model.execute(action)` runs neither.
/// @tparam Model The model type.
/// @tparam Action The action type.
/// @param model The model.
/// @param action The action.
/// @return What `execute` returned.
/// @throws ValidationError if the action fails its validator.
/// @throws std::exception whatever the guard or `execute` throws.
template <typename Model, typename Action>
auto executeChecked(Model& model, Action const& action) {
    if (!ActionValidator<Action>::ready(action)) {
        throw ValidationError{ModelTraits<Model>::typeId(), ActionTraits<Action>::typeId()};
    }
    detail::guardAction(action);
    return model.execute(action);
}

/// @brief One registered action of a model, as an application can enumerate it.
struct ActionFact {
    /// @brief The action's registered id.
    std::string actionType;
    /// @brief Whether it declares a guard.
    bool guarded = false;
};

/// @brief The registered actions of @p modelType.
/// @param modelType The model's registered id.
/// @return Its actions, by id; none for an unknown model.
[[nodiscard]] std::vector<ActionFact> actionFacts(std::string_view modelType);

}  // namespace morph::model
```

In `registry.hpp`: give `ActionEntry` a `bool guarded = false;` set in `registerAction<Model, Action>` from `Guarded<Action>` (forward-declare `ActionGuard`/`Guarded` there, or include the guard header's declarations from a small `detail/action_guard_fwd.hpp`, whichever keeps `registry.hpp` free of a cycle); in `runHandler` call `detail::guardAction(action);` as the first statement inside the `try` around `model.execute(action)` (so a refusal is journaled as a failure), and the same in the Task runner; define `actionFacts` over `ActionDispatcher::instance()`'s entries for the model, inline in `action_guard.hpp` (the dispatcher already answers `actionsOf(modelId)`; add `bool guardedAction(modelId, actionId) const`). In `bridge.hpp`'s `localOp` and `localTaskOp`, call `::morph::model::detail::guardAction(actionRef);` right after the `ActionValidator<Action>::ready` check, inside the same `try`. Doxygen every new public symbol (`@tparam`, `@param`, `@return`, `@throws`) and run the docs build from `AGENTS.md`'s CI notes on the changed headers: `cmake -S . -B build -G Ninja -DMORPH_BUILD_DOCUMENTATION=ON -DMORPH_BUILD_TESTS=OFF -DMORPH_BUILD_EXAMPLES=OFF` then `cmake --build build --target doc`, both under the build lock.

- [ ] **Step 6: Run the framework test**

Run: the framework suite commands from "Build and test commands" (`morph_tests`, `"[guard]"`). Expected: the four cases pass. Run the two mutations named in the file and restore each (touch the restored header: a restored file keeps its old mtime and ninja keeps the mutant). Then build and run the whole framework suite once, and a GCC sweep of `registry.hpp`/`bridge.hpp` through `tests/test_action_guard.cpp` (`g++ -std=c++23 -fsyntax-only` with the suite's include flags from `build/linux-everything/compile_commands.json`), since local builds are clang.

- [ ] **Step 7: lims adopts it**

In `declared.hpp`, after `requireAuthorized`, add:

```cpp
}  // namespace lims

/// @brief Every lims action that declares a rule is guarded by it: the framework runs the role check, the scope, the
///        value limit and the separation-of-duties pair on the decoded action, on every dispatch path.
/// @tparam Action A lims action with a declared rule.
template <typename Action>
    requires lims::DeclaresRule<Action>
struct morph::model::ActionGuard<Action> {
    /// @brief Refuses @p action unless its declared rule allows the current principal.
    /// @param action The decoded action.
    static void check(Action const& action) {
        lims::requirePrincipal();
        lims::requireRoleSomewhere<Action>();
        lims::requireAuthorized(action);
    }
};

namespace lims {
```

(include `<morph/core/action_guard.hpp>`). Then:

- Delete every `requireRoleSomewhere<…>()` and `requireAuthorized(action)` call from the lims model sources (`org_model.cpp`, `sample_model.cpp`, `ledger_*.cpp`, `price_list_model.cpp`, `invoice_*.cpp`, `billing_run_model.cpp`, `invoice_edit_model.cpp`). Where a handler looked a row up between the two calls to answer `NotFound`, the guard now runs first: a missing row gives an empty scope and the refusal "does not hold the … role at no company/site". Keep each handler's own `NotFound` after the guard, and change the Part A and Part B cases that expected `NotFound` from a caller who holds the role somewhere but names a missing row to expect `Forbidden` — list each changed case in decision 40, since README decision 23 promised `NotFound` there.
- In every suite, a direct `model.execute(action)` of a declared action whose case checks a refusal or relies on the rule becomes `morph::model::executeChecked(model, action)`. Find them with `grep -rn 'execute(lims::\(VerifyResult\|CreateCompany\|CreateSite\|AssignRole\|RevokeRole\|SetValueLimit\|Create\|Open\|Post\|Reverse\|Close\|Begin\|Set\|Register\|Submit\|Approve\|Return\|Lease\|Start\|Advance\)' examples/lims/tests examples/lims/screens/tests`. `lims::test::grant` uses `OrgModel` directly for the bootstrap; make it use `executeChecked` too, so the bootstrap carve-out is exercised as it is dispatched.
- `BillingScheduler::tick` calls `morph::model::executeChecked(runs, …)`.
- Delete `declaredRequirements()` and `requiredRole()` from `declared_requirements.hpp`, and the role branch from `LimsAuthorizer::authorize` (the guard runs on the server's dispatch path, with the payload; the authorizer authenticates). Keep `openMutatingActions()`.
- Rewrite the coverage case in `test_declared_authorization.cpp` over `morph::model::actionFacts` for every lims model: each action with an effect other than `None` is guarded or in `openMutatingActions()`, never both, and every id in `openMutatingActions()` is a registered action. The `Mutating` tuple goes.
- In `examples/IMPLEMENTATION.md` rule 1, after "Models must re-check their own preconditions and authorization (`Context::principal`) — the schema's `required` and the client gates are UX, not security.", add: "An authorization rule that reads the payload is declared as a `morph::model::ActionGuard` beside the action, which the framework runs on every dispatch path; a direct caller uses `morph::model::executeChecked`."

- [ ] **Step 8: Run everything**

Run: Configure, Build lims, Everything labelled for the rung, the Screen suite, and the framework suite. Expected: all pass, the Hypothesis 6 cases of both instances included (the edge no longer allows-then-the-model-refuses: the guard refuses on the server path; change those two cases to assert the refusal reaches the client in every mode, and that `LimsAuthorizer::authorize` alone no longer gates by role). Run the mutation "remove `lims::requireAuthorized(action)` from the lims `ActionGuard`" once and see the verification, approval and value-limit cases fail across lab, org, ledger and billing; restore it.

- [ ] **Step 9: Record decision 40 (either branch) and commit**

Seam branch:

```markdown
### 40. Declared authorization is a framework action guard (hypothesis 6, resolved)

`morph::model::ActionGuard<Action>` (`docs/spec/core/action_guards.md`) is a check the framework runs on the decoded
action on every dispatch path, after its validator and before `execute`. lims declares one constrained guard for every
action with a `lims::Authorization` rule, so lab's capture/verify pair, billing's create/approve pair and value limit,
and every scoped role are checked once, by the framework, for every module; no handler calls the check, and the edge
no longer keeps a table of roles. The rules themselves stay lims's. A caller that names a row that does not exist, and
holds the role somewhere, is now refused in words rather than told the row is missing, because the guard runs before
the handler looks the row up; the cases that changed are [list]. Coverage of "every mutating action is guarded or
deliberately open" is proved by enumerating `morph::model::actionFacts`, not by a list kept by hand. **Hypothesis 6
is resolved by the seam:** a declared rule that sees the payload is checked once by the framework for every module.
```

and change the last paragraph of decision 25 to: "This was instance 1 of 2; with billing's pair and limit as the second, the check became the framework's action guard (decision 40)."

Disposition branch:

```markdown
### 40. Declared authorization stays domain logic each model calls (hypothesis 6, resolved)

[The reasons from Step 1 or the review: which of the two conditions did not hold, or why a framework check cannot run
it without knowing the domain.] The rules, the scope lookup and the call in each handler stay lims's; the edge keeps
its table. **Hypothesis 6 is resolved as domain logic the models must own.**
```

```bash
cd /home/yaraslau/repo/morph
git add docs/spec include/morph tests examples/IMPLEMENTATION.md examples/lims
git commit -m "Run declared authorization as a framework action guard on every dispatch path

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

(For the disposition branch, commit only the README with "Record lims declared authorization as domain logic".) If the seam changed `include/morph`, file nothing: a seam built in the same branch is not a defect. If a defect in `include/morph` turned up on the way (a third dispatch path the guard missed, say), file it per `AGENTS.md` with its real output.

---

### Task 17: Part B's definition of done, measured

**Files:**
- Modify: `examples/lims/README.md` (Status, "What to implement", Modules), `codecov.yml` (the lims component's paths and target), `examples/LADDER.md` (lims's size, if it states one)

**Interfaces:**
- Consumes: everything above. Produces: nothing new in code.

- [ ] **Step 1: Full rung run and the boundary**

Run:
```
flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --preset linux-everything
flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --build build/linux-everything --target ladder_lims_tests ladder_lims_screen_tests ladder_lims_render_smoke ladder_lims_server morph_examples_client -j10
flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock ctest --test-dir build/linux-everything -j6 -L ladder-lims --output-on-failure
bash scripts/check_ctest_name_collisions.sh build/linux-everything
```
Expected: `100% tests passed`, the eight `lims.module_boundary.*` tests among them; no duplicate test names.

- [ ] **Step 2: Every hypothesis Part B touched has a recorded outcome**

Run: `grep -n '^### \(2[89]\|3[0-9]\|40\)\.' examples/lims/README.md` and `grep -n 'Hypothesis [0-9] is \|hypothesis [0-9].*\(confirmed\|refuted\|resolved\)' examples/lims/README.md`. Expected: decisions 28–40, and an outcome sentence for hypotheses 1 (38), 2 (31), 3 (37), 4 (34, 35), 5 (35), 6 (36, 40), 7 (39) and 8 (29, 33), plus decision 32's verdict on the cross-module effects finding and, if confirmed, the issue's number in the session's notes (not in the README: `AGENTS.md`). Every `[measured]` slot in those decisions is filled with the number its step printed.

- [ ] **Step 3: Every module follows the rules**

Run each check and read its output:
```
cd /home/yaraslau/repo/morph
grep -rn 'sqlite3_\|"SELECT \|"INSERT \|"UPDATE \|"DELETE ' examples/lims/{ledger,billing,posting}/ examples/lims/lab/src/events/
grep -rnE '^\s+(int|bool|double|float|std::int64_t|std::uint64_t) [a-z][A-Za-z]*( =|\{|;)' examples/lims/{ledger,billing,posting}/include/lims/*/dto/ examples/lims/posting/include
grep -rn 'BRIDGE_REGISTER_ACTION' examples/lims/{ledger,billing}/include | wc -l
grep -rn 'BRIDGE_REGISTER_EFFECTS' examples/lims/{ledger,billing}/include | wc -l
```
Expected: no raw SQL and no `sqlite3_` call (rule 4; a sanctioned raw statement, if Task 8 needed one, has its README finding line); no plain numeric or `bool` DTO field (rule 3); and every mutating action registered with effects (the coverage case in `test_declared_authorization.cpp` is the proof; the counts are for the size table).

- [ ] **Step 4: Size**

Count models, actions and screens as the design's size table does (registered models and actions, excluding tests):
```
cd /home/yaraslau/repo/morph
grep -rhn 'BRIDGE_REGISTER_MODEL(lims::' examples/lims/*/include | wc -l
grep -rhn 'BRIDGE_REGISTER_ACTION(lims::' examples/lims/*/include | wc -l
grep -c 'ui::screen(' examples/lims/screens/*.cpp
```
Record the three numbers in the README's Status paragraph beside the design's estimate (~16 models and ~115 actions after all four parts).

- [ ] **Step 5: Measure model coverage**

Follow the memory note `coverage-local-repro.md` (`scripts/coverage.sh` over the `clang-coverage` preset), every build wrapped in the lock at `-j10`, and read the `lims` component's covered/total from `llvm-cov export`'s JSON for `examples/lims/{lab,org,ledger,billing}/src/models/**` and `examples/lims/{lab,org,ledger,billing}/include/lims/*/models/**`. Add the ledger and billing paths to `codecov.yml`'s `lims` component (`- examples/lims/ledger/src/models/**`, `- examples/lims/ledger/include/lims/ledger/models/**`, and billing's two). Expected: at or above README decision 20's 98.96%; if below, every uncovered line is tested now or listed, in decision 20's style, with the reason it cannot be reached, and `codecov.yml`'s `target` sits a margin below the measured ceiling.

- [ ] **Step 6: Update the README and commit**

In `examples/lims/README.md`: the "What to implement" model sentence names `LedgerModel` and `LedgerReportModel` in ledger, `PriceListModel`, `InvoiceModel`, `InvoiceEditModel` and `BillingRunModel` in billing; the Modules table has the `posting`, `ledger` and `billing` rows and `app`'s row reads "the server edge's `LimsAuthorizer`, the event relay, the ledger port and the billing schedule"; and the Status paragraph adds: "Part B of the ERP design is built: the posting contract, the ledger (accounts, periods, entries, rates, reports) and billing (prices, billable lines from lab's events, invoices with approval, the edit lease, posting, the billing run) (decisions 28–40). Models [n], actions [n], screens [n]." Then:

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/README.md codecov.yml examples/LADDER.md
git commit -m "Record lims ERP Part B as built, with its measured coverage and size

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

## What Part B hands to Parts C and D

Not tasks; what the next plans start from.

- **Part C's second instances** meet the first instances recorded here: goods receipt (hypothesis 1, decision 38 — the framework seam for atomic work across models is designed against both postings before goods receipt is called done), `lab.ResultCaptured` → consumption (hypothesis 2, decision 31 — if inventory needs the same `IActionLog` adapter and its own `IReplayLedger`, that is the seam), lot reservation (hypothesis 3, decision 37), requisition approval and escalation (hypotheses 5 and 4, decision 35), purchase orders in a supplier's currency (hypothesis 8, decision 33), stock effects across companies (hypothesis 7, decision 39).
- **The cross-module effects gap** (decision 32) recurs a third time in Task 14 (`PostInvoice` cannot declare the ledger's `Ledger_Entry`); it is filed against `include/morph` and every Part C reaction will meet it.
- **Part D** builds the billing and ledger screens against their mockups; the lifecycle screen's billing panel (Task 9) is a functional panel, rebuilt there. A JPY price's form advertising two decimals (decision 33) and a pending-approvals list not told when a deadline passes (decision 35) are Part D's to meet.

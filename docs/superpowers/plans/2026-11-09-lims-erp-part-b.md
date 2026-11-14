# lims ERP Part B: Ledger and Billing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give lims a double-entry ledger and the billing that feeds it — Part B of `docs/superpowers/specs/2026-11-09-lims-erp-design.md` — and, on the way, build the first instance of hypotheses 1, 2, 3, 4, 5, 7 and 8 and the second instance of hypothesis 6 with morph as it stands, each with a test that measures the outcome and a README resolved decision that records it. Approval escalation, which the design listed as hypothesis 4's second instance, is measured and recorded as mis-framed (Task 12).

**Architecture:** Two new feature modules and one new shared module join `org`, `lab` and `app`: `posting` (shared; the `PostingRequest`/`PostingResult` contract only), `ledger` (accounts, periods, journal entries, exchange rates, reports; knows nothing of invoices) and `billing` (settings, clients, price lists, billable lines, invoices, the edit lease, the billing run; builds a `PostingRequest` and hands it to a port the app installs). `lab` writes an outbox row in `PublishSample`'s transaction; the app relays it with `morph::journal::OutboxRelay` into billing's idempotent consumer action. Every cross-module call — the event, the billing schedule, the dated rate, posting — is wired in `app`, the only module that sees every other, and runs as a dispatch: an internal client (`lims::app::InternalClient`, a `Bridge` over `SimulatedRemoteBackend` into the server's own `RemoteServer`) under a service principal, so it is authorized, journaled and fanned out like a user's call. Posting is measured two ways (Task 14): the ledger half dispatched, and the ledger half sharing billing's transaction. Money is `morph::units::Quantity<ErpUnit::money, 2>` over `morph::math::Rational`, with the currency a `CurrencyCode` field beside it; posters round half-up per line and add an explicit rounding line; the ledger validates and never rounds.

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
- **Out of scope (design):** "PDF and print-file rendering; a tax engine and statutory reports; payments, credit notes and supplier invoices; … bank and payment integration; electronic invoicing". Part D's screens are not built here; Task 12's measurement mounts a one-screen bundle of its own, so no lims screen changes.
- **Hypotheses Part B measures (design table, verbatim columns "Hypothesis" / "Refuted by"):**
  1. "**No atomic work across models.** Each `execute` takes its own pooled connection and commits alone, models are not ordered against each other, and an awaiting handler holds its own model's gate; there is no way to commit billing's state change and ledger's entry together. Gap-free numbering is part of it: a number assigned in billing's transaction is gap-free only if the entry commits with it." / "Both postings expressed with existing seams, and a failure injected between the halves leaves no partial state and no hole in either series". Part B builds instance 1 (invoice posting).
  2. "**Delivering events to a model consumer needs hand-rolled code per consumer.** `OutboxRelay` delivers at least once to an `IActionLog` sink, and `IReplayLedger` dedups by op id; neither delivers to another model." / "Both consumers receive each event exactly once in effect, across a restart, with only wiring code in `lims/app` and no per-consumer delivery loop or cursor". Instance 1: analysis published → billable line.
  3. "**No lease that reaches a model.** `IEnvelopeService::connectionClosed` and shared-instance attach counts exist, but no hook tells a model that the connection holding its lease closed." / "Both leases lapse on disconnect through an existing hook, without a timer of the model's own". Instance 1: draft invoice edit lease.
  4. "**No periodic trigger and no resume after restart.** … `TimeoutScheduler` is a detail type; nothing starts work on a schedule or resumes it after a restart." / "The billing run scheduled and resumable with public primitives and no per-module scheduler". Instance: the billing run. Approval escalation, the design's second candidate, is a deadline the approval rule reads, so nothing is scheduled for it; Task 12 measures that, records hypothesis 4 as mis-framed for escalation, and the design's row says so.
  5. "**No long-running approval state.** `forms::flows` are wizards inside one session; approvals last days, across principals, with limits, deadlines and escalation." / "Both lifecycles expressed as ordinary actions sharing no code but the declared authorization". Instance 1: invoice approval.
  6. "**Authorization cannot see the payload.** `IAuthorizer::authorize(ctx, modelType, actionType)` decides before the payload is decoded, so a scope, a value limit or a separation-of-duties rule can only be checked inside each model, by hand." / "A declared form of rule checked once by the framework for every module, or shown to be domain logic the models must own". Part A built instance 1 (README decision 25); Part B builds instance 2 (billing's create/approve pair and the approver's value limit) and then designs the seam or dispositions it (Tasks 17–19).
  7. "**Effects leak at row and company granularity.** `fanOut` re-authorizes each watcher, but at model and action level only, and `record::privateKeys` hides keys from non-writers; a principal of company A can still learn that company B's record changed." / "A company-B change reaching no company-A connection, in a two-company remote test". Instance 1: invoices.
  8. "**Money at ERP magnitude and with runtime currency.** `Rational` saturates and logs unless `checked*` is used; a `Quantity` unit is a compile-time tag, and `UnitTraits::convert` takes no date, so a currency known only from the record and a dated rate do not fit `Quantity`." / "A trial balance over 10⁷ lines of up to 10⁹ minor units each (totals within int64) summing exactly, with an over-range sum refused rather than saturated; and runtime currencies handled with `Quantity` as it stands". Instance 1: trial balance; the invoice half of instance 2 (orders are Part C).
- **How a hypothesis task is built (user, for this plan):** each hypothesis task builds its first instance in lims with existing morph seams, plus a test that measures the outcome (confirmed / refuted / mis-framed), and records it as a README resolved decision. A framework seam is designed only when a second instance needs the same hand-rolled guarantee. A measuring test asserts the behaviour it measured; if the assertion that pins the outcome fails, the outcome is the other one — change the assertion to what was measured and use the README text for that outcome, never weaken the test until it passes.
- **No cross-module work bypasses dispatch (user, for this plan; `examples/LADDER.md`, "Background jobs"):** the event consumer, the billing schedule and the ledger half of posting run as internal clients — a `Bridge` over `SimulatedRemoteBackend{server}` into the same `RemoteServer` a socket client reaches, under a service principal in the reserved `system:` namespace with a token the server mints — so each call is authorized, journaled and fanned out like a user's. Nothing outside the test support uses `morph::session::detail::ScopedContext`; bookmarks' and bank's servers decline it for the same reason. Where an internal client genuinely cannot do a job, the task records exactly why and files the missing seam against `include/morph` (Task 11 Step 9).
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

A Catch2 tag or name after the binary narrows a run, e.g. `… ladder_lims_tests "[ledger]"`. Task 18, which changes `include/morph`, also builds and runs the framework suite: `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --build build/linux-everything --target morph_tests -j10` and `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock timeout 300 build/linux-everything/tests/morph_tests "[guard]"` (the target and path are the framework suite's in `tests/CMakeLists.txt`; if they differ there, use what that file names).

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
- **Tests** are `examples/lims/tests/test_<topic>.cpp`, tagged `[lims][<topic>]`, start with `DbFixture fixture;`, and set principals with `lims::test::ScopedPrincipal` and roles with `lims::test::grant` (`tests/lims_test_support.hpp`). A test that needs a fixed instant uses `morph::ladder::ScopedClockOverride` (`examples/common/clock.hpp`). A test that needs a server uses `morph::ladder::testkit::BackendRig` in `Mode::Socket`: `rig.server()` for an internal client, `rig.connect()` for a connection opened on the server in process that registers, executes, watches and closes (Task 9 extends the rig). `lims::test::relayLabEvents()` (Task 9) runs one relay pass through an internal client, as the server does.

## File structure after Part B

```
examples/lims/
  CMakeLists.txt                     MODULES org posting lab ledger billing app; SHARED_MODULES org posting;
                                     USES lab:org ledger:org,posting billing:org,posting; probes
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
  app/include/lims/app/              declared_requirements.hpp, lims_authorizer.hpp, service_principals.hpp, internal_client.hpp,
                                     event_wiring.hpp, ledger_wiring.hpp, billing_scheduler.hpp
  app/src/                           internal_client.cpp, event_wiring.cpp, ledger_wiring.cpp, billing_scheduler.cpp
  src/server/main.cpp                + three internal clients, the ledger port, and the event relay and billing schedule on
                                       QTimers that post to one background thread
  tests/                             test_money, test_posting_contract, test_value_limits, test_ledger_chart, test_ledger_entries,
                                     test_ledger_reports, test_price_lists, test_domain_events, test_invoices, test_billing_run,
                                     test_invoice_approval, test_invoice_lease, test_invoice_posting, test_cross_module_effects,
                                     test_two_company_effects
  tests/compile_checks/              + posting_includes_lab, lab_includes_posting, app_includes_posting, ledger_includes_lab,
                                       billing_includes_ledger, ledger_includes_billing, cross_module_effects_probe
  screens/tests/                     + test_lims_approval_inbox.cpp (a one-screen bundle of its own; no lims screen changes)
examples/common/testkit/             backend_rig.hpp: rig.server(), rig.connect(); server_connection.hpp
cmake/morph_add_rung.cmake           USES <module>:<shared>,…; the `stress` label for "Stress: " cases
```

Module dependency after Part B: `org` → nothing; `posting` → `org`; `lab` → `org`; `ledger`, `billing` → `org`, `posting`; `app` → every module.

---

### Task 1: Money and the ids other modules refer to

**Files:**
- Create: `examples/lims/org/include/lims/org/core/money.hpp`, `examples/lims/org/include/lims/org/core/references.hpp`, `examples/lims/tests/test_money.cpp`
- Modify: `examples/lims/org/include/lims/org/core/errors.hpp` (add `OutOfRange`), `examples/lims/lab/include/lims/lab/core/types.hpp` (drop the four moved ids), `examples/IMPLEMENTATION.md` (the stale floor), `examples/lims/README.md` (decision 28)

**Interfaces:**
- Consumes: `lims::CurrencyCode` (`org/core/types.hpp`), `morph::units::Quantity`, `morph::units::UnitTraits`, `morph::math::checkedRoundToDecimalPlaces`, `morph::math::checkedAdd`, `morph::math::RoundingMode::HalfAwayFromZero`.
- Produces: `enum class lims::ErpUnit { money, rate, count }`; `lims::Amount = Quantity<ErpUnit::money, 2>`; `lims::Rate = Quantity<ErpUnit::rate, 6>`; `lims::Count = Quantity<ErpUnit::count, 0>`; `constexpr std::uint32_t lims::decimalsOf(CurrencyCode)`; `constexpr std::string_view lims::currencyName(CurrencyCode)`; `bool lims::fitsCurrency(Rational const&, CurrencyCode)`; `Rational lims::roundToCurrency(Rational const&, CurrencyCode)` (throws `OutOfRange`); `class lims::ExactSum { bool add(Rational const&); Rational total() const; }`; `class lims::OutOfRange : LimsError`; `lims::ClientId`, `lims::SampleId`, `lims::AnalysisId`, `lims::ResultId` now in `lims/org/core/references.hpp`, with their wire and shape tags unchanged.

Billing names a lab client, sample, result and analysis; it cannot include lab. The four ids move to org, unchanged in name, wire form and shape tag, so every journal entry that carries one still decodes.

- [ ] **Step 0: Record Part B's base commit**

Task 12 measures what Part B added since it began, so the start is written down once, before the first change:

```bash
cd /home/yaraslau/repo/morph
git rev-parse HEAD > /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/plan_b_base.txt
```

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
- Create: `examples/lims/posting/include/lims/posting/posting.hpp`, `examples/lims/tests/test_posting_contract.cpp`, `examples/lims/tests/compile_checks/posting_includes_lab.cpp`, `examples/lims/tests/compile_checks/lab_includes_posting.cpp`, `examples/lims/tests/compile_checks/app_includes_posting.cpp`
- Modify: `cmake/morph_add_rung.cmake` (`USES`), `examples/lims/CMakeLists.txt`, `examples/lims/README.md` (Modules table)

**Interfaces:**
- Consumes: `lims::Amount`, `lims::Rate`, `lims::CompanyId`, `lims::CurrencyCode`, `morph::time::Timestamp`.
- Produces: `lims::JournalEntryId`; `enum class lims::PostingSide { Debit, Credit }`; `struct lims::PostingLine { std::string accountCode; PostingSide side; Amount amount; Amount companyAmount; }`; `struct lims::PostingRequest { CompanyId company; Timestamp date; std::string sourceKey; std::string sourceReference; CurrencyCode currency; Rate rate; std::vector<PostingLine> lines; }`; `struct lims::PostingResult { JournalEntryId entryId; }`; the CMake target `ladder_lims_posting` (INTERFACE); `morph_add_rung(... USES <module>:<shared>[,<shared>...])`.

A feature module links the shared modules it declares it uses, and no other: lab posts nothing and records no posting, so it does not link `posting` (the design's dependency rule: "links `lims/org` and, where it posts or records postings, `lims/posting`"). `USES` is how a rung says which, per module; without it every feature module links every shared module, as before, so no other rung changes.

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

- [ ] **Step 4: Teach `morph_add_rung` per-module uses, add the module, and probe its boundary**

In `cmake/morph_add_rung.cmake`, change the link-rule paragraph of the header comment to:

```cmake
# Link rules, which make including another module's header a compile error:
#   - a shared module links the shared modules named before it in SHARED_MODULES, and nothing else of the rung;
#   - APP_MODULE links every other module;
#   - every other module links the shared modules USES names for it (USES <module>:<shared>[,<shared>...]), or,
#     when the rung gives no USES, every shared module. Once USES is given it must name every such module, so a
#     module added later cannot fall back to linking everything unseen.
```

add `USES` to `multiValueArgs` (`set(multiValueArgs MODULES SHARED_MODULES USES)`), and, inside `if(RUNG_MODULES)` after the loop that checks each shared module is in `MODULES`:

```cmake
        foreach(_use IN LISTS RUNG_USES)
            if(NOT _use MATCHES "^([A-Za-z0-9_]+):([A-Za-z0-9_,]+)$")
                message(FATAL_ERROR "morph_add_rung(NAME ${RUNG_NAME}): USES entry '${_use}' is not "
                                    "<module>:<shared>[,<shared>...]")
            endif()
            set(_user "${CMAKE_MATCH_1}")
            string(REPLACE "," ";" _used "${CMAKE_MATCH_2}")
            if(NOT _user IN_LIST RUNG_MODULES OR _user IN_LIST RUNG_SHARED_MODULES OR _user STREQUAL RUNG_APP_MODULE)
                message(FATAL_ERROR "morph_add_rung(NAME ${RUNG_NAME}): USES names ${_user}, which is not a feature "
                                    "module of MODULES")
            endif()
            if(DEFINED _uses_${_user})
                message(FATAL_ERROR "morph_add_rung(NAME ${RUNG_NAME}): USES names ${_user} twice")
            endif()
            foreach(_shared IN LISTS _used)
                if(NOT _shared IN_LIST RUNG_SHARED_MODULES)
                    message(FATAL_ERROR "morph_add_rung(NAME ${RUNG_NAME}): ${_user} uses ${_shared}, which is not in "
                                        "SHARED_MODULES")
                endif()
            endforeach()
            set(_uses_${_user} ${_used})
        endforeach()
        if(RUNG_USES)
            foreach(_module IN LISTS RUNG_MODULES)
                if(NOT _module IN_LIST RUNG_SHARED_MODULES AND NOT _module STREQUAL RUNG_APP_MODULE
                   AND NOT DEFINED _uses_${_module})
                    message(FATAL_ERROR "morph_add_rung(NAME ${RUNG_NAME}): USES is given but names nothing for "
                                        "${_module}")
                endif()
            endforeach()
        endif()
```

Change `elseif(RUNG_SHARED_MODULES OR RUNG_APP_MODULE)` to `elseif(RUNG_SHARED_MODULES OR RUNG_APP_MODULE OR RUNG_USES)` and its message to "SHARED_MODULES, USES and APP_MODULE need MODULES". In the per-module link loop, replace the final `else()` branch with:

```cmake
            elseif(RUNG_USES)
                set(_deps ${_uses_${_module}})
            else()
                set(_deps ${RUNG_SHARED_MODULES})
            endif()
```

In `examples/lims/CMakeLists.txt`, change the call to:

```cmake
morph_add_rung(NAME lims
    MODULES org posting lab app
    SHARED_MODULES org posting
    USES lab:org
    APP_MODULE app)
```

and, below the three existing `_lims_boundary_probe` lines, add:

```cmake
    _lims_boundary_probe(posting_includes_lab posting fail lims/lab/models/sample_model.hpp)
    _lims_boundary_probe(lab_includes_posting lab fail lims/posting/posting.hpp)
    _lims_boundary_probe(app_includes_posting app pass lims/posting/posting.hpp)
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

`examples/lims/tests/compile_checks/lab_includes_posting.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Compiled as part of lims/lab, it includes the posting contract and must fail to build: lab posts nothing, so its
// USES names org alone and posting is not on its include path. Outside its test the macro is undefined and this is
// empty.
#ifdef LIMS_MODULE_BOUNDARY_PROBE
#include <lims/posting/posting.hpp>
#endif
```

and `examples/lims/tests/compile_checks/app_includes_posting.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The control for lab_includes_posting: compiled as part of lims/app, which links every module, it includes the
// posting contract and must build, which shows the probe's header path is right and the refusal above is the link
// rule's. Outside its test the macro is undefined and this is empty.
#ifdef LIMS_MODULE_BOUNDARY_PROBE
#include <lims/posting/posting.hpp>
#endif
```

- [ ] **Step 5: Build and run**

Run: Configure, Build lims, Model suite `"[posting]"`, then Everything labelled for the rung. Expected: the two posting cases pass; `lims.module_boundary.posting_includes_lab`, `lims.module_boundary.lab_includes_posting` and `lims.module_boundary.app_includes_posting` pass; nothing else changes. Control the new rule once: change `USES lab:org` to `USES lab:org,posting`, reconfigure, and see `lims.module_boundary.lab_includes_posting` fail (the header now builds); restore it. Then misspell it `USES lab:orgg` and see the configure stop with "lab uses orgg, which is not in SHARED_MODULES"; restore it.

- [ ] **Step 6: Update the Modules table and commit**

In `examples/lims/README.md`'s "Modules" table add the row `` | `posting/` (shared) | the posting contract: `PostingRequest` and `PostingResult`, DTOs only | `org` | `` after `org/`, and leave `lab/`'s "Links" cell `` `org` ``.

```bash
cd /home/yaraslau/repo/morph
git add cmake/morph_add_rung.cmake examples/lims/posting examples/lims/CMakeLists.txt \
        examples/lims/tests/compile_checks/posting_includes_lab.cpp examples/lims/tests/compile_checks/lab_includes_posting.cpp \
        examples/lims/tests/compile_checks/app_includes_posting.cpp examples/lims/tests/test_posting_contract.cpp \
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

In `examples/lims/CMakeLists.txt` set `MODULES org posting lab ledger app` and `USES lab:org ledger:org,posting`, and add `_lims_boundary_probe(ledger_includes_lab ledger fail lims/lab/models/sample_model.hpp)`. Create `tests/compile_checks/ledger_includes_lab.cpp` in `posting_includes_lab.cpp`'s shape, its comment reading "Compiled as part of lims/ledger, it includes lab's header and must fail to build: the ledger knows no other feature module." In `declared_requirements.hpp` raise the array by three and add `requirementOf<CreateAccount>()`, `requirementOf<OpenPeriod>()`, `requirementOf<BeginClosePeriod>()` (include `lims/ledger/authorization/ledger_authorization.hpp`). In the coverage case add the three actions to `Mutating` and `"LedgerModel"` to the model list the dispatcher check walks. Add a `ledger/` row to the README's Modules table: `` | `ledger/` | accounts, periods, journal entries, exchange rates and the reports | `org`, `posting` | ``.

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

### Task 5: Ledger — journal entries, balancing and reversal

**Files:**
- Create: `examples/lims/ledger/include/lims/ledger/core/balance.hpp`, `examples/lims/ledger/src/core/balance.cpp`, `examples/lims/ledger/include/lims/ledger/dto/entry_dto.hpp`, `examples/lims/ledger/src/models/ledger_entries.cpp`, `examples/lims/tests/test_ledger_entries.cpp`
- Modify: `ledger_entity.hpp` (two records), `ledger_schema.cpp` (migration), `ledger_model.hpp` (five actions), `ledger_authorization.hpp`/`.cpp`, `declared_requirements.hpp`, `test_declared_authorization.cpp`

**Interfaces:**
- Consumes: `PostingLine`, `ExactSum`, `fitsCurrency`, `calendarMonthOf`, `monthStartMillis`.
- Produces: `std::optional<std::string> lims::entryProblem(std::span<PostingLine const> lines, CurrencyCode transaction, CurrencyCode company)`; `struct JournalEntryView { JournalEntryId id; CompanyId company; PeriodId period; Timestamp date; EntryState state; std::string sourceKey; std::string sourceReference; std::string memo; CurrencyCode currency; Rate rate; JournalEntryId reverses; JournalEntryId reversedBy; std::vector<PostingLine> lines; std::string createdBy; std::string postedBy; }`; actions `CreateDraftEntry { CompanyId company; Timestamp date; std::string memo; CurrencyCode currency; Rate rate; std::vector<PostingLine> lines; }`, `PostEntry { JournalEntryId entryId; }`, `ReverseEntry { JournalEntryId entryId; Timestamp date; }`, `GetEntry { JournalEntryId entryId; }`, `ListEntries { CompanyId company; }` → `ListEntriesResult`; record `Ledger_Entry`; in `ledger_entries.cpp`'s anonymous namespace the helpers Task 6 builds on (`loadEntry`, `toView`, `periodOf`, `requireRecordable`, `accountIds`, `writeEntry`, `headerFor`, `requireBalanced`, `companyCurrency`, `amountOf`).

An entry carries one transaction currency: every line's `amount` is in it, and `entryProblem` balances it and the company currency. That is narrower than the design's "sum to zero per transaction currency", which allows an entry mixing several; Task 7's decision 29 records why.

- [ ] **Step 1: Write the failing test**

Create `examples/lims/tests/test_ledger_entries.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Journal entries: balanced per currency and in company currency, never rounded by the ledger, immutable once posted,
// corrected by reversal.
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

```

Append to `ledger_schema.cpp`'s NOLINT block the migration below. It also creates the two tables Task 6 maps (`ledger_posting_sources`, `ledger_period_balances`), so Part B's migration ids stay in task order; Task 6 adds their records.

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

with `Authorization<PostEntry>::scopeOf` and `Authorization<ReverseEntry>::scopeOf` returning `entryCompany(action.entryId)`. Add the three to `declaredRequirements()` and the coverage tuple.

- [ ] **Step 6: Implement entries**

Create `examples/lims/ledger/src/models/ledger_entries.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <algorithm>
#include <cstdint>
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

}  // namespace lims
```

- [ ] **Step 7: Build and run**

Run: Build lims, Model suite `"[ledger]"`, Model suite `"[authorization]"`. Expected: all pass. Run the two mutations named in `test_ledger_entries.cpp` and restore each.

- [ ] **Step 8: Commit**

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/ledger examples/lims/tests examples/lims/app/include/lims/app/declared_requirements.hpp
git commit -m "Add lims journal entries: balanced, unrounded, immutable once posted, corrected by reversal

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---
### Task 6: Ledger — recording a posting once per source key, and closing a period

**Files:**
- Modify: `ledger_entity.hpp` (`PostingSourceRecord`, `PeriodBalanceRecord`), `entry_dto.hpp` (`ClosePeriod`), `ledger_model.hpp` (`ClosePeriod`, `record`, `rateOn`), `ledger_entries.cpp`, `ledger_authorization.hpp`/`.cpp`, `declared_requirements.hpp`, `test_declared_authorization.cpp`, `examples/lims/tests/test_ledger_entries.cpp`

**Interfaces:**
- Consumes: `PostingRequest`, `PostingResult`, `morph::offline::IReplayLedger`, Task 5's helpers in `ledger_entries.cpp`.
- Produces: `ClosePeriod { PeriodId periodId; }` → `PeriodView`; `static PostingResult LedgerModel::record(PostingRequest const& request)` (not an action); `static std::optional<Rational> LedgerModel::rateOn(CompanyId, CurrencyCode, CurrencyCode, Timestamp const&)` (declared here, stubbed until Task 7).

`record()` is not a registered action in this task: it is the ledger's whole half of a posting — validation, the rate check and the write — in a transaction of its own. How a poster's request reaches it is what Task 14 measures: as `RecordPosting`, an action only a service principal holding the service role at the company may send, and through a poster's own transaction. Either way the ledger records each `sourceKey` once per company through an `IReplayLedger` over `ledger_posting_sources`, written in the entry's own transaction, so a poster that retries gets the first entry back.

- [ ] **Step 1: Write the failing test**

Append to `examples/lims/tests/test_ledger_entries.cpp`:

```cpp
namespace {

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

Run: Build lims. Expected: compile error, `lims::LedgerModel::record` and `lims::ClosePeriod` are not declared.

- [ ] **Step 3: DTO and records**

In `entry_dto.hpp`, before the closing `}  // namespace lims`, add:

```cpp
/// @brief `Closing → Closed`: freezes the period's balances. Refused while a draft remains in it.
struct ClosePeriod {
    /// @brief The period.
    PeriodId periodId;

    /// @brief Whether a period is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return periodId.hasValue(); }
};
```

Append to `ledger_entity.hpp`:

```cpp
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

- [ ] **Step 4: Declare the model's new surface and rules**

In `ledger_model.hpp` add (Doxygen as in Task 5: `@param action`, `@return`, `NotFound` for a missing period, `IllegalTransition` for a period not closing, `Conflict` while a draft remains, `Forbidden`, `EmptyPrincipalError`):

```cpp
    PeriodView execute(const ClosePeriod& action);

    /// @brief Records a poster's request as a posted entry, once per company and source key.
    ///
    /// Not itself an action: postings reach it only from a module's document, through the app's wiring. Checks
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
BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::ClosePeriod, "ClosePeriod")
BRIDGE_REGISTER_EFFECTS(lims::ClosePeriod, ::morph::effects::updates<lims::PeriodView>)
```

In `ledger_authorization.hpp`:

```cpp
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

with `Authorization<ClosePeriod>::scopeOf` returning `periodCompany(action.periodId)`. Add it to `declaredRequirements()` and the coverage tuple.

- [ ] **Step 5: Implement recording and closing**

In `ledger_entries.cpp` add `#include <map>` and `#include <morph/offline/replay_ledger.hpp>`, add `PostingSources` to the anonymous namespace after `kMaxCurrencyCode`:

```cpp
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

```

and append, before the file's closing `}  // namespace lims`:

```cpp
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

```

`record` calls `rateOn`, the dated rate Task 7 stores. Declare it in `ledger_model.hpp` now, with the Doxygen Task 7 Step 4 gives it, and define it for this task at the end of `ledger_entries.cpp` as the ledger with no rates yet answers:

```cpp
std::optional<::morph::math::Rational> LedgerModel::rateOn(CompanyId /*company*/, CurrencyCode /*from*/,
                                                           CurrencyCode /*to*/, ::morph::time::Timestamp const& /*on*/) {
    return std::nullopt;
}
```

Task 7 deletes this definition and adds the real one beside the rate table, so a foreign-currency posting is refused until then; this task's tests post in the company's own currency only.

The `"posting"` author on a recorded entry names the poster's channel; the person who caused the posting is the poster's own record (the invoice's `postedBy`, Task 14).

- [ ] **Step 6: Build and run**

Run: Build lims, Model suite `"[ledger]"`, Model suite `"[authorization]"`. Expected: all pass. Run the two mutations this task added to `test_ledger_entries.cpp` and restore each.

- [ ] **Step 7: Commit**

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/ledger examples/lims/tests examples/lims/app/include/lims/app/declared_requirements.hpp
git commit -m "Record lims postings once per source key and close ledger periods without drafts

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---
### Task 7: Ledger — exchange rates, the reports, and money at ERP magnitude (hypothesis 8, first instance)

**Files:**
- Create: `examples/lims/ledger/include/lims/ledger/dto/rate_dto.hpp`, `examples/lims/ledger/include/lims/ledger/dto/report_dto.hpp`, `examples/lims/ledger/include/lims/ledger/models/ledger_report_model.hpp`, `examples/lims/ledger/src/models/ledger_rates.cpp`, `examples/lims/ledger/src/models/ledger_report_model.cpp`, `examples/lims/tests/test_ledger_reports.cpp`
- Modify: `cmake/morph_add_rung.cmake` (the `stress` label), `.github/workflows/ci.yml` (coverage excludes `stress`), `ledger_entity.hpp` (`ExchangeRateRecord`), `ledger_schema.cpp`, `ledger_model.hpp` (rate actions, `rateOn`'s Doxygen), `ledger_entries.cpp` (delete the interim `rateOn`), `ledger_authorization.hpp`, `declared_requirements.hpp`, `test_declared_authorization.cpp`, `examples/lims/README.md` (decision 29)

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
TEST_CASE("Stress: Hypothesis 8: ten million lines of up to 10^9 minor units total exactly, and an over-range total is "
          "refused",
          "[lims][ledger][h8][stress]") {
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

- [ ] **Step 6: Label the ten-million-line case `stress`**

The repository's convention for a long-running case is the ctest label `stress`, which CI's ladder legs leave out with `ctest -L ladder -LE stress`. A ladder case's labels come from `catch_discover_tests` uniformly, and the one per-case label it already gets is `journey`, from a post-pass keyed on the name's `Journey: ` prefix; `Stress: ` is keyed the same way. In `cmake/morph_add_rung.cmake`, in the generated `ladder_${_rung}_tests_journey_label.cmake` content, add after the `Journey: ` branch:

```cmake
    elseif(_ladder_test MATCHES \"Stress: \")
        set_tests_properties(\"\${_ladder_test}\" PROPERTIES LABELS \"ladder-${_rung};stress\")
```

so the block reads `if(... \"Journey: \") ... elseif(... \"Stress: \") ... endif()`, extend the comment above it to say that a `Stress: ` case gets `stress` the same way, and widen the configure-time guard's regex to `TEST_CASE\\(\"(Journey|Stress): [^\"]*;` with its message naming the prefix it found. In `.github/workflows/ci.yml`'s coverage step (`LLVM_PROFILE_FILE=… ctest --preset clang-coverage`), add `-LE stress`, since coverage instruments the summation loop ten million times and measures nothing it would not measure once.

Run: Configure, then `ctest --test-dir build/linux-everything -L stress -N` and `ctest --test-dir build/linux-everything -L ladder-lims -N | tail -1`. Expected: the first lists exactly `lims.Stress: Hypothesis 8: …`; the second's count includes it, so the rung's own run and the plan's "Everything labelled for the rung" still measure it. Control: rename the case without the `Stress: ` prefix, reconfigure, and see `-L stress -N` list nothing; restore it. The case runs in every local `ctest -L ladder-lims` and in a direct run of the binary; CI's ladder legs leave it out, as they do every `stress` case.

- [ ] **Step 7: Build and run, and record the measured time**

Run: Build lims, Model suite `"[ledger]"`. Expected: all pass. The `WARN` line prints the measured summation time; copy it into decision 29. Run the four mutations named in the file and restore each. Then run Everything labelled for the rung.

- [ ] **Step 8: Record decision 29 in the README**

Append, filling the two bracketed measurements from Step 7's output (the `WARN` line and the clamped value):

```markdown
### 29. The ledger validates and never rounds, and its totals are exact or refused (hypothesis 8, first instance)

A journal entry is recorded only if it has two or more lines whose debits equal their credits in the transaction
currency and in company currency, each amount a whole number of its currency's minor units (`lims::entryProblem`).
The ledger rounds nothing: a poster converts and rounds, half-up, line by line, and adds an explicit rounding line
where that leaves a difference. A posting is a poster's document reaching the ledger through the app's wiring
(decision 37), never a client's hand-made entry under a document's name, and is recorded once per company and source
key through an `IReplayLedger` over `ledger_posting_sources`, in the entry's own transaction, so a retry answers the
first entry. Entries are immutable once posted; a correction is a posted reversal naming the original. A period is open,
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

**One transaction currency per entry.** An entry carries one transaction currency, and balances in it and in company currency. The design allows an entry to
"sum to zero per transaction currency", which admits one entry mixing several; lims's posters each post one document
in one currency (an invoice in its client's, a receipt in its supplier's), and a single currency per entry keeps the
rate one fact of the entry rather than one per line, so `PostingRequest` carries one `currency` and one `rate` and
`entryProblem` checks that one. A document in several currencies would post one entry per currency; nothing in Parts
B and C produces one.
```

- [ ] **Step 9: Commit**

```bash
cd /home/yaraslau/repo/morph
git add cmake/morph_add_rung.cmake .github/workflows/ci.yml examples/lims/ledger examples/lims/tests \
        examples/lims/app/include/lims/app/declared_requirements.hpp examples/lims/README.md
git commit -m "Add lims exchange rates and ledger reports, and measure exact totals at ERP magnitude

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 8: Billing — the module, settings, billing clients and price lists

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

`examples/lims/CMakeLists.txt`: `MODULES org posting lab ledger billing app` and `USES lab:org ledger:org,posting billing:org,posting`; add `_lims_boundary_probe(billing_includes_ledger billing fail lims/ledger/models/ledger_model.hpp)` and `_lims_boundary_probe(ledger_includes_billing ledger fail lims/billing/models/price_list_model.hpp)`, with the two probe files in Task 2's shape (comments: "billing reaches the ledger only through the posting contract and the app's port" / "the ledger knows nothing of invoices"). Add the four declared actions to `declaredRequirements()` and the coverage tuple, `"PriceListModel"` to its model list, and a `billing/` row to the README's Modules table: `` | `billing/` | billing settings, clients, price lists, billable lines, invoices, the edit lease, billing runs | `org`, `posting` | ``.

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
### Task 9: Domain events — `lab.AnalysisPublished` becomes a billable line (hypothesis 2, first instance)

**Files:**
- Create: `examples/common/testkit/server_connection.hpp`, `examples/lims/app/include/lims/app/service_principals.hpp`, `examples/lims/app/include/lims/app/internal_client.hpp`, `examples/lims/app/src/internal_client.cpp`, `examples/lims/lab/include/lims/lab/events/lab_events.hpp`, `examples/lims/lab/include/lims/lab/events/lab_outbox_store.hpp`, `examples/lims/lab/src/events/lab_events.cpp`, `examples/lims/billing/include/lims/billing/dto/line_dto.hpp`, `examples/lims/billing/include/lims/billing/models/invoice_model.hpp`, `examples/lims/billing/src/models/invoice_model.cpp`, `examples/lims/app/include/lims/app/event_wiring.hpp`, `examples/lims/app/src/event_wiring.cpp`, `examples/lims/tests/billing_test_support.hpp`, `examples/lims/tests/test_domain_events.cpp`
- Modify: `examples/common/testkit/backend_rig.hpp` (`server()`, `connect()`, the events hello), `examples/common/testkit/test_backend_rig.cpp`, `examples/lims/org/include/lims/org/dto/role_dto.hpp` (`Service`), `examples/lims/org/src/models/org_model.cpp` (`kMaxRole`), `examples/lims/org/include/lims/org/core/model_support.hpp` (`isServicePrincipal`, `requireServicePrincipal`), `examples/lims/org/src/models/auth_model.cpp` (its reserved-namespace check uses `isServicePrincipal`), `examples/lims/lab/include/lims/lab/db/lims_entity.hpp` (`LabOutboxRecord`), `examples/lims/lab/src/db/schema.cpp` (migration), `examples/lims/lab/src/models/sample_model.cpp` (`writeState`), `billing_entity.hpp` (`BillableLineRecord`, `ConsumedEventRecord`), `billing_schema.cpp`, `billing_authorization.hpp`, `declared_requirements.hpp`, `test_declared_authorization.cpp`, `examples/lims/src/server/main.cpp` (the events client and the relay on a timer), `examples/lims/README.md` (decision 31)

**Interfaces:**
- Consumes: `morph::journal::OutboxRelay`, `morph::journal::LogEntry`, `morph::journal::IActionLog`, `morph::offline::IReplayLedger`, `morph::backend::RemoteServer`, `morph::backend::SimulatedRemoteBackend`, `morph::bridge::Bridge`, `morph::bridge::BridgeHandler`, `morph::async::Completion<T>::makeSettleable`, `morph::session::TokenIssuer`, `PriceListModel::priceOn`, `OrgModel::findSite`, `OrgModel::rolesIn`.
- Produces: in the testkit, `morph::ladder::testkit::EventsHello`, `class morph::ladder::testkit::ServerConnection { registerModel; execute; watch; changes; heardOf; eventually; close; }`, `RemoteServer& BackendRig::server()`, `std::unique_ptr<ServerConnection> BackendRig::connect()` (`Mode::Socket` only); `LimsRole::Service`; `bool lims::isServicePrincipal(std::string_view)`, `void lims::requireServicePrincipal(std::string_view action)`; `lims::app::kEventsPrincipal`, `kSchedulePrincipal`, `kPostingPrincipal`; `class lims::app::InternalClient { InternalClient(RemoteServer&, std::string_view secret, std::string principal); Completion<R> dispatch<Model>(Action); R call<Model>(Action); }`; `struct lims::AnalysisPublished { ResultId result; SampleId sample; AnalysisId analysis; ClientId client; SiteId site; Timestamp publishedAt; }`; `inline constexpr std::string_view lims::kAnalysisPublished = "lab.AnalysisPublished"`; `std::vector<morph::journal::LogEntry> lims::drainLabOutbox()`; `void lims::markLabOutboxRelayed(std::span<morph::journal::LogEntry const>)`; `void lims::writeAnalysisPublished(Lightweight::DataMapper&, db::SampleRecord const&, std::string const& principal)` (lab-internal); `RecordBillableLine { std::string eventId; ResultId result; SampleId sample; AnalysisId analysis; ClientId client; SiteId site; Timestamp publishedAt; }` → `BillableLineView { BillableLineId id; CompanyId company; SiteId site; ClientId client; SampleId sample; ResultId result; AnalysisId analysis; Timestamp publishedAt; BillableLineState state; Amount amount; CurrencyCode currency; InvoiceId invoice; std::string eventId; }`; `ListBillableLines { CompanyId company; ClientId client; SampleId sample; }` → `ListBillableLinesResult`; model `lims::InvoiceModel` (`"InvoiceModel"`); record `Billing_Line`; `class lims::app::BillingEventSink final : public morph::journal::IActionLog { explicit BillingEventSink(InternalClient&); }`; `std::size_t lims::app::relayLabEvents(InternalClient&)`; test helpers `lims::test::LabFixture { ClientId client; AnalysisId nitrate; SampleId sample; std::vector<ResultId> results; }`, `LabFixture lims::test::readyToPublish(std::string reference = "WW-1")`, `LabFixture lims::test::publishedSample(std::string reference = "WW-1")`, `void lims::test::billingFor(ClientId, CurrencyCode = EUR)`, `lims::test::kServiceSecret`, `void lims::test::grantService(std::string const&, Scope = seeded company)`, `std::size_t lims::test::relayLabEvents()`.

The event id is `lab.AnalysisPublished:<result id>:<sample version after publishing>`: one event per published result, and a redelivery carries the same id. The payload is `AnalysisPublished` as JSON; billing never sees that type — the app decodes it and sends billing its own `RecordBillableLine`, so neither module includes the other.

Billing's consumer is an action, `RecordBillableLine`, and the relay reaches it the way a socket client would: `lims::app::InternalClient` is a `Bridge` over `SimulatedRemoteBackend` into the server's own `RemoteServer`, under `system:events` with a token the server mints from its own secret (`examples/LADDER.md`, "Background jobs"; bookmarks' metadata worker is the same shape). So the line is written inside a dispatch — authorized by the edge and by the declared rule, journaled, its effects fanned out to watchers — and nothing installs a session by hand. A `system:` principal is one no sign-in yields (`LimsLogin` refuses the namespace), and it holds `LimsRole::Service` at a company like any role, through `AssignRole`; `RecordBillableLine` asks for both, so a person given the role still cannot send it.

A line whose client is not registered for billing at the sample's company, or for which no price is in force, is recorded `Unpriced` with no amount, not refused: a consumer that refuses an event stops the relay at that event, and every later event waits behind it (measured below).

- [ ] **Step 1: A rig connection that closes on its own, and the rig's server**

The lims suites need what `BackendRig` does not yet give: one connection closed while others stay, and the events each received — Task 13's lease lapse, Task 15's and Task 16's fan-out, Task 11's progress. The rig is extended here, in the testkit every rung shares, rather than a raw-server rig copied into lims.

Create `examples/common/testkit/server_connection.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <glaze/glaze.hpp>
#include <memory>
#include <morph/core/connection_writer.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

/// @file
/// A connection opened on a `RemoteServer` in process, as a socket server opens one for a client: it registers
/// models, executes, watches records and closes, and keeps every event the server writes to it. `BackendRig::connect`
/// hands these out in `Mode::Socket`, so a test can close one connection while others stay and see what each was
/// told.

namespace morph::ladder::testkit {

/// @brief Negotiates server-sent events for the connection its kind arrives on, as the UI service's `ui-hello` does.
class EventsHello final : public ::morph::backend::IEnvelopeService {
public:
    using IEnvelopeService::handle;

    /// @brief The kind it serves.
    static constexpr std::string_view kKind = "testkit-events-hello";

    /// @brief Whether it serves @p kind.
    /// @param kind The envelope's kind.
    /// @return `true` for `testkit-events-hello`.
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

/// @brief One connection on a server, opened in process, and every event the server sent it.
///
/// Destroy it before the rig that made it: the server it holds runs on the rig's workers.
class ServerConnection {
public:
    /// @brief Opens a connection on @p server that takes events.
    /// @param server The server; its config must serve `EventsHello`, as `BackendRig`'s does.
    explicit ServerConnection(std::shared_ptr<::morph::backend::RemoteServer> server)
        : _server{std::move(server)}, _sent{std::make_shared<Sent>()}, _cid{_server->openConnection()} {
        _server->setConnectionWriter(_cid, {.write = [sent = _sent](std::string message) {
                                                std::scoped_lock const lock{sent->mutex};
                                                sent->messages.push_back(std::move(message));
                                            },
                                            .backlog = [] { return std::size_t{0}; }});
        ::morph::wire::Envelope hello;
        hello.kind = std::string{EventsHello::kKind};
        hello.callId = nextCall();
        static_cast<void>(_server->handleInline(::morph::wire::encode(hello), _cid));
    }

    ~ServerConnection() { close(); }
    ServerConnection(ServerConnection const&) = delete;
    ServerConnection& operator=(ServerConnection const&) = delete;
    ServerConnection(ServerConnection&&) = delete;
    ServerConnection& operator=(ServerConnection&&) = delete;

    /// @brief Registers a private instance of @p typeId on this connection.
    /// @param typeId The model's registered id.
    /// @param session Who registers it.
    /// @return The instance id; 0 when the server refused.
    std::uint64_t registerModel(std::string_view typeId, ::morph::session::Context const& session) {
        ::morph::wire::Envelope env;
        env.kind = "register";
        env.callId = nextCall();
        env.typeId = std::string{typeId};
        env.session = session;
        return request(std::move(env)).modelId;
    }

    /// @brief Executes @p actionType with @p body on instance @p modelId of @p modelType.
    /// @param modelId The instance `registerModel` gave.
    /// @param modelType The model's registered id.
    /// @param actionType The action's registered id.
    /// @param body The action as JSON.
    /// @param session Who executes it.
    /// @return The reply envelope (`ok` or `err`).
    ::morph::wire::Envelope execute(std::uint64_t modelId, std::string_view modelType, std::string_view actionType,
                                    std::string body, ::morph::session::Context const& session) {
        ::morph::wire::Envelope env;
        env.kind = "execute";
        env.callId = nextCall();
        env.modelId = modelId;
        env.modelType = std::string{modelType};
        env.actionType = std::string{actionType};
        env.body = std::move(body);
        env.session = session;
        return request(std::move(env));
    }

    /// @brief Adds @p records to this connection's watch set.
    /// @param records Record wire names.
    /// @param session Who watches.
    /// @return The reply.
    ::morph::wire::Envelope watch(std::vector<std::string> records, ::morph::session::Context const& session) {
        ::morph::wire::Envelope env;
        env.kind = std::string{::morph::wire::kWatchKind};
        env.callId = nextCall();
        env.session = session;
        env.body = glz::write_json(::morph::wire::WatchBody{.records = std::move(records), .instances = {}, .replace = false})
                       .value_or(std::string{});
        return ::morph::wire::decode(_server->handleInline(::morph::wire::encode(env), _cid));
    }

    /// @brief Every `changes` event the server sent this connection, decoded, in order.
    /// @return The bodies.
    [[nodiscard]] std::vector<::morph::wire::ChangesBody> changes() const {
        std::vector<std::string> messages;
        {
            std::scoped_lock const lock{_sent->mutex};
            messages = _sent->messages;
        }
        std::vector<::morph::wire::ChangesBody> bodies;
        for (auto const& message : messages) {
            auto const env = ::morph::wire::decode(message);
            if (env.kind != ::morph::wire::kEventKind) {
                continue;
            }
            ::morph::wire::EventBody event;
            if (glz::read_json(event, env.body) || event.topic != ::morph::wire::kChangesTopic) {
                continue;
            }
            if (auto body = ::morph::wire::readChangesBody(event.payload.str)) {
                bodies.push_back(std::move(*body));
            }
        }
        return bodies;
    }

    /// @brief Whether any `changes` event sent so far names @p record.
    /// @param record A record wire name.
    /// @return `true` when one does.
    [[nodiscard]] bool heardOf(std::string_view record) const {
        for (auto const& body : changes()) {
            for (auto const& change : body.records) {
                if (change.record == record) {
                    return true;
                }
            }
        }
        return false;
    }

    /// @brief Polls @p pred until it holds or @p budget passes.
    /// @tparam Pred A callable returning `bool`.
    /// @param pred The condition.
    /// @param budget How long to wait.
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

    /// @brief Closes the connection, as the transport does when its socket goes. Idempotent.
    void close() {
        if (!_closed.exchange(true)) {
            _server->closeConnection(_cid);
        }
    }

private:
    struct Sent {
        std::mutex mutex;
        std::vector<std::string> messages;
    };

    [[nodiscard]] std::uint64_t nextCall() { return ++_callId; }

    ::morph::wire::Envelope request(::morph::wire::Envelope env) {
        auto promise = std::make_shared<std::promise<std::string>>();
        auto answer = promise->get_future();
        _server->handle(::morph::wire::encode(env),
                        [promise](std::string reply) { promise->set_value(std::move(reply)); }, _cid);
        return ::morph::wire::decode(answer.get());
    }

    std::shared_ptr<::morph::backend::RemoteServer> _server;
    std::shared_ptr<Sent> _sent;
    ::morph::backend::ConnectionId _cid;
    std::uint64_t _callId = 0;
    std::atomic<bool> _closed{false};
};

}  // namespace morph::ladder::testkit
```

The shapes are read from `tests/test_push_watch.cpp` (its `Rig`, `FanHello`, `executeOn`) and `include/morph/core/remote.hpp`; the field names `ConnectionWriter::write`/`backlog`, `ConnectionFacts::events` and `Envelope::modelId` are from those headers. Not verified by reading: that `register` is authorised by `LimsAuthorizer` without a role (its `authorizeRegister` is `IAuthorizer`'s default) — if `register` answers `err "unauthorized"`, read `session.hpp`'s `authorizeRegister` and give `LimsAuthorizer` the override `SigningAuthorizer` needs.

In `examples/common/testkit/backend_rig.hpp`, include `"testkit/server_connection.hpp"` and `<morph/core/remote.hpp>`'s `ServerConfig`. In `Mode::Socket`, build the server with a config that serves `EventsHello`:

```cpp
            case Mode::Socket: {
                _workerPool = std::make_unique<::morph::exec::ThreadPoolExecutor>(4);
                ::morph::backend::ServerConfig config;
                // Lets a connection from connect() take events; a socket client never sends its kind.
                config.services.push_back(std::make_shared<EventsHello>());
                if (authorizer) {
                    _server = std::make_shared<::morph::backend::RemoteServer>(*_workerPool, authorizer, config);
                } else {
                    _server = std::make_shared<::morph::backend::RemoteServer>(*_workerPool, config);
                }
```

(the rest of the case unchanged), and add, after `url()`:

```cpp
    /// @brief The server the rig's clients reach, for an internal client of the application under test.
    /// @return The server, owned by this rig.
    /// @throws std::logic_error in `Local`/`LocalSingleThread`, which run no server.
    [[nodiscard]] ::morph::backend::RemoteServer& server() {
        if (_mode != Mode::Socket) {
            throw std::logic_error("BackendRig::server: only Mode::Socket runs a server");
        }
        return *_server;
    }

    /// @brief Opens a connection on the rig's server in process: one a test can close while the others stay, and
    ///        that keeps what the server sent it.
    /// @return The connection; destroy it before this rig.
    /// @throws std::logic_error in `Local`/`LocalSingleThread`, which run no server.
    [[nodiscard]] std::unique_ptr<ServerConnection> connect() {
        if (_mode != Mode::Socket) {
            throw std::logic_error("BackendRig::connect: only Mode::Socket runs a server");
        }
        return std::make_unique<ServerConnection>(_server);
    }
```

In `examples/common/testkit/test_backend_rig.cpp` add:

```cpp
// Mutation: drop EventsHello from the Socket-mode config (the watch is answered, but the connection takes no events,
// which the lims fan-out cases then fail on).
TEST_CASE("BackendRig: connect() opens a closable connection on the Socket-mode server", "[testkit][rig]") {
    morph::ladder::testkit::BackendRig rig{morph::ladder::testkit::Mode::Socket, 0};
    auto first = rig.connect();
    auto second = rig.connect();
    CHECK(first->watch({}, {}).kind == "ok");
    first->close();
    first->close();  // idempotent
    CHECK(second->watch({}, {}).kind == "ok");
    CHECK_NOTHROW(rig.server());
    morph::ladder::testkit::BackendRig local{morph::ladder::testkit::Mode::Local, 1};
    CHECK_THROWS_AS(local.connect(), std::logic_error);
    CHECK_THROWS_AS(local.server(), std::logic_error);
}
```

Run: `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --build build/linux-everything --target ladder_common_tests -j10`, then `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock ctest --test-dir build/linux-everything -L ladder-0 --output-on-failure`. Expected: the new case and every existing testkit case pass; the rungs' Socket-mode suites are unchanged by the added service.

- [ ] **Step 2: Service principals, the service role, and the internal client**

Append `Service` to `LimsRole` in `role_dto.hpp` (`Service, ///< May run one module's reaction to another's, or a scheduled job: held only by the server's own `system:` principals.`), its `roleName` case `"service"`, the enumerator to the `glz::enumerate`, and set `kMaxRole` in `org_model.cpp` to `static_cast<int>(LimsRole::Service)`. In `org/core/model_support.hpp` add:

```cpp
/// @brief The principal namespace reserved for the server's own clients; no sign-in yields a principal in it.
inline constexpr std::string_view kServicePrincipalPrefix = "system:";

/// @brief Whether @p principal is one of the server's own clients.
/// @param principal The principal.
/// @return `true` for `system:<name>` with a non-empty name.
[[nodiscard]] inline bool isServicePrincipal(std::string_view principal) noexcept {
    return principal.starts_with(kServicePrincipalPrefix) && principal.size() > kServicePrincipalPrefix.size();
}

/// @brief Throws `Forbidden` unless the authenticated principal is one of the server's own clients.
/// @param action The action, for the refusal.
/// @throws Forbidden when it is not.
inline void requireServicePrincipal(std::string_view action) {
    if (!isServicePrincipal(currentPrincipal())) {
        throw Forbidden{std::string{action} + ": only the server's own clients send it"};
    }
}
```

and make `auth_model.cpp`'s reserved-namespace check call `isServicePrincipal` (deleting its own `kServicePrincipalPrefix`). Add `LimsRole::Service` to `test_value_limits.cpp`'s naming case: `CHECK(lims::roleName(lims::LimsRole::Service) == "service");`.

Create `examples/lims/app/include/lims/app/service_principals.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string_view>

/// @file
/// The server's own clients. Each acts through `InternalClient` as a principal in the reserved `system:` namespace,
/// which no sign-in yields, and holds `LimsRole::Service` at each company it acts for, assigned like any role
/// (`AssignRole`, the administration screen); the server grants itself nothing.

namespace lims::app {

/// @brief Relays lab's events into billing.
inline constexpr std::string_view kEventsPrincipal = "system:events";

/// @brief Runs the billing schedule.
inline constexpr std::string_view kSchedulePrincipal = "system:billing-schedule";

/// @brief Records billing's postings in the ledger.
inline constexpr std::string_view kPostingPrincipal = "system:posting";

}  // namespace lims::app
```

Create `examples/lims/app/include/lims/app/internal_client.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <optional>
#include <string>
#include <string_view>

/// @file
/// The server's own client: background work dispatches through the same `RemoteServer` a socket client reaches,
/// under a service principal whose token the server mints, so it is authorized, journaled and fanned out like a
/// user's call (`examples/LADDER.md`, "Background jobs").

namespace lims::app {

/// @brief A `Bridge` over `SimulatedRemoteBackend` into a server, acting as one service principal.
///
/// The bridge belongs to a one-thread executor of its own, where every call is issued and settled, so `call` may
/// block any other thread — a relay pass, a schedule tick — without blocking the thread that delivers its answer.
/// `call` must not be made on that executor; a Task handler awaits `dispatch` instead.
class InternalClient {
public:
    /// @brief A client of @p server acting as @p principal.
    /// @param server The server. Borrowed: it must outlive this client.
    /// @param secret The token secret the server's authorizer verifies with.
    /// @param principal A principal in the reserved `system:` namespace.
    /// @throws ValidationError if @p principal is not a service principal.
    InternalClient(::morph::backend::RemoteServer& server, std::string_view secret, std::string principal);

    /// @brief Releases the bridge on its own executor. Every `dispatch` must have settled.
    ~InternalClient();
    InternalClient(InternalClient const&) = delete;
    InternalClient& operator=(InternalClient const&) = delete;
    InternalClient(InternalClient&&) = delete;
    InternalClient& operator=(InternalClient&&) = delete;

    /// @brief Dispatches @p action on a fresh instance of @p Model.
    /// @tparam Model The model.
    /// @tparam Action The action.
    /// @param action The action.
    /// @return Its result, settled on this client's executor; `co_await` it from a Task handler.
    template <typename Model, typename Action>
    [[nodiscard]] ::morph::async::Completion<typename ::morph::model::ActionTraits<Action>::Result> dispatch(
        Action action);

    /// @brief Dispatches @p action on a fresh instance of @p Model and waits for its result.
    /// @tparam Model The model.
    /// @tparam Action The action.
    /// @param action The action.
    /// @return Its result.
    /// @throws std::exception the server's refusal or the action's failure.
    template <typename Model, typename Action>
    typename ::morph::model::ActionTraits<Action>::Result call(Action action);

    /// @brief The principal it acts as.
    /// @return It.
    [[nodiscard]] std::string const& principal() const noexcept { return _principal; }

private:
    /// @brief Runs @p task on the bridge's executor and waits for it.
    void onOwner(std::function<void()> const& task);

    std::string _principal;
    ::morph::exec::ThreadPoolExecutor _owner{1};
    std::optional<::morph::bridge::Bridge> _bridge;
};

template <typename Model, typename Action>
::morph::async::Completion<typename ::morph::model::ActionTraits<Action>::Result> InternalClient::dispatch(
    Action action) {
    using Result = typename ::morph::model::ActionTraits<Action>::Result;
    auto [completion, promise] = ::morph::async::Completion<Result>::makeSettleable(&_owner);
    auto settle = std::make_shared<typename ::morph::async::Completion<Result>::Promise>(std::move(promise));
    _owner.post([this, settle, action = std::move(action)]() mutable {
        try {
            // The handler is kept alive by its call's callbacks: execute() returns before the server has run the
            // action, and a handler destroyed here would deregister its instance under the pending call.
            auto handler = std::make_shared<::morph::bridge::BridgeHandler<Model>>(*_bridge, &_owner);
            handler->execute(std::move(action))
                .then([settle, handler](Result result) { settle->resolve(std::move(result)); })
                .onError([settle, handler](std::exception_ptr const& error) { settle->reject(error); });
        } catch (...) {
            settle->reject(std::current_exception());
        }
    });
    return std::move(completion);
}

template <typename Model, typename Action>
typename ::morph::model::ActionTraits<Action>::Result InternalClient::call(Action action) {
    using Result = typename ::morph::model::ActionTraits<Action>::Result;
    auto settled = std::make_shared<std::promise<Result>>();
    auto answer = settled->get_future();
    _owner.post([this, settled, action = std::move(action)]() mutable {
        try {
            auto handler = std::make_shared<::morph::bridge::BridgeHandler<Model>>(*_bridge, &_owner);
            handler->execute(std::move(action))
                .then([settled, handler](Result result) { settled->set_value(std::move(result)); })
                .onError([settled, handler](std::exception_ptr const& error) { settled->set_exception(error); });
        } catch (...) {
            settled->set_exception(std::current_exception());
        }
    });
    return answer.get();
}

}  // namespace lims::app
```

and `examples/lims/app/src/internal_client.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "lims/app/internal_client.hpp"

#include <cstdint>
#include <morph/session/session_auth.hpp>

#include "lims/org/core/errors.hpp"
#include "lims/org/core/model_support.hpp"

namespace lims::app {

namespace {

/// @brief When a service token expires: 2100-01-01. The server mints it at start-up and never re-issues it.
constexpr std::int64_t kServiceTokenExpiresAtMs = 4102444800000;

}  // namespace

InternalClient::InternalClient(::morph::backend::RemoteServer& server, std::string_view secret, std::string principal)
    : _principal{std::move(principal)} {
    if (!isServicePrincipal(_principal)) {
        throw ValidationError{"InternalClient: '" + _principal + "' is not in the reserved 'system:' namespace"};
    }
    // Minted here, not through LimsLogin, which refuses the reserved namespace: that refusal is what keeps a client
    // from obtaining this authority. The secret is the authorizer's, so the token verifies like a user's.
    ::morph::session::TokenIssuer const issuer{std::string{secret}, ::morph::session::hmacSha256};
    ::morph::session::Context session;
    session.principal = _principal;
    session.token = issuer.issue(::morph::session::SessionToken{
        .principal = _principal, .issuedAtMs = 0, .expiresAtMs = kServiceTokenExpiresAtMs, .roles = {}});
    onOwner([this, &server, &session] {
        _bridge.emplace(std::make_unique<::morph::backend::SimulatedRemoteBackend>(server), _owner);
        _bridge->setDefaultSession(session);
    });
}

InternalClient::~InternalClient() {
    onOwner([this] { _bridge.reset(); });
}

void InternalClient::onOwner(std::function<void()> const& task) {
    std::promise<void> done;
    auto finished = done.get_future();
    _owner.post([&task, &done] {
        try {
            task();
            done.set_value();
        } catch (...) {
            done.set_exception(std::current_exception());
        }
    });
    finished.get();
}

}  // namespace lims::app
```

`Completion<T>::makeSettleable` and `Promise::resolve`/`reject` are `include/morph/core/completion.hpp`'s ("Settleable promise seam"); the `Bridge(std::unique_ptr<IBackend>, IExecutor&)` construction and `setDefaultSession` are bookmarks' `App`'s. If a debug build's owner check refuses a one-thread `ThreadPoolExecutor` as a bridge owner, read `docs/spec/core/executor.md` for the serial executor it names for an owner off the GUI thread and use that; record which in decision 31.

- [ ] **Step 3: The shared test fixture**

Create `examples/lims/tests/billing_test_support.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <morph/util/rational.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "lims/app/event_wiring.hpp"
#include "lims/app/internal_client.hpp"
#include "lims/app/service_principals.hpp"
#include "lims/billing/models/price_list_model.hpp"
#include "lims/lab/models/analysis_catalog_model.hpp"
#include "lims/lab/models/sample_model.hpp"
#include "lims_test_support.hpp"
#include "testkit/backend_rig.hpp"

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

/// @brief The secret the test servers' authorizer and the service tokens share.
inline constexpr std::string_view kServiceSecret = "lims-service-test-secret-at-least-32-bytes";

/// @brief Gives @p principal the service role in @p scope, unless it holds it there already.
/// @param principal A service principal.
/// @param scope The scope; the seeded company by default.
inline void grantService(std::string const& principal, Scope scope = Scope{seededCompany()}) {
    auto const held = OrgModel::rolesIn(principal, scope);
    if (std::ranges::find(held, LimsRole::Service) == held.end()) {
        grant(principal, LimsRole::Service, scope);
    }
}

/// @brief One relay pass over lab's outbox into billing, as the server runs it: through an internal client of a
///        server of its own, as the events principal, which this gives the service role at the seeded company.
/// @return How many events were relayed.
/// @throws std::exception from billing's consumer, as `lims::app::relayLabEvents` does.
inline std::size_t relayLabEvents() {
    grantService(std::string{app::kEventsPrincipal});
    ::morph::ladder::testkit::BackendRig rig{::morph::ladder::testkit::Mode::Socket, 0, limsAuthorizer(kServiceSecret)};
    app::InternalClient events{rig.server(), kServiceSecret, std::string{app::kEventsPrincipal}};
    return app::relayLabEvents(events);
}

}  // namespace lims::test
```

If `CaptureConcentration`'s result type names its id other than `.id`, or `SampleModel` requires the capture on an `OpenSample`-attached handler, follow `test_declared_authorization.cpp`'s `twoSites()`, which this mirrors.

- [ ] **Step 4: Write the failing test**

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
#include <morph/session/session_auth.hpp>
#include <span>
#include <stdexcept>
#include <string>

#include "billing_test_support.hpp"
#include "lims/app/event_wiring.hpp"
#include "lims/app/internal_client.hpp"
#include "lims/billing/models/invoice_model.hpp"
#include "lims/lab/db/lims_entity.hpp"
#include "lims/lab/events/lab_events.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

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

// Hypothesis 2. Mutation: drop the replay-ledger lookup from InvoiceModel's RecordBillableLine (the redelivered batch
// bills the result twice).
TEST_CASE("Hypothesis 2: an event bills once, across a relay that dies before marking it and a restart",
          "[lims][events][h2]") {
    DbFixture fixture;
    auto const lab = lims::test::publishedSample();
    lims::test::billingFor(lab.client);

    // A relay that delivers and then dies before it marks the batch: what a crash between the two looks like.
    {
        lims::test::grantService(std::string{lims::app::kEventsPrincipal});
        morph::ladder::testkit::BackendRig rig{morph::ladder::testkit::Mode::Socket, 0,
                                               lims::test::limsAuthorizer(lims::test::kServiceSecret)};
        lims::app::InternalClient events{rig.server(), lims::test::kServiceSecret,
                                         std::string{lims::app::kEventsPrincipal}};
        morph::journal::OutboxRelay dying;
        dying.drainOutbox = [] { return lims::drainLabOutbox(); };
        dying.markRelayed = [](std::span<morph::journal::LogEntry const> /*rows*/) {
            throw std::runtime_error{"process killed before marking the batch"};
        };
        dying.sink = std::make_shared<lims::app::BillingEventSink>(events);
        CHECK_THROWS_WITH(dying.relay(), Catch::Matchers::ContainsSubstring("process killed"));
    }
    auto const afterCrash = linesOf(lab.sample);
    REQUIRE(afterCrash.size() == 1U);
    CHECK(afterCrash[0].state == lims::BillableLineState::Unbilled);
    CHECK(*afterCrash[0].amount == morph::math::Rational{40, morph::math::DecimalPlaces{2}});
    CHECK(lims::drainLabOutbox().size() == 1U);

    // The restart: a fresh relay over the same database redelivers the event, and billing recognises it.
    CHECK(lims::test::relayLabEvents() == 1U);
    auto const afterRestart = linesOf(lab.sample);
    REQUIRE(afterRestart.size() == 1U);
    CHECK(afterRestart[0].id == afterCrash[0].id);
    CHECK(lims::drainLabOutbox().empty());
    CHECK(lims::test::relayLabEvents() == 0U);
}

TEST_CASE("domain events: a line with no price in force is recorded unpriced, not refused", "[lims][events]") {
    DbFixture fixture;
    auto const lab = lims::test::publishedSample();
    CHECK(lims::test::relayLabEvents() == 1U);
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
    CHECK_THROWS(lims::test::relayLabEvents());
    CHECK(lims::drainLabOutbox().size() == 2U);
    CHECK(linesOf(lab.sample).empty());
}

// Mutation: drop requireServicePrincipal from RecordBillableLine (bea's line is recorded).
TEST_CASE("domain events: a billable line is recorded only by the server's own client", "[lims][events]") {
    DbFixture fixture;
    auto const lab = lims::test::publishedSample();
    lims::test::billingFor(lab.client);
    lims::test::grantService("bea");  // even holding the role, a person is not the event relay
    morph::ladder::testkit::BackendRig rig{morph::ladder::testkit::Mode::Socket, 1,
                                           lims::test::limsAuthorizer(lims::test::kServiceSecret)};
    morph::session::TokenIssuer const issuer{std::string{lims::test::kServiceSecret}, morph::session::hmacSha256};
    rig.bridge(0).setDefaultSession(lims::test::signedAs(issuer, "bea"));
    auto invoices = rig.client<lims::InvoiceModel>(0);
    CHECK_THROWS_WITH(morph::ladder::testkit::awaitQt(invoices.execute(lims::RecordBillableLine{
                          .eventId = "lab.AnalysisPublished:1:9",
                          .result = lims::ResultId{1},
                          .sample = lab.sample,
                          .analysis = lab.nitrate,
                          .client = lab.client,
                          .site = lims::test::seededSite(),
                          .publishedAt = morph::time::Timestamp{lims::test::at(2026, 11, 10)}})),
                      Catch::Matchers::ContainsSubstring("only the server's own clients"));
    CHECK(linesOf(lab.sample).empty());
}
```

The poison event has no site, so it fails `RecordBillableLine::validate()` and the dispatch is refused with `ValidationError`, which `BillingEventSink::append` rethrows. `OutboxRelay::relay` appends in drain order and rethrows, so the good event behind it is never delivered and nothing is marked; the drained order is by outbox id, which puts the poison first.

- [ ] **Step 5: Run it and see it fail**

Run: Build lims. Expected: compile error, `lims/app/event_wiring.hpp` not found.

- [ ] **Step 6: Lab writes the event with the fact**

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

- [ ] **Step 7: Billing's lines and its consumer action**

Create `examples/lims/billing/include/lims/billing/dto/line_dto.hpp` with `RecordBillableLine` (documented "Bills one published result: what lab's `lab.AnalysisPublished` becomes in billing. Sent only by the app's event relay, as a service principal; a redelivered event id answers the line it made"; `validate()` requires an event id of at most 128 bytes and every id and the date), `BillableLineView`, `ListBillableLines` (`validate()` always `true`; an empty filter field matches every line) and `ListBillableLinesResult`, and `BRIDGE_REGISTER_RECORD(lims::BillableLineView, "Billing_Line")`.

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
    /// Priced at the price in force on the publishing date (`PriceListModel::priceOn`); recorded `Unpriced` with no
    /// amount when there is none. The event id is recorded in the line's own transaction through an
    /// `IReplayLedger`, so a redelivered event answers the line it made.
    /// @param action The event, as billing is told it.
    /// @return The line.
    /// @throws ValidationError if the event names no event id, site, client, sample, result, analysis or date.
    /// @throws Forbidden if the caller is not one of the server's own clients holding the service role at the
    ///         site's company.
    /// @throws NotFound if the site does not exist.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    BillableLineView execute(const RecordBillableLine& action);
```

registered `BRIDGE_REGISTER_MODEL(lims::InvoiceModel, "InvoiceModel")`, `BRIDGE_REGISTER_ACTION(lims::InvoiceModel, lims::RecordBillableLine, "RecordBillableLine")` with `BRIDGE_REGISTER_EFFECTS(lims::RecordBillableLine, ::morph::effects::changes<lims::BillableLineView>)`, and `BRIDGE_REGISTER_ACTION(lims::InvoiceModel, lims::ListBillableLines, "ListBillableLines", ::morph::model::Loggable::No)`. In `billing_authorization.hpp`:

```cpp
/// @brief A billable line is recorded by a service principal holding the service role at the site's company.
template <>
struct Authorization<RecordBillableLine> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::Service;

    /// @brief The site the published sample belongs to.
    /// @param action The action.
    /// @return It.
    [[nodiscard]] static Scope scopeOf(RecordBillableLine const& action) { return Scope{action.site}; }
};
```

and add it to `declaredRequirements()` (raising the array) and the coverage tuple, `"InvoiceModel"` to its model list.

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
    // Borrowed for one RecordBillableLine; never reseated.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    Lightweight::DataMapper& _mapper;
};

/// @brief The scope billing's consumed events are recorded in.
constexpr std::string_view kLineEvents = "billing.lines";

}  // namespace

BillableLineView InvoiceModel::execute(const RecordBillableLine& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"RecordBillableLine: event '" + action.eventId + "' does not name everything a line needs"};
    }
    requireServicePrincipal("RecordBillableLine");
    requireRoleSomewhere<RecordBillableLine>();
    auto const site = OrgModel::findSite(action.site);
    if (!site.has_value()) {
        throw NotFound{"RecordBillableLine: site " + std::to_string(*action.site) + " does not exist"};
    }
    requireAuthorized(action);
    CompanyId const company = site->companyId;
    auto const price = PriceListModel::priceOn(company, action.client, action.analysis, action.publishedAt);
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    ConsumedEvents consumed{mapper};
    if (auto const line = consumed.lookup(kLineEvents, action.eventId)) {
        return lineView(mapper, std::stoll(*line));
    }
    db::BillableLineRecord row;
    row.companyId = *company;
    row.siteId = *action.site;
    row.clientId = *action.client;
    row.sampleId = *action.sample;
    row.resultId = *action.result;
    row.analysisId = *action.analysis;
    row.publishedAt = millisOf(action.publishedAt);
    row.eventId = Lightweight::SqlAnsiString<128>{action.eventId};
    if (price.has_value()) {
        row.state = static_cast<int>(BillableLineState::Unbilled);
        row.amountNum = (*price->amount).numerator;
        row.amountDen = (*price->amount).denominator;
        row.currency = static_cast<int>(price->currency);
    } else {
        row.state = static_cast<int>(BillableLineState::Unpriced);
    }
    mapper.Create(row);
    consumed.record(kLineEvents, action.eventId, std::to_string(row.id.Value()));
    sqlTxn.Commit();
    return lineView(mapper, static_cast<std::int64_t>(row.id.Value()));
}
```

`lineView(mapper, id)` (anonymous namespace) loads the row and maps it, range-checking `state` and `currency` as Task 4's `toView` does and building `amount` only when both optionals are engaged. `ListBillableLines` answers the lines of the companies the caller can see, filtered by each engaged field of the query, sorted by id.

- [ ] **Step 8: The app's wiring**

Create `examples/lims/app/include/lims/app/event_wiring.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <morph/journal/action_log.hpp>
#include <mutex>
#include <string_view>
#include <vector>

#include "lims/app/internal_client.hpp"

/// @file
/// The one place lab's events meet billing: a journal sink that dispatches each `lab.AnalysisPublished` to billing's
/// consumer action through an internal client, and one relay pass over lab's outbox into it.

namespace lims::app {

/// @brief A `morph::journal::IActionLog` that delivers lab's events to billing instead of storing them.
///
/// `OutboxRelay` delivers to an action log; billing's consumer is an action. This adapter joins them: it decodes lab's
/// event and dispatches `RecordBillableLine` through the internal client, waiting for its answer, so the relay marks
/// a batch only once billing has committed every line in it. Delivery is at least once (the relay's contract) and
/// billing's consumer makes it once in effect.
class BillingEventSink final : public ::morph::journal::IActionLog {
public:
    /// @brief A sink dispatching through @p client.
    /// @param client The events principal's client. Borrowed: it must outlive this sink.
    explicit BillingEventSink(InternalClient& client) : _client{client} {}

    /// @brief Delivers @p entry to billing.
    /// @param entry An entry `drainLabOutbox` produced.
    /// @throws ValidationError if the entry is not a `lab.AnalysisPublished` or its payload does not decode.
    /// @throws std::exception billing's refusal of the line.
    void append(::morph::journal::LogEntry entry) override;

    /// @brief Nothing to flush: every `append` committed before it returned.
    void flush() override {}

    /// @brief The entries delivered so far through this sink, for @p entityKey or all.
    /// @param entityKey A sample id, or empty for every entry.
    /// @return The entries, in delivery order.
    [[nodiscard]] std::vector<::morph::journal::LogEntry> entries(std::string_view entityKey = {}) const override;

private:
    // Borrowed for the sink's life; never reseated.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    InternalClient& _client;
    mutable std::mutex _mutex;
    std::vector<::morph::journal::LogEntry> _delivered;
};

/// @brief One relay pass: every event in lab's outbox to billing, then marked relayed.
/// @param client The events principal's client.
/// @return How many events were relayed.
/// @throws std::exception from billing's consumer; the batch is then left unmarked and the next pass retries it.
std::size_t relayLabEvents(InternalClient& client);

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

namespace lims::app {

void BillingEventSink::append(::morph::journal::LogEntry entry) {
    if (entry.actionType != kAnalysisPublished) {
        throw ValidationError{"billing does not consume " + entry.actionType};
    }
    AnalysisPublished published;
    if (auto const error = glz::read<glz::opts{.error_on_unknown_keys = false}>(published, entry.payload)) {
        throw ValidationError{"event " + entry.idempotencyKey + " does not decode"};
    }
    static_cast<void>(_client.call<InvoiceModel>(RecordBillableLine{.eventId = entry.idempotencyKey,
                                                                    .result = published.result,
                                                                    .sample = published.sample,
                                                                    .analysis = published.analysis,
                                                                    .client = published.client,
                                                                    .site = published.site,
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

std::size_t relayLabEvents(InternalClient& client) {
    ::morph::journal::OutboxRelay relay;
    relay.drainOutbox = [] { return drainLabOutbox(); };
    relay.markRelayed = [](std::span<::morph::journal::LogEntry const> rows) { markLabOutboxRelayed(rows); };
    relay.sink = std::make_shared<BillingEventSink>(client);
    return relay.relay().relayed;
}

}  // namespace lims::app
```

If `glz::read` with that option spelling does not compile against the pinned glaze, use `glz::read_json(published, entry.payload)` — lab writes the payload, so it has no unknown keys.

In `src/server/main.cpp` (include `<QTimer>`, `<morph/core/executor.hpp>`, `"lims/app/event_wiring.hpp"`, `"lims/app/internal_client.hpp"` and `"lims/app/service_principals.hpp"`), after the server is listening:

```cpp
        // The server's own clients, and one thread their blocking passes run on: a relay pass waits for billing's
        // answers, and waiting on the Qt thread would stall every socket this server serves.
        lims::app::InternalClient events{*server, secret, std::string{lims::app::kEventsPrincipal}};
        morph::exec::ThreadPoolExecutor background{1};
        // Lab's events reach billing through this relay; a pass that fails is logged and retried on the next tick,
        // since the batch stays unmarked.
        QTimer relayTimer;
        QObject::connect(&relayTimer, &QTimer::timeout, [&background, &events] {
            background.post([&events] {
                try {
                    static_cast<void>(lims::app::relayLabEvents(events));
                } catch (std::exception const& error) {
                    std::cerr << "lims-server: event relay: " << error.what() << '\n';
                }
            });
        });
        relayTimer.start(1000);
```

The declaration order is the teardown order in reverse: the timer stops first, `background` joins its thread, then the client releases its bridge before `server` goes. Add to the file's environment comment: "The server's own clients act as `system:events` (and, from Tasks 11 and 14, `system:billing-schedule` and `system:posting`); each needs `LimsRole::Service` at every company it serves, assigned by a supervisor like any role."

- [ ] **Step 9: Build, run, and measure hypothesis 2 against its criterion**

The criterion, stated before measuring, is the design's refutation condition: hypothesis 2 is **refuted** if the consumer receives each event exactly once in effect across a restart, with only wiring code in `lims/app` and no per-consumer delivery loop or cursor. `OutboxRelay` is the delivery loop and lab's outbox rows are the cursor, so the expected outcome is refuted unless the measurement shows otherwise. Billing's consumer action and its idempotency ledger are the consumer itself — the design requires consumers to be idempotent by event id — not delivery code.

Run: Build lims, Model suite `"[events]"`, then Everything labelled for the rung. Expected: the six cases pass, existing suites unchanged. Run the h2 mutation and the `requireServicePrincipal` mutation and restore them. Then read what the consumer needed, and keep the output for decision 31:

```bash
cd /home/yaraslau/repo/morph
grep -nE '\b(for|while)\b' examples/lims/app/src/event_wiring.cpp
grep -rniE 'cursor|last_?(event|id)|position|offset' examples/lims/app examples/lims/billing/src examples/lims/billing/include
grep -n 'OutboxRelay' examples/lims/app/src/event_wiring.cpp
wc -l examples/lims/app/include/lims/app/internal_client.hpp examples/lims/app/src/internal_client.cpp
```

Expected: the only loop in `event_wiring.cpp` is `entries()`'s filter over what was delivered (the `IActionLog` read API, not delivery); no cursor, last-delivered id or position anywhere in app or billing; `OutboxRelay` drives the pass. The last line is the internal client's cost, which Tasks 11 and 14 reuse unchanged.

- [ ] **Step 10: Record decision 31**

Append to the README the text for the outcome Step 9 measured. Refuted (the expected outcome):

```markdown
### 31. Lab's events reach billing through an outbox, morph's relay and a dispatched consumer (hypothesis 2, first instance)

`PublishSample` writes one `lab.AnalysisPublished` row per result into `lims_outbox`, in the transaction that
publishes, with the event id `lab.AnalysisPublished:<result>:<sample version>`. `lims::app::relayLabEvents` runs one
`morph::journal::OutboxRelay` pass: `drainOutbox` and `markRelayed` are lab's, and the sink is
`lims::app::BillingEventSink`, which decodes the event and dispatches `RecordBillableLine` through
`lims::app::InternalClient` — a `Bridge` over `SimulatedRemoteBackend` into the server's own `RemoteServer`, as
`system:events`, with a token the server mints — so the line is written inside a dispatch: authorized by the edge and
by its declared rule (the service role at the site's company, and a `system:` principal), journaled, and fanned out.
Billing records each event id in `billing_consumed_events` through an `IReplayLedger`, in the line's transaction, so
a redelivery answers the line it made. A result with no price in force is billed `Unpriced` rather than refused. The
server relays once a second, on a background thread a `QTimer` posts to.

**Hypothesis 2 is refuted for this instance.** The criterion, set before measuring, was the design's: each event
received exactly once in effect across a restart, with only wiring code in `lims/app` and no per-consumer delivery
loop or cursor. Measured in `tests/test_domain_events.cpp`: a relay that delivers and dies before marking leaves one
line and the event still in the outbox; a fresh relay redelivers it and billing answers the same line. The delivery
loop is `OutboxRelay`'s, the cursor is the outbox's own rows, and what `lims/app` holds for this consumer is wiring: a
sink that decodes and dispatches, and the relay's configuration. The internal client is [measured] lines in
`app/` and is shared by every background job. A consumer that refuses an event stops the relay at it: `OutboxRelay`
delivers a batch in order and marks it only whole, so every later event waits behind a poison one — measured in the
same file, and the reason billing records an unpriced line rather than refusing it. Inventory's consumer of
`lab.ResultCaptured` in Part C is the second instance.
```

Confirmed instead (a loop or cursor of the consumer's own was needed, or a redelivery billed twice): replace the bold paragraph with "**Hypothesis 2 is confirmed for this instance.** The criterion, set before measuring, was the design's: [as above]. Measured in `tests/test_domain_events.cpp`: [what failed — the loop or cursor the consumer needed, with the file and lines Step 9 printed, or the double line]." Mis-framed instead (delivery held with wiring alone, but for a reason the hypothesis does not name — for instance only because the consumer is an action dispatched like a user's, which no part of `OutboxRelay` or `IReplayLedger` provides): "**Hypothesis 2 is mis-framed for this instance.** [What carried delivery, and why the hypothesis's premise does not describe it.]"

- [ ] **Step 11: Commit**

```bash
cd /home/yaraslau/repo/morph
git add examples/common/testkit examples/lims/org examples/lims/lab examples/lims/billing examples/lims/app \
        examples/lims/src/server/main.cpp examples/lims/tests examples/lims/README.md
git commit -m "Bill each published lims result once, through lab's outbox, morph's relay and a dispatched consumer

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---
### Task 10: Invoices — drafts, totals, tax, and the invoice's runtime currency (hypothesis 8, the invoice half of instance 2)

**Files:**
- Create: `examples/lims/billing/include/lims/billing/dto/invoice_dto.hpp`, `examples/lims/billing/include/lims/billing/core/invoice_math.hpp`, `examples/lims/billing/src/core/invoice_math.cpp`, `examples/lims/billing/include/lims/billing/db/billing_store.hpp`, `examples/lims/billing/src/models/invoice_drafts.cpp`, `examples/lims/tests/test_invoices.cpp`
- Modify: `billing_entity.hpp` (`InvoiceRecord`), `billing_schema.cpp`, `invoice_model.hpp`, `billing_authorization.hpp`, `declared_requirements.hpp`, `test_declared_authorization.cpp`, `examples/lims/README.md` (decision 32)

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

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <morph/forms/forms.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <vector>

#include "billing_test_support.hpp"
#include "lims/app/event_wiring.hpp"
#include "lims/billing/core/invoice_math.hpp"
#include "lims/billing/db/billing_entity.hpp"
#include "lims/billing/models/invoice_model.hpp"
#include "lims/org/core/calendar.hpp"
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
    lims::test::relayLabEvents();
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
    lims::test::relayLabEvents();
    ScopedPrincipal const bea{"bea"};
    CHECK_THROWS_WITH(lims::InvoiceModel{}.execute(lims::CreateInvoice{
                          .company = lims::test::seededCompany(), .site = lims::test::seededSite(), .client = lab.client}),
                      Catch::Matchers::ContainsSubstring("billing settings"));
}

// Hypothesis 8, the invoice half of instance 2: the currency is known only from the record, so the model keeps it.
// Mutation: drop the currency check from draftInvoice (the yen line joins the euro invoice; the refusal check fails).
TEST_CASE("Hypothesis 8: an amount's currency is a field the type does not see, so the model keeps currencies apart",
          "[lims][billing][h8]") {
    DbFixture fixture;
    settings(exact(19, 100, 6));
    auto const lab = lims::test::publishedSample();
    lims::test::billingFor(lab.client);
    lims::test::relayLabEvents();
    // A second unbilled line of the same client at the same site, priced in yen: what a price list in another
    // currency would leave behind. Written below the model, since no action makes one.
    {
        Lightweight::DataMapper mapper;
        lims::db::BillableLineRecord yen;
        yen.companyId = *lims::test::seededCompany();
        yen.siteId = *lims::test::seededSite();
        yen.clientId = *lab.client;
        yen.sampleId = *lab.sample;
        yen.resultId = 9001;
        yen.analysisId = *lab.nitrate;
        yen.publishedAt = lims::millisOf(morph::time::Timestamp{lims::test::at(2026, 11, 10)});
        yen.state = static_cast<int>(lims::BillableLineState::Unbilled);
        yen.amountNum = 4000;
        yen.amountDen = 1;
        yen.currency = static_cast<int>(lims::CurrencyCode::JPY);
        yen.eventId = Lightweight::SqlAnsiString<128>{"test:yen:1"};
        mapper.Create(yen);
    }
    // Both amounts are `lims::Amount`, the one money unit, so nothing in the type refuses 40 euros beside 4000 yen;
    // the model does.
    {
        ScopedPrincipal const bea{"bea"};
        CHECK_THROWS_WITH(lims::InvoiceModel{}.execute(lims::CreateInvoice{.company = lims::test::seededCompany(),
                                                                           .site = lims::test::seededSite(),
                                                                           .client = lab.client}),
                          Catch::Matchers::ContainsSubstring("is in JPY"));
    }
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

`InvoiceRecord` (`billing_invoices`): `id`; `company_id`, `site_id`, `client_id` (`std::int64_t`); `currency`, `state` (`int`); `number` `SqlAnsiString<32>`; `net_num`/`_den`, `tax_num`/`_den`, `gross_num`/`_den`; `reference` `SqlAnsiString<128>`; `created_by`, `submitted_by`, `approved_by`, `posted_by` `SqlAnsiString<64>`; `created_at`, `submitted_at`, `deadline_at` `std::int64_t`; `gross_company_num`/`_den` `std::optional<std::int64_t>` (the gross in the company's currency, set at submission: Tasks 12 and 14); `entry_id` `std::optional<std::int64_t>`. Migration `20261110000008 "Create billing_invoices"`.

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

Implement both in `invoice_drafts.cpp`. `draftInvoice`: read `PriceListModel::settingsOf(company)` (`NotFound` "company N has no billing settings") and `PriceListModel::billingClient(client, company)` (`NotFound`); select the `Unbilled` lines with that company, site and client; none → `std::nullopt`; every line's currency must equal the client's, else `ValidationError` "line N is in <currency>; client M is billed in <currency>" (the type cannot keep currencies apart, so the model does); `invoiceTotals` over their amounts at the settings' tax rate; create the `InvoiceRecord` (state `Draft`, `created_by`, `created_at = nowMillis()`), then set each line `Invoiced` with its `invoice_id`. `invoiceView` maps the row (range-checking `state` and `currency`), loads its lines by `invoice_id`, and sets `level = deadline_at > 0 && nowMillis() >= deadline_at ? ApprovalLevel::Company : ApprovalLevel::Site`. `InvoiceModel::execute(CreateInvoice)`: the conventions' action shape, then `draftInvoice` inside one transaction; `std::nullopt` → `Conflict` "client N has no unbilled lines at site M". `GetInvoice` answers `NotFound` for an invoice of a company the caller cannot see; `ListInvoices` answers the visible company's invoices by id.

Declare `template <> struct Authorization<CreateInvoice> : CompanyRule<CreateInvoice, LimsRole::BillingClerk> {};`, register `"CreateInvoice"` with `BRIDGE_REGISTER_EFFECTS(lims::CreateInvoice, ::morph::effects::changes<lims::InvoiceView>, ::morph::effects::changes<lims::BillableLineView>)`, `"GetInvoice"` and `"ListInvoices"` as reads, add `CreateInvoice` to the edge table and the coverage tuple, `"InvoiceModel"` to its model list.

- [ ] **Step 5: Build and run**

Run: Build lims, Model suite `"[invoices]"`, Model suite `"[h8]"`, Model suite `"[authorization]"`. Expected: all pass. Run the three mutations named in the file and restore them.

- [ ] **Step 6: Record decision 32**

```markdown
### 32. An invoice is one client's lines at one site, in the client's currency; the type does not know which (hypothesis 8, invoice half of instance 2)

`CreateInvoice` collects every unbilled line of a client at a site and marks them invoiced; tax is the company's flat
rate on the net total, rounded half-up once, and the gross is net plus tax. The site is the invoice's because its
approval is judged there (decision 34).

**Hypothesis 8's runtime-currency half is confirmed for invoices.** Measured in `tests/test_invoices.cpp`: with the
currency known only from the record, the amount is a `Quantity` over the one money unit, so a yen line among a euro
client's lines is refused by `draftInvoice`, not by the type, and the served schema of a JPY price's form still
advertises `x-decimalPlaces: 2` — the declared precision is the type's. What keeps currencies apart is model code:
`draftInvoice` refuses a line in another currency and `fitsCurrency` refuses fractional yen. `Quantity` holds the amount exactly; it cannot hold its currency.
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
- Modify: `billing_entity.hpp` (`BillingRunRecord`, `BillingRunItemRecord`), `billing_schema.cpp`, `invoice_drafts.cpp` (a fault point), `billing_authorization.hpp`/`.cpp`, `declared_requirements.hpp`, `test_declared_authorization.cpp`, `src/server/main.cpp`, `examples/lims/README.md` (decision 33)

**Interfaces:**
- Consumes: `draftInvoice`, `PriceListModel::settingsOf`, `dayOfMonth`, `monthLabel`, `OrgModel::rolesIn`, `lims::app::InternalClient`, `lims::app::kSchedulePrincipal`, `requireServicePrincipal`, `BackendRig::connect`.
- Produces: `void lims::setFaultHook(std::function<void(std::string_view)>)`, `void lims::faultPoint(std::string_view)`; `StartBillingRun { CompanyId company; }` → `BillingRunView { BillingRunId id; CompanyId company; std::string label; BillingRunState state; Count total; Count done; Timestamp startedAt; std::string startedBy; }`; `AdvanceBillingRun { BillingRunId runId; Count batch; }` → `BillingRunView`; `RunBillingSchedule { Count batch; }` → `BillingScheduleResult { std::vector<BillingRunView> runs; }`; `ListBillingRuns { CompanyId company; }` → `ListBillingRunsResult`; model `lims::BillingRunModel` (`"BillingRunModel"`); record `Billing_Run`; `class lims::app::BillingScheduler { BillingScheduler(InternalClient&, Count batch); BillingScheduleResult tick(); }`.

A run belongs to a company and a calendar month (`label`, `YYYY-MM`): at most one per pair. Starting it writes one item per (client, site) with unbilled lines — the run's cursor. Advancing drafts one item's invoice and marks the item done in one transaction, so an interruption anywhere leaves each item either wholly done or wholly pending, and a resumed run drafts only what is pending. Progress is `done` of `total`.

The schedule is one action, `RunBillingSchedule`, dispatched by the app through an internal client as `system:billing-schedule`: each tick starts the run of every company due and served, then advances every unfinished run by one batch. One batch per tick, each tick a dispatch, is what makes progress observable: every tick's `changes<BillingRunView>` reaches a watching clerk, and the run's `done` moves between ticks rather than jumping to the end inside one call. The schedule acts only for companies where its principal holds `LimsRole::Service`, so what it may do is in the role assignments.

- [ ] **Step 1: Write the failing test**

Create `examples/lims/tests/test_billing_run.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Hypothesis 4, first instance: the billing run starts on its day, advances one batch per scheduled tick so its
// progress is seen, and a run interrupted mid-way resumes on the next tick without drafting any invoice twice. The
// schedule dispatches through an internal client, as the server's does.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <map>
#include <morph/session/session_auth.hpp>
#include <morph/util/rational.hpp>
#include <stdexcept>
#include <string>

#include "billing_test_support.hpp"
#include "clock.hpp"
#include "lims/app/billing_scheduler.hpp"
#include "lims/app/internal_client.hpp"
#include "lims/app/service_principals.hpp"
#include "lims/billing/core/fault_hook.hpp"
#include "lims/billing/models/billing_run_model.hpp"
#include "lims/billing/models/invoice_model.hpp"
#include "testkit/backend_rig.hpp"
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
///        day 25; the schedule principal holding the service role at the seeded company. The lines are relayed.
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
    lims::test::relayLabEvents();
    lims::test::grantService(std::string{lims::app::kSchedulePrincipal});
}

/// @brief A server of the case's own and the schedule's client on it, as the server process holds them.
struct ScheduleServer {
    morph::ladder::testkit::BackendRig rig{morph::ladder::testkit::Mode::Socket, 0,
                                           lims::test::limsAuthorizer(lims::test::kServiceSecret)};
    lims::app::InternalClient client{rig.server(), lims::test::kServiceSecret,
                                     std::string{lims::app::kSchedulePrincipal}};
};

/// @brief A batch of @p items.
[[nodiscard]] lims::Count batchOf(std::int64_t items) { return lims::Count{Rational{items, DecimalPlaces{0}}}; }

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

// Mutation: let dueCompanies ignore an existing run for the month (the later tick starts a second run, refused with
// Conflict, which the tick reports).
TEST_CASE("Hypothesis 4: the billing run starts on its day of the month, once", "[lims][billing][run][h4]") {
    DbFixture fixture;
    threeClientsToBill();
    ScheduleServer server;
    lims::app::BillingScheduler scheduler{server.client, batchOf(50)};
    {
        ScopedClockOverride const clock{lims::test::at(2026, 11, 24)};
        CHECK(scheduler.tick().runs.empty());
        CHECK(runs().empty());
    }
    {
        ScopedClockOverride const clock{lims::test::at(2026, 11, 25)};
        static_cast<void>(scheduler.tick());
        REQUIRE(runs().size() == 1U);
        CHECK(runs()[0].label == "2026-11");
        CHECK(runs()[0].state == lims::BillingRunState::Finished);
        CHECK(runs()[0].startedBy == lims::app::kSchedulePrincipal);
    }
    ScopedClockOverride const later{lims::test::at(2026, 11, 25, 15)};
    static_cast<void>(scheduler.tick());
    CHECK(runs().size() == 1U);
    CHECK(invoicesPerClient().size() == 3U);
}

// Mutation: advance every running run to its end in one tick (the first tick answers 3 done, and the watcher hears
// one change instead of three).
TEST_CASE("billing run: each tick advances one batch, and a watching clerk sees the progress change",
          "[lims][billing][run]") {
    DbFixture fixture;
    threeClientsToBill();
    ScopedClockOverride const clock{lims::test::at(2026, 11, 25)};
    ScheduleServer server;
    morph::session::TokenIssuer const issuer{std::string{lims::test::kServiceSecret}, morph::session::hmacSha256};
    auto const watcher = server.rig.connect();
    REQUIRE(watcher->watch({"Billing_Run"}, lims::test::signedAs(issuer, "bea")).kind == "ok");
    lims::app::BillingScheduler scheduler{server.client, batchOf(1)};
    std::size_t heard = 0;
    for (std::int64_t done = 1; done <= 3; ++done) {
        CAPTURE(done);
        auto const ticked = scheduler.tick();
        REQUIRE(ticked.runs.size() == 1U);
        CHECK(*ticked.runs[0].done == Rational{done, DecimalPlaces{0}});
        CHECK(*ticked.runs[0].total == Rational{3, DecimalPlaces{0}});
        CHECK(morph::ladder::testkit::ServerConnection::eventually(
            [&] { return watcher->changes().size() > heard && watcher->heardOf("Billing_Run"); }));
        heard = watcher->changes().size();
    }
    CHECK(runs()[0].state == lims::BillingRunState::Finished);
}

TEST_CASE("billing run: a clerk starts and advances a run by hand, item by item", "[lims][billing][run]") {
    DbFixture fixture;
    threeClientsToBill();
    ScopedClockOverride const clock{lims::test::at(2026, 11, 25)};
    ScopedPrincipal const bea{"bea"};
    lims::BillingRunModel model;
    auto run = model.execute(lims::StartBillingRun{.company = lims::test::seededCompany()});
    CHECK(*run.total == Rational{3, DecimalPlaces{0}});
    CHECK(*run.done == Rational{0, DecimalPlaces{0}});
    run = model.execute(lims::AdvanceBillingRun{.runId = run.id, .batch = batchOf(1)});
    CHECK(*run.done == Rational{1, DecimalPlaces{0}});
    CHECK(run.state == lims::BillingRunState::Running);
    run = model.execute(lims::AdvanceBillingRun{.runId = run.id, .batch = batchOf(5)});
    CHECK(run.state == lims::BillingRunState::Finished);
    CHECK_THROWS_AS(model.execute(lims::StartBillingRun{.company = lims::test::seededCompany()}), lims::Conflict);
}

// Mutation: drop requireServicePrincipal from RunBillingSchedule (bea's tick starts the run).
TEST_CASE("billing run: only the server's own client runs the schedule", "[lims][billing][run]") {
    DbFixture fixture;
    threeClientsToBill();
    lims::test::grantService("bea");
    ScopedClockOverride const clock{lims::test::at(2026, 11, 25)};
    ScopedPrincipal const bea{"bea"};
    CHECK_THROWS_WITH(lims::BillingRunModel{}.execute(lims::RunBillingSchedule{.batch = batchOf(50)}),
                      Catch::Matchers::ContainsSubstring("only the server's own clients"));
    CHECK(runs().empty());
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
        ScheduleServer server;
        lims::app::BillingScheduler scheduler{server.client, batchOf(50)};
        CHECK_THROWS_WITH(scheduler.tick(), Catch::Matchers::ContainsSubstring("server stopped"));
    }
    REQUIRE(runs().size() == 1U);
    CHECK(runs()[0].state == lims::BillingRunState::Running);
    CHECK(*runs()[0].done == Rational{1, DecimalPlaces{0}});
    CHECK(invoicesPerClient().size() == 1U);

    // The restart: the hook is gone, a new server and scheduler tick, and the run resumes where it stood.
    lims::setFaultHook({});
    ScheduleServer restarted;
    lims::app::BillingScheduler scheduler{restarted.client, batchOf(50)};
    ScopedClockOverride const later{lims::test::at(2026, 11, 25, 10)};
    static_cast<void>(scheduler.tick());
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

`run_dto.hpp`: the structs from Interfaces; `StartBillingRun::validate()` requires the company; `AdvanceBillingRun::validate()` the run and a batch; `RunBillingSchedule::validate()` a batch (documented "One tick of the billing schedule: starts the run of every company due whose service role the caller holds, then advances every unfinished run of those companies by one batch. Sent only by the app's schedule, as a service principal."); `BillingScheduleResult` (documented "The runs the tick advanced, as they stand after it"); `BRIDGE_REGISTER_RECORD(lims::BillingRunView, "Billing_Run")`.

Records: `BillingRunRecord` (`billing_runs`: `id`, `company_id`, `label` `SqlAnsiString<7>`, `state` int, `started_at`, `started_by` `SqlAnsiString<64>`) and `BillingRunItemRecord` (`billing_run_items`: `id`, `BelongsTo<&BillingRunRecord::id> run_id`, `client_id`, `site_id`, `state` int — 0 pending, 1 done — and `invoice_id` `std::optional<std::int64_t>`). Migration `20261110000009 "Create billing_runs and billing_run_items"`.

`billing_run_model.cpp` — the run's two steps once, in the anonymous namespace, and the three actions over them:

```cpp
namespace {

/// @brief A batch's size as a whole number of items.
[[nodiscard]] std::int64_t itemsIn(Count const& batch) { return (*batch).numerator / (*batch).denominator; }

/// @brief Starts @p company's run for the month of now, in the caller's transaction: the run and one pending item per
///        client and site with unbilled lines.
/// @return The run's id.
/// @throws Conflict if the company already ran this month.
std::int64_t startRun(Lightweight::DataMapper& mapper, CompanyId company, std::string const& author) {
    std::string const label = monthLabel(calendarMonthOf(::morph::ladder::now()));
    if (!mapper.Query<db::BillingRunRecord>()
             .Where(::Lightweight::FieldNameOf<&db::BillingRunRecord::companyId>, "=", *company)
             .Where(::Lightweight::FieldNameOf<&db::BillingRunRecord::label>, "=", label)
             .All()
             .empty()) {
        throw Conflict{"company " + std::to_string(*company) + " already ran " + label};
    }
    db::BillingRunRecord run;
    run.companyId = *company;
    run.label = Lightweight::SqlAnsiString<7>{label};
    run.state = static_cast<int>(BillingRunState::Running);
    run.startedAt = nowMillis();
    run.startedBy = Lightweight::SqlAnsiString<64>{author};
    mapper.Create(run);
    std::set<std::pair<std::int64_t, std::int64_t>> pairs;
    for (auto const& line : mapper.Query<db::BillableLineRecord>()
                                .Where(::Lightweight::FieldNameOf<&db::BillableLineRecord::companyId>, "=", *company)
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
    return static_cast<std::int64_t>(run.id.Value());
}

/// @brief Drafts up to @p items pending items of run @p runId, and finishes the run when none is left.
void advanceRun(Lightweight::DataMapper& mapper, std::int64_t runId, CompanyId company, std::int64_t items,
                std::string const& author) {
    for (auto item : mapper.Query<db::BillingRunItemRecord>()
                         .Where(::Lightweight::FieldNameOf<&db::BillingRunItemRecord::run>, "=", runId)
                         .Where(::Lightweight::FieldNameOf<&db::BillingRunItemRecord::state>, "=", 0)
                         .All()) {
        if (items-- == 0) {
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
                              .Where(::Lightweight::FieldNameOf<&db::BillingRunItemRecord::run>, "=", runId)
                              .Where(::Lightweight::FieldNameOf<&db::BillingRunItemRecord::state>, "=", 0)
                              .All()
                              .empty();
    if (!pending) {
        auto runs = mapper.Query<db::BillingRunRecord>()
                        .Where(::Lightweight::FieldNameOf<&db::BillingRunRecord::id>, "=", runId)
                        .All();
        runs.front().state = static_cast<int>(BillingRunState::Finished);
        mapper.Update(runs.front());
    }
}

}  // namespace

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
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    std::int64_t const id = startRun(mapper, action.company, ::morph::session::current()->principal);
    sqlTxn.Commit();
    return runView(mapper, id);
}

BillingRunView BillingRunModel::execute(const AdvanceBillingRun& action) {
    requirePrincipal();
    if (!action.validate() || itemsIn(action.batch) < 1) {
        throw ValidationError{"AdvanceBillingRun: a run and a batch of at least one are required"};
    }
    requireRoleSomewhere<AdvanceBillingRun>();
    Lightweight::DataMapper mapper;
    auto const runs = mapper.Query<db::BillingRunRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::BillingRunRecord::id>, "=", *action.runId)
                          .All();
    if (runs.empty()) {
        throw NotFound{"AdvanceBillingRun: run " + std::to_string(*action.runId) + " does not exist"};
    }
    requireAuthorized(action);
    advanceRun(mapper, *action.runId, CompanyId{runs.front().companyId.Value()}, itemsIn(action.batch),
               ::morph::session::current()->principal);
    return runView(mapper, *action.runId);
}

BillingScheduleResult BillingRunModel::execute(const RunBillingSchedule& action) {
    requirePrincipal();
    if (!action.validate() || itemsIn(action.batch) < 1) {
        throw ValidationError{"RunBillingSchedule: a batch of at least one is required"};
    }
    requireServicePrincipal("RunBillingSchedule");
    requireRoleSomewhere<RunBillingSchedule>();
    requireAuthorized(action);
    std::string const principal = ::morph::session::current()->principal;
    // The schedule acts only where its principal holds the service role, so the role assignments say what it may do.
    auto const serves = [&principal](CompanyId company) {
        auto const held = OrgModel::rolesIn(principal, Scope{company});
        return std::ranges::find(held, LimsRole::Service) != held.end();
    };
    Lightweight::DataMapper mapper;
    for (CompanyId const company : dueCompanies(mapper, ::morph::ladder::now())) {
        if (serves(company)) {
            Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
            static_cast<void>(startRun(mapper, company, principal));
            sqlTxn.Commit();
        }
    }
    BillingScheduleResult result;
    for (auto const& run : runningRuns(mapper)) {
        CompanyId const company{run.companyId.Value()};
        if (serves(company)) {
            auto const id = static_cast<std::int64_t>(run.id.Value());
            advanceRun(mapper, id, company, itemsIn(action.batch), principal);
            result.runs.push_back(runView(mapper, id));
        }
    }
    return result;
}
```

`runView(mapper, id)`, `dueCompanies(mapper, now)` and `runningRuns(mapper)` are in the anonymous namespace too: `runView` maps the run with `total` = its items and `done` = its done items, as `Count`s; `dueCompanies` answers every company with billing settings whose `billing_day <= dayOfMonth(now)` and no run labelled `monthLabel(calendarMonthOf(now))`; `runningRuns` every `BillingRunRecord` in `Running`. `ListBillingRuns` is the conventions' list. Header Doxygen as Task 4's (each `@throws`, including `NotFound` for a company without settings, `Conflict` for a second run in a month, and for `RunBillingSchedule` `Forbidden` "only the server's own clients send it"). Registrations: `"StartBillingRun"`, `"AdvanceBillingRun"` and `"RunBillingSchedule"`, each with `BRIDGE_REGISTER_EFFECTS(…, ::morph::effects::changes<lims::BillingRunView>, ::morph::effects::changes<lims::InvoiceView>, ::morph::effects::changes<lims::BillableLineView>)`; `"ListBillingRuns"` as a read. Rules: `Authorization<StartBillingRun> : CompanyRule<StartBillingRun, LimsRole::BillingClerk>`; `Authorization<AdvanceBillingRun>` with `role = BillingClerk` and `scopeOf` reading the run's company in `billing_authorization.cpp`; `Authorization<RunBillingSchedule>` with `role = LimsRole::Service` and no scope (it spans companies; `serves` above judges each). Add the three to the edge table and the coverage tuple, `"BillingRunModel"` to its model list.

- [ ] **Step 5: The scheduler, in the app**

Create `examples/lims/app/include/lims/app/billing_scheduler.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/util/rational.hpp>

#include "lims/app/internal_client.hpp"
#include "lims/billing/dto/run_dto.hpp"
#include "lims/org/core/money.hpp"

/// @file
/// The billing schedule: each tick is one dispatched `RunBillingSchedule`, as the schedule principal. The server
/// ticks it once at start-up, which resumes a run a restart interrupted, and every ten seconds after.

namespace lims::app {

/// @brief Ticks billing's schedule through an internal client.
class BillingScheduler {
public:
    /// @brief A schedule dispatching through @p client.
    /// @param client The schedule principal's client. Borrowed: it must outlive this scheduler.
    /// @param batch How many items one tick drafts per run.
    explicit BillingScheduler(InternalClient& client,
                              Count batch = Count{::morph::math::Rational{50, ::morph::math::DecimalPlaces{0}}})
        : _client{client}, _batch{std::move(batch)} {}

    /// @brief One tick: starts each due company's run and advances each unfinished run by one batch.
    /// @return The runs it advanced.
    /// @throws std::exception the dispatch's refusal or failure; what committed stays, and the next tick resumes.
    BillingScheduleResult tick();

private:
    // Borrowed for the scheduler's life; never reseated.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    InternalClient& _client;
    Count _batch;
};

}  // namespace lims::app
```

and `billing_scheduler.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "lims/app/billing_scheduler.hpp"

#include "lims/billing/models/billing_run_model.hpp"

namespace lims::app {

BillingScheduleResult BillingScheduler::tick() {
    return _client.call<BillingRunModel>(RunBillingSchedule{.batch = _batch});
}

}  // namespace lims::app
```

- [ ] **Step 6: The server ticks it**

In `src/server/main.cpp` (include `"lims/app/billing_scheduler.hpp"`), declare the schedule's client and scheduler beside `events`, above `background`, so `background` joins before either goes:

```cpp
        lims::app::InternalClient schedule{*server, secret, std::string{lims::app::kSchedulePrincipal}};
        lims::app::BillingScheduler scheduler{schedule};
```

and after the relay timer:

```cpp
        // The billing schedule: once now, which resumes a run a restart interrupted, then every ten seconds, one batch
        // per run per tick.
        auto const tickSchedule = [&background, &scheduler] {
            background.post([&scheduler] {
                try {
                    static_cast<void>(scheduler.tick());
                } catch (std::exception const& error) {
                    std::cerr << "lims-server: billing schedule: " << error.what() << '\n';
                }
            });
        };
        tickSchedule();
        QTimer scheduleTimer;
        QObject::connect(&scheduleTimer, &QTimer::timeout, tickSchedule);
        scheduleTimer.start(10'000);
```

- [ ] **Step 7: Build and run**

Run: Build lims, Model suite `"[run]"`, Model suite `"[authorization]"`, Everything labelled for the rung. Expected: all pass. Run the four mutations named in the file and restore them.

- [ ] **Step 8: Measure what the internal client cost, and whether it could do the job**

```bash
cd /home/yaraslau/repo/morph
wc -l examples/lims/app/include/lims/app/billing_scheduler.hpp examples/lims/app/src/billing_scheduler.cpp
git diff --stat "$(cat /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/plan_b_base.txt)" -- examples/lims/src/server/main.cpp
grep -rn 'ScopedContext' examples/lims --include='*.cpp' --include='*.hpp' | grep -v '/tests/'
```

Keep the output for decision 33. Expected: the scheduler is a handful of lines over `InternalClient` (Task 9's, unchanged); `main.cpp` gains a client, a scheduler and a timer; the last command prints nothing — no lims code outside the tests installs a session by hand.

If the internal client genuinely could not run the schedule — a dispatch it cannot make, a principal the edge cannot admit, a result it cannot wait for — record exactly why in decision 33, with the failing output, and file the missing seam, since bookmarks' and bank's servers already decline `morph::session::detail::ScopedContext` (`examples/bookmarks/src/server/main.cpp`, `examples/bank/src/server/main.cpp`) and lims would be the third place. First `gh label list --limit 100 | grep -i 'area'` and pick the `area:` label for sessions (`area: session` at the time of writing). Write the body to `/tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/plan_b_rev_scoped_context_issue.md`: the verification status (what was measured, on `git rev-parse HEAD`, what was not); the real output of the failing case; the three places, with the comment in each server that declines `ScopedContext`; and what would close it (a public way to run a model action as a principal outside a dispatch, or an internal client that does the job). Then:

```bash
cd /home/yaraslau/repo/morph
gh issue create --title "No public way to run a model action as a principal outside a dispatch" \
  --label enhancement --label "area: session" \
  --body-file /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/plan_b_rev_scoped_context_issue.md
```

No `triage:` label. If the internal client did the job, file nothing.

- [ ] **Step 9: Record decision 33**

The confirmed text (the expected outcome, since the trigger and the tick action are lims's own):

```markdown
### 33. The billing run is a cursor in billing's tables, ticked through an internal client by a timer in the server (hypothesis 4, first instance)

A run belongs to a company and a month; starting it records one item per client and site with unbilled lines, and
advancing it drafts one item's invoice and marks the item done in one transaction. A run interrupted anywhere leaves
every item wholly done or wholly pending, and resuming drafts only what is pending, so no invoice is drafted twice.
Progress is the run's done items of its total. The schedule is one action, `RunBillingSchedule`: it starts every
due company's run and advances every unfinished run by one batch. `lims::app::BillingScheduler` dispatches it through
an internal client as `system:billing-schedule`, which acts only where it holds the service role; the server ticks
it at start-up and every ten seconds, so each tick's change to `Billing_Run` reaches a watching clerk.

**Hypothesis 4 is confirmed for this instance.** Measured in `tests/test_billing_run.cpp`: the run starts on its day
and once; each tick advances one batch, and a clerk watching `Billing_Run` is told after each; a run failed after its
first invoice shows 1 of 3 done, and a fresh server and scheduler after the "restart" finish it with one invoice per
client. The dispatch is public (an internal client, [measured] lines over Task 9's), but nothing in morph starts work
on a schedule or resumes it: the trigger is a `QTimer` in the server, the resume is a cursor table of billing's own,
and the tick is billing's own action, a scheduler per module. `TimeoutScheduler`, the one timer morph has, is a
detail type. Approval escalation, the design's second candidate, is not scheduled work (decision 34), so this is the
one instance Part B has.
```

If the internal client could not do it, add after the bold paragraph: "The internal client could not [what], measured as [output]; the missing seam is filed against `include/morph`." If the schedule ran with public morph primitives and no scheduler of billing's own, use instead: "**Hypothesis 4 is refuted for this instance.** [Which public primitives started and resumed it.]"

- [ ] **Step 10: Commit**

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/billing examples/lims/app examples/lims/src/server/main.cpp examples/lims/tests \
        examples/lims/README.md
git commit -m "Schedule the lims billing run through an internal client, one batch per tick, resumable after a restart

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---
### Task 12: Invoice approval — four eyes, value limits, a deadline that escalates (hypotheses 5 and 6; escalation measured against hypothesis 4)

**Files:**
- Create: `examples/lims/billing/src/models/invoice_approval.cpp`, `examples/lims/tests/test_invoice_approval.cpp`, `examples/lims/screens/tests/test_lims_approval_inbox.cpp`
- Modify: `examples/lims/org/include/lims/org/authorization/declared.hpp` (several separation pairs per action, `authorizationFactsOf`), `invoice_dto.hpp` (three actions), `invoice_model.hpp`, `billing_authorization.hpp`/`.cpp`, `declared_requirements.hpp`, `test_declared_authorization.cpp`, `billing_test_support.hpp` (`dispatchAs`, `approvalSetUp`, `draftedInvoice`, `submit`), `examples/lims/README.md` (decisions 34 and 35)

**Interfaces:**
- Consumes: `Authorization`, `requireAuthorized`, `authorizationVerdict`, `AuthorizationFacts`, `OrgModel::rolesIn`, `OrgModel::valueLimit`, `OrgModel::findSite`, `OrgModel::findCompany`, `PriceListModel::settingsOf`, `invoiceView`, `RunBillingSchedule`, `ScreenHarness`.
- Produces: `SubmitInvoice { InvoiceId invoiceId; }`, `ApproveInvoice { InvoiceId invoiceId; }` → `InvoiceView`; `ListPendingApprovals {}` → `ListInvoicesResult`; in `declared.hpp`, `struct lims::SeparatedFrom { std::string_view action; std::string_view refusal; }`, `concept lims::DeclaresSeparations` (`static constexpr std::array<SeparatedFrom, N> separations` and `static std::array<std::optional<std::string>, N> firstPerformersOf(Action const&)`), `struct lims::JudgedFacts { AuthorizationFacts facts; std::string where; std::string sameActorRefusal; }` and `JudgedFacts lims::authorizationFactsOf<Action>(Action const&, std::string const& principal)`; `Authorization<ApproveInvoice>` with `role = InvoiceApprover`, `scopeOf` (the invoice's site before its deadline, its company after), `amountOf` (gross in company currency, as submission recorded it), `limitOf` (the principal's value limit at the scope's company), `separations` (`CreateInvoice`, `SubmitInvoice`) and `firstPerformersOf` (the invoice's creator and submitter); `Authorization<SubmitInvoice>` (billing clerk at the invoice's company); test helpers `R lims::test::dispatchAs<Model>(std::string const& principal, Action)`, `LabFixture lims::test::approvalSetUp()`, `InvoiceView lims::test::draftedInvoice()`, `void lims::test::submit(InvoiceView const&)`.

Lifecycle: `Draft → Submitted` (a billing clerk; the deadline is now plus the company's approval hours; the gross in company currency is recorded), `Submitted → Approved` (an invoice approver who neither created nor submitted it, whose value limit covers its gross in company currency, holding the role at the invoice's site — or, once the deadline has passed, at its company). The design's lifecycle is draft → submitted → approved → posted and names no way back, so there is no return-to-draft action. Escalation is the deadline the rule reads: nothing runs at the deadline, the scope the rule asks for changes, and `ListPendingApprovals` shows an invoice to whoever could approve it now.

Two separation pairs guard approval. `CreateInvoice` ↔ `ApproveInvoice` is the design's "one principal may not both create and approve one invoice"; `SubmitInvoice` ↔ `ApproveInvoice` keeps four eyes on an invoice the billing run drafted, whose creator is the schedule's service principal, so that the clerk who submits it is not also the one who approves it. Part A's declaration holds one pair; `declared.hpp` grows a form for several, and lab's single pair keeps its form.

A foreign-currency invoice's value in company currency needs the ledger's dated rate, which billing reaches only from Task 14 on: until then `SubmitInvoice` refuses one ("invoice N is in USD and no rate values it in EUR"), and this task's cases are in the company's currency.

- [ ] **Step 1: Several separation pairs per action, and the facts a rule is judged on**

In `examples/lims/org/include/lims/org/authorization/declared.hpp` (include `<array>` and `<cstddef>`), after `DeclaresSeparation`:

```cpp
/// @brief One separation-of-duties pair of an action with several: the first half's action id and the refusal.
struct SeparatedFrom {
    /// @brief The first half's registered action id.
    std::string_view action;
    /// @brief What the refusal says after the principal's name, such as "created this invoice and cannot also
    ///        approve it".
    std::string_view refusal;
};

/// @brief Whether @p Action declares several separation-of-duties pairs: `separations`, and `firstPerformersOf`
///        answering who performed each pair's first half on the same subject, in the same order.
/// @tparam Action The action type.
template <typename Action>
concept DeclaresSeparations = DeclaresRule<Action> && requires(Action const& action) {
    { Authorization<Action>::separations.size() } -> std::convertible_to<std::size_t>;
    { Authorization<Action>::firstPerformersOf(action)[0] } -> std::convertible_to<std::optional<std::string>>;
};

/// @brief A rule's facts for one action and principal, with where it was judged and the words a same-actor refusal
///        uses.
struct JudgedFacts {
    /// @brief The facts.
    AuthorizationFacts facts;
    /// @brief The scope, in words: "at company 2", or "in any scope".
    std::string where;
    /// @brief The refusal's words when the verdict is `SameActor`; empty without a pair.
    std::string sameActorRefusal;
};
```

Replace the body of `requireAuthorized` with a call to a new `authorizationFactsOf`, which holds what its body built, so the pending list (Step 6) judges by the same facts:

```cpp
/// @brief Gathers what @p Action's rule judges @p principal on, from the declaration and the payload.
/// @tparam Action A declared action.
/// @param action The action.
/// @param principal The principal.
/// @return The facts, where they were judged, and a same-actor refusal's words.
template <typename Action>
    requires DeclaresRule<Action>
[[nodiscard]] JudgedFacts authorizationFactsOf(Action const& action, std::string const& principal) {
    using Rule = Authorization<Action>;
    JudgedFacts judged{.facts = AuthorizationFacts{.principal = principal,
                                                   .required = Rule::role,
                                                   .held = {},
                                                   .firstPerformer = std::nullopt,
                                                   .amount = std::nullopt,
                                                   .limit = std::nullopt},
                       .where = "in any scope",
                       .sameActorRefusal = {}};
    if constexpr (DeclaresScope<Action>) {
        Scope const scope = Rule::scopeOf(action);
        judged.facts.held = OrgModel::rolesIn(principal, scope);
        judged.where = "at " + scopeName(scope);
        if constexpr (DeclaresValueLimit<Action>) {
            judged.facts.amount = Rule::amountOf(action);
            judged.facts.limit = Rule::limitOf(principal, scope);
        }
    } else {
        judged.facts.held = OrgModel::rolesAnywhere(principal);
    }
    if constexpr (DeclaresSeparation<Action>) {
        judged.facts.firstPerformer = Rule::firstPerformerOf(action);
        judged.sameActorRefusal = std::string{Rule::sameActorRefusal};
    } else if constexpr (DeclaresSeparations<Action>) {
        auto const performers = Rule::firstPerformersOf(action);
        for (std::size_t pair = 0; pair < performers.size(); ++pair) {
            if (performers[pair] == principal) {
                judged.facts.firstPerformer = performers[pair];
                judged.sameActorRefusal = std::string{Rule::separations[pair].refusal};
                break;
            }
        }
    }
    return judged;
}

template <typename Action>
    requires DeclaresRule<Action>
void requireAuthorized(Action const& action) {
    using Rule = Authorization<Action>;
    std::string const principal = ::morph::session::current()->principal;
    auto const judged = authorizationFactsOf(action, principal);
    std::string const actionType{::morph::model::ActionTraits<Action>::typeId()};
    Verdict const verdict = authorizationVerdict(judged.facts);
    if (verdict == Verdict::Allowed) {
        return;
    }
    if (verdict == Verdict::MissingRole) {
        throw Forbidden{actionType + ": '" + principal + "' does not hold the " + std::string{roleName(Rule::role)} +
                        " role " + judged.where};
    }
    if (verdict == Verdict::NoLimit) {
        throw Forbidden{actionType + ": '" + principal + "' has no value limit " + judged.where};
    }
    if (verdict == Verdict::OverLimit) {
        throw Forbidden{actionType + ": the amount is above '" + principal + "''s value limit " + judged.where};
    }
    if (!judged.sameActorRefusal.empty()) {
        throw Forbidden{actionType + ": '" + principal + "' " + judged.sameActorRefusal};
    }
    throw Forbidden{actionType + ": refused"};
}
```

`requireAuthorized` keeps its Doxygen; `authorizationFactsOf` and the new types are documented above. Extend the file's declaration comment with the several-pairs form. Run Model suite `"[authorization]"` and `"[verification]"` once: lab's single pair must refuse as before.

- [ ] **Step 2: The shared approval fixtures**

Append to `billing_test_support.hpp` (include `<morph/session/session_auth.hpp>`, `"clock.hpp"`, `"lims/billing/models/invoice_model.hpp"`, `"lims/org/dto/limit_dto.hpp"`, `"lims/org/models/org_model.hpp"` and `"testkit/pump.hpp"`):

```cpp
/// @brief Runs @p action on a fresh @p Model as @p principal through an in-process bridge, as a client's call runs.
///
/// A handler that is a coroutine (a Task handler) is only built by a direct `execute`; a dispatch drives it, so a
/// suite sends such an action this way.
/// @tparam Model The model.
/// @tparam Action The action.
/// @param principal Who sends it.
/// @param action The action.
/// @return Its result.
/// @throws std::exception the action's failure.
template <typename Model, typename Action>
auto dispatchAs(std::string const& principal, Action action) {
    ::morph::ladder::testkit::BackendRig rig{::morph::ladder::testkit::Mode::Local, 1};
    ::morph::session::TokenIssuer const issuer{std::string{kServiceSecret}, ::morph::session::hmacSha256};
    rig.bridge(0).setDefaultSession(signedAs(issuer, principal));
    auto handler = rig.client<Model>(0);
    return ::morph::ladder::testkit::awaitQt(handler.execute(std::move(action)));
}

/// @brief Billing settings for the seeded company (19 % tax, 48 approval hours, billing day 25), value limits —
///        paula 100 and tom 10 as approvers at the seeded site, carl 1000 and bea 1000 as approvers at the company —
///        and one published, relayed sample of a client billed in EUR. Set at 09:00 on 10 November 2026.
/// @return The sample.
inline LabFixture approvalSetUp() {
    ::morph::ladder::ScopedClockOverride const clock{at(2026, 11, 10)};
    grant("sam", LimsRole::Supervisor);
    {
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
        OrgModel org;
        auto const limit = [&org](std::string principal, std::int64_t units) {
            org.execute(SetValueLimit{.principal = std::move(principal),
                                      .company = seededCompany(),
                                      .limit = Amount{::morph::math::Rational{units, ::morph::math::DecimalPlaces{2}}}});
        };
        limit("paula", 100);
        limit("carl", 1000);
        limit("tom", 10);
        limit("bea", 1000);
    }
    grant("paula", LimsRole::InvoiceApprover, Scope{seededSite()});
    grant("tom", LimsRole::InvoiceApprover, Scope{seededSite()});
    grant("carl", LimsRole::InvoiceApprover);
    grant("bea", LimsRole::InvoiceApprover);
    auto lab = publishedSample();
    billingFor(lab.client);
    relayLabEvents();
    return lab;
}

/// @brief `approvalSetUp`, and the client's invoice drafted by bea at the seeded site (gross 47.60).
/// @return The invoice.
inline InvoiceView draftedInvoice() {
    auto const lab = approvalSetUp();
    ::morph::ladder::ScopedClockOverride const clock{at(2026, 11, 10)};
    ScopedPrincipal const bea{"bea"};
    return InvoiceModel{}.execute(CreateInvoice{.company = seededCompany(), .site = seededSite(), .client = lab.client});
}

/// @brief Submits @p invoice as bea at 09:00 on 10 November, dispatched as a client sends it.
/// @param invoice The invoice.
inline void submit(InvoiceView const& invoice) {
    ::morph::ladder::ScopedClockOverride const clock{at(2026, 11, 10)};
    static_cast<void>(dispatchAs<InvoiceModel>("bea", SubmitInvoice{.invoiceId = invoice.id}));
}
```

`submit` and the new cases dispatch `SubmitInvoice` rather than call it, because Task 14 makes its handler a coroutine that awaits the ledger's rate; dispatched, the cases do not change then.

- [ ] **Step 3: Write the failing tests**

Create `examples/lims/tests/test_invoice_approval.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Invoice approval. Hypothesis 5: an approval that lasts days and passes between principals is ordinary actions and
// the declared authorization. Hypothesis 6, second instance: billing's create/approve and submit/approve pairs and the
// approver's value limit, which the server edge cannot see. Escalation at a deadline, which the design listed as
// hypothesis 4's second instance, with nothing scheduled.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <morph/session/session_auth.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <string_view>

#include "billing_test_support.hpp"
#include "clock.hpp"
#include "lims/app/service_principals.hpp"
#include "lims/billing/models/billing_run_model.hpp"
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

}  // namespace

// Mutation: drop the separation-of-duties members from Authorization<ApproveInvoice> (bea approves her own invoice).
TEST_CASE("invoice approval: four eyes and a value limit, judged in the model", "[lims][billing][approval]") {
    DbFixture fixture;
    auto const invoice = lims::test::draftedInvoice();
    lims::test::submit(invoice);
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

// Escalation, which the design listed as hypothesis 4's second instance: nothing is scheduled. Mutation: judge the
// scope at the site whatever the time (paula still approves after the deadline, and carl's pending list stays as it
// was).
TEST_CASE("Hypothesis 4 (mis-framed for escalation): an approval past its deadline goes to a company-scoped approver, "
          "with nothing scheduled",
          "[lims][billing][approval][h4]") {
    DbFixture fixture;
    auto const invoice = lims::test::draftedInvoice();
    lims::test::submit(invoice);
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
    auto const invoice = lims::test::draftedInvoice();
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
    auto const invoice = lims::test::draftedInvoice();
    lims::test::submit(invoice);
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


// Mutation: drop the SubmitInvoice pair from Authorization<ApproveInvoice> (bea approves the invoice she submitted).
TEST_CASE("invoice approval: an invoice the billing run drafted still needs a second person to approve it",
          "[lims][billing][approval]") {
    DbFixture fixture;
    auto const lab = lims::test::approvalSetUp();
    lims::test::grantService(std::string{lims::app::kSchedulePrincipal});
    lims::InvoiceId id;
    {
        // What one schedule tick does for this company, called as its principal; Task 11 measures the dispatch.
        ScopedClockOverride const clock{lims::test::at(2026, 11, 25)};
        ScopedPrincipal const schedule{std::string{lims::app::kSchedulePrincipal}};
        static_cast<void>(lims::BillingRunModel{}.execute(
            lims::RunBillingSchedule{.batch = lims::Count{Rational{50, DecimalPlaces{0}}}}));
        auto const drafted =
            lims::InvoiceModel{}.execute(lims::ListInvoices{.company = lims::test::seededCompany()}).invoices;
        REQUIRE(drafted.size() == 1U);
        CHECK(drafted[0].createdBy == lims::app::kSchedulePrincipal);
        CHECK(drafted[0].client == lab.client);
        id = drafted[0].id;
    }
    ScopedClockOverride const clock{lims::test::at(2026, 11, 25, 10)};
    static_cast<void>(lims::test::dispatchAs<lims::InvoiceModel>("bea", lims::SubmitInvoice{.invoiceId = id}));
    lims::InvoiceModel invoices;
    {
        ScopedPrincipal const bea{"bea"};
        CHECK_THROWS_WITH(invoices.execute(lims::ApproveInvoice{.invoiceId = id}),
                          Catch::Matchers::ContainsSubstring("submitted this invoice"));
    }
    ScopedPrincipal const carl{"carl"};
    auto const approved = invoices.execute(lims::ApproveInvoice{.invoiceId = id});
    CHECK(approved.state == lims::InvoiceState::Approved);
    CHECK(approved.submittedBy == "bea");
    CHECK(approved.approvedBy == "carl");
}
```

Create `examples/lims/screens/tests/test_lims_approval_inbox.cpp`, a one-screen bundle of its own so no lims screen changes:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Escalation is a deadline the approval rule reads, and no action runs when it passes (decision 34). What that leaves
// on a screen: an approver's inbox is not told.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <expected>
#include <memory>
#include <morph/ui/author.hpp>
#include <morph/ui/service.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <testkit/db_fixture.hpp>
#include <testkit/screen_harness.hpp>

#include "billing_test_support.hpp"
#include "clock.hpp"
#include "lims/billing/models/invoice_model.hpp"

namespace {

namespace ui = morph::ui;
using morph::examples::testing::RigMode;
using morph::examples::testing::ScreenHarness;
using morph::ladder::ScopedClockOverride;
using morph::ladder::testkit::DbFixture;

constexpr std::string_view kInbox = "test.approvalInbox";

/// @brief A bundle of one screen: the caller's pending approvals, counted, and a button that asks again.
/// @return The bundle, or the refusal.
std::expected<std::shared_ptr<ui::ServedBundle const>, ui::Refusal> inboxBundle() {
    ui::ScreenRegistry screens;
    auto registered = screens.add(ui::screen(std::string{kInbox}, [](ui::Scope& screen) {
        auto const invoices = screen.model<lims::InvoiceModel>("invoices");
        auto const pending = screen.query<lims::ListPendingApprovals>("pending", invoices);
        auto const rows = pending.value().at(&lims::ListInvoicesResult::invoices);
        return ui::column({ui::text({.text = ui::format("{0} pending", {ui::count(rows)})}).testId("pendingCount"),
                           ui::button({.label = "Refresh", .onClick = ui::refetch(pending)}).testId("refresh")});
    }));
    if (!registered) {
        return std::unexpected(registered.error());
    }
    ui::UiRegistry served{ui::ApplicationInfo{.id = "lims-approval-inbox-test", .version = "1.0"}, screens.catalog()};
    ui::CatalogEntry entry;
    entry.id = std::string{kInbox};
    entry.title = "Approvals";
    if (auto added = served.addScreen(std::move(entry), (*registered)->text); !added) {
        return std::unexpected(std::move(added.error()));
    }
    return served.build();
}

}  // namespace

// The expected outcome: past the deadline, paula's inbox still lists the invoice she can no longer approve, until it
// asks again. If the CHECK_FALSE fails, something told the inbox: record what in decision 34.
TEST_CASE("approval inbox: a site approver's inbox is not told when an approval's deadline passes",
          "[lims-screens][billing][h4]") {
    DbFixture fixture;
    auto const invoice = lims::test::draftedInvoice();
    lims::test::submit(invoice);
    std::optional<ScopedClockOverride> clock;
    clock.emplace(lims::test::at(2026, 11, 12, 8));
    ScreenHarness harness{morph::client::LocalApp{.name = "lims-approval-inbox", .bundle = &inboxBundle, .setUp = {}},
                          std::string{kInbox}, RigMode::Local, {}, morph::client::LocalSetup{.db = {}, .user = "paula"}};
    REQUIRE(harness.waitForText("pendingCount", "1 pending"));

    clock.reset();
    clock.emplace(lims::test::at(2026, 11, 12, 10));  // the deadline was 09:00
    bool const told = harness.waitFor(
        [&harness] { return harness.text("pendingCount") == std::optional<std::string>{"0 pending"}; },
        std::chrono::milliseconds{1500});
    CHECK_FALSE(told);

    // The control: asked again, the same panel shows the escalation, so the panel can show it.
    harness.click("refresh");
    CHECK(harness.waitForText("pendingCount", "0 pending"));
    CHECK(harness.problems().empty());
}
```

The bundle follows `lims_screens.cpp`'s `bundle()` (`ScreenRegistry`, `UiRegistry`, `addScreen`, `build`); if `screen.query` without a body or `ui::refetch` is spelled differently in `include/morph/ui/author.hpp`, use that header's spelling. `ScopedClockOverride` is process-wide (`examples/common/clock.hpp`), so the model calls the harness makes on its own threads read it.

- [ ] **Step 4: Run them and see them fail**

Run: Build lims. Expected: compile error, `lims::SubmitInvoice` is not declared.

- [ ] **Step 5: The rules**

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

/// @brief An invoice is approved by an invoice approver who neither created nor submitted it and whose value limit
///        covers it: at the invoice's site while its approval is within its deadline, at its company after. The
///        scope moving from site to company at the deadline is the escalation.
template <>
struct Authorization<ApproveInvoice> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::InvoiceApprover;

    /// @brief The pairs: whoever created the invoice, and whoever submitted it, may not also approve it.
    static constexpr std::array<SeparatedFrom, 2> separations{{
        {.action = ::morph::model::ActionTraits<CreateInvoice>::typeId(),
         .refusal = "created this invoice and cannot also approve it"},
        {.action = ::morph::model::ActionTraits<SubmitInvoice>::typeId(),
         .refusal = "submitted this invoice and cannot also approve it"},
    }};

    /// @brief The invoice's site before its deadline, its company from the deadline on.
    /// @param action The action.
    /// @return The scope; an empty company when there is no such invoice.
    [[nodiscard]] static Scope scopeOf(ApproveInvoice const& action);

    /// @brief The invoice's gross in its company's currency, as its submission recorded it.
    /// @param action The action.
    /// @return The amount; zero when there is no such invoice.
    /// @throws ValidationError when the submission recorded no company-currency gross.
    [[nodiscard]] static ::morph::math::Rational amountOf(ApproveInvoice const& action);

    /// @brief @p principal's value limit at the company of @p scope.
    /// @param principal The principal.
    /// @param scope The scope `scopeOf` gave.
    /// @return The limit, or nothing.
    [[nodiscard]] static std::optional<::morph::math::Rational> limitOf(std::string_view principal,
                                                                       Scope const& scope);

    /// @brief Who created the invoice and who submitted it, in `separations`' order.
    /// @param action The action.
    /// @return The two; each empty when there is no such invoice or no submission.
    [[nodiscard]] static std::array<std::optional<std::string>, 2> firstPerformersOf(ApproveInvoice const& action);
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

::morph::math::Rational Authorization<ApproveInvoice>::amountOf(ApproveInvoice const& action) {
    auto const row = invoiceRow(action.invoiceId);
    if (!row.has_value()) {
        return ::morph::math::Rational{0, ::morph::math::DecimalPlaces{0}};
    }
    if (!row->grossCompanyNum.Value().has_value() || !row->grossCompanyDen.Value().has_value()) {
        throw ValidationError{"ApproveInvoice: invoice " + std::to_string(*action.invoiceId) +
                              " has no value in its company's currency"};
    }
    auto const base = OrgModel::findCompany(CompanyId{row->companyId.Value()})->baseCurrency;
    return ::morph::math::Rational{::morph::math::Numerator{*row->grossCompanyNum.Value()},
                                   ::morph::math::Denominator{*row->grossCompanyDen.Value()},
                                   ::morph::math::DecimalPlaces{decimalsOf(base)}};
}

std::optional<::morph::math::Rational> Authorization<ApproveInvoice>::limitOf(std::string_view principal,
                                                                             Scope const& scope) {
    if (auto const* company = std::get_if<CompanyId>(&scope)) {
        return OrgModel::valueLimit(principal, *company);
    }
    auto const site = OrgModel::findSite(std::get<SiteId>(scope));
    return site.has_value() ? OrgModel::valueLimit(principal, site->companyId) : std::nullopt;
}

std::array<std::optional<std::string>, 2> Authorization<ApproveInvoice>::firstPerformersOf(
    ApproveInvoice const& action) {
    auto const row = invoiceRow(action.invoiceId);
    if (!row.has_value()) {
        return {std::nullopt, std::nullopt};
    }
    std::string submitter{row->submittedBy.Value().ToStringView()};
    return {std::optional<std::string>{std::string{row->createdBy.Value().ToStringView()}},
            submitter.empty() ? std::nullopt : std::optional<std::string>{std::move(submitter)}};
}
```

and `Authorization<SubmitInvoice>::scopeOf` returning the invoice's company. Put `SubmitInvoice` and `ApproveInvoice` in the edge table and the coverage tuple.

- [ ] **Step 6: The actions**

Add the DTOs (`validate()` requires the id; `ListPendingApprovals::validate()` is `true`) and register in `invoice_model.hpp`: `"SubmitInvoice"` and `"ApproveInvoice"`, each `BRIDGE_REGISTER_EFFECTS(…, ::morph::effects::updates<lims::InvoiceView>)`; `"ListPendingApprovals"` a read. Create `invoice_approval.cpp`:

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
    CompanyId const company{row.companyId.Value()};
    auto const currency = static_cast<CurrencyCode>(row.currency.Value());
    auto const base = OrgModel::findCompany(company)->baseCurrency;
    if (currency != base) {
        throw ValidationError{"SubmitInvoice: invoice " + std::to_string(*action.invoiceId) + " is in " +
                              std::string{currencyName(currency)} + " and no rate values it in " +
                              std::string{currencyName(base)}};
    }
    auto const settings = PriceListModel::settingsOf(company);
    std::int64_t const hours = (*settings->approvalHours).numerator / (*settings->approvalHours).denominator;
    std::int64_t const now = nowMillis();
    row.state = static_cast<int>(InvoiceState::Submitted);
    row.submittedBy = Lightweight::SqlAnsiString<64>{::morph::session::current()->principal};
    row.submittedAt = now;
    row.deadlineAt = now + hours * 3'600'000;
    // The value the approver's limit is judged against, in the company's currency.
    row.grossCompanyNum = row.grossNum.Value();
    row.grossCompanyDen = row.grossDen.Value();
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

The state check comes before `requireAuthorized` in approval so that approving an approved invoice is an `IllegalTransition` for everyone, not a refusal that names who created it. `invoiceRecord(mapper, id, where)` (anonymous namespace) loads the row or throws `NotFound`. `ListPendingApprovals`:

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
        try {
            if (authorizationVerdict(authorizationFactsOf(approve, principal).facts) == Verdict::Allowed) {
                result.invoices.push_back(invoiceView(mapper, static_cast<std::int64_t>(row.id.Value())));
            }
        } catch (ValidationError const&) {
            continue;  // no company-currency value: nobody can approve it, so it is nobody's to act on
        }
    }
    return result;
}
```

The pending list asks the same declared rule `ApproveInvoice` is judged by, through the same `authorizationFactsOf`; it is the one place the rule is evaluated without the action.

- [ ] **Step 7: Build and run**

Run: Build lims, Model suite `"[approval]"`, Model suite `"[authorization]"`, then Screen suite `"[h4]"`. Expected: all pass, the h5 and h6 cases in all three modes; the inbox case passes as written (expected, inferred from the effects being derived from executed actions only: no action runs at a deadline). Run the five mutations named in `test_invoice_approval.cpp` and restore each. If the inbox case's `CHECK_FALSE(told)` fails, something told the inbox: change it to `CHECK(told)`, find what in the harness's event path, and use the refuted text in Step 9.

- [ ] **Step 8: Measure the second instance of hypothesis 6 as code**

Record what the second instance needed that the first already had, for Task 17, over the range since Part B began (Task 1 Step 0 wrote the base):

```bash
cd /home/yaraslau/repo/morph
base=$(cat /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/plan_b_base.txt)
grep -rn 'requireAuthorized(action)' examples/lims/*/src | wc -l
grep -rn 'requireRoleSomewhere<' examples/lims/*/src | wc -l
grep -c 'requirementOf<' examples/lims/app/include/lims/app/declared_requirements.hpp
grep -n 'std::tuple<' -A12 examples/lims/tests/test_declared_authorization.cpp | head -20
git diff --stat "$base" -- examples/lims/billing/include/lims/billing/authorization examples/lims/billing/src/authorization \
    examples/lims/org/include/lims/org/authorization/declared.hpp
```

Keep the output in `/tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/plan_b_h6_measure.txt`.

- [ ] **Step 9: Record decisions 34 and 35**

The design's hypothesis-4 row names the billing run as the one instance and says why escalation is not one; decision 34 records the measurement behind it. Append, with the inbox paragraph for the outcome Step 7 measured:

```markdown
### 34. Invoice approval is ordinary actions on the invoice's row, and escalation is its deadline (hypothesis 5; hypothesis 4 mis-framed for escalation)

`SubmitInvoice` and `ApproveInvoice` move an invoice from draft to submitted to approved; the state, who submitted and
approved it, the approval deadline and the gross in company currency are columns of its row. The deadline is the
submission time plus the company's approval hours. Before it, the approver's role is judged at the invoice's site (a
role at the company holds there too); from it, only at the company: an approver at the site no longer can, and
`ListPendingApprovals` shows the invoice to whoever could approve it now, through the same declared rule. The design's
lifecycle names no way back from submitted, so there is none.

**Hypothesis 5 is refuted for this instance.** Measured in `tests/test_invoice_approval.cpp`: an invoice submitted
by one principal on one rig is approved by another a day later on a new rig, in `Local`, `LocalSingleThread` and
`Socket` mode; four eyes and the value limit hold. The lifecycle needed no state outside the row and no code shared
with lab's verification but the declared authorization (`lims::Authorization`, `requireAuthorized`,
`authorizationFactsOf`, `authorizationVerdict`). Requisition approval in Part C is the second instance.

**Hypothesis 4 is mis-framed for escalation.** The design listed approval escalation as hypothesis 4's second
instance, but escalation is not scheduled work: it is a deadline the rule reads when an approver acts or lists, so
nothing has to start at the deadline or resume after a restart. Measured in the same file with the clock before and
after the deadline. The one consequence is an effects question, not a scheduling one, measured in
`screens/tests/test_lims_approval_inbox.cpp`: a site approver's inbox, mounted before the deadline, still lists the
invoice after it, and shows the escalation only when it asks again, because no action ran and so no effect was
declared or fanned out. Hypothesis 4 therefore has one instance in this design, the billing run (decision 33), and
the design's row says so. Part C's requisition escalation is expected to take this shape.
```

If the inbox was told, replace the measured sentence with "a site approver's inbox, mounted before the deadline, dropped the invoice after it without asking again: [what told it]".

```markdown
### 35. Billing's approval pairs and the approver's value limit are declared, and checked in the model (hypothesis 6, second instance)

`Authorization<ApproveInvoice>` declares the invoice approver role in a scope its payload's invoice decides (site or,
past the deadline, company), a value limit (the invoice's gross in company currency, recorded at submission, against
the approver's limit at that company, `SetValueLimit`), and two separation-of-duties pairs: with `CreateInvoice`, and
with `SubmitInvoice`, so an invoice the billing run drafted — its creator the schedule's service principal — still
needs a second person. `lims/org/authorization/declared.hpp` holds a form for several pairs beside lab's single one.

**Hypothesis 6 is confirmed for its second instance.** Measured in `tests/test_invoice_approval.cpp`:
`LimsAuthorizer` allows both the invoice's creator and an approver over their limit (it sees the principal, the model
and the action), and the model refuses both, in every mode; the clerk who submitted a run-drafted invoice is refused
too. The same hand-rolled guarantee recurred: each declared action's handler calls `requireRoleSomewhere` and
`requireAuthorized` itself ([measured] calls across lab, org, ledger and billing), the edge's table restates every
declaration by hand ([measured] entries), and the coverage test lists every mutating action by hand because nothing
enumerates a model's actions publicly. Task 17 decides the seam.
```

- [ ] **Step 10: Commit**

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/org/include/lims/org/authorization/declared.hpp examples/lims/billing examples/lims/tests \
        examples/lims/screens/tests/test_lims_approval_inbox.cpp \
        examples/lims/app/include/lims/app/declared_requirements.hpp examples/lims/README.md
git commit -m "Approve lims invoices with four eyes, value limits and deadline escalation

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---
### Task 13: The draft invoice's edit lease, its release, and its lapse on disconnect (hypothesis 3, first instance)

**Files:**
- Create: `examples/lims/billing/include/lims/billing/dto/lease_dto.hpp`, `examples/lims/billing/include/lims/billing/models/invoice_edit_model.hpp`, `examples/lims/billing/src/models/invoice_edit_model.cpp`, `examples/lims/tests/test_invoice_lease.cpp`
- Modify: `billing_entity.hpp` (`InvoiceLeaseRecord`), `billing_schema.cpp`, `invoice_model.hpp` (`GetInvoiceLease`), `billing_authorization.hpp`/`.cpp`, `declared_requirements.hpp`, `test_declared_authorization.cpp`, `examples/lims/README.md` (decision 36)

**Interfaces:**
- Consumes: `invoiceView`, `BackendRig::connect`, `ServerConnection` (Task 9), `lims::test::signedAs`.
- Produces: `LeaseInvoice { InvoiceId invoiceId; }` → `InvoiceLeaseView { InvoiceId invoice; std::string holder; Timestamp expiresAt; }`; `ReleaseInvoice {}` → `InvoiceLeaseView`; `RemoveInvoiceLine { BillableLineId line; }` → `InvoiceView`; `SetInvoiceReference { std::string reference; }` → `InvoiceView`; `GetInvoiceLease { InvoiceId invoiceId; }` (on `InvoiceModel`) → `InvoiceLeaseView` (empty holder when free); model `lims::InvoiceEditModel` (`"InvoiceEditModel"`, one private instance per editor); record `Billing_Lease` (keyed by `invoice`).

A lease is a row: the invoice, the holder, a token naming the editing instance, and an expiry fifteen minutes after the last edit. `LeaseInvoice` takes a free or expired lease, or renews its own; another holder's live lease is refused in words naming them. Each edit requires the instance's own live lease and renews it. When the instance is destroyed — its screen unmounts locally, or the server reclaims it when its connection closes — it deletes its lease by token. Expiry needs no timer: an expired lease is free to the next `LeaseInvoice`. The edits are the invoice editor's (`examples/lims/design/mockups/21-InvoiceEditor.dc.html`: a "Remove" per line, and the client reference), so a lease has something to guard; `ReleaseInvoice` is what the editor's "Close" sends, and its declared effect is what a second clerk's "Tell me when it's free" waits on.

- [ ] **Step 1: Write the failing test**

The connections come from `BackendRig::connect()` (Task 9), which closes one connection while others stay and keeps what each was sent.

Create `examples/lims/tests/test_invoice_lease.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Hypothesis 3, first instance: a draft invoice is leased to the clerk editing it; a second clerk is told who holds
// it; the lease lapses after a timeout and when the holder's connection closes; a release reaches a second clerk's
// connection, and the lapse is measured for the same.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <glaze/glaze.hpp>
#include <morph/session/session_auth.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "billing_test_support.hpp"
#include "clock.hpp"
#include "lims/billing/models/invoice_edit_model.hpp"
#include "lims/billing/models/invoice_model.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::ScopedClockOverride;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::ServerConnection;

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
    lims::test::relayLabEvents();
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

/// @brief Who a session says @p principal is, signed with the case's secret.
/// @param principal The principal.
/// @return The session.
[[nodiscard]] morph::session::Context as(std::string const& principal) {
    morph::session::TokenIssuer const issuer{std::string{kSecret}, morph::session::hmacSha256};
    return lims::test::signedAs(issuer, principal);
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

// Mutation: declare ReleaseInvoice with no effect (bob's connection hears nothing when alice lets go).
TEST_CASE("invoice lease: a release reaches a second clerk's connection", "[lims][billing][lease]") {
    DbFixture fixture;
    auto const invoice = draft();
    BackendRig rig{Mode::Socket, 0, lims::test::limsAuthorizer(kSecret)};
    auto const lease = glz::write_json(lims::LeaseInvoice{.invoiceId = invoice.id}).value_or(std::string{});
    auto const alice = rig.connect();
    auto const bob = rig.connect();
    auto const aliceEditor = alice->registerModel("InvoiceEditModel", as("alice"));
    REQUIRE(alice->execute(aliceEditor, "InvoiceEditModel", "LeaseInvoice", lease, as("alice")).kind == "ok");
    REQUIRE(bob->watch({"Billing_Lease"}, as("bob")).kind == "ok");
    auto const before = bob->changes().size();

    REQUIRE(alice->execute(aliceEditor, "InvoiceEditModel", "ReleaseInvoice", "{}", as("alice")).kind == "ok");
    CHECK(ServerConnection::eventually([&] { return bob->changes().size() > before && bob->heardOf("Billing_Lease"); }));
    CHECK(holderOf(invoice.id).empty());
}

// Hypothesis 3. Mutation: remove the release from ~InvoiceEditModel (bob's lease is refused until the timeout).
TEST_CASE("Hypothesis 3: the lease lapses when the holder's connection closes", "[lims][billing][lease][h3]") {
    DbFixture fixture;
    auto const invoice = draft();
    BackendRig rig{Mode::Socket, 0, lims::test::limsAuthorizer(kSecret)};
    auto const lease = glz::write_json(lims::LeaseInvoice{.invoiceId = invoice.id}).value_or(std::string{});
    auto const alice = rig.connect();
    auto const aliceEditor = alice->registerModel("InvoiceEditModel", as("alice"));
    REQUIRE(alice->execute(aliceEditor, "InvoiceEditModel", "LeaseInvoice", lease, as("alice")).kind == "ok");

    auto const bob = rig.connect();
    REQUIRE(bob->watch({"Billing_Lease"}, as("bob")).kind == "ok");
    auto const bobEditor = bob->registerModel("InvoiceEditModel", as("bob"));
    auto const refused = bob->execute(bobEditor, "InvoiceEditModel", "LeaseInvoice", lease, as("bob"));
    CHECK(refused.kind == "err");
    CHECK_THAT(refused.message, Catch::Matchers::ContainsSubstring("being edited by 'alice'"));
    auto const heardBefore = bob->changes().size();

    alice->close();
    // What bob's connection is told of the lapse itself, before bob acts: the lapse runs no action, so the expected
    // outcome is nothing. If this CHECK_FALSE fails, the lapse was told: record what carried it in decision 36.
    bool const toldOfLapse =
        ServerConnection::eventually([&] { return bob->changes().size() > heardBefore; }, std::chrono::milliseconds{1000});
    CHECK_FALSE(toldOfLapse);

    bool const lapsed = ServerConnection::eventually(
        [&] { return bob->execute(bobEditor, "InvoiceEditModel", "LeaseInvoice", lease, as("bob")).kind == "ok"; });
    CHECK(lapsed);
    CHECK(holderOf(invoice.id) == "bob");
}
```

- [ ] **Step 2: Run it and see it fail**

Run: Build lims. Expected: compile error, `lims/billing/models/invoice_edit_model.hpp` not found.

- [ ] **Step 3: The lease**

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

- [ ] **Step 4: Build and run**

Run: Build lims, Model suite `"[lease]"`, Model suite `"[authorization]"`. Expected (the h3 lapse inferred from `RemoteServer::closeConnection`'s documentation: "A *private* instance … is erased outright"; the release's event from its declared `changes<InvoiceLeaseView>`; the lapse's silence from effects being derived from executed actions only): all pass. If `CHECK(lapsed)` fails, the destructor did not run on close, or ran without a database connection: the hypothesis is confirmed instead — change the check to `CHECK_FALSE(lapsed)`, assert that bob's lease succeeds only after the clock passes the expiry, and use the confirmed text in Step 5. If `CHECK_FALSE(toldOfLapse)` fails, the lapse reached bob's connection: change it to `CHECK(toldOfLapse)`, read from the `changes` body what carried it, and say so in Step 5's text. Run the three mutations and restore them.

- [ ] **Step 5: Record decision 36**

The refuted text (the measured outcome if Step 4 passed as written):

```markdown
### 36. A draft invoice is leased to its editor, and the lease lapses with the editor's instance (hypothesis 3, first instance)

`InvoiceEditModel` is one private instance per editor. `LeaseInvoice` writes a lease row — the invoice, the holder,
the instance's token, an expiry fifteen minutes after the last edit — and refuses another editor's live lease in words
naming its holder; `GetInvoiceLease` shows the holder to anyone. Each edit (removing a line, setting the client
reference) requires the instance's own live lease and renews it. An expired lease is free to the next
`LeaseInvoice`, so the timeout needs no timer. `ReleaseInvoice` declares `changes<InvoiceLeaseView>`, so a second
clerk watching `Billing_Lease` is told when the holder lets go — measured on a second connection.

**Hypothesis 3 is refuted for this instance, through an existing hook that is not a lease hook.** Measured in
`tests/test_invoice_lease.cpp` over a real `RemoteServer`: closing the holder's connection frees the lease, and the
second clerk takes it without waiting for the timeout. The hook is the instance's destructor: `closeConnection`
erases a private instance, and `~InvoiceEditModel` deletes its lease by token. What that costs, stated plainly: the
release does database work in a destructor, on whatever thread drops the last reference (the server strand when a
connection closes), and a failure there can only be logged; a crash that kills the process releases nothing, and the
timeout is then the only lapse. And the lapse is not told: a second clerk watching `Billing_Lease` hears of a release
but not of a lapse on disconnect, because the destructor is not an action and nothing derives a change from it —
measured on the same second connection; the clerk learns of it only by asking. Lot reservation in Part C is the second
instance.
```

If confirmed instead: "**Hypothesis 3 is confirmed for this instance.** Measured in `tests/test_invoice_lease.cpp`: closing the holder's connection [what was observed]; the lease lapsed only at its timeout. No hook told the model that its connection closed." If the lapse was told, replace "And the lapse is not told: … asking." with what carried it.

- [ ] **Step 6: Commit**

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/billing examples/lims/tests examples/lims/app/include/lims/app/declared_requirements.hpp \
        examples/lims/README.md
git commit -m "Lease lims draft invoices to their editor and measure the release and the lapse on a second connection

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---
### Task 14: Posting an invoice — two commits in sequence, or one shared transaction, under failure injection (hypothesis 1, first instance)

**Files:**
- Create: `examples/lims/ledger/include/lims/ledger/db/ledger_store.hpp`, `examples/lims/billing/include/lims/billing/core/ledger_port.hpp`, `examples/lims/billing/src/core/ledger_port.cpp`, `examples/lims/billing/src/models/invoice_posting.cpp`, `examples/lims/app/include/lims/app/ledger_wiring.hpp`, `examples/lims/app/src/ledger_wiring.cpp`, `examples/lims/tests/test_invoice_posting.cpp`
- Modify: `posting.hpp` (`RateQuery`, `RateAnswer`), `entry_dto.hpp` (`RecordPosting`), `rate_dto.hpp` (`GetRateOn`), `ledger_model.hpp`, `ledger_entries.cpp` (`record` over `recordPostingIn`; `RecordPosting`), `ledger_rates.cpp` (`GetRateOn`), `ledger_authorization.hpp`, `invoice_math.hpp`/`.cpp` (`postingFor`), `billing_store.hpp`, `invoice_drafts.cpp` (`assignNumberAndPost`), `billing_entity.hpp` (`NumberSeriesRecord`), `billing_schema.cpp`, `invoice_dto.hpp` (`PostInvoice`), `invoice_model.hpp` (`SubmitInvoice` and `PostInvoice` as Task handlers), `invoice_approval.cpp` (`SubmitInvoice` values a foreign-currency invoice), `billing_authorization.hpp`/`.cpp`, `declared_requirements.hpp`, `test_declared_authorization.cpp`, `billing_test_support.hpp` (`books`, `billingSettings`, `approvedInvoice`, `PostingRig`), `src/server/main.cpp`, `examples/lims/README.md` (decision 37)

**Interfaces:**
- Consumes: `LedgerModel::record`, `LedgerModel::rateOn`, `lims::app::InternalClient::dispatch`, `lims::app::kPostingPrincipal`, `fiscalYearOf`, `roundToCurrency`, `faultPoint`, `BackendRig::connect`, `dispatchAs`.
- Produces: `struct lims::RateQuery { CompanyId company; CurrencyCode from; CurrencyCode to; Timestamp on; }`, `struct lims::RateAnswer { Rate rate; }` (in `posting.hpp`); `RecordPosting { PostingRequest request; }` → `PostingResult` and `GetRateOn { RateQuery query; }` → `RateAnswer` on `LedgerModel`; `PostingResult lims::recordPostingIn(Lightweight::DataMapper&, PostingRequest const&)` (ledger-internal, in `ledger/db/ledger_store.hpp`; `record` is it inside its own transaction); `enum class lims::PostingMode { Dispatched, SharedTransaction }`; `struct lims::LedgerPort { PostingMode mode; std::function<Completion<PostingResult>(PostingRequest)> post; std::function<PostingResult(Lightweight::DataMapper&, PostingRequest const&)> postIn; std::function<Completion<RateAnswer>(RateQuery)> rateOn; }`, `void lims::setLedgerPort(LedgerPort)`, `LedgerPort const& lims::ledgerPort()`; `struct lims::InvoiceToPost { CompanyId company; CurrencyCode companyCurrency; CurrencyCode currency; Rational rate; Timestamp date; std::string sourceKey; std::string sourceReference; Rational net; Rational tax; Rational gross; std::string receivableAccount; std::string revenueAccount; std::string taxAccount; std::string roundingAccount; }`; `PostingRequest lims::postingFor(InvoiceToPost const&)`; `std::string lims::assignNumberAndPost(Lightweight::DataMapper&, std::int64_t invoiceId, PostingResult const& entry, std::string const& author)` (billing-internal); `PostInvoice { InvoiceId invoiceId; }` → `InvoiceView`; `void lims::app::installLedgerPort(InternalClient&, PostingMode)`; test helpers `lims::test::books()`, `lims::test::billingSettings()`, `InvoiceView lims::test::approvedInvoice(std::string reference = "WW-1", CurrencyCode = EUR)`, `class lims::test::PostingRig`.

The posting is the design's "two calls in sequence", measured two ways, because the reason it commits twice is the way the ledger's half is reached, and that is what hypothesis 1 is about:

- **Dispatched** (`PostingMode::Dispatched`): the ledger's half is a real ledger action, `RecordPosting`, dispatched by the app's internal client as `system:posting`, which holds `LimsRole::Service` at the company — so it is authorized by the edge and by its declared rule, journaled like any action, and its `changes<JournalEntryView>` reaches watchers. `PostInvoice` is a Task handler that awaits it; the ledger commits alone (its source key is `billing.invoice:<id>`, recorded once), then billing assigns the next number of the company's fiscal-year series, marks the invoice posted and commits.
- **One shared transaction** (`PostingMode::SharedTransaction`): the ledger's half is `recordPostingIn`, called by the port with billing's own open mapper inside billing's transaction. Both halves commit once, with seams that exist — the design's refutation condition, on its letter. What it gives up is measured: the ledger's write is no ledger action, so no `Ledger_Entry` change reaches a watching connection, no ledger rule authorizes it, and no ledger journal entry records it.

The number is assigned in billing's commit either way, so a failure before it leaves no hole in the series; the fiscal year is the entry's date's, so a retry that converges on an entry recorded before a year boundary numbers it in that entry's year. The dated rate is a ledger read, `GetRateOn`, dispatched the same way; `SubmitInvoice` becomes a Task handler that awaits it to value a foreign-currency invoice in company currency, which Task 12 could not.

- [ ] **Step 1: The contract's rate, the ledger's actions, and the ledger's half below its model**

Append to `posting.hpp`, before its closing namespace:

```cpp
/// @brief What a poster asks the ledger before it converts: the rate from one currency to another valid on a date.
struct RateQuery {
    /// @brief The company whose rates are asked.
    CompanyId company;
    /// @brief The currency converted from.
    CurrencyCode from = CurrencyCode::EUR;
    /// @brief The currency converted to.
    CurrencyCode to = CurrencyCode::EUR;
    /// @brief The date the rate must be valid on.
    ::morph::time::Timestamp on;
};

/// @brief The ledger's answer to a `RateQuery`.
struct RateAnswer {
    /// @brief Units of `to` per unit of `from`; empty when no rate is valid on the date.
    Rate rate;
};
```

and give `PostingResult` the entry's date, so a poster that retries learns which period and fiscal year the entry it converged on is in:

```cpp
/// @brief What the ledger recorded.
struct PostingResult {
    /// @brief The journal entry.
    JournalEntryId entryId;
    /// @brief Its date: the request's, or, for a repeated key, the first request's.
    ::morph::time::Timestamp date;
};
```

In `test_posting_contract.cpp`, change the result case to `PostingResult const result{.entryId = lims::JournalEntryId{9}, .date = {}};` and `CHECK_THAT(*json, Catch::Matchers::ContainsSubstring(R"("entryId":9)"));` (include `<catch2/matchers/catch_matchers_string.hpp>`).

Create `ledger_store.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>

#include "lims/posting/posting.hpp"

/// @file
/// Recording a posting through a caller's open mapper. Ledger-internal: `LedgerModel::record` is it inside a
/// transaction of its own, and the app's port hands it billing's mapper for the shared-transaction posting.

namespace lims {

/// @brief Records @p request as a posted entry through @p mapper, once per company and source key; the caller
///        commits.
/// @param mapper The caller's mapper, inside its transaction.
/// @param request The request.
/// @return The entry and its date; the first one recorded under the request's key when it is repeated.
/// @throws ValidationError, NotFound, Conflict as `LedgerModel::record` documents.
[[nodiscard]] PostingResult recordPostingIn(Lightweight::DataMapper& mapper, PostingRequest const& request);

}  // namespace lims
```

In `ledger_entries.cpp`, move `record`'s body into `recordPostingIn(mapper, request)` — the validation, the rate check, and everything from `PostingSources sources{mapper};` to `sources.record(...)` — with a repeated key answering the first entry's id and date:

```cpp
    if (auto const known = sources.lookup(scope, request.sourceKey)) {
        auto const first = loadEntry(mapper, std::stoll(*known));
        return PostingResult{.entryId = JournalEntryId{std::stoll(*known)},
                             .date = timestampFromMillis(first->header.dateMs.Value())};
    }
```

and a new one answering `PostingResult{.entryId = JournalEntryId{id}, .date = request.date}`; then make `record`:

```cpp
PostingResult LedgerModel::record(PostingRequest const& request) {
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    auto const result = recordPostingIn(mapper, request);
    sqlTxn.Commit();
    return result;
}
```

The ledger's two actions. In `entry_dto.hpp`:

```cpp
/// @brief The ledger's half of a posting, as an action: records the request once per company and source key. Sent
///        only by the app's posting client, as a service principal.
struct RecordPosting {
    /// @brief The request.
    PostingRequest request;

    /// @brief Whether a company and a source key are named.
    /// @return `true` when they are.
    [[nodiscard]] bool validate() const noexcept { return request.company.hasValue() && !request.sourceKey.empty(); }
};
```

and in `rate_dto.hpp`:

```cpp
/// @brief Reads the rate valid on a date, as a poster asks it.
struct GetRateOn {
    /// @brief The question.
    RateQuery query;

    /// @brief Whether a company and a date are named.
    /// @return `true` when they are.
    [[nodiscard]] bool validate() const noexcept { return query.company.hasValue() && query.on.hasValue(); }
};
```

`LedgerModel` gains `PostingResult execute(const RecordPosting& action);` (Doxygen: `ValidationError`, `NotFound`, `Conflict` as `record`; `Forbidden` "only the server's own clients send it" and for a principal without the service role at the request's company; `EmptyPrincipalError`) and `RateAnswer execute(const GetRateOn& action);` (an empty rate for a company the caller cannot see, or no rate on the date):

```cpp
PostingResult LedgerModel::execute(const RecordPosting& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"RecordPosting: a company and a source key are required"};
    }
    requireServicePrincipal("RecordPosting");
    requireRoleSomewhere<RecordPosting>();
    requireAuthorized(action);
    return record(action.request);
}
```

```cpp
// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
RateAnswer LedgerModel::execute(const GetRateOn& action) {
    if (!action.validate()) {
        throw ValidationError{"GetRateOn: a company and a date are required"};
    }
    auto const visibleCompanies = OrgModel::visibleCompanies(currentPrincipal());
    if (std::ranges::find(visibleCompanies, action.query.company) == visibleCompanies.end()) {
        return RateAnswer{};
    }
    auto const rate = rateOn(action.query.company, action.query.from, action.query.to, action.query.on);
    return rate.has_value() ? RateAnswer{.rate = Rate{*rate}} : RateAnswer{};
}
```

(`RecordPosting` in `ledger_entries.cpp`, `GetRateOn` in `ledger_rates.cpp`.) Register `BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::RecordPosting, "RecordPosting")` with `BRIDGE_REGISTER_EFFECTS(lims::RecordPosting, ::morph::effects::changes<lims::JournalEntryView>)`, and `BRIDGE_REGISTER_ACTION(lims::LedgerModel, lims::GetRateOn, "GetRateOn", ::morph::model::Loggable::No)`. In `ledger_authorization.hpp`:

```cpp
/// @brief A posting is recorded by a service principal holding the service role at its company.
template <>
struct Authorization<RecordPosting> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::Service;

    /// @brief The request's company.
    /// @param action The action.
    /// @return It.
    [[nodiscard]] static Scope scopeOf(RecordPosting const& action) { return Scope{action.request.company}; }
};
```

and add `RecordPosting` to the edge table and the coverage tuple.

- [ ] **Step 2: Billing's port to the ledger**

Create `examples/lims/billing/include/lims/billing/core/ledger_port.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <cstdint>
#include <functional>
#include <morph/core/completion.hpp>

#include "lims/posting/posting.hpp"

/// @file
/// What billing needs of the ledger, without including it: posting a request, and the dated rate. The app installs
/// the port at start-up, before the first dispatch; billing-internal, since it names the ORM.

namespace lims {

/// @brief How a posting reaches the ledger.
enum class PostingMode : std::uint8_t {
    Dispatched,         ///< As the ledger's own action, dispatched by the app; it commits alone, before billing.
    SharedTransaction,  ///< Through billing's open mapper, in billing's transaction; both halves commit once.
};

/// @brief The ledger as billing reaches it.
struct LedgerPort {
    /// @brief Which of `post` and `postIn` `PostInvoice` uses.
    PostingMode mode = PostingMode::Dispatched;
    /// @brief Records a posting as the ledger's action; settles with its entry, or the ledger's refusal.
    std::function<::morph::async::Completion<PostingResult>(PostingRequest)> post;
    /// @brief Records a posting through the caller's open mapper, in the caller's transaction.
    std::function<PostingResult(Lightweight::DataMapper&, PostingRequest const&)> postIn;
    /// @brief The rate valid on a date; settles with an empty rate when there is none.
    std::function<::morph::async::Completion<RateAnswer>(RateQuery)> rateOn;
};

/// @brief Installs @p port. Called once at start-up (and by tests), before any billing action runs.
/// @param port The port; an empty one uninstalls.
void setLedgerPort(LedgerPort port);

/// @brief The installed port; its members empty until one is installed.
/// @return It.
[[nodiscard]] LedgerPort const& ledgerPort();

}  // namespace lims
```

and `billing/src/core/ledger_port.cpp` holding it in a function-local static. Create `examples/lims/app/include/lims/app/ledger_wiring.hpp` declaring `void installLedgerPort(InternalClient& posting, PostingMode mode);` ("Gives billing the ledger: `RecordPosting` and `GetRateOn` dispatched through @p posting, and `recordPostingIn` for the shared transaction. Called once at start-up, before the first dispatch. @param posting The posting principal's client; it must outlive the port. @param mode Which way `PostInvoice` posts.") and `ledger_wiring.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "lims/app/ledger_wiring.hpp"

#include "lims/billing/core/ledger_port.hpp"
#include "lims/ledger/db/ledger_store.hpp"
#include "lims/ledger/models/ledger_model.hpp"

namespace lims::app {

void installLedgerPort(InternalClient& posting, PostingMode mode) {
    setLedgerPort(LedgerPort{
        .mode = mode,
        .post = [&posting](PostingRequest request) {
            return posting.dispatch<LedgerModel>(RecordPosting{.request = std::move(request)});
        },
        .postIn = [](Lightweight::DataMapper& mapper, PostingRequest const& request) {
            return recordPostingIn(mapper, request);
        },
        .rateOn = [&posting](RateQuery query) {
            return posting.dispatch<LedgerModel>(GetRateOn{.query = std::move(query)});
        }});
}

}  // namespace lims::app
```

- [ ] **Step 3: The fixtures**

Append to `billing_test_support.hpp` (include `"lims/app/ledger_wiring.hpp"`, `"lims/billing/core/ledger_port.hpp"` and `"lims/ledger/models/ledger_model.hpp"`):

```cpp
/// @brief The seeded company's books: accounts 1200, 2200, 4000, 6900 and November 2026 open, made by `acc`, an
///        accountant there.
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

/// @brief Whether the posting principal holds the service role a `PostingRig` installs it with.
enum class PostingRole : std::uint8_t {
    Granted,   ///< `system:posting` holds `LimsRole::Service` at the seeded company.
    Withheld,  ///< It holds no role: the ledger's rule has nothing to allow.
};

/// @brief A server, the posting principal's client on it and the ledger port installed over them, as the server
///        process wires them; two socket clients for the clerks. The port is uninstalled when it goes.
class PostingRig {
public:
    /// @brief Installs the port in @p mode.
    /// @param mode Which way `PostInvoice` posts.
    /// @param role Whether the posting principal holds the service role.
    explicit PostingRig(PostingMode mode, PostingRole role = PostingRole::Granted) {
        if (role == PostingRole::Granted) {
            grantService(std::string{app::kPostingPrincipal});
        }
        app::installLedgerPort(_client, mode);
    }
    ~PostingRig() { setLedgerPort({}); }
    PostingRig(PostingRig const&) = delete;
    PostingRig& operator=(PostingRig const&) = delete;
    PostingRig(PostingRig&&) = delete;
    PostingRig& operator=(PostingRig&&) = delete;

    /// @brief The server and its clients.
    /// @return The rig.
    [[nodiscard]] ::morph::ladder::testkit::BackendRig& rig() noexcept { return _rig; }

    /// @brief Posts @p invoice as @p principal over socket client @p index, as a clerk's screen sends it.
    /// @param invoice The invoice.
    /// @param principal Who posts it.
    /// @param index The socket client.
    /// @return The posted invoice.
    /// @throws std::exception the posting's refusal or failure.
    InvoiceView post(InvoiceView const& invoice, std::string const& principal = "bea", std::size_t index = 0) {
        ::morph::session::TokenIssuer const issuer{std::string{kServiceSecret}, ::morph::session::hmacSha256};
        _rig.bridge(index).setDefaultSession(signedAs(issuer, principal));
        auto clerk = _rig.client<InvoiceModel>(index);
        return ::morph::ladder::testkit::awaitQt(clerk.execute(PostInvoice{.invoiceId = invoice.id}));
    }

private:
    ::morph::ladder::testkit::BackendRig _rig{::morph::ladder::testkit::Mode::Socket, 2, limsAuthorizer(kServiceSecret)};
    app::InternalClient _client{_rig.server(), kServiceSecret, std::string{app::kPostingPrincipal}};
};

/// @brief An approved invoice for one published sample in @p currency: drafted and submitted by `bea`, approved by
///        `carl`, an approver at the company with a limit of 1000. Needs `billingSettings()` first, and a
///        `PostingRig` when @p currency is not the company's.
/// @param reference The sample's reference, distinct per invoice.
/// @param currency The client's currency.
/// @return The invoice.
inline InvoiceView approvedInvoice(std::string reference = "WW-1", CurrencyCode currency = CurrencyCode::EUR) {
    {
        ScopedPrincipal const sam{"sam"};
        OrgModel{}.execute(SetValueLimit{.principal = "carl", .company = seededCompany(),
                                         .limit = Amount{::morph::math::Rational{1000, ::morph::math::DecimalPlaces{2}}}});
    }
    grant("carl", LimsRole::InvoiceApprover);
    auto const lab = publishedSample(std::move(reference));
    billingFor(lab.client, currency);
    relayLabEvents();
    InvoiceId id;
    {
        ScopedPrincipal const bea{"bea"};
        id = InvoiceModel{}.execute(CreateInvoice{.company = seededCompany(), .site = seededSite(), .client = lab.client}).id;
    }
    static_cast<void>(dispatchAs<InvoiceModel>("bea", SubmitInvoice{.invoiceId = id}));
    ScopedPrincipal const carl{"carl"};
    return InvoiceModel{}.execute(ApproveInvoice{.invoiceId = id});
}
```

`grant("carl", …)` twice in one case (two `approvedInvoice` calls) must not conflict; if Part A's `grant` refuses a repeated assignment, guard it as `grantService` does.

- [ ] **Step 4: Write the failing test**

Create `examples/lims/tests/test_invoice_posting.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Hypothesis 1, first instance: posting an invoice is billing's state change and the ledger's entry. Measured two
// ways — the ledger's half dispatched as its own action (two commits in sequence), and the ledger's half sharing
// billing's transaction (one commit) — each under a failure injected between the halves, and each for what it keeps
// of the ledger's authorization, effects and journal.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <algorithm>
#include <chrono>
#include <memory>
#include <morph/journal/action_log.hpp>
#include <morph/session/session_auth.hpp>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "billing_test_support.hpp"
#include "clock.hpp"
#include "lims/billing/core/fault_hook.hpp"
#include "lims/billing/core/invoice_math.hpp"
#include "lims/ledger/core/balance.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"

using lims::PostingMode;
using lims::test::ScopedPrincipal;
using morph::ladder::ScopedClockOverride;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::ServerConnection;
using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

namespace {

struct FaultHookGuard {
    FaultHookGuard() = default;
    FaultHookGuard(FaultHookGuard const&) = delete;
    FaultHookGuard& operator=(FaultHookGuard const&) = delete;
    FaultHookGuard(FaultHookGuard&&) = delete;
    FaultHookGuard& operator=(FaultHookGuard&&) = delete;
    ~FaultHookGuard() { lims::setFaultHook({}); }
};

/// @brief An action log that keeps what it is given: whose actions were journaled.
class Recorder final : public morph::journal::IActionLog {
public:
    void append(morph::journal::LogEntry entry) override {
        std::scoped_lock const lock{_mutex};
        _entries.push_back(std::move(entry));
    }
    void flush() override {}
    [[nodiscard]] std::vector<morph::journal::LogEntry> entries(std::string_view entityKey = {}) const override {
        std::scoped_lock const lock{_mutex};
        std::vector<morph::journal::LogEntry> matching;
        for (auto const& entry : _entries) {
            if (entityKey.empty() || entry.entityKey == entityKey) {
                matching.push_back(entry);
            }
        }
        return matching;
    }
    /// @brief Whether @p modelType journaled @p actionType.
    [[nodiscard]] bool holds(std::string_view modelType, std::string_view actionType) const {
        return std::ranges::any_of(entries(), [&](morph::journal::LogEntry const& entry) {
            return entry.modelType == modelType && entry.actionType == actionType;
        });
    }

private:
    mutable std::mutex _mutex;
    std::vector<morph::journal::LogEntry> _entries;
};

/// @brief The seeded company's entries, as acc sees them.
[[nodiscard]] std::vector<lims::JournalEntryView> entries() {
    ScopedPrincipal const acc{"acc"};
    return lims::LedgerModel{}.execute(lims::ListEntries{.company = lims::test::seededCompany()}).entries;
}

/// @brief @p invoice as bea sees it now.
[[nodiscard]] lims::InvoiceView current(lims::InvoiceView const& invoice) {
    ScopedPrincipal const bea{"bea"};
    return lims::InvoiceModel{}.execute(lims::GetInvoice{.invoiceId = invoice.id});
}

/// @brief A session for @p principal, signed with the rig's secret.
[[nodiscard]] morph::session::Context as(std::string const& principal) {
    morph::session::TokenIssuer const issuer{std::string{lims::test::kServiceSecret}, morph::session::hmacSha256};
    return lims::test::signedAs(issuer, principal);
}

[[nodiscard]] Rational exact(std::int64_t num, std::int64_t den, std::uint32_t places) {
    return Rational{Numerator{num}, Denominator{den}, DecimalPlaces{places}};
}

/// @brief Fails the posting at @p point for as long as it lives.
void failAt(std::string const& point) {
    lims::setFaultHook([point](std::string_view at) {
        if (at == point) {
            throw std::runtime_error{"server stopped while posting"};
        }
    });
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

// Mutation: round the company values to the transaction currency's decimals (the yen values keep cents, and the
// entry is refused for them).
TEST_CASE("invoice posting: a EUR invoice posted into a JPY company rounds to whole yen and books the yen difference",
          "[lims][billing][posting]") {
    lims::InvoiceToPost const eur{.company = lims::CompanyId{2},
                                  .companyCurrency = lims::CurrencyCode::JPY,
                                  .currency = lims::CurrencyCode::EUR,
                                  .rate = exact(15001, 100, 6),
                                  .date = morph::time::Timestamp{lims::test::at(2026, 11, 20)},
                                  .sourceKey = "billing.invoice:11",
                                  .sourceReference = "invoice 11",
                                  .net = exact(1005, 100, 2),
                                  .tax = exact(191, 100, 2),
                                  .gross = exact(1196, 100, 2),
                                  .receivableAccount = "1200",
                                  .revenueAccount = "4000",
                                  .taxAccount = "2200",
                                  .roundingAccount = "6900"};
    auto const request = lims::postingFor(eur);
    REQUIRE(request.lines.size() == 4U);
    // 11.96 × 150.01 = 1794.1196 → 1794; 10.05 × 150.01 = 1507.6005 → 1508; 1.91 × 150.01 = 286.5191 → 287;
    // 1794 − 1795 = −1 yen, booked to the rounding account on the debit side, with no euro amount.
    CHECK(*request.lines[0].companyAmount == exact(1794, 1, 0));
    CHECK(*request.lines[1].companyAmount == exact(1508, 1, 0));
    CHECK(*request.lines[2].companyAmount == exact(287, 1, 0));
    CHECK(request.lines[3].accountCode == "6900");
    CHECK(request.lines[3].side == lims::PostingSide::Debit);
    CHECK((*request.lines[3].amount).isZero());
    CHECK(*request.lines[3].companyAmount == exact(1, 1, 0));
    CHECK_FALSE(lims::entryProblem(request.lines, lims::CurrencyCode::EUR, lims::CurrencyCode::JPY).has_value());
}

// Mutation: skip the series update in assignNumberAndPost (the second invoice is numbered 000001 too).
TEST_CASE("invoice posting: an approved invoice gets the next number of its company's fiscal year, and its entry",
          "[lims][billing][posting]") {
    auto const mode = GENERATE(PostingMode::Dispatched, PostingMode::SharedTransaction);
    CAPTURE(mode);
    DbFixture fixture;
    ScopedClockOverride const clock{lims::test::at(2026, 11, 20)};
    lims::test::books();
    lims::test::billingSettings();
    lims::test::PostingRig posting{mode};
    auto const first = lims::test::approvedInvoice("WW-1");
    auto const second = lims::test::approvedInvoice("WW-2");
    auto const posted = posting.post(first);
    CHECK(posted.state == lims::InvoiceState::Posted);
    CHECK(posted.number == "INV-2026-000001");
    CHECK(posted.postedBy == "bea");
    REQUIRE(entries().size() == 1U);
    CHECK(entries()[0].id == posted.entry);
    CHECK(entries()[0].sourceKey == "billing.invoice:" + std::to_string(*first.id));
    CHECK(posting.post(second).number == "INV-2026-000002");
    CHECK_THROWS_AS(posting.post(first), std::exception);
}

// Hypothesis 1, two commits in sequence. Mutation: assign the number before the ledger's half (the failure leaves
// INV-2026-000001 assigned to an invoice with no entry, and the retry's number check fails).
TEST_CASE("Hypothesis 1: dispatched, a failure between the ledger's commit and billing's leaves an entry for an "
          "unposted invoice, and no hole",
          "[lims][billing][posting][h1]") {
    auto const point = GENERATE(std::string{"post.afterLedger"}, std::string{"post.beforeCommit"});
    CAPTURE(point);
    DbFixture fixture;
    ScopedClockOverride const clock{lims::test::at(2026, 11, 20)};
    lims::test::books();
    lims::test::billingSettings();
    lims::test::PostingRig posting{PostingMode::Dispatched};
    auto const invoice = lims::test::approvedInvoice();
    FaultHookGuard const guard;
    failAt(point);
    CHECK_THROWS_WITH(posting.post(invoice), Catch::Matchers::ContainsSubstring("server stopped"));

    // The partial state: the ledger holds the entry, billing does not hold the posting.
    REQUIRE(entries().size() == 1U);
    CHECK(entries()[0].sourceKey == "billing.invoice:" + std::to_string(*invoice.id));
    CHECK(current(invoice).state == lims::InvoiceState::Approved);
    CHECK(current(invoice).number.empty());
    CHECK_FALSE(current(invoice).entry.hasValue());

    // No hole: the number was never assigned. A retry converges on the one entry.
    lims::setFaultHook({});
    auto const retried = posting.post(invoice);
    CHECK(retried.number == "INV-2026-000001");
    REQUIRE(entries().size() == 1U);
    CHECK(retried.entry == entries()[0].id);
}

// Hypothesis 1, one shared transaction. Mutation: commit billing's mapper before the fault point (the entry survives
// the failure).
TEST_CASE("Hypothesis 1: in one shared transaction, a failure between the halves leaves no partial state and no hole",
          "[lims][billing][posting][h1]") {
    auto const point = GENERATE(std::string{"post.afterLedger"}, std::string{"post.beforeCommit"});
    CAPTURE(point);
    DbFixture fixture;
    ScopedClockOverride const clock{lims::test::at(2026, 11, 20)};
    lims::test::books();
    lims::test::billingSettings();
    lims::test::PostingRig posting{PostingMode::SharedTransaction};
    auto const invoice = lims::test::approvedInvoice();
    FaultHookGuard const guard;
    failAt(point);
    CHECK_THROWS_WITH(posting.post(invoice), Catch::Matchers::ContainsSubstring("server stopped"));
    CHECK(entries().empty());
    CHECK(current(invoice).state == lims::InvoiceState::Approved);
    CHECK(current(invoice).number.empty());

    lims::setFaultHook({});
    auto const retried = posting.post(invoice);
    CHECK(retried.number == "INV-2026-000001");
    REQUIRE(entries().size() == 1U);
    CHECK(retried.entry == entries()[0].id);
}

// What each way keeps of the ledger's own action: its authorization, its effect on a watching connection, its journal
// entry. The control is billing's own effect, which both keep. Mutation: declare RecordPosting with no effect (the
// dispatched case's Ledger_Entry check fails).
TEST_CASE("Hypothesis 1: what the shared transaction gives up of the ledger's action", "[lims][billing][posting][h1]") {
    auto const mode = GENERATE(PostingMode::Dispatched, PostingMode::SharedTransaction);
    CAPTURE(mode);
    bool const dispatched = mode == PostingMode::Dispatched;
    DbFixture fixture;
    ScopedClockOverride const clock{lims::test::at(2026, 11, 20)};
    lims::test::books();
    lims::test::billingSettings();

    // Authorization, first, while the posting principal holds no role: the ledger's rule refuses — if it runs.
    {
        lims::test::PostingRig withheld{mode, lims::test::PostingRole::Withheld};
        auto const invoice = lims::test::approvedInvoice("WW-1");
        if (dispatched) {
            CHECK_THROWS(withheld.post(invoice));
            CHECK(entries().empty());
        } else {
            CHECK(withheld.post(invoice).state == lims::InvoiceState::Posted);
            CHECK(entries().size() == 1U);
        }
    }

    // Effects and journal: an accountant watching the ledger, a clerk watching invoices, every action journaled.
    auto const journal = std::make_shared<Recorder>();
    morph::journal::ScopedActionLog const log{journal};
    lims::test::PostingRig posting{mode};
    auto const invoice = lims::test::approvedInvoice("WW-2");
    auto const accountant = posting.rig().connect();
    auto const clerk = posting.rig().connect();
    REQUIRE(accountant->watch({"Ledger_Entry"}, as("acc")).kind == "ok");
    REQUIRE(clerk->watch({"Billing_Invoice"}, as("bea")).kind == "ok");
    static_cast<void>(posting.post(invoice));
    CHECK(ServerConnection::eventually([&] { return clerk->heardOf("Billing_Invoice"); }));
    bool const ledgerHeard =
        ServerConnection::eventually([&] { return accountant->heardOf("Ledger_Entry"); }, std::chrono::milliseconds{1500});
    CHECK(ledgerHeard == dispatched);
    CHECK(journal->holds("InvoiceModel", "PostInvoice"));
    CHECK(journal->holds("LedgerModel", "RecordPosting") == dispatched);
}

// Mutation: number by the fiscal year of now() rather than of the entry's date (the retry on 2 January numbers
// INV-2027-000001).
TEST_CASE("invoice posting: a retry numbers the invoice in the fiscal year of the entry it converges on",
          "[lims][billing][posting]") {
    DbFixture fixture;
    lims::test::books();
    {
        ScopedPrincipal const acc{"acc"};
        lims::LedgerModel{}.execute(lims::OpenPeriod{.company = lims::test::seededCompany(),
                                                     .month = morph::time::Timestamp{lims::test::at(2026, 12, 1)}});
    }
    lims::test::billingSettings();
    lims::test::PostingRig posting{PostingMode::Dispatched};
    lims::InvoiceView invoice;
    {
        ScopedClockOverride const newYearsEve{lims::test::at(2026, 12, 31)};
        invoice = lims::test::approvedInvoice();
        FaultHookGuard const guard;
        failAt("post.afterLedger");
        CHECK_THROWS(posting.post(invoice));
    }
    ScopedClockOverride const january{lims::test::at(2027, 1, 2)};
    auto const retried = posting.post(invoice);
    CHECK(retried.number == "INV-2026-000001");
    CHECK(entries()[0].date == morph::time::Timestamp{lims::test::at(2026, 12, 31)});
}

// Mutation: value a foreign-currency invoice at one in SubmitInvoice (carl's limit is judged against 47.60, not 43.79,
// and the entry's company values are refused).
TEST_CASE("invoice posting: a USD invoice is valued and posted at the ledger's dated rate", "[lims][billing][posting]") {
    DbFixture fixture;
    ScopedClockOverride const clock{lims::test::at(2026, 11, 20)};
    lims::test::books();
    {
        ScopedPrincipal const acc{"acc"};
        lims::LedgerModel{}.execute(lims::SetExchangeRate{.company = lims::test::seededCompany(),
                                                          .from = lims::CurrencyCode::USD,
                                                          .to = lims::CurrencyCode::EUR,
                                                          .validFrom = morph::time::Timestamp{lims::test::at(2026, 11, 1)},
                                                          .rate = lims::Rate{exact(92, 100, 6)}});
    }
    lims::test::billingSettings();
    lims::test::PostingRig posting{PostingMode::Dispatched};
    auto const invoice = lims::test::approvedInvoice("WW-1", lims::CurrencyCode::USD);
    auto const posted = posting.post(invoice);
    REQUIRE(entries().size() == 1U);
    auto const entry = entries()[0];
    CHECK(entry.id == posted.entry);
    CHECK(entry.currency == lims::CurrencyCode::USD);
    // 47.60 × 0.92 = 43.792 → 43.79; 40.00 × 0.92 = 36.80; 7.60 × 0.92 = 6.992 → 6.99; no rounding line.
    REQUIRE(entry.lines.size() == 3U);
    CHECK(*entry.lines[0].companyAmount == exact(4379, 100, 2));
}

TEST_CASE("invoice posting: a posting the ledger refuses changes nothing in billing and leaves no hole",
          "[lims][billing][posting]") {
    auto const mode = GENERATE(PostingMode::Dispatched, PostingMode::SharedTransaction);
    CAPTURE(mode);
    DbFixture fixture;
    ScopedClockOverride const clock{lims::test::at(2026, 11, 20)};
    lims::test::books();
    lims::test::billingSettings();
    lims::test::PostingRig posting{mode};
    auto const invoice = lims::test::approvedInvoice();
    {
        ScopedPrincipal const acc{"acc"};
        lims::LedgerModel ledger;
        auto const november = ledger.execute(lims::ListPeriods{.company = lims::test::seededCompany()}).periods.at(0);
        ledger.execute(lims::BeginClosePeriod{.periodId = november.id});
        ledger.execute(lims::ClosePeriod{.periodId = november.id});
    }
    CHECK_THROWS_WITH(posting.post(invoice), Catch::Matchers::ContainsSubstring("closed"));
    CHECK(entries().empty());
    {
        ScopedPrincipal const acc{"acc"};
        lims::LedgerModel{}.execute(lims::OpenPeriod{.company = lims::test::seededCompany(),
                                                     .month = morph::time::Timestamp{lims::test::at(2026, 12, 1)}});
    }
    ScopedClockOverride const december{lims::test::at(2026, 12, 2)};
    CHECK(posting.post(invoice).number == "INV-2026-000001");
}

// Mutation: read the series' next number outside the posting's transaction (two concurrent posts take the same
// number, or one fails on the database lock).
TEST_CASE("invoice posting: two clerks posting at once get consecutive numbers", "[lims][billing][posting][socket-only]") {
    DbFixture fixture;
    ScopedClockOverride const clock{lims::test::at(2026, 11, 20)};
    lims::test::books();
    lims::test::billingSettings();
    lims::test::PostingRig posting{PostingMode::Dispatched};
    auto const first = lims::test::approvedInvoice("WW-1");
    auto const second = lims::test::approvedInvoice("WW-2");
    lims::test::grant("bob", lims::LimsRole::BillingClerk);
    posting.rig().bridge(0).setDefaultSession(as("bea"));
    posting.rig().bridge(1).setDefaultSession(as("bob"));
    auto bea = posting.rig().client<lims::InvoiceModel>(0);
    auto bob = posting.rig().client<lims::InvoiceModel>(1);
    auto beaPosting = bea.execute(lims::PostInvoice{.invoiceId = first.id});
    auto bobPosting = bob.execute(lims::PostInvoice{.invoiceId = second.id});
    std::set<std::string> const numbers{morph::ladder::testkit::awaitQt(std::move(beaPosting)).number,
                                        morph::ladder::testkit::awaitQt(std::move(bobPosting)).number};
    CHECK(numbers == std::set<std::string>{"INV-2026-000001", "INV-2026-000002"});
}
```

Every case posts through `PostingRig::post`, a socket client of the rig's server, because `PostInvoice` is a Task handler and only a dispatch drives one, and because the effects case needs the posting on the server its watchers are on. The fiscal-year case assumes the seeded company's fiscal year starts in January; if Part A seeds another start, use the last day before it and the second day of it instead.

- [ ] **Step 5: Run it and see it fail**

Run: Build lims. Expected: compile error, `lims/billing/core/ledger_port.hpp` not found.

- [ ] **Step 6: Billing's half, the mapping, the actions**

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
std::string assignNumberAndPost(Lightweight::DataMapper& mapper, std::int64_t invoiceId, PostingResult const& entry,
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
    invoice.entryId = *entry.entryId;
    mapper.Update(invoice);
    CompanyId const company{invoice.companyId.Value()};
    // The entry's date, not the clock: a retry converges on the entry the first attempt recorded, which may lie in
    // the fiscal year before the retry's.
    auto const fiscalYear = fiscalYearOf(entry.date, OrgModel::findCompany(company)->fiscalYearStart);
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

Not verified by reading: whether Lightweight opens SQLite transactions `DEFERRED` with a busy timeout. If the concurrency case fails with "database is locked", read `examples/common/testkit/db_busy_fixture.hpp` and `docs/LIGHTWEIGHT-CONSTRAINTS.md` for what the ladder already knows about SQLite locking, and record the outcome in decision 37 as a measured finding (the two posts then serialise only by retrying, which is the model's to do).

`InvoiceModel`'s `SubmitInvoice` and `PostInvoice` handlers are Task handlers (`core::async::Task<InvoiceView> execute(SubmitInvoice action);`, `core::async::Task<InvoiceView> execute(PostInvoice action);`, `#include <core/async/Task.hpp>`, as `examples/bank/include/bank/models/budget_model.hpp` declares one): each may await the ledger, and an awaiting handler holds only its own instance's gate (`docs/spec/core/coroutines.md`, "Not re-entrant: the action gate"), so awaiting a `LedgerModel` action deadlocks nothing. `PostInvoice` in `invoice_posting.cpp`:

```cpp
core::async::Task<InvoiceView> InvoiceModel::execute(PostInvoice action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"PostInvoice: an invoice is required"};
    }
    requireRoleSomewhere<PostInvoice>();
    InvoiceView invoice;
    {
        Lightweight::DataMapper mapper;
        invoice = invoiceView(mapper, *action.invoiceId);
    }
    requireAuthorized(action);
    if (invoice.state != InvoiceState::Approved) {
        throw IllegalTransition{"PostInvoice: only an approved invoice is posted"};
    }
    auto const& port = ledgerPort();
    if (!port.post || !port.postIn || !port.rateOn) {
        throw ValidationError{"PostInvoice: no ledger is wired to post to"};
    }
    auto const company = OrgModel::findCompany(invoice.company);
    auto const settings = PriceListModel::settingsOf(invoice.company);
    auto const now = ::morph::ladder::now();
    ::morph::math::Rational rate{1, ::morph::math::DecimalPlaces{6}};
    if (invoice.currency != company->baseCurrency) {
        auto const dated = co_await port.rateOn(
            RateQuery{.company = invoice.company, .from = invoice.currency, .to = company->baseCurrency, .on = now});
        if (!dated.rate.hasValue()) {
            throw ValidationError{"PostInvoice: no " + std::string{currencyName(invoice.currency)} + "/" +
                                  std::string{currencyName(company->baseCurrency)} + " rate today"};
        }
        rate = *dated.rate;
    }
    PostingRequest request = postingFor(InvoiceToPost{.company = invoice.company,
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
                                                      .roundingAccount = settings->roundingAccount});
    std::string const author = ::morph::session::current()->principal;
    if (port.mode == PostingMode::SharedTransaction) {
        // One transaction: the ledger's entry is written through billing's own mapper and commits with the number.
        Lightweight::DataMapper mapper;
        Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
        PostingResult const posted = port.postIn(mapper, request);
        faultPoint("post.afterLedger");
        static_cast<void>(assignNumberAndPost(mapper, *invoice.id, posted, author));
        sqlTxn.Commit();
        co_return invoiceView(mapper, *invoice.id);
    }
    // The first commit: the ledger records the entry as its own action.
    PostingResult const posted = co_await port.post(std::move(request));
    faultPoint("post.afterLedger");
    // The second commit: billing numbers and posts the invoice. Between the two, an entry exists for an invoice that
    // is not posted; that window is what hypothesis 1 names.
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    static_cast<void>(assignNumberAndPost(mapper, *invoice.id, posted, author));
    sqlTxn.Commit();
    co_return invoiceView(mapper, *invoice.id);
}
```

In `invoice_approval.cpp`, `SubmitInvoice` becomes a Task handler that values a foreign-currency invoice at the ledger's rate on the submission date instead of refusing it: it reads the row in a scope of its own; when the currencies differ it `co_await`s `ledgerPort().rateOn(RateQuery{company, currency, base, now})`, refuses with Task 12's words when no port is installed or no rate is valid, and converts the gross with `checkedMul` (`OutOfRange` when it does not fit) and `roundToCurrency(·, base)`; then, in a new mapper and transaction, it re-reads the row, refuses unless it is still a draft (another instance may have submitted it while this one awaited), and writes what Task 12 wrote with `gross_company` set to the converted value. Its same-currency path awaits nothing.

Register `"PostInvoice"` with `BRIDGE_REGISTER_EFFECTS(lims::PostInvoice, ::morph::effects::updates<lims::InvoiceView>)` — the entry the shared transaction makes in the ledger is the ledger's `Ledger_Entry`, which billing cannot name (Task 15 measures that). Declare `Authorization<PostInvoice>` (billing clerk at the invoice's company), add it to the edge table and the coverage tuple.

- [ ] **Step 7: The server's posting client**

In `src/server/main.cpp` (include `"lims/app/ledger_wiring.hpp"` and `"lims/billing/core/ledger_port.hpp"`), beside `events` and `schedule`, above `background`:

```cpp
        lims::app::InternalClient posting{*server, secret, std::string{lims::app::kPostingPrincipal}};
        // The posting mode decision 37 names.
        lims::app::installLedgerPort(posting, lims::PostingMode::Dispatched);
```

and, after `QCoreApplication::exec()` returns, `lims::setLedgerPort({});` before the clients go.

- [ ] **Step 8: Build and run**

Run: Build lims, Model suite `"[posting]"`, Model suite `"[approval]"` (its cases now dispatch a Task handler), Everything labelled for the rung. Expected: all pass. Run the nine mutations named in the file and restore each. Then read the measured outcome off the three `[h1]` cases and write it down before Step 9:

- (a) dispatched: does a failure between the halves leave an entry for an unposted invoice? (expected: yes) Does it leave a hole? (expected: no)
- (b) shared transaction: does a failure leave any partial state or a hole? (expected: no to both)
- (b) gives up: is `Ledger_Entry` heard on a watching connection? (expected: no) Does the ledger's rule refuse without the service role? (expected: no — the posting succeeds) Does the journal hold a `LedgerModel` `RecordPosting`? (expected: no; and if (a)'s journal check also fails, lims's models are not journaled by dispatch, so the journal is not something (b) gives up: change that check to `CHECK_FALSE(... )` for both modes and drop the journal from decision 37's costs.)

- [ ] **Step 9: Record decision 37**

Choose the bold paragraph by what Step 8 wrote down, and keep the server's `installLedgerPort` mode as the paragraph says:

- **Refuted** — (b) left no partial state and no hole, and gave up none of the three: the server installs `PostingMode::SharedTransaction`.
- **Confirmed, with the workaround's cost** — (b) left no partial state and no hole but gave up at least one of the three (the expected outcome): the server keeps `PostingMode::Dispatched`.
- **Mis-framed** — (b) left partial state or a hole too, or (a) left none: the server keeps `PostingMode::Dispatched`, and the paragraph says what the gap is instead.

```markdown
### 37. Posting an invoice: two commits in sequence keep each module's action; one transaction gives the ledger's up (hypothesis 1, first instance)

`PostInvoice` builds the invoice's `PostingRequest` (`lims::postingFor`: receivable debit, revenue and tax credits,
each converted at the ledger's dated rate and rounded half-up on its own, and a line to the company's rounding account
for the difference, whole yen in a yen company) and hands it to the ledger through the port the app installs
(`lims::app::installLedgerPort`). Billing then assigns the next number of the company's fiscal-year series
(`INV-<fiscal year>-<six digits>`, the fiscal year of the entry's date, named by the calendar year it begins in),
marks the invoice posted with its entry, and commits. The number is assigned in billing's commit, so no failure leaves
a hole in the series. The port reaches the ledger one of two ways, and both were measured in
`tests/test_invoice_posting.cpp`. Dispatched, the ledger's half is `RecordPosting`, the ledger's own action, sent by
the app's internal client as `system:posting`: it is authorized by its declared rule (the service role at the
company), journaled, and its `Ledger_Entry` change reaches a watching accountant — and it commits alone, so a failure
before billing's commit leaves the ledger holding an entry for an invoice that is approved, unnumbered and unposted,
which a retry converges (the ledger answers the entry it recorded). In one shared transaction, the port writes the
entry through billing's own mapper (`recordPostingIn`) and both halves commit once: a failure anywhere leaves nothing,
and no hole. The server posts [Dispatched | SharedTransaction].

[One of:]

**Hypothesis 1 is confirmed, with the workaround's cost.** The database can make the two halves one fact, and lims
did, with seams that exist — which is the design's refutation condition on its letter. But only by not running the
ledger's half as the ledger's action: in the shared transaction no `Ledger_Entry` change reached the watching
accountant, the ledger's rule never ran (the posting succeeded with the posting principal holding no role), and the
journal holds no ledger entry for it [drop any of the three Step 8 did not measure as lost]. With the ledger's half as
its action, the window is real. What morph lacks is a way to run two models' actions as one fact while keeping each
one's authorization, effects and journal. The server keeps the dispatched posting, whose window converges on retry,
until that seam exists. Goods receipt in Part C is the second instance, and the seam is designed against both.

**Hypothesis 1 is refuted for this instance.** One shared transaction made the two halves one fact with seams that
exist, and gave up nothing measured: [what carried the effect, the authorization and the journal]. The server posts
in one transaction.

**Hypothesis 1 is mis-framed for this instance.** [What Step 8 measured that the hypothesis does not describe: the
shared transaction still left partial state or a hole, because …; or the dispatched posting left no window, because …]
The gap is [what it is].
```

Keep the one paragraph that applies, fill its brackets from Step 8, and set the server's mode to match.

- [ ] **Step 10: Commit**

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/posting examples/lims/ledger examples/lims/billing examples/lims/app \
        examples/lims/src/server/main.cpp examples/lims/tests examples/lims/README.md
git commit -m "Post lims invoices dispatched and in one transaction, and measure what each keeps and leaves

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---
### Task 15: Does one module's write reach another module's watcher? (the cross-module effects candidate)

**Files:**
- Create: `examples/lims/tests/test_cross_module_effects.cpp`, `examples/lims/tests/compile_checks/cross_module_effects_probe.cpp`
- Modify: `examples/lims/CMakeLists.txt` (the probe's two executables and their tests), `examples/lims/README.md` (decision 38)

**Interfaces:**
- Consumes: `PostInvoice` in `PostingMode::SharedTransaction` (Task 14), `PostingRig`, `RecordBillableLine`, `lims::app::relayLabEvents`, `InternalClient`, `BackendRig::connect`, `morph::effects::actionsHolding`, `morph::model::EffectsRegistrationError`.
- Produces: a measured verdict, cause by cause, on README decision 26's finding; the issue, if confirmed.

README decision 26 inferred from reading `include/morph/core/effects.hpp` that a module's action cannot declare effects on another module's records, so a screen showing one module's records is not refreshed when another module's action changes them; it was never reproduced. Two different causes hide in that sentence, and each is measured on its own:

- **(i) An action cannot declare the cross-module effect.** The clean case is `PostInvoice` in one shared transaction (Task 14): billing's action writes a `Ledger_Entry` inside itself, and billing cannot name the ledger's record — it does not include the ledger (the `billing_includes_ledger` probe), and the composing code, which sees both, cannot add the declaration either: a second `BRIDGE_REGISTER_EFFECTS` for the same action makes the whole effects configuration refuse to resolve ("declares different effects in two registrations", `include/morph/core/effects.hpp`).
- **(ii) A write outside any action derives no changes.** The relay-made billing line is the case: the same `RecordBillableLine` body writes the same row whether it is dispatched through the events client or run in process outside any dispatch; only the first is an executed action the server derives changes from.

- [ ] **Step 1: The declaration probe**

Create `examples/lims/tests/compile_checks/cross_module_effects_probe.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Built twice, linked with every lims module. With LIMS_PROBE_DECLARES_LEDGER_EFFECT it adds, from the composing
// code, the declaration billing cannot make: PostInvoice changes the ledger's Ledger_Entry. Either way it reads the
// effects registry once and prints whether the configuration resolved.

#include <iostream>
#include <morph/core/effects.hpp>

#include "lims/billing/models/invoice_model.hpp"
#include "lims/ledger/dto/entry_dto.hpp"

#ifdef LIMS_PROBE_DECLARES_LEDGER_EFFECT
// NOLINTBEGIN(misc-const-correctness)
BRIDGE_REGISTER_EFFECTS(lims::PostInvoice, ::morph::effects::updates<lims::InvoiceView>,
                        ::morph::effects::changes<lims::JournalEntryView>)
// NOLINTEND(misc-const-correctness)
#endif

int main() {
    try {
        static_cast<void>(::morph::effects::actionsHolding("Billing_Invoice"));
        std::cout << "effects resolved\n";
        return 0;
    } catch (::morph::model::EffectsRegistrationError const& error) {
        std::cout << error.what() << '\n';
        return 1;
    }
}
```

In `examples/lims/CMakeLists.txt`, inside the `if(MORPH_BUILD_TESTS AND NOT EMSCRIPTEN)` block, after the boundary probes:

```cmake
    # The composing code declaring one module's action's effect on another module's record: the probe adds a second
    # effects registration for PostInvoice and must be refused; the control, without it, must resolve.
    foreach(_variant IN ITEMS refused control)
        add_executable(lims_cross_module_effects_${_variant} tests/compile_checks/cross_module_effects_probe.cpp)
        target_link_libraries(lims_cross_module_effects_${_variant} PRIVATE ladder_lims_lib)
        set_target_properties(lims_cross_module_effects_${_variant} PROPERTIES EXPORT_COMPILE_COMMANDS OFF)
        add_test(NAME lims.cross_module_effects.${_variant} COMMAND lims_cross_module_effects_${_variant})
    endforeach()
    target_compile_definitions(lims_cross_module_effects_refused PRIVATE LIMS_PROBE_DECLARES_LEDGER_EFFECT=1)
    set_tests_properties(lims.cross_module_effects.refused PROPERTIES LABELS ladder-lims
        PASS_REGULAR_EXPRESSION "declares different effects in two registrations")
    set_tests_properties(lims.cross_module_effects.control PROPERTIES LABELS ladder-lims
        PASS_REGULAR_EXPRESSION "effects resolved")
```

`PASS_REGULAR_EXPRESSION` judges the output, not the exit code, so the refused variant passes by printing the refusal. If `ladder_lims_lib`'s whole-archive links need Qt's runtime path on this platform, copy the `DL_PATHS`/`ENVIRONMENT` the rung's test executables get in `cmake/morph_add_rung.cmake`.

- [ ] **Step 2: Write the connection test**

Create `examples/lims/tests/test_cross_module_effects.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The cross-module effects question, cause by cause, on watching connections: (i) billing's PostInvoice writes the
// ledger's Ledger_Entry inside itself and cannot declare it; (ii) the relay's billing line reaches a watcher when its
// write is a dispatched action, and not when the same write runs outside any dispatch.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <glaze/glaze.hpp>
#include <morph/session/session_auth.hpp>
#include <string>

#include "billing_test_support.hpp"
#include "clock.hpp"
#include "lims/app/event_wiring.hpp"
#include "lims/app/internal_client.hpp"
#include "lims/app/service_principals.hpp"
#include "lims/billing/models/invoice_model.hpp"
#include "lims/ledger/models/ledger_model.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::ScopedClockOverride;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::ServerConnection;

namespace {

/// @brief A session for @p principal, signed with the test servers' secret.
[[nodiscard]] morph::session::Context as(std::string const& principal) {
    morph::session::TokenIssuer const issuer{std::string{lims::test::kServiceSecret}, morph::session::hmacSha256};
    return lims::test::signedAs(issuer, principal);
}

}  // namespace

// Cause (i). The control is the ledger's own action on the same connection, which it hears.
TEST_CASE("cross-module effects (i): billing's write of a ledger entry inside its own action reaches no ledger "
          "watcher",
          "[lims][effects]") {
    DbFixture fixture;
    ScopedClockOverride const clock{lims::test::at(2026, 11, 20)};
    lims::test::books();
    lims::test::billingSettings();
    lims::test::PostingRig posting{lims::PostingMode::SharedTransaction};
    auto const invoice = lims::test::approvedInvoice();
    auto const accountant = posting.rig().connect();
    REQUIRE(accountant->watch({"Ledger_Entry"}, as("acc")).kind == "ok");

    auto const posted = posting.post(invoice);
    REQUIRE(posted.state == lims::InvoiceState::Posted);
    bool const heard = ServerConnection::eventually([&] { return accountant->heardOf("Ledger_Entry"); },
                                                    std::chrono::milliseconds{1500});
    CHECK_FALSE(heard);

    // The control: the ledger's own action, dispatched on the same server, is heard on the same connection.
    auto const ledger = accountant->registerModel("LedgerModel", as("acc"));
    auto const draft = glz::write_json(lims::CreateDraftEntry{
                                           .company = lims::test::seededCompany(),
                                           .date = morph::time::Timestamp{lims::test::at(2026, 11, 20)},
                                           .memo = "control",
                                           .currency = lims::CurrencyCode::EUR,
                                           .rate = lims::Rate{morph::math::Rational{1, morph::math::DecimalPlaces{6}}},
                                           .lines = {lims::PostingLine{.accountCode = "6900",
                                                                       .side = lims::PostingSide::Debit,
                                                                       .amount = lims::Amount{morph::math::Rational{
                                                                           1, morph::math::DecimalPlaces{2}}},
                                                                       .companyAmount = lims::Amount{morph::math::Rational{
                                                                           1, morph::math::DecimalPlaces{2}}}},
                                                     lims::PostingLine{.accountCode = "2200",
                                                                       .side = lims::PostingSide::Credit,
                                                                       .amount = lims::Amount{morph::math::Rational{
                                                                           1, morph::math::DecimalPlaces{2}}},
                                                                       .companyAmount = lims::Amount{morph::math::Rational{
                                                                           1, morph::math::DecimalPlaces{2}}}}}})
                           .value_or(std::string{});
    REQUIRE(accountant->execute(ledger, "LedgerModel", "CreateDraftEntry", draft, as("acc")).kind == "ok");
    CHECK(ServerConnection::eventually([&] { return accountant->heardOf("Ledger_Entry"); }));
}

// Cause (ii). Mutation: declare RecordBillableLine with no effect (the dispatched line is not heard either).
TEST_CASE("cross-module effects (ii): the relay's billing line reaches a watcher as a dispatched action, and not "
          "when the same write runs outside one",
          "[lims][effects]") {
    DbFixture fixture;
    auto const lab = lims::test::publishedSample();
    lims::test::billingFor(lab.client);
    lims::test::grantService(std::string{lims::app::kEventsPrincipal});
    BackendRig rig{Mode::Socket, 0, lims::test::limsAuthorizer(lims::test::kServiceSecret)};
    auto const watcher = rig.connect();
    REQUIRE(watcher->watch({"Billing_Line"}, as("bea")).kind == "ok");

    // Outside any dispatch: the consumer's body, run in process as the events principal, writes the line.
    {
        ScopedPrincipal const events{std::string{lims::app::kEventsPrincipal}};
        static_cast<void>(lims::InvoiceModel{}.execute(lims::RecordBillableLine{
            .eventId = "test:outside:1",
            .result = lims::ResultId{9001},
            .sample = lab.sample,
            .analysis = lab.nitrate,
            .client = lab.client,
            .site = lims::test::seededSite(),
            .publishedAt = morph::time::Timestamp{lims::test::at(2026, 11, 10)}}));
    }
    bool const heardOutside = ServerConnection::eventually([&] { return watcher->heardOf("Billing_Line"); },
                                                           std::chrono::milliseconds{1500});
    CHECK_FALSE(heardOutside);

    // Dispatched: the relay, through the events client on the same server.
    lims::app::InternalClient events{rig.server(), lims::test::kServiceSecret, std::string{lims::app::kEventsPrincipal}};
    REQUIRE(lims::app::relayLabEvents(events) == 1U);
    CHECK(ServerConnection::eventually([&] { return watcher->heardOf("Billing_Line"); }));
}
```

- [ ] **Step 3: Build and run**

Run: Configure, Build lims (and `--target lims_cross_module_effects_refused lims_cross_module_effects_control`), then `ctest --test-dir build/linux-everything -R 'lims\.cross_module_effects' --output-on-failure` under the lock, and Model suite `"[effects]"`. Expected (inferred from `effects.hpp` and from the server deriving changes only from executed actions' declared effects): both probes pass — the refused variant prints the refusal, the control resolves; case (i)'s `CHECK_FALSE(heard)` holds and its control is heard; case (ii)'s outside write is not heard and the dispatched one is. Run the mutation and restore it. Keep the outputs:

```bash
cd /home/yaraslau/repo/morph
flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock \
  build/linux-everything/examples/lims/lims_cross_module_effects_refused \
  > /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/plan_b_effects_output.txt 2>&1
flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock \
  timeout 300 build/linux-everything/examples/lims/ladder_lims_tests "[effects]" -s 2>&1 | tail -60 \
  >> /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/plan_b_effects_output.txt
```

(The executables' path is the rung's build directory; use `find build/linux-everything -name 'lims_cross_module_effects_refused'` if it differs.) If a measurement comes out the other way, change its check to what was measured and use that cause's other text in Step 4.

- [ ] **Step 4: Record decision 38, and file what is a framework gap**

Append to the README, keeping for each cause the sentence its measurement chose:

```markdown
### 38. One module's write reaches another module's watcher only as that module's own dispatched action

Two causes, measured apart in `tests/test_cross_module_effects.cpp` and by the `lims.cross_module_effects.*` probes.

**(i) An action cannot declare an effect on another module's record — confirmed.** `PostInvoice` posting in one
shared transaction (decision 37) writes the ledger's `Ledger_Entry` inside billing's action; an accountant watching
`Ledger_Entry` is not told, while the ledger's own `CreateDraftEntry` on the same connection is heard. Billing cannot
name the ledger's record (it does not include the ledger), and the composing code, which sees both, cannot add the
declaration: a second `BRIDGE_REGISTER_EFFECTS` for `PostInvoice` makes the effects configuration refuse to resolve,
"action `PostInvoice` declares different effects in two registrations". [Or, refuted: what carried the change.]

**(ii) A write outside any action derives no changes — confirmed, and not a gap.** `RecordBillableLine` run in process
outside any dispatch writes the line and no watcher of `Billing_Line` is told; the same write dispatched through the
events client is heard. Changes are derived from executed actions by design, and the internal client is how a
reaction becomes one (decision 31). [Or, if the dispatched line was not heard either: "— confirmed, and a gap: even
dispatched through the server's own client, the line reached no watcher: [output]".]

This is the second place decision 26's finding appears — org's role changes not reaching lab's sample list was the
first — so cause (i) is filed against `include/morph` as a missing framework seam: a way for the code that composes
modules to state that one module's action changes another module's records.
```

If (i) is confirmed, file the issue. Pick the label first (`gh label list --limit 100 | grep -i 'area'`; `area: core` covers `core/effects.hpp`), write the body to `/tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/plan_b_effects_issue.md`, and run:

```bash
cd /home/yaraslau/repo/morph
gh issue create --title "Effects: no way to declare that one module's action changes another module's records" \
  --label enhancement --label "area: core" \
  --body-file /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/plan_b_effects_issue.md
```

The body names each cause separately, each with its evidence and status: cause (i) — reproduced, on `git rev-parse HEAD`, by `lims.cross_module_effects.refused` (its output line) and the (i) case (its `-s` lines from the output file); cause (ii) — included only if the dispatched line was not heard either, with that output, and otherwise stated as measured and not a gap (changes derive from executed actions; an internal client makes a reaction one); the two places (org→lab inferred in decision 26, billing→ledger measured here); what was not verified (a screen over these records rather than a raw connection — the screen's refresh is driven by the same `changes` events, so it is inferred to behave the same); and what would close it (a declared cross-module effect reaching the (i) case's watcher without a refetch). No `triage:` label.

- [ ] **Step 5: Commit**

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/CMakeLists.txt examples/lims/tests/compile_checks/cross_module_effects_probe.cpp \
        examples/lims/tests/test_cross_module_effects.cpp examples/lims/README.md
git commit -m "Measure apart why one lims module's write does or does not reach another module's watcher

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---
### Task 16: Does a company-B change reach a company-A connection? (hypothesis 7, first instance)

**Files:**
- Create: `examples/lims/tests/test_two_company_effects.cpp`
- Modify: `examples/lims/README.md` (decision 39)

**Interfaces:**
- Consumes: `BackendRig::connect`, `ServerConnection`, `RecordBillableLine`, `CreateInvoice`, `PriceListModel`, `OrgModel`, `lims::test::grantService`.
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
#include <chrono>
#include <glaze/glaze.hpp>
#include <morph/session/session_auth.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <string_view>

#include "billing_test_support.hpp"
#include "lims/app/service_principals.hpp"
#include "lims/billing/models/invoice_model.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::ServerConnection;
using morph::math::DecimalPlaces;
using morph::math::Rational;

namespace {

constexpr std::string_view kSecret = "lims-two-company-effects-secret-at-least-32";

/// @brief A session for @p principal, signed with the case's secret.
[[nodiscard]] morph::session::Context as(std::string const& principal) {
    morph::session::TokenIssuer const issuer{std::string{kSecret}, morph::session::hmacSha256};
    return lims::test::signedAs(issuer, principal);
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
    // The unbilled line company B invoices: written as the relay writes one, called in process, since this case
    // measures the invoice's fan-out, not the relay's.
    lims::test::grantService(std::string{lims::app::kEventsPrincipal}, lims::Scope{company.id});
    ScopedPrincipal const events{std::string{lims::app::kEventsPrincipal}};
    static_cast<void>(lims::InvoiceModel{}.execute(lims::RecordBillableLine{
        .eventId = "test:osaka:1",
        .result = lims::ResultId{901},
        .sample = lims::SampleId{901},
        .analysis = lims::AnalysisId{1},
        .client = lims::ClientId{1},
        .site = site.id,
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
    BackendRig rig{Mode::Socket, 0, lims::test::limsAuthorizer(kSecret)};
    auto const alice = rig.connect();
    auto const olga = rig.connect();
    auto const olgaModel = olga->registerModel("InvoiceModel", as("olga"));
    REQUIRE(alice->watch({"Billing_Invoice"}, as("alice")).kind == "ok");
    REQUIRE(olga->watch({"Billing_Invoice"}, as("olga")).kind == "ok");

    auto const body = glz::write_json(lims::CreateInvoice{.company = companyB, .site = siteB, .client = lims::ClientId{1}})
                          .value_or(std::string{});
    auto const created = olga->execute(olgaModel, "InvoiceModel", "CreateInvoice", body, as("olga"));
    REQUIRE(created.kind == "ok");

    // The control: the writer's own connection hears of it, so the watch and the fan-out work.
    REQUIRE(ServerConnection::eventually([&] { return olga->heardOf("Billing_Invoice"); }));
    bool const leaked = ServerConnection::eventually([&] { return alice->heardOf("Billing_Invoice"); },
                                                     std::chrono::milliseconds{1000});
    for (auto const& sent : alice->changes()) {
        for (auto const& change : sent.records) {
            UNSCOPED_INFO("sent to company A: " << change.record << (change.keys ? " with keys" : " as a whole"));
        }
    }
    CHECK(leaked);
}
```

The `UNSCOPED_INFO` lines print what company A was sent when the case fails; run it once with `-s` to see them when it passes, for decision 39 (whether the event carries company B's invoice key). The connections come from `BackendRig::connect()` (Task 9).

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
### Task 17: Hypothesis 6's second instance — decide from the measurement, then specify the seam and have it reviewed, or disposition the rule

**Files (seam branch):**
- Create: `docs/spec/core/action_guards.md`
- Modify: `docs/spec/README.md` (the spec's index line), `docs/spec/core/registry.md` and `docs/spec/core/bridge.md` (the guard's place in each path)

**Files (disposition branch):** `examples/lims/README.md` (decision 40) only.

**Interfaces:**
- Consumes: decision 35, `/tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/plan_b_h6_measure.txt`, `include/morph/core/registry.hpp`, `include/morph/core/bridge.hpp`, `include/morph/session/session.hpp`, `lims/org/authorization/declared.hpp`.
- Produces: the reviewed spec of `morph::model::ActionGuard` (seam branch), which Tasks 18 and 19 implement; or decision 40's disposition, after which Tasks 18 and 19 are not done.

This task, Task 18 and Task 19 are conditional on the measured recurrence: the seam is designed only if Task 12's measurement shows the same hand-rolled guarantee in a second module.

- [ ] **Step 1: Decide from what was measured**

Read decision 35 and `/tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/plan_b_h6_measure.txt`. The seam branch applies when both hold: (a) every declared action's handler calls `requireRoleSomewhere` and `requireAuthorized` itself, in four modules, so a forgotten call is an unguarded action that compiles and passes every test that does not name it; (b) the edge restates every declaration in `declaredRequirements()` because `IAuthorizer::authorize` sees no payload and nothing enumerates a model's actions. Both are mechanism — bookmarks or a CRM would need a check that runs after decoding on every dispatch path unchanged — and both are now needed by two modules (lab's pair, billing's pair and limit). The roles, scopes, limits and pairs stay lims's: the framework runs a declared check, it does not know what the check decides. If (a) or (b) does not hold, take the disposition branch (Step 2) and skip the rest of this task and Tasks 18 and 19.

- [ ] **Step 2: Disposition branch only — record decision 40 and commit**

```markdown
### 40. Declared authorization stays domain logic each model calls (hypothesis 6, resolved)

[The reasons from Step 1 or the review: which of the two conditions did not hold, or why a framework check cannot run
it without knowing the domain.] The rules, the scope lookup and the call in each handler stay lims's; the edge keeps
its table. **Hypothesis 6 is resolved as domain logic the models must own.**
```

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/README.md
git commit -m "Record lims declared authorization as domain logic

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

Then stop: Tasks 18 and 19 are not done.

- [ ] **Step 3: Seam branch — write the spec**

Create `docs/spec/core/action_guards.md`. It states, in present tense and without history (`AGENTS.md`):

- **What a guard is.** A check an application declares per action type, by specialising `morph::model::ActionGuard<Action>` with `static void check(Action const&)`. The framework runs it on every dispatch path, on the decoded action, after the gates `prepareAction` applies (`reconcileDeclaredPrecision`, `enforceQuantityBounds`, `recomputeAll`, `ActionValidator<Action>::ready`) and before `Model::execute`. A guard that throws refuses the action; the exception is the execute's failure and is journaled as `Outcome::Failed` when the action is loggable, exactly as a throw from `execute` is, because a refused attempt is audit-worthy (lims decision 4).
- **Where it runs.** `ActionDispatcher`'s `runHandler` and its Task runner (the server path, so every remote topology); `Bridge`'s `localOp` and `localTaskOp` (the in-process path). `session::current()` is the execute's session on both, so a guard reads the principal the model would. On the server it runs on the model's strand, so it may read the model's store as `execute` would.
- **Why not `IAuthorizer`.** `authorize(ctx, modelType, actionType)` decides before the payload is decoded and is shared by every model; a guard is per action, sees the payload, and runs where the model runs. The authorizer keeps authentication and model-level gates; a guard is what reads the payload.
- **Direct calls.** Calling `model.execute(action)` directly runs no guard. `morph::model::executeChecked(model, action)` runs the validator, the guard and `execute` in that order, for a caller that holds a model directly — a unit test of a guard's outcome, or code that runs a model in process; lims's background work dispatches through internal clients, so the guard runs on its path like any client's. `IMPLEMENTATION.md` rule 1's "models must re-check their own authorization" is met by a guard declared beside the action.
- **Enumeration.** `morph::model::actionFacts(modelType)` lists a registered model's actions with whether each is guarded, so an application can prove every mutating action is guarded or deliberately open without listing them by hand.
- **What it does not do.** It does not order guards against each other, compose checks across models, or know roles, scopes, limits or pairs.
- **Its two consumers.** lims's lab (capture/verify pair) and billing (create/approve pair with a value limit), both through one constrained specialisation over `lims::DeclaresRule`.
- **Alternatives considered,** each with why not: a payload-receiving `IAuthorizer` overload (it runs on the server strand before the model's strand, so it cannot read the model's rows in order with the execute); a registration macro with a function pointer (a trait composes with a constrained partial specialisation, which is what lets one lims declaration cover every module's actions); leaving the call in each handler (the measured recurrence).

Add the file to `docs/spec/README.md`'s index, and one paragraph each to `docs/spec/core/registry.md` (the server path) and `docs/spec/core/bridge.md` (the local path) pointing to it.


- [ ] **Step 4: One independent review**

Dispatch one review subagent on the default model (not Fable), giving it `docs/spec/core/action_guards.md`, `include/morph/core/registry.hpp`, `include/morph/core/bridge.hpp`, `include/morph/session/session.hpp`, and lims's `org/include/lims/org/authorization/declared.hpp` and README decisions 25 and 35 — the code and the evidence, not this plan's conclusion. Ask it whether the seam is mechanism, whether the two call sites cover every dispatch path (it should look for a third), whether journaling a refusal is right — lims decision 4 journals refused transitions but not unauthenticated attempts, and the lims guard's `requirePrincipal` would now throw inside the journaled `try` — and what the spec leaves undecided. Address every finding in the spec before Task 18; if it shows the rule is domain logic that cannot be run by the framework without knowing the domain, take the disposition branch instead (Step 2) and record its reasoning there.

- [ ] **Step 5: Commit the reviewed spec**

```bash
cd /home/yaraslau/repo/morph
git add docs/spec/core/action_guards.md docs/spec/README.md docs/spec/core/registry.md docs/spec/core/bridge.md
git commit -m "Specify morph action guards: a declared per-action check run on every dispatch path

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 18: The action guard in `include/morph`, with its tests (seam branch of Task 17)

**Files:**
- Create: `include/morph/core/action_guard.hpp`, `tests/test_action_guard.cpp`
- Modify: `include/morph/core/registry.hpp` (`runHandler`, the Task runner, `ActionEntry`, `actionFacts`), `include/morph/core/bridge.hpp` (`localOp`, `localTaskOp`), `tests/CMakeLists.txt` (the new test file, if the suite lists its sources)

**Interfaces:**
- Consumes: the spec Task 17 committed, `morph::model::detail::ActionDispatcher::prepareAction`, `runHandler`, `Bridge`'s `localOp`/`localTaskOp`, `ActionValidator`.
- Produces: `template <typename Action> struct morph::model::ActionGuard;` (customisation point: `static void check(Action const&)`), `template <typename Action> concept morph::model::Guarded`, `template <typename Action> void morph::model::detail::guardAction(Action const&)`, `template <typename Model, typename Action> ActionTraits<Action>::Result morph::model::executeChecked(Model&, Action const&)` (validate, guard, execute — for a caller that runs a model directly), `struct morph::model::ActionFact { std::string actionType; bool guarded; }` and `std::vector<ActionFact> morph::model::actionFacts(std::string_view modelType)`.

Done only on Task 17's seam branch. No lims file changes here; Task 19 moves lims onto the guard.

- [ ] **Step 1: Write the failing framework test**

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

- [ ] **Step 2: Implement the seam**

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

- [ ] **Step 3: Run the framework test**

Run: the framework suite commands from "Build and test commands" (`morph_tests`, `"[guard]"`). Expected: the four cases pass. Run the two mutations named in the file and restore each (touch the restored header: a restored file keeps its old mtime and ninja keeps the mutant). Then build and run the whole framework suite once, and a GCC sweep of `registry.hpp`/`bridge.hpp` through `tests/test_action_guard.cpp` (`g++ -std=c++23 -fsyntax-only` with the suite's include flags from `build/linux-everything/compile_commands.json`), since local builds are clang.

- [ ] **Step 4: Commit**

```bash
cd /home/yaraslau/repo/morph
git add include/morph/core/action_guard.hpp include/morph/core/registry.hpp include/morph/core/bridge.hpp \
        tests/test_action_guard.cpp tests/CMakeLists.txt
git commit -m "Run a declared action guard on every dispatch path, before Model::execute

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

If a defect in `include/morph` turned up on the way (a third dispatch path the guard missed, say), file it per `AGENTS.md` with its real output; the seam itself, built in this branch, is not a defect and is not filed.

---

### Task 19: lims moves onto the action guard (seam branch of Task 17)

**Files:**
- Modify: `examples/IMPLEMENTATION.md` (rule 1), `examples/lims/org/include/lims/org/authorization/declared.hpp` (the lims guard), every lims model `.cpp` that calls `requireRoleSomewhere`/`requireAuthorized` (the calls go), `examples/lims/app/include/lims/app/declared_requirements.hpp` and `lims_authorizer.hpp` (the edge table goes), the lims suites that call a declared action's `execute` directly to test a refusal, `examples/lims/tests/test_declared_authorization.cpp` (coverage by enumeration), `examples/lims/README.md` (decision 40, and decision 25's last paragraph)

**Interfaces:**
- Consumes: `morph::model::ActionGuard`, `morph::model::executeChecked`, `morph::model::actionFacts` (Task 18), `lims::requireRoleSomewhere`, `lims::requireAuthorized`, `lims::DeclaresRule`.
- Produces: in lims, one constrained specialisation `template <typename Action> requires lims::DeclaresRule<Action> struct morph::model::ActionGuard<Action>`.

Done only on Task 17's seam branch, after Task 18. The handlers' own `requireServicePrincipal` checks stay: they are not part of a declared rule, and the internal clients dispatch, so the guard runs on their path too.

- [ ] **Step 1: lims adopts the guard**

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
- In every suite, a direct `model.execute(action)` of a declared action whose case checks a refusal or relies on the rule becomes `morph::model::executeChecked(model, action)`. Find them with `grep -rn 'execute(lims::\(VerifyResult\|CreateCompany\|CreateSite\|AssignRole\|RevokeRole\|SetValueLimit\|Create\|Open\|Post\|Reverse\|Close\|Begin\|Set\|Register\|Submit\|Approve\|Lease\|Start\|Advance\|Record\|Run\)' examples/lims/tests examples/lims/screens/tests`. `lims::test::grant` uses `OrgModel` directly for the bootstrap; make it use `executeChecked` too, so the bootstrap carve-out is exercised as it is dispatched.
- Delete `declaredRequirements()` and `requiredRole()` from `declared_requirements.hpp`, and the role branch from `LimsAuthorizer::authorize` (the guard runs on the server's dispatch path, with the payload; the authorizer authenticates). Keep `openMutatingActions()`.
- Rewrite the coverage case in `test_declared_authorization.cpp` over `morph::model::actionFacts` for every lims model: each action with an effect other than `None` is guarded or in `openMutatingActions()`, never both, and every id in `openMutatingActions()` is a registered action. The `Mutating` tuple goes.
- In `examples/IMPLEMENTATION.md` rule 1, after "Models must re-check their own preconditions and authorization (`Context::principal`) — the schema's `required` and the client gates are UX, not security.", add: "An authorization rule that reads the payload is declared as a `morph::model::ActionGuard` beside the action, which the framework runs on every dispatch path; a direct caller uses `morph::model::executeChecked`."


- [ ] **Step 2: Run everything**

Run: Configure, Build lims, Everything labelled for the rung, the Screen suite, and the framework suite. Expected: all pass, the Hypothesis 6 cases of both instances included (the edge no longer allows-then-the-model-refuses: the guard refuses on the server path; change those two cases to assert the refusal reaches the client in every mode, and that `LimsAuthorizer::authorize` alone no longer gates by role). Run the mutation "remove `lims::requireAuthorized(action)` from the lims `ActionGuard`" once and see the verification, approval and value-limit cases fail across lab, org, ledger and billing; restore it.

- [ ] **Step 3: Record decision 40 and commit**

```markdown
### 40. Declared authorization is a framework action guard (hypothesis 6, resolved)

`morph::model::ActionGuard<Action>` (`docs/spec/core/action_guards.md`) is a check the framework runs on the decoded
action on every dispatch path, after its validator and before `execute`. lims declares one constrained guard for every
action with a `lims::Authorization` rule, so lab's capture/verify pair, billing's approval pairs and value limit,
and every scoped role are checked once, by the framework, for every module; no handler calls the check, and the edge
keeps no table of roles. The rules themselves stay lims's. A caller that names a row that does not exist, and
holds the role somewhere, is refused in words rather than told the row is missing, because the guard runs before
the handler looks the row up; the cases that changed are [list]. Coverage of "every mutating action is guarded or
deliberately open" is proved by enumerating `morph::model::actionFacts`, not by a list kept by hand. **Hypothesis 6
is resolved by the seam:** a declared rule that sees the payload is checked once by the framework for every module.
```

and change the last paragraph of decision 25 to: "This is instance 1 of 2; with billing's approval pairs and value limit as the second, the check is the framework's action guard (decision 40)."

```bash
cd /home/yaraslau/repo/morph
git add examples/IMPLEMENTATION.md examples/lims
git commit -m "Move lims declared authorization onto the framework action guard

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---
### Task 20: Part B's definition of done, measured

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
Expected: `100% tests passed`, the nine `lims.module_boundary.*` tests and the two `lims.cross_module_effects.*` tests among them; no duplicate test names.

- [ ] **Step 2: Every hypothesis Part B touched has a recorded outcome**

Run: `grep -n '^### \(2[89]\|3[0-9]\|40\)\.' examples/lims/README.md` and `grep -n 'Hypothesis [0-9] is \|hypothesis [0-9].*\(confirmed\|refuted\|resolved\|mis-framed\)' examples/lims/README.md`. Expected: decisions 28–40, and an outcome sentence for hypotheses 1 (37), 2 (31), 3 (36), 4 (33, and mis-framed for escalation in 34), 5 (34), 6 (35, 40), 7 (39) and 8 (29, 32), plus decision 38's verdict on the cross-module effects finding, cause by cause, and the numbers of any issue filed (Task 15's, and Task 11's if it filed one) in the session's notes (not in the README: `AGENTS.md`). Every `[measured]` slot in those decisions is filled with the number its step printed.

- [ ] **Step 3: Every module follows the rules**

Run each check and read its output:
```
cd /home/yaraslau/repo/morph
grep -rn 'sqlite3_\|"SELECT \|"INSERT \|"UPDATE \|"DELETE ' examples/lims/{ledger,billing,posting}/ examples/lims/lab/src/events/
grep -rnE '^\s+(int|bool|double|float|std::int64_t|std::uint64_t) [a-z][A-Za-z]*( =|\{|;)' examples/lims/{ledger,billing,posting}/include/lims/*/dto/ examples/lims/posting/include
grep -rn 'BRIDGE_REGISTER_ACTION' examples/lims/{ledger,billing}/include | wc -l
grep -rn 'BRIDGE_REGISTER_EFFECTS' examples/lims/{ledger,billing}/include | wc -l
grep -rn 'ScopedContext' examples/lims --include='*.cpp' --include='*.hpp' | grep -v '/tests/'
```
Expected: no raw SQL and no `sqlite3_` call (rule 4; a sanctioned raw statement, if Task 9 needed one, has its README finding line); no plain numeric or `bool` DTO field (rule 3); every mutating action registered with effects (the coverage case in `test_declared_authorization.cpp` is the proof; the counts are for the size table); and no session installed by hand outside the tests — every cross-module job dispatches through an internal client.

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

In `examples/lims/README.md`: the "What to implement" model sentence names `LedgerModel` and `LedgerReportModel` in ledger, `PriceListModel`, `InvoiceModel`, `InvoiceEditModel` and `BillingRunModel` in billing; the Modules table has the `posting`, `ledger` and `billing` rows and `app`'s row reads "the server edge's `LimsAuthorizer`, the server's own clients (`InternalClient`), the event relay, the ledger port and the billing schedule"; and the Status paragraph adds: "Part B of the ERP design is built: the posting contract, the ledger (accounts, periods, entries, rates, reports) and billing (prices, billable lines from lab's events, invoices with approval, the edit lease, posting, the billing run) (decisions 28–40). Models [n], actions [n], screens [n]." Then:

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

- **Part C's second instances** meet the first instances recorded here: goods receipt (hypothesis 1, decision 37 — measured the same two ways; if hypothesis 1 is confirmed, the framework seam for atomic work across models is designed against both postings before goods receipt is called done), `lab.ResultCaptured` → consumption (hypothesis 2, decision 31 — a second consumer dispatched through its own sink and consumer action over the same relay), lot reservation (hypothesis 3, decision 36), requisition approval (hypothesis 5, decision 34; its escalation is a deadline the rule reads, which decision 34 records as not hypothesis 4's), purchase orders in a supplier's currency (hypothesis 8, decision 32), stock effects across companies (hypothesis 7, decision 39).
- **The internal client** (`lims::app::InternalClient`, Task 9) is how every Part C background job and cross-module reaction dispatches; nothing installs a session by hand.
- **The cross-module effects gap** (decision 38, cause (i)) is filed against `include/morph` if confirmed; every Part C action that writes another module's records inside itself meets it.
- **Part D** builds the billing and ledger screens against their mockups. A JPY price's form advertising two decimals (decision 32), an approvals inbox not told when a deadline passes (decision 34), and a lease's lapse on disconnect not told to a waiting clerk (decision 36) are Part D's to meet.

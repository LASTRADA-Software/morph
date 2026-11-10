# lims ERP Part C: Inventory and Procurement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give lims a stock of reagents and consumables and the procurement that fills it — Part C of `docs/superpowers/specs/2026-11-09-lims-erp-design.md` — and, on the way, build the second instance of hypotheses 1, 2, 3, 5, 7 and 8 (and a third module's use of hypothesis 6's declared authorization) with morph as it stands, each with a test that measures the outcome and a README resolved decision that records it; then decide, from both instances, which seams morph must grow, specify each and have it reviewed.

**Architecture:** One new feature module, `inventory`, joins `org`, `posting`, `lab`, `ledger`, `billing` and `app`. It links `org` and `posting` and nothing else. It holds items with a stock unit, locations at a site, lots with an expiry, stock on hand per lot, the movements that change it, consumption recipes per analysis, requisitions with their approval, suppliers, purchase orders in the supplier's currency, goods receipts, and lot reservations per sample. Lab writes a second event, `lab.ResultCaptured`, into its outbox in the capture's transaction; the app's relay now routes each event type to its consumer (billing's `RecordBillableLine`, inventory's `RecordConsumption`), each dispatched through an internal client. Goods receipt and consumption post to the ledger through the posting port Part B Task 14 built, which this part moves into the shared `posting` module so both posters reach one port. Every cross-module call runs as a dispatch through `lims::app::InternalClient` under a `system:` principal. Every state transition is a conditional update, every sum and product of money or stock is checked, and every action that can move a row into a filtered list declares `changes`.

**Tech Stack:** C++23, CMake ≥ 3.25 (Ninja), morph (header-only), LASTRADA Lightweight ORM over SQLite, glaze, Catch2, Qt 6 (Core; `QTimer` in the server).

**Dependencies on Part B.** Part B's README decisions end at 37 today; 38–41 are reserved for Part B Tasks 14 (decision 38, hypothesis 1's first instance: posting an invoice), 15 (39, cross-module effects), 16 (40, hypothesis 7's first instance) and 17 (41, hypothesis 6 resolved). Part C's decisions start at 42 and its migrations at `20261120000001`.

| Part C task | Waits for | Can start |
|---|---|---|
| 1. Inventory module, items, lots, stock | nothing | **now** |
| 2. Requisitions and their approval | Task 1 | as soon as Task 1 lands — parallel with Part B |
| 3. Suppliers and purchase orders in the supplier's currency | Task 2 | after Task 2 — parallel with Part B |
| 4. Goods receipt posted to the ledger | Task 3 **and Part B Task 14** (the posting port, `recordPostingIn`, `GetRateOn`, decision 38) | after Part B Task 14 |
| 5. `lab.ResultCaptured` becomes a consumption | Task 1; its Step 8 (posting the cost) also waits for Task 4 | Steps 1–7 as soon as Task 1 lands — parallel with Part B |
| 6. Lot reservation per sample | Task 5 | after Task 5's Step 7 — parallel with Part B |
| 7. Stock changes across companies | Task 1 **and Part B Task 16** (decision 40) | after Part B Task 16 |
| 8. The seams both instances call for | Tasks 2–7 and Part B Tasks 14–17 | last |
| 9. Part C's definition of done | everything above | last |

Authorization in every task follows whatever decision 41 (Part B Task 17) left: if the action guard landed (Part B Tasks 18–19), an action declares its `Authorization<Action>` and the guard runs it, and the handler calls neither `requireRoleSomewhere` nor `requireAuthorized`; otherwise the handler calls both, in Part A's order. The code below shows the second form, which is what the tree holds today; a lane that finds the guard in place drops the two calls and keeps everything else. Read `billing/src/models/invoice_approval.cpp` before the first handler to see which form is current.

## Global Constraints

Copied from the design, `examples/IMPLEMENTATION.md`, `AGENTS.md`, the Part B plan and the README decisions Part B recorded. Every task's requirements include this section.

- **Principle (design):** "A piece of work lands on one side by two questions, both of which must be answered yes before anything moves into `include/morph`: 1. **Is it mechanism?** … 2. **Has it been needed twice?** … Inside lims, two modules count as two places." "So a guarantee confirmed once is recorded as confirmed and built in lims; its second instance is built against a framework seam designed for both."
- **Dependency rule (design):** "Each module is its own static library with its own include directory, DTOs, entities, migrations and models. A module's target links `lims/org` and, where it posts or records postings, `lims/posting`; nothing else. … A compile check (`tests/compile_checks/`) includes billing from inventory and must fail to build: the control that proves the boundary is real."
- **Part C (design, verbatim):** "Items (reagents, consumables) with a stock unit; quantities are `morph::units::Quantity`, so a receipt in mL of a reagent stocked in L converts exactly. Lots with an expiry date, at a location in a site. Stock on hand is per item, lot and location. A **requisition** is approved by a principal whose value limit covers its total, and escalates past its deadline, as invoices do. An approved requisition becomes a purchase order to a supplier, in the supplier's currency. A **goods receipt** against an order creates lots and posts goods-received against goods-received-not-invoiced, atomically with the receipt: the second posting instance. Analysis definitions declare reagent consumption per analysis. `lab.ResultCaptured` consumes from the first-expiring lot and posts the consumption cost. **Reservation.** Assigning analyses to a sample reserves the lots they will consume. Two benches reserving the last lot race: one wins, the other is told which principal holds it; a reservation lapses when its holder's connection closes or after a timeout. Reservation is per sample, so lims decision 19 (no worksheets) stands; a reservation that must span several samples atomically is its stated condition for reopening, and this design does not need one."
- **Out of Part C (design):** "supplier invoices and three-way match, stock counts, reorder jobs. Each would repeat an instance already covered." Part D builds the inventory screens; this plan changes no screen.
- **Hypotheses Part C measures (design table, "Hypothesis" / "Refuted by"; Part B's outcome in brackets):**
  1. "**No atomic work across models.** … there is no way to commit billing's state change and ledger's entry together. Gap-free numbering is part of it" / "Both postings expressed with existing seams, and a failure injected between the halves leaves no partial state and no hole in either series". [Decision 38, Part B Task 14.] Part C builds instance 2: goods receipt (Task 4).
  2. "**Delivering events to a model consumer needs hand-rolled code per consumer.**" / "Both consumers receive each event exactly once in effect, across a restart, with only wiring code in `lims/app` and no per-consumer delivery loop or cursor". [Decision 32: refuted for billing; billing hand-wrote about 70 lines of consumer-side idempotence, and "Part C's second consumer is to be compared against this cost: if it copies the adapter, the lookup and the race catch, that is a framework seam to promote, a ledger morph provides over a consumer's own table"; a second event type in `lims_outbox` "needs a routing sink … inferred from reading `OutboxRelay`, not measured, and the second instance decides it".] Instance 2: result captured → consumption (Task 5).
  3. "**No lease that reaches a model.**" / "Both leases lapse on disconnect through an existing hook, without a timer of the model's own". [Decision 37: refuted for the invoice lease through the instance's destructor, at the cost of database work in a destructor on the server strand and a lapse no watcher is told of.] Instance 2: lot reservation (Task 6).
  5. "**No long-running approval state.**" / "Both lifecycles expressed as ordinary actions sharing no code but the declared authorization". [Decision 35: refuted for invoices.] Instance 2: requisition approval (Task 2).
  6. "**Authorization cannot see the payload.**" [Decisions 25, 36, 41.] Requisition approval's limits and pairs are a third module's declarations (Task 2).
  7. "**Effects leak at row and company granularity.**" / "A company-B change reaching no company-A connection, in a two-company remote test". [Decision 40, Part B Task 16.] Instance 2: stock (Task 7).
  8. "**Money at ERP magnitude and with runtime currency.**" / "… runtime currencies handled with `Quantity` as it stands". [Decision 33: the runtime-currency half confirmed for invoices.] The orders half of instance 2: purchase orders in a supplier's currency (Task 3); and stock units known only from the item's row repeat the same shape (Task 1).
  - Hypothesis 4 has no Part C instance: requisition escalation is a deadline the approval rule reads (decision 35), measured as such in Task 2.
- **How a hypothesis task is built (user, for the Part B plan, kept):** each instance is built in lims with existing morph seams, with a test that measures the outcome (confirmed / refuted / mis-framed), recorded as a README resolved decision. A measuring test asserts the behaviour it measured; if the assertion that pins the outcome fails, the outcome is the other one — change the assertion to what was measured and use the README text for that outcome, never weaken the test until it passes. Every measuring test names its mutation in a `// Mutation:` comment, and the step that adds it runs that mutation once and restores it. Results are labelled measured or inferred, never rounded up.
- **The second instance (design):** "Each confirmed hypothesis is designed (a spec under `docs/spec/`, one independent review) and implemented in `include/morph` in the same branch, before the part holding its second instance is called done." Task 8 designs and reviews each seam both instances call for and writes its implementation plan; Task 9 does not call Part C done until those plans are executed.
- **Lessons Part B paid for (user, for this plan):**
  - *State transitions are conditional updates.* A transition writes `UPDATE … WHERE id = ? AND state = <the state read>` through the query builder (`Connection().Query(table).Update().Set(…).Where(…)`) and counts the rows; zero rows is a `Conflict`. Two concurrent contenders get one success and one `Conflict`, never a raw SQL error: a busy or locked SQLite error during a contended write is turned into `Conflict` only when the row the write was conditional on is no longer as it was read (`invoice_edit_model.cpp`'s `isBusy` and `leaseMovedOn`), and surfaces as itself otherwise.
  - *Money and stock only through the checked operations.* `checkedAdd`, `checkedSub`, `checkedMul`, `checkedDiv`, `ExactSum` and `roundToCurrency`; `Rational`'s `+` and `*` saturate and log, and a saturated total is a wrong total that looks plausible. An overflow is `OutOfRange`.
  - *`changes<R>`, not `updates<R>`, when a row can newly enter a filtered list* (decision 35): `updates` refetches only the entries that already hold the key.
  - *Internal clients run as `system:` principals through a Bridge* (`lims::app::InternalClient`). Nothing outside `tests/` uses `morph::session::detail::ScopedContext`.
  - *SQLite's rollback journal:* a transaction that reads first holds a shared lock that a concurrent writer's commit then waits on. Read on a separate `Lightweight::DataMapper`, or open the transaction with its first write.
  - *GCC's `-Wshadow`:* no lambda parameter, local or member shares a name with an enclosing one (the price-list test's lambdas were renamed for it). *clang-format:* 119 columns.
- **Rule 1 (IMPLEMENTATION.md):** "All business logic, all invariants, and all persistence access live in plain, single-threaded model classes with typed actions." "Models must re-check their own preconditions and authorization (`Context::principal`)". "Action failures are thrown as the app's typed error set."
- **Rule 2 (IMPLEMENTATION.md):** "Screens never say what to refresh. Every action that changes data declares its effects with `BRIDGE_REGISTER_EFFECTS` beside its registration, over records registered with `BRIDGE_REGISTER_RECORD` … The rung's bundle loads with no refusal and no warning." Work through `docs/KEEPING-SCREENS-CURRENT.md`'s checklist for every new or changed action.
- **Rule 3 (IMPLEMENTATION.md):** "The only plain type permitted in an action/result field is `std::string`"; per-entity strong id types; closed sets as `enum class` with `glz::meta`; "Forbidden in any DTO field: `int`, `int64_t`, `double`, `float`, `bool`, raw enums." Numbers are `Quantity` (`Amount`, `Rate`, `Count`, and Task 1's `Measure`); dates are `morph::time::Timestamp`.
- **Rule 4 (IMPLEMENTATION.md):** all persistence through the Lightweight ORM; migrations are the only DDL; no `sqlite3_*`, no hand-written SQL outside Lightweight's facilities, except the escape tier with a README finding. Prefer the `DataMapper` API; the conditional update is the query builder (README finding "A conditional UPDATE is the query builder, not the DataMapper").
- **Rule 5 (IMPLEMENTATION.md):** every model 100 % unit tested; model tests run the backend matrix (`Local` / `LocalSingleThread` / `Socket`); every README invariant is a named test.
- **AGENTS.md, comments:** what the code does now and why; no history, no issue numbers, no commit hashes. Public API keeps complete `@param`/`@tparam`/`@return`.
- **AGENTS.md, filing bar:** defects in `include/morph`/`src` get an issue (verification status, real output, what would change the verdict; no `triage:` label by hand); `examples/` and CI configuration are fixed in place; "The *same* example defect in a second place — File it against `include/morph`".
- **Design review (user):** a framework spec under `docs/spec/` gets one independent review subagent on the default model (never Fable) before it is implemented.
- **Lane rules (user):** every build/configure is wrapped in `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock`, builds at `-j10` at most; tests run under the same lock with `timeout 300`, or as `ctest -j6 -L ladder-lims`. Implementation lanes run on Sonnet; two or three lanes at a time, one compile at a time; lanes run builds and tests in the foreground. Commit as the current git user, with the session's attribution trailer. Another lane codes on the same branch: rebase before each commit and keep each change to the files the task names.

## Build and test commands

As Part B's: the `linux-everything` preset (`build/linux-everything`: clang, Release, `MORPH_BUILD_LADDER=ON`, `MORPH_LADDER_RUNGS=all`).

- Configure: `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --preset linux-everything`
- Build lims: `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --build build/linux-everything --target ladder_lims_tests ladder_lims_screen_tests ladder_lims_render_smoke ladder_lims_server morph_examples_client -j10`
- Model suite: `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock timeout 300 build/linux-everything/examples/lims/ladder_lims_tests`
- Everything labelled for the rung: `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock ctest --test-dir build/linux-everything -j6 -L ladder-lims --output-on-failure`
- GCC shadow sweep over the files a task changed (local builds are clang): `flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --build build/gcc-debug --target ladder_lims_tests -j10`, reading the output for `-Wshadow` and `-Wrange-loop-construct`.

A Catch2 tag after the binary narrows a run, e.g. `… ladder_lims_tests "[inventory]"`.

## Conventions every task uses

Part B's conventions hold unchanged (namespaces and ids, rows to values, a model action's order, lists filtered by `OrgModel::visibleCompanies`, declared authorization tables, registration at the bottom of the model header, per-module `.clang-tidy`, tests in `tests/test_<topic>.cpp` starting with `DbFixture fixture;`). Part C adds:

- **Names in `lims` are shared by every module.** Every module keeps the `lims` namespace, and the app, the screens and the tests include several modules at once, so a name inventory declares must not exist in another module: `RequisitionLevel`, not `ApprovalLevel` (billing's); `setInventoryFaultHook`/`inventoryFaultPoint`, not billing's `setFaultHook`/`faultPoint`; `InventorySettingsView`, not `SettingsView`. Before adding a type or a free function to `namespace lims`, `grep -rn '\b<Name>\b' examples/lims/*/include`.
- **Tables** are prefixed `inventory_`. Migration ids are `202611200000NN`, in the order the tasks add them.
- **Records** are registered as `"Inventory_<Name>"`.
- **A conditional update** in inventory goes through one helper, `lims::inventory::changedRows` (Task 1), which executes a builder's SQL and answers the rows it changed, so every transition reads the same way: build, `Where` the id and every column the action read, count, `Conflict` on anything but one.
- **A read that precedes a write** in the same action runs on its own `Lightweight::DataMapper`, outside the write's transaction, or comes after the transaction's first write; a comment at the site says which.
- **Service principals.** Part C adds no new `system:` principal: `system:events` relays lab's events to both consumers, and goods receipt posts as `system:posting`, both in `lims::app::service_principals.hpp`.
- **Formatting.** The code in this plan is formatted by hand; run `clang-format -i` (the repository's `.clang-format`, 119 columns) on every file a step changes before building it.
- **Test support.** `tests/inventory_test_support.hpp` (Task 1) holds what every inventory suite starts from: `stockSetUp()`, `openingLot(...)`, `litres(n)`, `millilitres(n)`, and the clock helpers from `lims_test_support.hpp`.

## File structure after Part C

```
examples/lims/
  CMakeLists.txt                       MODULES … inventory …; USES … inventory:org,posting; five more probes
  org/include/lims/org/dto/            role_dto.hpp: StockKeeper, Purchaser, PurchaseApprover; limit_dto.hpp: role
  org/src/db/org_schema.cpp            + 20261120000003 (a value limit belongs to a role)
  posting/include/lims/posting/        ledger_port.hpp (moved from billing in Task 4)
  lab/include/lims/lab/events/         lab_events.hpp: + lab.ResultCaptured
  lab/src/events/lab_events.cpp        + writeResultCaptured
  inventory/include/.clang-tidy
  inventory/include/lims/inventory/core/   types.hpp, units.hpp, fault_hook.hpp, stock_math.hpp
  inventory/include/lims/inventory/dto/    stock_dto.hpp, recipe_dto.hpp, requisition_dto.hpp, order_dto.hpp,
                                           receipt_dto.hpp, consumption_dto.hpp, reservation_dto.hpp
  inventory/include/lims/inventory/db/     inventory_entity.hpp, inventory_store.hpp
  inventory/include/lims/inventory/models/ stock_model.hpp, procurement_model.hpp, reservation_model.hpp
  inventory/include/lims/inventory/authorization/ inventory_authorization.hpp
  inventory/src/{core,db,models,authorization}/…
  app/include/lims/app/                event_wiring.hpp: LabEventRouter; ledger_wiring.hpp: one port for both posters
  app/src/                             event_wiring.cpp, ledger_wiring.cpp
  tests/                               inventory_test_support.hpp, test_stock.cpp, test_requisitions.cpp,
                                       test_purchase_orders.cpp, test_goods_receipt.cpp, test_consumption.cpp,
                                       test_reservation.cpp, test_two_company_stock.cpp
  tests/compile_checks/                + inventory_includes_billing, inventory_includes_lab, inventory_includes_ledger,
                                         inventory_includes_posting, billing_includes_inventory
docs/spec/                             the seams Task 8 specifies, each reviewed
```

Module dependency after Part C: `org` → nothing; `posting` → `org`; `lab` → `org`; `ledger`, `billing`, `inventory` → `org`, `posting`; `app` → every module.

---

### Task 1: The inventory module — items, units, locations, lots and stock on hand

**Files:**
- Create: `examples/lims/inventory/include/.clang-tidy`, `examples/lims/inventory/include/lims/inventory/core/types.hpp`, `…/core/units.hpp`, `…/core/fault_hook.hpp`, `…/dto/stock_dto.hpp`, `…/db/inventory_entity.hpp`, `…/db/inventory_store.hpp`, `…/models/stock_model.hpp`, `…/authorization/inventory_authorization.hpp`, `examples/lims/inventory/src/core/units.cpp`, `…/src/core/fault_hook.cpp`, `…/src/db/inventory_schema.cpp`, `…/src/db/inventory_store.cpp`, `…/src/models/stock_model.cpp`, `…/src/authorization/inventory_authorization.cpp`, `examples/lims/tests/inventory_test_support.hpp`, `examples/lims/tests/test_stock.cpp`, the five probes in `examples/lims/tests/compile_checks/`
- Modify: `examples/lims/CMakeLists.txt`, `examples/lims/org/include/lims/org/dto/role_dto.hpp` (three roles), `examples/lims/app/include/lims/app/declared_requirements.hpp`, `examples/lims/tests/test_declared_authorization.cpp`, `examples/lims/README.md` (Modules table, decision 42)

**Interfaces:**
- Consumes: `lims::CompanyId`, `lims::SiteId`, `lims::Amount`, `lims::Count`, `lims::CurrencyCode`, `lims::ExactSum`, `lims::OrgModel::findSite`, `lims::OrgModel::findCompany`, `lims::OrgModel::visibleCompanies`, `lims::CompanyRule`, `morph::table::TableQuery`, `morph::table::apply`, `morph::units::UnitTraits`, `morph::units::UnitRelation`.
- Produces: ids `ItemId`, `LocationId`, `LotId`, `MovementId` (`lims.itemId`, `lims.locationId`, `lims.lotId`, `lims.movementId`); `enum class lims::MovementKind { Opening, Receipt, Consumption, Shortfall }`; `enum class lims::InventoryUnit { measure }`, `lims::Measure = Quantity<InventoryUnit::measure, 6>`; `enum class lims::StockUnit { litre, millilitre, kilogram, gram, piece }` with `UnitTraits` relations mL→L and g→kg; `std::string_view lims::stockUnitName(StockUnit)`; `Rational lims::convertStock(Rational const&, StockUnit from, StockUnit to)` (throws `ValidationError` across dimensions, `OutOfRange` on overflow); `LimsRole::StockKeeper`, `LimsRole::Purchaser`, `LimsRole::PurchaseApprover`; actions `SetInventorySettings`, `CreateItem`, `CreateLocation`, `RecordOpeningLot` (mutating), `GetInventorySettings`, `ListItems`, `ListLocations`, `StockOnHand`, `ListStockMovements` (reads) on model `lims::StockModel` (`"StockModel"`, unkeyed); views `InventorySettingsView`, `ItemView`, `LocationView`, `LotStockView`, `StockMovementView`; records `Inventory_Settings`, `Inventory_Item`, `Inventory_Location`, `Inventory_Lot` (keyed by lot), `Inventory_Movement`; `std::size_t lims::inventory::changedRows(Lightweight::DataMapper&, std::string const& sql)`; `void lims::setInventoryFaultHook(std::function<void(std::string_view, Lightweight::SqlConnection&)>)`, `void lims::inventoryFaultPoint(std::string_view, Lightweight::SqlConnection&)`; test helpers `lims::test::StockSetUp lims::test::stockSetUp()`, `LotStockView lims::test::openingLot(ItemId, LocationId, std::string lotNumber, morph::time::DateTime expires, Measure quantity, StockUnit unit)`, `Measure lims::test::litres(std::int64_t thousandths)`, `Measure lims::test::millilitres(std::int64_t)`.

An item has a stock unit, chosen once, and every quantity of it on hand is held in that unit. A quantity entered in another unit of the same dimension (a receipt of 250 mL of a reagent stocked in L) is converted exactly to the stock unit before it is stored. The unit of an item is known only from its row, so the action's quantity cannot be a `Quantity<StockUnit::litre>`: it is a `Measure` — an exact decimal in a carrier unit — with a `StockUnit` field beside it, the shape decision 28 gave money. The conversion is `convertStock`, which reads the relation from `UnitTraits<StockUnit>::relations` at run time; the test measures that `Quantity`'s own conversion gives the same answer when both units are known at compile time, which is the evidence for decision 42.

A lot is created two ways: by a goods receipt (Task 4) or, for stock a company owned before lims, by `RecordOpeningLot`, which posts nothing (the opening balance is the accountant's own manual entry, Part B Task 5). Stock on hand per lot is a column of the lot, changed only by movements in the same transaction as the movement row, so the movements of a lot always sum to its stock on hand; `StockOnHand` shows a company's lots with their live reserved quantity (zero until Task 6).

- [ ] **Step 1: Write the failing test**

Create `examples/lims/tests/inventory_test_support.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <morph/util/datetime.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <utility>

#include "clock.hpp"
#include "lims/inventory/core/units.hpp"
#include "lims/inventory/dto/stock_dto.hpp"
#include "lims/inventory/models/stock_model.hpp"
#include "lims_test_support.hpp"

/// @file
/// What every inventory suite starts from: settings for the seeded company, a reagent stocked in litres, a cold room
/// at the seeded site, and `kim`, a stock keeper there.

namespace lims::test {

/// @brief @p thousandths / 1000 of the carrier unit: `litres(250)` is 0.250.
/// @param thousandths The amount in thousandths.
/// @return The measure, at six places.
[[nodiscard]] inline Measure litres(std::int64_t thousandths) {
    return Measure{::morph::math::Rational{::morph::math::Numerator{thousandths}, ::morph::math::Denominator{1000},
                                           ::morph::math::DecimalPlaces{6}}};
}

/// @brief A whole number of the carrier unit: `millilitres(250)` is 250.
/// @param whole The amount.
/// @return The measure, at six places.
[[nodiscard]] inline Measure millilitres(std::int64_t whole) {
    return Measure{::morph::math::Rational{whole, ::morph::math::DecimalPlaces{6}}};
}

/// @brief What `stockSetUp` made.
struct StockSetUp {
    /// @brief The nitrate reagent, stocked in litres.
    ItemId reagent;
    /// @brief The cold room at the seeded site.
    LocationId coldRoom;
};

/// @brief Inventory settings for the seeded company (stock 3100, goods received not invoiced 1600, consumption 5100,
///        rounding 6900, 48 approval hours), the nitrate reagent `NO3-R` stocked in litres, and the cold room at the
///        seeded site. `sam` is supervisor and `kim` stock keeper at the seeded company.
/// @return The ids.
inline StockSetUp stockSetUp() {
    grant("sam", LimsRole::Supervisor);
    grant("kim", LimsRole::StockKeeper);
    {
        ScopedPrincipal const sam{"sam"};
        StockModel{}.execute(SetInventorySettings{
            .company = seededCompany(),
            .stockAccount = "3100",
            .grniAccount = "1600",
            .consumptionAccount = "5100",
            .roundingAccount = "6900",
            .approvalHours = Count{::morph::math::Rational{48, ::morph::math::DecimalPlaces{0}}}});
    }
    ScopedPrincipal const kim{"kim"};
    StockModel stock;
    auto const item = stock.execute(
        CreateItem{.company = seededCompany(), .code = "NO3-R", .name = "Nitrate reagent", .unit = StockUnit::litre});
    auto const room = stock.execute(CreateLocation{.site = seededSite(), .name = "Cold room"});
    return StockSetUp{.reagent = item.id, .coldRoom = room.id};
}

/// @brief An opening lot recorded by `kim`, at 12.50 per stock unit in the company's currency.
/// @param item The item.
/// @param location The location.
/// @param lotNumber The supplier's lot number.
/// @param expires When it expires.
/// @param quantity How much.
/// @param unit The unit @p quantity is in.
/// @return The lot as it stands.
inline LotStockView openingLot(ItemId item, LocationId location, std::string lotNumber, ::morph::time::DateTime expires,
                               Measure quantity, StockUnit unit) {
    ScopedPrincipal const kim{"kim"};
    return StockModel{}.execute(RecordOpeningLot{
        .item = item,
        .location = location,
        .lotNumber = std::move(lotNumber),
        .expiresAt = ::morph::time::Timestamp{expires},
        .quantity = std::move(quantity),
        .unit = unit,
        .unitCost = Amount{::morph::math::Rational{::morph::math::Numerator{1250}, ::morph::math::Denominator{100},
                                                   ::morph::math::DecimalPlaces{2}}}});
}

}  // namespace lims::test
```

Every literal `Rational` in the tests is built from a numerator, a denominator and places, never by arithmetic: `Rational`'s operators saturate.

Create `examples/lims/tests/test_stock.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Inventory: items with a stock unit, locations at a site, lots with an expiry, and stock on hand per lot, changed only
// by movements that sum to it. A quantity in another unit of the item's dimension converts exactly.

#include <Lightweight/SqlConnection.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdint>
#include <morph/util/quantity.hpp>
#include <morph/util/rational.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

#include "inventory_test_support.hpp"
#include "lims/inventory/core/fault_hook.hpp"
#include "lims/inventory/core/units.hpp"
#include "lims/inventory/models/stock_model.hpp"
#include "lims/org/core/errors.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::at;
using lims::test::litres;
using lims::test::millilitres;
using lims::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;
using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

namespace {

/// @brief The rational @p numerator / @p denominator at six places.
/// @param numerator The numerator.
/// @param denominator The denominator.
/// @return It.
[[nodiscard]] Rational six(std::int64_t numerator, std::int64_t denominator) {
    return Rational{Numerator{numerator}, Denominator{denominator}, DecimalPlaces{6}};
}

}  // namespace

// Mutation: make convertStock multiply by the relation's ratio in the wrong direction (250 mL becomes 250000 L).
TEST_CASE("stock: a quantity in millilitres of an item stocked in litres converts exactly", "[lims][inventory]") {
    CHECK(lims::convertStock(six(250, 1), lims::StockUnit::millilitre, lims::StockUnit::litre) == six(1, 4));
    CHECK(lims::convertStock(six(1, 4), lims::StockUnit::litre, lims::StockUnit::millilitre) == six(250, 1));
    CHECK(lims::convertStock(six(3, 1), lims::StockUnit::gram, lims::StockUnit::kilogram) == six(3, 1000));
    CHECK(lims::convertStock(six(7, 1), lims::StockUnit::piece, lims::StockUnit::piece) == six(7, 1));
    CHECK_THROWS_AS(lims::convertStock(six(1, 1), lims::StockUnit::litre, lims::StockUnit::kilogram),
                    lims::ValidationError);
}

// Decision 42's evidence: Quantity converts the same pair exactly when both units are compile-time arguments, and an
// action's field cannot be such a Quantity because the item's unit is known only from its row.
TEST_CASE("stock: Quantity's own conversion agrees when both units are known at compile time", "[lims][inventory]") {
    morph::units::Quantity<lims::StockUnit::millilitre, 3> const received{Rational{250, DecimalPlaces{3}}};
    morph::units::Quantity<lims::StockUnit::litre, 3> const stocked = received;
    REQUIRE(stocked.hasValue());
    CHECK(*stocked == Rational{Numerator{1}, Denominator{4}, DecimalPlaces{3}});
}

// Mutation: skip the unit conversion in RecordOpeningLot (the lot holds 250 L instead of 0.25 L).
TEST_CASE("stock: an opening lot entered in millilitres is held in the item's unit", "[lims][inventory]") {
    DbFixture fixture;
    auto const setUp = lims::test::stockSetUp();
    auto const lot = lims::test::openingLot(setUp.reagent, setUp.coldRoom, "L-0042", at(2027, 3, 31),
                                            millilitres(250), lims::StockUnit::millilitre);
    CHECK(lot.unit == lims::StockUnit::litre);
    CHECK(*lot.onHand == six(1, 4));
    ScopedPrincipal const kim{"kim"};
    auto const stock = lims::StockModel{}.execute(lims::StockOnHand{.company = lims::test::seededCompany(), .item = {}});
    REQUIRE(stock.lots.size() == 1);
    CHECK(stock.lots.front().lotNumber == "L-0042");
    CHECK(*stock.lots.front().reserved == six(0, 1));
}

// Mutation: write the movement row outside the lot's transaction, after the commit (a failure injected after the lot's
// write leaves stock on hand with no movement).
TEST_CASE("stock: a lot's movements always sum to its stock on hand", "[lims][inventory]") {
    DbFixture fixture;
    auto const setUp = lims::test::stockSetUp();
    lims::setInventoryFaultHook([](std::string_view point, Lightweight::SqlConnection& /*connection*/) {
        if (point == "lot.afterLotWrite") {
            throw std::runtime_error{"injected"};
        }
    });
    CHECK_THROWS(lims::test::openingLot(setUp.reagent, setUp.coldRoom, "L-0001", at(2027, 1, 31), litres(1000),
                                        lims::StockUnit::litre));
    lims::setInventoryFaultHook({});
    ScopedPrincipal const kim{"kim"};
    lims::StockModel stock;
    CHECK(stock.execute(lims::StockOnHand{.company = lims::test::seededCompany(), .item = {}}).lots.empty());
    auto const lot = lims::test::openingLot(setUp.reagent, setUp.coldRoom, "L-0001", at(2027, 1, 31), litres(1000),
                                            lims::StockUnit::litre);
    auto const moves = stock.execute(lims::ListStockMovements{.item = setUp.reagent, .query = {}});
    REQUIRE(moves.rows.size() == 1);
    CHECK(moves.rows.front().kind == lims::MovementKind::Opening);
    CHECK(*moves.rows.front().quantity == *lot.onHand);
}

TEST_CASE("stock: a lot number is unique per item, and a quantity must be positive", "[lims][inventory]") {
    DbFixture fixture;
    auto const setUp = lims::test::stockSetUp();
    static_cast<void>(lims::test::openingLot(setUp.reagent, setUp.coldRoom, "L-0042", at(2027, 3, 31), litres(500),
                                             lims::StockUnit::litre));
    CHECK_THROWS_AS(lims::test::openingLot(setUp.reagent, setUp.coldRoom, "L-0042", at(2027, 3, 31), litres(500),
                                           lims::StockUnit::litre),
                    lims::Conflict);
    CHECK_THROWS_AS(lims::test::openingLot(setUp.reagent, setUp.coldRoom, "L-0043", at(2027, 3, 31), litres(0),
                                           lims::StockUnit::litre),
                    lims::ValidationError);
    CHECK_THROWS_AS(lims::test::openingLot(setUp.reagent, setUp.coldRoom, "L-0044", at(2027, 3, 31), litres(500),
                                           lims::StockUnit::kilogram),
                    lims::ValidationError);
}

TEST_CASE("stock: a person without the stock keeper role cannot create an item, and sees no other company's lots",
          "[lims][inventory][authorization]") {
    DbFixture fixture;
    auto const setUp = lims::test::stockSetUp();
    static_cast<void>(lims::test::openingLot(setUp.reagent, setUp.coldRoom, "L-0042", at(2027, 3, 31), litres(500),
                                             lims::StockUnit::litre));
    lims::test::grant("ann", lims::LimsRole::Analyst);
    ScopedPrincipal const ann{"ann"};
    lims::StockModel stock;
    CHECK_THROWS_WITH(stock.execute(lims::CreateItem{.company = lims::test::seededCompany(),
                                                     .code = "X",
                                                     .name = "X",
                                                     .unit = lims::StockUnit::piece}),
                      Catch::Matchers::ContainsSubstring("does not hold the stock-keeper role"));
    ScopedPrincipal const stranger{"stranger"};
    CHECK(stock.execute(lims::StockOnHand{.company = lims::test::seededCompany(), .item = {}}).lots.empty());
}

TEST_CASE("stock: the inventory model across the backend matrix", "[lims][inventory][matrix]") {
    using morph::ladder::testkit::Mode;
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto const setUp = lims::test::stockSetUp();
    static_cast<void>(lims::test::openingLot(setUp.reagent, setUp.coldRoom, "L-0042", at(2027, 3, 31), litres(500),
                                             lims::StockUnit::litre));
    auto const stock = lims::test::dispatchAs<lims::StockModel>(
        "kim", lims::StockOnHand{.company = lims::test::seededCompany(), .item = setUp.reagent}, mode);
    CHECK(stock.lots.size() == 1);
}
```

`dispatchAs` and `kServiceSecret` live in `billing_test_support.hpp` today, and `dispatchAs` dispatches in `Mode::Local`; move both, unchanged in behaviour, to `lims_test_support.hpp`, giving `dispatchAs` a `Mode mode = Mode::Local` parameter after the action and building its rig as `test_domain_events.cpp`'s matrix case builds one (`BackendRig rig{mode, 1, limsAuthorizer(kServiceSecret)}`), so the matrix case passes its mode. `billing_test_support.hpp` includes `lims_test_support.hpp` already, and billing's suites keep calling `dispatchAs` with two arguments. Add `#include <catch2/generators/catch_generators.hpp>` for `GENERATE`.

- [ ] **Step 2: Run it and see it fail**

Run: Build lims. Expected: compile error, `lims/inventory/core/units.hpp` not found.

- [ ] **Step 3: The module, its boundary and its roles**

In `examples/lims/CMakeLists.txt`: `MODULES org posting lab ledger billing inventory app`, `USES lab:org ledger:org,posting billing:org,posting inventory:org,posting`, and under the existing probes:

```cmake
    _lims_boundary_probe(inventory_includes_billing inventory fail lims/billing/models/price_list_model.hpp)
    _lims_boundary_probe(inventory_includes_lab inventory fail lims/lab/models/sample_model.hpp)
    _lims_boundary_probe(inventory_includes_ledger inventory fail lims/ledger/models/ledger_model.hpp)
    _lims_boundary_probe(inventory_includes_posting inventory pass lims/posting/posting.hpp)
    _lims_boundary_probe(billing_includes_inventory billing fail lims/inventory/models/stock_model.hpp)
```

Each probe file is the shape of `billing_includes_ledger.cpp`: an SPDX line, two comment lines saying what it includes and why it must fail (or, for `inventory_includes_posting`, that it is the control), and the include inside `#ifdef LIMS_MODULE_BOUNDARY_PROBE`. `inventory_includes_billing` is the design's named control ("includes billing from inventory and must fail to build"). Copy `billing/include/.clang-tidy` to `inventory/include/.clang-tidy` with "billing" replaced by "inventory".

In `role_dto.hpp`, append three roles after `Service`, so every stored role keeps its integer:

```cpp
    StockKeeper,       ///< May keep a company's stock: items, locations, lots, recipes and receipts.
    Purchaser,         ///< May buy for a company: suppliers and purchase orders.
    PurchaseApprover,  ///< May approve somebody else's requisition up to their value limit for it.
```

with `roleName` answering `"stock-keeper"`, `"purchaser"` and `"purchase-approver"`, and the `glz::meta<lims::LimsRole>` enumeration extended in the same order. Search for every other `switch` over `LimsRole` (`grep -rn 'case LimsRole::Service' examples/lims`) and add the three cases where the switch lists every enumerator.

- [ ] **Step 4: Units, ids and the carrier**

Create `inventory/include/lims/inventory/core/units.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/util/quantity.hpp>
#include <morph/util/rational.hpp>
#include <string_view>
#include <utility>

/// @file
/// The units stock is kept in, and the carrier an action's quantity travels in. An item's unit is a fact of its row,
/// so an action cannot name it as a template argument: the quantity is a `Measure` and its unit a `StockUnit` field
/// beside it, converted with `convertStock`, which reads the relations `Quantity` itself converts with.

namespace lims {

/// @brief A unit stock is kept or counted in.
enum class StockUnit : std::uint8_t {
    litre,       ///< Litre.
    millilitre,  ///< Millilitre.
    kilogram,    ///< Kilogram.
    gram,        ///< Gram.
    piece,       ///< One item: a box, a column, a vial.
};

/// @brief The carrier unit of a quantity whose unit is a `StockUnit` field beside it.
enum class InventoryUnit : std::uint8_t {
    measure,  ///< An exact decimal in the unit named beside it.
};

}  // namespace lims

/// @brief Unit metadata and the exact relations within each dimension.
template <>
struct morph::units::UnitTraits<lims::StockUnit> {
    /// @brief Display metadata for @p unit.
    /// @param unit The unit.
    /// @return Its id, display text and default decimals.
    static constexpr morph::units::UnitMeta meta(lims::StockUnit unit) noexcept {
        switch (unit) {
            case lims::StockUnit::litre:
                return {.id = "litre", .display = "L", .defaultDecimals = 3};
            case lims::StockUnit::millilitre:
                return {.id = "millilitre", .display = "mL", .defaultDecimals = 0};
            case lims::StockUnit::kilogram:
                return {.id = "kilogram", .display = "kg", .defaultDecimals = 3};
            case lims::StockUnit::gram:
                return {.id = "gram", .display = "g", .defaultDecimals = 0};
            case lims::StockUnit::piece:
                return {.id = "piece", .display = "pc", .defaultDecimals = 0};
            default:
                break;
        }
        std::unreachable();
    }

    /// @brief 1 mL is 1/1000 L; 1 g is 1/1000 kg. Pieces convert to nothing.
    static constexpr std::array<morph::units::UnitRelation<lims::StockUnit>, 2> relations{{
        {lims::StockUnit::millilitre, lims::StockUnit::litre,
         morph::math::Rational{morph::math::Numerator{1}, morph::math::Denominator{1000}, morph::math::DecimalPlaces{3}}},
        {lims::StockUnit::gram, lims::StockUnit::kilogram,
         morph::math::Rational{morph::math::Numerator{1}, morph::math::Denominator{1000}, morph::math::DecimalPlaces{3}}},
    }};
};

/// @brief The carrier has no display of its own: the unit beside it is shown.
template <>
struct morph::units::UnitTraits<lims::InventoryUnit> {
    /// @brief Display metadata.
    /// @param unit The unit.
    /// @return Its id and six default decimals.
    static constexpr morph::units::UnitMeta meta(lims::InventoryUnit unit) noexcept {
        static_cast<void>(unit);
        return {.id = "measure", .display = "", .defaultDecimals = 6};
    }
};

namespace lims {

/// @brief A quantity in the `StockUnit` named beside it, exact to six places.
using Measure = ::morph::units::Quantity<InventoryUnit::measure, 6>;

/// @brief The display text of @p unit.
/// @param unit The unit.
/// @return `L`, `mL`, `kg`, `g` or `pc`.
[[nodiscard]] constexpr std::string_view stockUnitName(StockUnit unit) noexcept {
    return ::morph::units::UnitTraits<StockUnit>::meta(unit).display;
}

/// @brief Converts @p value from @p from to @p to, exactly, through `UnitTraits<StockUnit>::relations`.
/// @param value The value in @p from.
/// @param from Its unit.
/// @param to The unit wanted.
/// @return The value in @p to.
/// @throws ValidationError when the two units are not the same unit and no relation joins them.
/// @throws OutOfRange when the converted value does not fit.
[[nodiscard]] ::morph::math::Rational convertStock(::morph::math::Rational const& value, StockUnit from, StockUnit to);

}  // namespace lims

/// @brief Reflects `StockUnit` as its enumerator name.
template <>
struct glz::meta<lims::StockUnit> {
    using enum lims::StockUnit;

    /// @brief The enumerator ⇄ name mapping glaze reads and writes.
    static constexpr auto value = glz::enumerate(litre, millilitre, kilogram, gram, piece);
};
```

`src/core/units.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "lims/inventory/core/units.hpp"

#include <string>

#include "lims/org/core/errors.hpp"

namespace lims {

::morph::math::Rational convertStock(::morph::math::Rational const& value, StockUnit from, StockUnit to) {
    if (from == to) {
        return value;
    }
    for (auto const& relation : ::morph::units::UnitTraits<StockUnit>::relations) {
        // A relation is used in both directions: 1 from == fromTo · to, so the reverse divides.
        if (relation.from == from && relation.to == to) {
            auto const product = ::morph::math::checkedMul(value, relation.fromTo);
            if (!product.has_value()) {
                throw OutOfRange{"a quantity is too large to convert exactly"};
            }
            return *product;
        }
        if (relation.from == to && relation.to == from) {
            auto const quotient = ::morph::math::checkedDiv(value, relation.fromTo);
            if (!quotient.has_value()) {
                throw OutOfRange{"a quantity is too large to convert exactly"};
            }
            return *quotient;
        }
    }
    throw ValidationError{"a quantity in " + std::string{stockUnitName(from)} + " cannot be held in " +
                          std::string{stockUnitName(to)}};
}

}  // namespace lims
```

`core/types.hpp`: `LIMS_DEFINE_STRONG_ID` for `ItemId`, `LocationId`, `LotId`, `MovementId` (and their `_WIRE` and `_SHAPE_TAG` with tags `lims.itemId`, `lims.locationId`, `lims.lotId`, `lims.movementId`), and `enum class MovementKind : std::uint8_t { Opening, Receipt, Consumption, Shortfall }` with its `glz::meta`. `Shortfall` is a consumption that found too little stock (Task 5); it names no lot.

`core/fault_hook.hpp`/`.cpp`: billing's `fault_hook.hpp` and `.cpp` copied, with the two functions renamed `setInventoryFaultHook` and `inventoryFaultPoint` (Conventions: a name in `lims` exists once).

- [ ] **Step 5: Tables, and the conditional-update helper**

`db/inventory_entity.hpp` (namespace `lims::db`, the shape of `billing_entity.hpp`, the field index in a trailing comment):

- `InventorySettingsRecord` (`inventory_settings`): `id`, `company_id`, `stock_account`, `grni_account`, `consumption_account`, `rounding_account` (`SqlAnsiString<16>` each), `approval_hours` (`int64`).
- `ItemRecord` (`inventory_items`): `id`, `company_id`, `code` (`SqlAnsiString<32>`), `name` (`SqlAnsiString<128>`), `unit` (`int`, `StockUnit`).
- `LocationRecord` (`inventory_locations`): `id`, `company_id`, `site_id`, `name` (`SqlAnsiString<64>`).
- `LotRecord` (`inventory_lots`): `id`, `item_id`, `location_id`, `company_id`, `site_id`, `lot_number` (`SqlAnsiString<32>`), `expires_at` (`int64` ms), `on_hand_num`, `on_hand_den`, `unit_cost_num`, `unit_cost_den` (cost per stock unit in the company's currency), `reservation_version` (`int64`, 0; Task 6 writes it first in every reservation, so contenders for one lot are ordered by a conditional update).
- `MovementRecord` (`inventory_movements`): `id`, `company_id`, `item_id`, `lot_id` (`std::optional<std::int64_t>`, empty for a shortfall), `kind` (`int`), `quantity_num`, `quantity_den` (in the item's unit; negative for consumption), `at` (`int64`), `by` (`SqlAnsiString<64>`), `source` (`SqlAnsiString<128>`: `opening`, `receipt:<id>`, the consuming event's id).

Migrations in `src/db/inventory_schema.cpp`, inside the same `NOLINTBEGIN`/`NOLINTEND` as `billing_schema.cpp`:

```cpp
LIGHTWEIGHT_SQL_MIGRATION(20261120000001, "Create inventory settings, items, locations and lots") {
    plan.CreateTableIfNotExists("inventory_settings")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("company_id", Bigint())
        .RequiredColumn("stock_account", Varchar(16))
        .RequiredColumn("grni_account", Varchar(16))
        .RequiredColumn("consumption_account", Varchar(16))
        .RequiredColumn("rounding_account", Varchar(16))
        .RequiredColumn("approval_hours", Bigint());
    plan.CreateUniqueIndex("idx_inventory_settings_company", "inventory_settings", {"company_id"});
    plan.CreateTableIfNotExists("inventory_items")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("company_id", Bigint())
        .RequiredColumn("code", Varchar(32))
        .RequiredColumn("name", Varchar(128))
        .RequiredColumn("unit", Integer());
    // An item code names one item per company: the index refuses the second of two creates that raced.
    plan.CreateUniqueIndex("idx_inventory_items_company_code", "inventory_items", {"company_id", "code"});
    plan.CreateTableIfNotExists("inventory_locations")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("company_id", Bigint())
        .RequiredColumn("site_id", Bigint())
        .RequiredColumn("name", Varchar(64));
    plan.CreateUniqueIndex("idx_inventory_locations_site_name", "inventory_locations", {"site_id", "name"});
    plan.CreateTableIfNotExists("inventory_lots")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("item_id", Bigint())
        .RequiredColumn("location_id", Bigint())
        .RequiredColumn("company_id", Bigint())
        .RequiredColumn("site_id", Bigint())
        .RequiredColumn("lot_number", Varchar(32))
        .RequiredColumn("expires_at", Bigint())
        .RequiredColumn("on_hand_num", Bigint())
        .RequiredColumn("on_hand_den", Bigint())
        .RequiredColumn("unit_cost_num", Bigint())
        .RequiredColumn("unit_cost_den", Bigint())
        .RequiredColumn("reservation_version", Bigint());
    // A supplier's lot number names one lot of an item: a second receipt of it is refused, not merged.
    plan.CreateUniqueIndex("idx_inventory_lots_item_number", "inventory_lots", {"item_id", "lot_number"});
}

LIGHTWEIGHT_SQL_MIGRATION(20261120000002, "Create inventory_movements") {
    plan.CreateTableIfNotExists("inventory_movements")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("company_id", Bigint())
        .RequiredColumn("item_id", Bigint())
        .Column("lot_id", Bigint())
        .RequiredColumn("kind", Integer())
        .RequiredColumn("quantity_num", Bigint())
        .RequiredColumn("quantity_den", Bigint())
        .RequiredColumn("at", Bigint())
        .RequiredColumn("by", Varchar(64))
        .RequiredColumn("source", Varchar(128));
    plan.CreateIndex("idx_inventory_movements_item", "inventory_movements", {"item_id"});
}
```

If `plan.CreateIndex` is not the non-unique index builder's name in the pinned Lightweight (`.cache/cpm/lightweight/<pin>/src/Lightweight/SqlQuery/Migrate.hpp`), use the name that file declares; do not drop the index.

`db/inventory_store.hpp` (inventory-internal; no model header includes it):

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "lims/inventory/db/inventory_entity.hpp"
#include "lims/inventory/dto/stock_dto.hpp"

/// @file
/// Inventory's ORM-level reads and writes shared by its models.

namespace lims::inventory {

/// @brief Executes @p sql, a statement a query builder made, and answers how many rows it changed.
///
/// Every transition in inventory is a conditional update: the caller `Where`s the id and every column it read, and a
/// count other than one is a `Conflict`, so two callers acting on one read cannot both succeed.
/// @param mapper The caller's mapper, inside its transaction.
/// @param sql The statement.
/// @return The rows it changed.
[[nodiscard]] std::size_t changedRows(Lightweight::DataMapper& mapper, std::string const& sql);

/// @brief Writes one movement and moves its lot's stock on hand by it, through the caller's transaction.
///
/// The lot's update is conditional on the stock on hand the caller read, so a concurrent movement on the same lot makes
/// this one a `Conflict` rather than a lost update.
/// @param mapper The caller's mapper, inside its transaction.
/// @param lot The lot as the caller read it.
/// @param kind The movement's kind.
/// @param quantity The change in the item's unit: positive in, negative out.
/// @param source What caused it.
/// @param principal Who.
/// @return The lot's new stock on hand.
/// @throws Conflict if the lot changed since it was read.
/// @throws ValidationError if the movement would take the lot below zero.
/// @throws OutOfRange if the new stock on hand does not fit.
::morph::math::Rational moveLot(Lightweight::DataMapper& mapper, db::LotRecord const& lot, MovementKind kind,
                                ::morph::math::Rational const& quantity, std::string const& source,
                                std::string const& principal);

/// @brief The view of a lot, with its live reserved quantity.
/// @param mapper The mapper to read through.
/// @param lot The lot's row.
/// @return The view.
/// @throws ValidationError if a column holds a value outside its enumeration.
[[nodiscard]] LotStockView lotView(Lightweight::DataMapper& mapper, db::LotRecord const& lot);

/// @brief The row of item @p item.
/// @param mapper The mapper to read through.
/// @param item The item.
/// @return It, or nothing.
[[nodiscard]] std::optional<db::ItemRecord> findItem(Lightweight::DataMapper& mapper, std::int64_t item);

}  // namespace lims::inventory
```

`moveLot` in `src/db/inventory_store.cpp`:

```cpp
::morph::math::Rational moveLot(Lightweight::DataMapper& mapper, db::LotRecord const& lot, MovementKind kind,
                                ::morph::math::Rational const& quantity, std::string const& source,
                                std::string const& principal) {
    ::morph::math::Rational const before{::morph::math::Numerator{lot.onHandNum.Value()},
                                         ::morph::math::Denominator{lot.onHandDen.Value()},
                                         ::morph::math::DecimalPlaces{6}};
    auto const after = ::morph::math::checkedAdd(before, quantity);
    if (!after.has_value()) {
        throw OutOfRange{"lot " + std::string{lot.lotNumber.Value().ToStringView()} + ": stock on hand does not fit"};
    }
    if (after->numerator < 0) {
        throw ValidationError{"lot " + std::string{lot.lotNumber.Value().ToStringView()} + " holds less than that"};
    }
    auto update = mapper.Connection().Query(db::LotRecord::TableName).Update();
    update.Set("on_hand_num", after->numerator).Set("on_hand_den", after->denominator);
    (void)update.Where("id", "=", static_cast<std::int64_t>(lot.id.Value()))
        .Where("on_hand_num", "=", lot.onHandNum.Value())
        .Where("on_hand_den", "=", lot.onHandDen.Value());
    if (changedRows(mapper, update.ToSql()) != 1U) {
        throw Conflict{"lot " + std::string{lot.lotNumber.Value().ToStringView()} + " changed meanwhile"};
    }
    inventoryFaultPoint("lot.afterLotWrite", mapper.Connection());
    db::MovementRecord movement;
    movement.companyId = lot.companyId.Value();
    movement.itemId = lot.itemId.Value();
    movement.lotId = static_cast<std::int64_t>(lot.id.Value());
    movement.kind = static_cast<int>(kind);
    movement.quantityNum = quantity.numerator;
    movement.quantityDen = quantity.denominator;
    movement.at = nowMillis();
    movement.by = Lightweight::SqlAnsiString<64>{principal};
    movement.source = Lightweight::SqlAnsiString<128>{source};
    mapper.Create(movement);
    return *after;
}
```

`changedRows` is the two lines `invoice_edit_model.cpp` has (`SqlStatement{mapper.Connection()}.ExecuteDirect(sql).NumRowsAffected()`). `lotView` reads the reserved quantity as zero until Task 6 adds the reservations table (Task 6 changes it to the live sum).

- [ ] **Step 6: The DTOs and the model**

`dto/stock_dto.hpp`, every struct with a `validate()` (as billing's DTOs), registered records at the end:

- `SetInventorySettings { CompanyId company; std::string stockAccount, grniAccount, consumptionAccount, roundingAccount; Count approvalHours; }` (each account 1–16 bytes; hours ≥ 1), `explicitSubmit = true` → `InventorySettingsView` (the same fields and `InventorySettingsId`-free: keyed by `company`); `GetInventorySettings { CompanyId company; }` → `InventorySettingsView`.
- `CreateItem { CompanyId company; std::string code; std::string name; StockUnit unit = StockUnit::piece; }` (code 1–32, name 1–128) → `ItemView { ItemId id; CompanyId company; std::string code; std::string name; StockUnit unit; }`; `ListItems { CompanyId company; }` → `ListItemsResult { std::vector<ItemView> items; }`.
- `CreateLocation { SiteId site; std::string name; }` → `LocationView { LocationId id; CompanyId company; SiteId site; std::string name; }`; `ListLocations { CompanyId company; }` → `ListLocationsResult { std::vector<LocationView> locations; }`.
- `RecordOpeningLot { ItemId item; LocationId location; std::string lotNumber; ::morph::time::Timestamp expiresAt; Measure quantity; StockUnit unit = StockUnit::piece; Amount unitCost; }` (lot number 1–32; quantity and cost engaged) → `LotStockView`.
- `LotStockView { LotId lot; ItemId item; std::string itemCode; std::string lotNumber; LocationId location; SiteId site; CompanyId company; ::morph::time::Timestamp expiresAt; Measure onHand; Measure reserved; StockUnit unit; Amount unitCost; CurrencyCode currency; }`; `StockOnHand { CompanyId company; ItemId item; }` (item optional, `optionalFields{"item"}`) → `StockOnHandResult { std::vector<LotStockView> lots; }`, lots by expiry then id.
- `StockMovementView { MovementId id; ItemId item; LotId lot; MovementKind kind; Measure quantity; StockUnit unit; ::morph::time::Timestamp at; std::string by; std::string source; }`; `ListStockMovements { ItemId item; ::morph::table::TableQuery query; }` → `::morph::table::Page<StockMovementView>`.
- `BRIDGE_REGISTER_RECORD(lims::InventorySettingsView, "Inventory_Settings", ::morph::record::key<&lims::InventorySettingsView::company>)`, `ItemView` `"Inventory_Item"`, `LocationView` `"Inventory_Location"`, `LotStockView` `"Inventory_Lot"` keyed by `lot`, `StockMovementView` `"Inventory_Movement"`.

`models/stock_model.hpp`: `class StockModel` (unkeyed, holds no connection), one `execute` per action with complete doc comments in the style of `invoice_model.hpp`, and registration:

```cpp
// NOLINTBEGIN(misc-const-correctness)
BRIDGE_REGISTER_MODEL(lims::StockModel, "StockModel")
BRIDGE_REGISTER_ACTION(lims::StockModel, lims::SetInventorySettings, "SetInventorySettings")
BRIDGE_REGISTER_EFFECTS(lims::SetInventorySettings, ::morph::effects::changes<lims::InventorySettingsView>)
BRIDGE_REGISTER_ACTION(lims::StockModel, lims::GetInventorySettings, "GetInventorySettings", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::StockModel, lims::CreateItem, "CreateItem")
BRIDGE_REGISTER_EFFECTS(lims::CreateItem, ::morph::effects::changes<lims::ItemView>)
BRIDGE_REGISTER_ACTION(lims::StockModel, lims::ListItems, "ListItems", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::StockModel, lims::CreateLocation, "CreateLocation")
BRIDGE_REGISTER_EFFECTS(lims::CreateLocation, ::morph::effects::changes<lims::LocationView>)
BRIDGE_REGISTER_ACTION(lims::StockModel, lims::ListLocations, "ListLocations", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::StockModel, lims::RecordOpeningLot, "RecordOpeningLot")
BRIDGE_REGISTER_EFFECTS(lims::RecordOpeningLot, ::morph::effects::changes<lims::LotStockView>,
                        ::morph::effects::changes<lims::StockMovementView>)
BRIDGE_REGISTER_ACTION(lims::StockModel, lims::StockOnHand, "StockOnHand", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::StockModel, lims::ListStockMovements, "ListStockMovements", ::morph::model::Loggable::No)
// NOLINTEND(misc-const-correctness)
```

`RecordOpeningLot` in `src/models/stock_model.cpp`:

```cpp
// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
LotStockView StockModel::execute(const RecordOpeningLot& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"RecordOpeningLot: an item, a location, a lot number, an expiry, a quantity and a cost "
                              "are required"};
    }
    requireRoleSomewhere<RecordOpeningLot>();
    // Reads on a mapper of their own: the transaction below opens with its first write.
    Lightweight::DataMapper reader;
    auto const item = inventory::findItem(reader, *action.item);
    if (!item.has_value()) {
        throw NotFound{"RecordOpeningLot: item " + std::to_string(*action.item) + " does not exist"};
    }
    auto const location = inventory::findLocation(reader, *action.location);
    if (!location.has_value() || location->companyId.Value() != item->companyId.Value()) {
        throw NotFound{"RecordOpeningLot: location " + std::to_string(*action.location) + " is not one of " +
                       std::string{item->code.Value().ToStringView()} + "'s company"};
    }
    requireAuthorized(action);
    auto const unit = inventory::stockUnitOf(*item);
    auto const quantity = convertStock(*action.quantity, action.unit, unit);
    if (quantity.numerator <= 0) {
        throw ValidationError{"RecordOpeningLot: the quantity must be positive"};
    }
    auto const company = OrgModel::findCompany(CompanyId{item->companyId.Value()});
    if (!company.has_value() || !fitsCurrency(*action.unitCost, company->baseCurrency)) {
        throw ValidationError{"RecordOpeningLot: the unit cost is not a whole number of the company's minor units"};
    }
    std::string const principal = ::morph::session::current()->principal;
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    db::LotRecord lot;
    lot.itemId = static_cast<std::int64_t>(item->id.Value());
    lot.locationId = static_cast<std::int64_t>(location->id.Value());
    lot.companyId = item->companyId.Value();
    lot.siteId = location->siteId.Value();
    lot.lotNumber = Lightweight::SqlAnsiString<32>{action.lotNumber};
    lot.expiresAt = millisOf(action.expiresAt);
    lot.onHandNum = 0;
    lot.onHandDen = 1;
    lot.unitCostNum = (*action.unitCost).numerator;
    lot.unitCostDen = (*action.unitCost).denominator;
    lot.reservationVersion = 0;
    try {
        mapper.Create(lot);
    } catch (Lightweight::SqlException const&) {
        // The unique index on (item, lot number) refused it; any other failure finds no such lot and surfaces.
        if (inventory::lotNumberTaken(*action.item, action.lotNumber)) {
            throw Conflict{"RecordOpeningLot: lot " + action.lotNumber + " of " +
                           std::string{item->code.Value().ToStringView()} + " exists already"};
        }
        throw;
    }
    static_cast<void>(inventory::moveLot(mapper, lot, MovementKind::Opening, quantity, "opening", principal));
    sqlTxn.Commit();
    return inventory::lotView(mapper, inventory::lotRecord(mapper, static_cast<std::int64_t>(lot.id.Value())));
}
```

Add `findLocation`, `stockUnitOf` (range-checked; a value outside `StockUnit` is a `ValidationError`), `lotNumberTaken` (its own mapper) and `lotRecord` (`NotFound` when absent) to `inventory_store.hpp`/`.cpp` beside `findItem`, each with complete doc comments. The other handlers follow Part B's shapes: `SetInventorySettings` is `SetBillingSettings`'s (an upsert by company); `CreateItem` and `CreateLocation` create a row and turn the unique index's refusal into a `Conflict` the way `RecordOpeningLot` does; `StockOnHand` lists lots of companies in `OrgModel::visibleCompanies(currentPrincipal())`, filtered by the payload's company and item; `ListStockMovements` reads the item's movements (`Where(item_id)`), drops any of a company the caller cannot see, sorts by id and pages with `::morph::table::apply<StockMovementView>(rows, key, action.query)`, as `SampleModel`'s `ListSamples` does, turning `apply`'s error into `ValidationError`. Stock movements are hypothesis 10's second instance in Part D; this task keeps the list's shape and measures nothing of its scale.

- [ ] **Step 7: Declared authorization**

`authorization/inventory_authorization.hpp` (the shape of `billing_authorization.hpp`):

```cpp
/// @brief A company's inventory settings are a supervisor's.
template <>
struct Authorization<SetInventorySettings> : CompanyRule<SetInventorySettings, LimsRole::Supervisor> {};

/// @brief An item is created by a stock keeper at its company.
template <>
struct Authorization<CreateItem> : CompanyRule<CreateItem, LimsRole::StockKeeper> {};

/// @brief A location is created by a stock keeper at its site.
template <>
struct Authorization<CreateLocation> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::StockKeeper;

    /// @brief The site the payload names.
    /// @param action The action.
    /// @return It.
    [[nodiscard]] static Scope scopeOf(CreateLocation const& action) { return Scope{action.site}; }
};

/// @brief An opening lot is recorded by a stock keeper at the location's site.
template <>
struct Authorization<RecordOpeningLot> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::StockKeeper;

    /// @brief The site of the location the payload names.
    /// @param action The action.
    /// @return It; an empty site when there is no such location.
    [[nodiscard]] static Scope scopeOf(RecordOpeningLot const& action);
};
```

`scopeOf` for `RecordOpeningLot` in `src/authorization/inventory_authorization.cpp` reads the location on its own mapper. Add the four to `lims::app::declaredRequirements()` (include `lims/inventory/authorization/inventory_authorization.hpp`; raise the array's size by four from whatever it is) and to the `Mutating` tuple and the model list of `test_declared_authorization.cpp`'s coverage case.

- [ ] **Step 8: Build and run**

Run: Build lims, then Model suite `"[inventory]"`, then Model suite `"[authorization]"`, then Everything labelled for the rung. Expected: every case passes, and the five new `lims.module_boundary.*` tests pass (four refused includes, one control). Run the three mutations named in `test_stock.cpp`, each once, and see its case fail; restore each, and touch the restored file so ninja rebuilds it. Then the GCC shadow sweep.

- [ ] **Step 9: Record decision 42 and the module**

In the README's Modules table add `| \`inventory/\` | items with a stock unit, locations at a site, lots with an expiry, stock on hand per lot and the movements that change it | \`org\`, \`posting\` |`, and append:

```markdown
### 42. Stock is held in the item's unit, and a quantity's unit travels beside it

An item has one stock unit, chosen when it is created, and every lot of it holds its stock on hand in that unit. A
quantity entered in another unit of the same dimension is converted exactly before it is stored (250 mL of a reagent
stocked in L is 0.25 L), by `lims::convertStock`, which reads the relation from `UnitTraits<StockUnit>::relations`; a
quantity in another dimension is refused. A lot's stock on hand changes only through a movement written in the same
transaction, and the lot's update is conditional on the stock on hand that was read, so a lot's movements always sum
to it and two movements on one lot cannot both apply to the same read. A lot number names one lot of an item. An
opening lot, for stock a company held before lims, posts nothing: its value is the accountant's opening entry.

**The runtime-unit shape of hypothesis 8 recurs for stock.** Measured in `tests/test_stock.cpp`: `Quantity` converts
250 mL to 0.25 L exactly when both units are template arguments, and an action cannot use that, because an item's
unit is a fact of its row. The quantity is therefore a `Measure` (an exact decimal in a carrier unit) with a
`StockUnit` field beside it, and the conversion is inventory's own walk over the same relations — 30 lines in
`units.cpp`. This is the shape decision 28 gave money, for the same reason: a `Quantity`'s unit is a compile-time
tag. Purchase orders in a supplier's currency (decision 44) are hypothesis 8's named second instance; this is a third
place the same shape is needed, recorded here and weighed with both when the seam is decided.
```

- [ ] **Step 10: Commit**

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/CMakeLists.txt examples/lims/inventory examples/lims/org/include/lims/org/dto/role_dto.hpp \
        examples/lims/app/include/lims/app/declared_requirements.hpp examples/lims/tests examples/lims/README.md
git commit -m "Add lims's inventory module with items, units, locations, lots and stock on hand

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---
### Task 2: Requisitions — four eyes, a value limit per role, and a deadline that escalates (hypothesis 5, second instance)

**Files:**
- Create: `examples/lims/inventory/include/lims/inventory/dto/requisition_dto.hpp`, `…/models/procurement_model.hpp`, `examples/lims/inventory/src/models/requisitions.cpp`, `examples/lims/tests/test_requisitions.cpp`
- Modify: `examples/lims/org/include/lims/org/dto/limit_dto.hpp` (`role`), `examples/lims/org/include/lims/org/db/org_entity.hpp` (`ValueLimitRecord::role`), `examples/lims/org/src/db/org_schema.cpp` (migration `20261120000003`), `examples/lims/org/include/lims/org/models/org_model.hpp` and `examples/lims/org/src/models/org_model.cpp` (`valueLimit` takes the role), `examples/lims/billing/src/authorization/billing_authorization.cpp` (`limitOf` passes `InvoiceApprover`), `examples/lims/tests/billing_test_support.hpp` (its `SetValueLimit`s name the role), `…/core/types.hpp` (requisition ids and enums), `…/db/inventory_entity.hpp`, `…/src/db/inventory_schema.cpp` (migration `20261120000004`), `…/authorization/inventory_authorization.hpp`/`.cpp`, `examples/lims/tests/inventory_test_support.hpp` (`requisitionSetUp`), `declared_requirements.hpp`, `test_declared_authorization.cpp`, `examples/lims/README.md` (decision 43; the finding on the raw `DROP INDEX`)

**Interfaces:**
- Consumes: `StockModel`, `ItemView`, `Measure`, `convertStock`, `inventory::changedRows`, `inventoryFaultPoint`, `authorizationFactsOf`, `authorizationVerdict`, `SeparatedFrom`, `OrgModel::findSite`, `OrgModel::findCompany`, `BackendRig::connect`, `ServerConnection`.
- Produces: `SetValueLimit::role` and `ValueLimitView::role` (`LimsRole`, `InvoiceApprover` by default); `static std::optional<Rational> OrgModel::valueLimit(std::string_view principal, CompanyId company, LimsRole role)`; ids `RequisitionId`, `RequisitionLineId` (`lims.requisitionId`, `lims.requisitionLineId`); `enum class lims::RequisitionState { Draft, Submitted, Approved, Ordered }`; `enum class lims::RequisitionLevel { Site, Company }`; actions `CreateRequisition { CompanyId company; SiteId site; std::string reason; }`, `AddRequisitionLine { RequisitionId requisition; ItemId item; Measure quantity; StockUnit unit; Amount estimate; }`, `SubmitRequisition { RequisitionId requisitionId; }`, `ApproveRequisition { RequisitionId requisitionId; }` (mutating, each → `RequisitionView`), `GetRequisition { RequisitionId requisitionId; }` → `RequisitionView`, `ListRequisitions { CompanyId company; }` and `ListPendingRequisitions {}` → `ListRequisitionsResult { std::vector<RequisitionView> requisitions; }` (reads), on model `lims::ProcurementModel` (`"ProcurementModel"`, unkeyed); `RequisitionLineView { RequisitionLineId id; ItemId item; std::string itemCode; Measure quantity; StockUnit unit; Amount estimate; }`; `RequisitionView { RequisitionId id; CompanyId company; SiteId site; std::string reason; RequisitionState state; std::vector<RequisitionLineView> lines; Amount total; CurrencyCode currency; std::string requestedBy; std::string submittedBy; std::string approvedBy; ::morph::time::Timestamp submittedAt; ::morph::time::Timestamp approvalDeadline; RequisitionLevel level; }`; record `Inventory_Requisition`; test helper `lims::test::RequisitionSetUp lims::test::requisitionSetUp()`.

A requisition is a request to buy: a site, a reason, and lines of item, quantity and an estimated line total in the company's currency. Whoever holds any role at the site may raise one; only its requester adds lines and submits it. Submission records the deadline — now plus the company's approval hours from the inventory settings — and the total, which the approver's limit is judged against. A purchase approver approves it at the requisition's site before the deadline and at its company from the deadline on, within their value limit for that role, and may not be the one who raised or submitted it. This is invoice approval's shape (decision 35), built again with nothing shared but the declared authorization: hypothesis 5's second instance.

The mockups' scenario gives one person two limits — invoice approver up to €10,000.00, requisition approver up to €5,000.00 — so a value limit now belongs to a role as well as a principal and a company. That is org's domain (decision 29) and is changed there; billing passes `InvoiceApprover`.

- [ ] **Step 1: Write the failing test**

Append to `examples/lims/tests/inventory_test_support.hpp`, before the closing namespace, and include `lims/inventory/models/procurement_model.hpp`, `lims/org/dto/limit_dto.hpp` and `lims/org/models/org_model.hpp`:

```cpp
/// @brief What `requisitionSetUp` made.
struct RequisitionSetUp {
    /// @brief The stock `stockSetUp` made.
    StockSetUp stock;
    /// @brief A draft requisition by `rita` at the seeded site for 2 L of the reagent, estimated at 180.00.
    RequisitionView draft;
};

/// @brief `stockSetUp`, purchase approvers and their limits for requisitions, and a draft by `rita`, an analyst at the
///        seeded site. Limits for the purchase approver role at the seeded company: `anna` 5000.00 and `tom` 10.00,
///        approvers at the seeded site; `julia` 50000.00, approver at the company. Set at 09:00 on 10 November 2026.
/// @return The ids and the draft.
inline RequisitionSetUp requisitionSetUp() {
    ::morph::ladder::ScopedClockOverride const clock{at(2026, 11, 10)};
    RequisitionSetUp setUp{.stock = stockSetUp(), .draft = {}};
    {
        ScopedPrincipal const sam{"sam"};
        OrgModel org;
        auto const limit = [&org](std::string principal, std::int64_t cents) {
            org.execute(SetValueLimit{
                .principal = std::move(principal),
                .company = seededCompany(),
                .limit = Amount{::morph::math::Rational{::morph::math::Numerator{cents},
                                                        ::morph::math::Denominator{100},
                                                        ::morph::math::DecimalPlaces{2}}},
                .role = LimsRole::PurchaseApprover});
        };
        limit("anna", 500000);
        limit("tom", 1000);
        limit("julia", 5000000);
    }
    grant("anna", LimsRole::PurchaseApprover, Scope{seededSite()});
    grant("tom", LimsRole::PurchaseApprover, Scope{seededSite()});
    grant("julia", LimsRole::PurchaseApprover);
    grant("rita", LimsRole::Analyst, Scope{seededSite()});
    ScopedPrincipal const rita{"rita"};
    ProcurementModel procurement;
    auto const draft = procurement.execute(
        CreateRequisition{.company = seededCompany(), .site = seededSite(), .reason = "Nitrate reagent for November"});
    setUp.draft = procurement.execute(AddRequisitionLine{
        .requisition = draft.id,
        .item = setUp.stock.reagent,
        .quantity = litres(2000),
        .unit = StockUnit::litre,
        .estimate = Amount{::morph::math::Rational{::morph::math::Numerator{18000}, ::morph::math::Denominator{100},
                                                   ::morph::math::DecimalPlaces{2}}}});
    return setUp;
}
```

Create `examples/lims/tests/test_requisitions.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Hypothesis 5, second instance: a requisition is raised, submitted and approved as ordinary actions on its row, four
// eyes and a value limit per role hold, its approval escalates from the site to the company at its deadline, and
// an open approvals inbox is not told when the deadline passes.

#include <Lightweight/SqlConnection.hpp>
#include <Lightweight/SqlStatement.hpp>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <glaze/glaze.hpp>
#include <morph/session/session_auth.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "clock.hpp"
#include "inventory_test_support.hpp"
#include "lims/inventory/core/fault_hook.hpp"
#include "lims/inventory/models/procurement_model.hpp"
#include "lims/org/core/errors.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::at;
using lims::test::ScopedPrincipal;
using morph::ladder::ScopedClockOverride;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::ServerConnection;

namespace {

constexpr std::string_view kSecret = "lims-requisition-test-secret-at-least-32";

/// @brief Submits @p requisition as rita at 09:00 on 10 November 2026.
/// @param requisition The requisition.
/// @return It, submitted.
lims::RequisitionView submitted(lims::RequisitionView const& requisition) {
    ScopedClockOverride const clock{at(2026, 11, 10)};
    ScopedPrincipal const rita{"rita"};
    return lims::ProcurementModel{}.execute(lims::SubmitRequisition{.requisitionId = requisition.id});
}

/// @brief Approves @p requisition as @p principal.
/// @param principal The approver.
/// @param requisition The requisition.
/// @return It, approved.
lims::RequisitionView approveAs(std::string const& principal, lims::RequisitionView const& requisition) {
    ScopedPrincipal const approver{principal};
    return lims::ProcurementModel{}.execute(lims::ApproveRequisition{.requisitionId = requisition.id});
}

/// @brief The requisitions @p principal could approve now.
/// @param principal The principal.
/// @return Their ids.
std::vector<std::int64_t> pendingFor(std::string const& principal) {
    ScopedPrincipal const who{principal};
    std::vector<std::int64_t> ids;
    for (auto const& requisition :
         lims::ProcurementModel{}.execute(lims::ListPendingRequisitions{}).requisitions) {
        ids.push_back(*requisition.id);
    }
    return ids;
}

/// @brief A session signed with the case's secret.
/// @param principal The principal.
/// @return The session.
morph::session::Context as(std::string const& principal) {
    morph::session::TokenIssuer const issuer{std::string{kSecret}, morph::session::hmacSha256};
    return lims::test::signedAs(issuer, principal);
}

}  // namespace

// Mutation: drop the CreateRequisition pair from Authorization<ApproveRequisition>::separations (rita, made a purchase
// approver, approves her own requisition).
TEST_CASE("requisitions: the requester cannot approve her own requisition", "[lims][inventory][requisition]") {
    DbFixture fixture;
    auto const setUp = lims::test::requisitionSetUp();
    auto const requisition = submitted(setUp.draft);
    CHECK(requisition.state == lims::RequisitionState::Submitted);
    lims::test::grant("rita", lims::LimsRole::PurchaseApprover);
    {
        ScopedPrincipal const sam{"sam"};
        lims::OrgModel{}.execute(lims::SetValueLimit{
            .principal = "rita",
            .company = lims::test::seededCompany(),
            .limit = lims::Amount{morph::math::Rational{100000, morph::math::DecimalPlaces{2}}},
            .role = lims::LimsRole::PurchaseApprover});
    }
    ScopedClockOverride const clock{at(2026, 11, 10, 10)};
    CHECK_THROWS_WITH(approveAs("rita", requisition),
                      Catch::Matchers::ContainsSubstring("requested this requisition and cannot also approve it"));
    CHECK(approveAs("anna", requisition).approvedBy == "anna");
}

// Mutation: make Authorization<ApproveRequisition>::limitOf read the invoice approver's limit (tom's purchase limit of
// 10.00 is never consulted; with no invoice limit he has none and is refused for the wrong reason, which the message
// check catches).
TEST_CASE("requisitions: the value limit is the approver's limit for this role", "[lims][inventory][requisition]") {
    DbFixture fixture;
    auto const setUp = lims::test::requisitionSetUp();
    {
        ScopedPrincipal const sam{"sam"};
        lims::OrgModel{}.execute(lims::SetValueLimit{
            .principal = "tom",
            .company = lims::test::seededCompany(),
            .limit = lims::Amount{morph::math::Rational{100000000, morph::math::DecimalPlaces{2}}},
            .role = lims::LimsRole::InvoiceApprover});
    }
    auto const requisition = submitted(setUp.draft);
    ScopedClockOverride const clock{at(2026, 11, 10, 10)};
    CHECK_THROWS_WITH(approveAs("tom", requisition), Catch::Matchers::ContainsSubstring("value limit"));
    CHECK(approveAs("anna", requisition).state == lims::RequisitionState::Approved);
}

// Hypothesis 4 is not this: escalation is the deadline the rule reads. Mutation: make scopeOf answer the site whatever
// the deadline (anna approves after it, and julia's pending list misses the requisition before it).
TEST_CASE("requisitions: approval moves from the site to the company at the deadline", "[lims][inventory][requisition]") {
    DbFixture fixture;
    auto const setUp = lims::test::requisitionSetUp();
    auto const requisition = submitted(setUp.draft);
    {
        ScopedClockOverride const before{at(2026, 11, 11)};
        CHECK(pendingFor("anna") == std::vector<std::int64_t>{*requisition.id});
        CHECK(pendingFor("julia") == std::vector<std::int64_t>{*requisition.id});
    }
    ScopedClockOverride const after{at(2026, 11, 12, 10)};
    CHECK(pendingFor("anna").empty());
    CHECK(pendingFor("julia") == std::vector<std::int64_t>{*requisition.id});
    CHECK_THROWS_AS(approveAs("anna", requisition), lims::Forbidden);
    auto const approved = approveAs("julia", requisition);
    CHECK(approved.level == lims::RequisitionLevel::Company);
    CHECK(approved.approvedBy == "julia");
}

// Mutation: drop the state condition from ApproveRequisition's update (the second approval overwrites the first).
TEST_CASE("requisitions: an approval that lost the race to another is a Conflict", "[lims][inventory][requisition]") {
    DbFixture fixture;
    auto const setUp = lims::test::requisitionSetUp();
    auto const requisition = submitted(setUp.draft);
    ScopedClockOverride const clock{at(2026, 11, 10, 10)};
    // Between anna's read and her write, julia's approval commits on the same row.
    lims::setInventoryFaultHook([id = *requisition.id](std::string_view point, Lightweight::SqlConnection& connection) {
        if (point == "requisition.beforeStateChange") {
            Lightweight::SqlStatement statement{connection};
            static_cast<void>(statement.ExecuteDirect(
                "UPDATE inventory_requisitions SET state = 2, approved_by = 'julia' WHERE id = " + std::to_string(id)));
        }
    });
    CHECK_THROWS_AS(approveAs("anna", requisition), lims::Conflict);
    lims::setInventoryFaultHook({});
    ScopedPrincipal const rita{"rita"};
    CHECK(lims::ProcurementModel{}.execute(lims::GetRequisition{.requisitionId = requisition.id}).approvedBy ==
          "julia");
}

// The lesson Part B paid for, measured: two approvers acting at once give one success and one Conflict, never a raw
// SQL error. The clock override is process-wide, so both threads read the same instant. Mutation: read the
// requisition inside the write's transaction (the loser sees "database is locked").
TEST_CASE("requisitions: two approvers at once give one approval and one Conflict", "[lims][inventory][requisition]") {
    DbFixture fixture;
    auto const setUp = lims::test::requisitionSetUp();
    for (int round = 0; round < 10; ++round) {
        auto draft = setUp.draft;
        if (round > 0) {
            ScopedPrincipal const rita{"rita"};
            lims::ProcurementModel procurement;
            auto const fresh = procurement.execute(lims::CreateRequisition{
                .company = lims::test::seededCompany(), .site = lims::test::seededSite(), .reason = "round"});
            draft = procurement.execute(lims::AddRequisitionLine{.requisition = fresh.id,
                                                                 .item = setUp.stock.reagent,
                                                                 .quantity = lims::test::litres(1000),
                                                                 .unit = lims::StockUnit::litre,
                                                                 .estimate = setUp.draft.total});
        }
        auto const requisition = submitted(draft);
        ScopedClockOverride const clock{at(2026, 11, 10, 10)};
        std::atomic<int> approved{0};
        std::atomic<int> conflicts{0};
        std::vector<std::string> other;
        std::mutex otherMutex;
        auto const attempt = [&](std::string const& principal) {
            // Each thread has its own principal (the session is thread-local) and its own connection.
            try {
                ScopedPrincipal const who{principal};
                static_cast<void>(
                    lims::ProcurementModel{}.execute(lims::ApproveRequisition{.requisitionId = requisition.id}));
                ++approved;
            } catch (lims::Conflict const&) {
                ++conflicts;
            } catch (std::exception const& error) {
                std::scoped_lock const lock{otherMutex};
                other.emplace_back(error.what());
            }
        };
        std::thread first{attempt, std::string{"anna"}};
        std::thread second{attempt, std::string{"julia"}};
        first.join();
        second.join();
        INFO("round " << round << ", other failures: " << other.size());
        CHECK(approved == 1);
        CHECK(conflicts == 1);
        CHECK(other.empty());
    }
}

// Hypothesis 5. Mutation: keep the submitter in the model instance instead of the row (the approver on the new rig is
// told nothing was submitted).
TEST_CASE("Hypothesis 5: a requisition submitted on one rig is approved on another a day later",
          "[lims][inventory][requisition][h5]") {
    for (Mode const mode : {Mode::Local, Mode::LocalSingleThread, Mode::Socket}) {
        DYNAMIC_SECTION("mode " << static_cast<int>(mode)) {
            DbFixture fixture;
            auto const setUp = lims::test::requisitionSetUp();
            {
                ScopedClockOverride const clock{at(2026, 11, 10)};
                CHECK(lims::test::dispatchAs<lims::ProcurementModel>(
                          "rita", lims::SubmitRequisition{.requisitionId = setUp.draft.id}, mode)
                          .state == lims::RequisitionState::Submitted);
            }
            ScopedClockOverride const nextDay{at(2026, 11, 11)};
            auto const approved = lims::test::dispatchAs<lims::ProcurementModel>(
                "anna", lims::ApproveRequisition{.requisitionId = setUp.draft.id}, mode);
            CHECK(approved.state == lims::RequisitionState::Approved);
            CHECK(approved.submittedBy == "rita");
        }
    }
}

// The second time this is measured (decision 35 is the first): nothing tells an open inbox that a deadline passed.
// Control mutation: submit a second requisition after the clock passes the deadline (julia's connection hears that
// change, which shows the check can see one).
TEST_CASE("requisitions: an approver's open inbox is not told when a deadline passes",
          "[lims][inventory][requisition][effects]") {
    DbFixture fixture;
    auto const setUp = lims::test::requisitionSetUp();
    static_cast<void>(submitted(setUp.draft));
    BackendRig rig{Mode::Socket, 0, lims::test::limsAuthorizer(kSecret)};
    auto const julia = rig.connect();
    REQUIRE(julia->watch({"Inventory_Requisition"}, as("julia")).kind == "ok");
    auto const heardBefore = julia->changes().size();
    ScopedClockOverride const past{at(2026, 11, 12, 10)};
    bool const told = ServerConnection::eventually([&] { return julia->changes().size() > heardBefore; },
                                                   std::chrono::milliseconds{1000});
    CHECK_FALSE(told);
    CHECK(pendingFor("anna").empty());
}
```

Add `#include <mutex>` and `#include <exception>` to the includes. `dispatchAs` with a mode is Task 1's.

- [ ] **Step 2: Run it and see it fail**

Run: Build lims. Expected: compile error, `lims/inventory/models/procurement_model.hpp` not found.

- [ ] **Step 3: A value limit belongs to a role**

`limit_dto.hpp`: `SetValueLimit` gains, last, `LimsRole role = LimsRole::InvoiceApprover;` (doc: "The role the limit is for: `InvoiceApprover` or `PurchaseApprover`.") and `validate()` also requires `role == LimsRole::InvoiceApprover || role == LimsRole::PurchaseApprover`; `ValueLimitView` gains `LimsRole role` after `company`. `ValueLimitRecord` gains `Light::Field<std::optional<int>, Light::SqlRealName{"role"}> role; // 7`. `org_schema.cpp`:

```cpp
LIGHTWEIGHT_SQL_MIGRATION(20261120000003, "A value limit belongs to a role") {
    plan.AlterTable("org_value_limits").AddNotRequiredColumn("role", Integer());
    // Every limit set before limits had a role was an invoice approver's: no other role had one.
    static_assert(static_cast<int>(lims::LimsRole::InvoiceApprover) == 5,
                  "the statement spells the invoice approver role as a literal");
    plan.Update("org_value_limits").Set("role", 5).WhereExpression("role IS NULL");
    // The index of one limit per principal and company becomes one per principal, company and role. Lightweight's
    // migration builder drops an index by column, not by name, so the drop is raw SQL, rule 4's escape tier.
    plan.RawSql("DROP INDEX idx_org_value_limits_principal_company");
    plan.CreateUniqueIndex("idx_org_value_limits_principal_company_role", "org_value_limits",
                           {"principal", "company_id", "role"});
}
```

`OrgModel::valueLimit(principal, company, role)` adds `.Where(FieldNameOf<&db::ValueLimitRecord::role>, "=", static_cast<int>(role))`; `SetValueLimit`'s upsert finds the existing row by principal, company and role; `toView` reads `role`, and a row whose role is empty or outside `LimsRole` is a `ValidationError` (a migrated row always has one). `ListValueLimits` is unchanged but for the column. In `billing_authorization.cpp`, `Authorization<ApproveInvoice>::limitOf` calls `OrgModel::valueLimit(principal, company, LimsRole::InvoiceApprover)`. In `billing_test_support.hpp` and every billing suite that sets a limit (`grep -rn 'SetValueLimit{' examples/lims/tests examples/lims/screens`), add `.role = lims::LimsRole::InvoiceApprover` — designated initializers must name it last, after `limit`, and the default keeps the value, so a call site that omits it still means the invoice approver; name it anyway, so a reader sees which limit is set.

- [ ] **Step 4: Rows, DTOs and the model**

`core/types.hpp`: `RequisitionId`, `RequisitionLineId` with wire and shape tags; `RequisitionState { Draft, Submitted, Approved, Ordered }` and `RequisitionLevel { Site, Company }` with `glz::meta`. `inventory_entity.hpp`:

- `RequisitionRecord` (`inventory_requisitions`): `id`, `company_id`, `site_id`, `reason` (`SqlAnsiString<128>`), `state` (`int`), `requested_by`, `submitted_by`, `approved_by` (`SqlAnsiString<64>`), `created_at`, `submitted_at`, `deadline_at` (`int64`, 0 until set), `total_num`, `total_den` (company currency), `currency` (`int`, the company's base currency when raised).
- `RequisitionLineRecord` (`inventory_requisition_lines`): `id`, `requisition_id`, `item_id`, `quantity_num`, `quantity_den` (stock unit), `estimate_num`, `estimate_den`.

```cpp
LIGHTWEIGHT_SQL_MIGRATION(20261120000004, "Create inventory_requisitions and their lines") {
    plan.CreateTableIfNotExists("inventory_requisitions")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("company_id", Bigint())
        .RequiredColumn("site_id", Bigint())
        .RequiredColumn("reason", Varchar(128))
        .RequiredColumn("state", Integer())
        .RequiredColumn("requested_by", Varchar(64))
        .RequiredColumn("submitted_by", Varchar(64))
        .RequiredColumn("approved_by", Varchar(64))
        .RequiredColumn("created_at", Bigint())
        .RequiredColumn("submitted_at", Bigint())
        .RequiredColumn("deadline_at", Bigint())
        .RequiredColumn("total_num", Bigint())
        .RequiredColumn("total_den", Bigint())
        .RequiredColumn("currency", Integer());
    plan.CreateTableIfNotExists("inventory_requisition_lines")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("requisition_id", Bigint())
        .RequiredColumn("item_id", Bigint())
        .RequiredColumn("quantity_num", Bigint())
        .RequiredColumn("quantity_den", Bigint())
        .RequiredColumn("estimate_num", Bigint())
        .RequiredColumn("estimate_den", Bigint());
}
```

`requisition_dto.hpp`: the structs from Interfaces, each with `validate()` (reason 1–128 bytes; a line's item, quantity and estimate engaged; ids engaged) and `explicitSubmit = true` on the mutating ones; `BRIDGE_REGISTER_RECORD(lims::RequisitionView, "Inventory_Requisition")`.

`procurement_model.hpp`: `class ProcurementModel` with one `execute` per action (complete doc comments: what each throws, as `InvoiceModel`'s), registered:

```cpp
// NOLINTBEGIN(misc-const-correctness)
BRIDGE_REGISTER_MODEL(lims::ProcurementModel, "ProcurementModel")
BRIDGE_REGISTER_ACTION(lims::ProcurementModel, lims::CreateRequisition, "CreateRequisition")
BRIDGE_REGISTER_EFFECTS(lims::CreateRequisition, ::morph::effects::changes<lims::RequisitionView>)
BRIDGE_REGISTER_ACTION(lims::ProcurementModel, lims::AddRequisitionLine, "AddRequisitionLine")
BRIDGE_REGISTER_EFFECTS(lims::AddRequisitionLine, ::morph::effects::updates<lims::RequisitionView>)
BRIDGE_REGISTER_ACTION(lims::ProcurementModel, lims::SubmitRequisition, "SubmitRequisition")
BRIDGE_REGISTER_EFFECTS(lims::SubmitRequisition, ::morph::effects::changes<lims::RequisitionView>)
BRIDGE_REGISTER_ACTION(lims::ProcurementModel, lims::ApproveRequisition, "ApproveRequisition")
BRIDGE_REGISTER_EFFECTS(lims::ApproveRequisition, ::morph::effects::changes<lims::RequisitionView>)
BRIDGE_REGISTER_ACTION(lims::ProcurementModel, lims::GetRequisition, "GetRequisition", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::ProcurementModel, lims::ListRequisitions, "ListRequisitions", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::ProcurementModel, lims::ListPendingRequisitions, "ListPendingRequisitions",
                       ::morph::model::Loggable::No)
// NOLINTEND(misc-const-correctness)
```

`SubmitRequisition` moves a row into the approvers' filtered list and `ApproveRequisition` moves it out of it and into the purchaser's list of approved requisitions (Task 3), so both declare `changes` (decision 35); `AddRequisitionLine` changes a row only where it is already listed, so `updates` suffices.

In `src/models/requisitions.cpp`, the transition helper and the two transitions:

```cpp
namespace {

constexpr std::int64_t kMillisPerHour = 3'600'000;

/// @brief The row of @p requisition, read on a mapper of its own so the caller's transaction opens with its write.
/// @param requisition The requisition.
/// @param action The action's name, for the refusal.
/// @return The row.
/// @throws NotFound if there is no such requisition, or it is of a company the caller cannot see.
[[nodiscard]] db::RequisitionRecord requisitionRecord(RequisitionId requisition, char const* action) {
    Lightweight::DataMapper reader;
    auto const rows = reader.Query<db::RequisitionRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::RequisitionRecord::id>, "=", *requisition)
                          .All();
    auto const visible = OrgModel::visibleCompanies(currentPrincipal());
    if (rows.empty() ||
        std::ranges::find(visible, CompanyId{rows.front().companyId.Value()}) == visible.end()) {
        throw NotFound{std::string{action} + ": requisition " + std::to_string(*requisition) + " does not exist"};
    }
    return rows.front();
}

/// @brief Moves requisition @p id from @p from by a conditional update, and counts the rows it changed.
///
/// A fault point sits before the write, so a test makes a concurrent action win it.
/// @tparam Set Callable adding the columns to change to the update's builder.
/// @tparam Guard Callable adding further columns the caller read and requires unchanged.
/// @param mapper The caller's mapper, inside its transaction.
/// @param id The requisition.
/// @param from The state the caller read.
/// @param set Adds the columns to change.
/// @param guard Adds the further conditions.
/// @return Whether exactly one row matched.
template <typename Set, typename Guard>
[[nodiscard]] bool moveRequisition(Lightweight::DataMapper& mapper, std::int64_t id, RequisitionState from,
                                   Set const& set, Guard const& guard) {
    inventoryFaultPoint("requisition.beforeStateChange", mapper.Connection());
    auto query = mapper.Connection().Query(db::RequisitionRecord::TableName).Update();
    set(query);
    (void)query.Where("id", "=", id).Where("state", "=", static_cast<int>(from));
    guard(query);
    return inventory::changedRows(mapper, query.ToSql()) == 1U;
}

}  // namespace

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
RequisitionView ProcurementModel::execute(const SubmitRequisition& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"SubmitRequisition: a requisition is required"};
    }
    requireRoleSomewhere<SubmitRequisition>();
    auto const row = requisitionRecord(action.requisitionId, "SubmitRequisition");
    requireAuthorized(action);
    std::string const principal = ::morph::session::current()->principal;
    if (row.state.Value() != static_cast<int>(RequisitionState::Draft)) {
        throw IllegalTransition{"SubmitRequisition: only a draft is submitted"};
    }
    if (std::string_view{row.requestedBy.Value().ToStringView()} != principal) {
        throw Forbidden{"SubmitRequisition: only its requester submits a requisition"};
    }
    if (row.totalNum.Value() <= 0) {
        throw ValidationError{"SubmitRequisition: a requisition with no line has nothing to approve"};
    }
    auto const settings = StockModel::settingsOf(CompanyId{row.companyId.Value()});
    if (!settings.has_value()) {
        throw NotFound{"SubmitRequisition: company " + std::to_string(row.companyId.Value()) +
                       " has no inventory settings"};
    }
    auto const& hoursValue = *settings->approvalHours;
    std::int64_t const hours = hoursValue.numerator / hoursValue.denominator;
    std::int64_t const now = nowMillis();
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    // Conditional on the total read: a line added between the read and the write changed it, and the approver's limit
    // must be judged against the requisition as it is.
    if (!moveRequisition(
            mapper, static_cast<std::int64_t>(row.id.Value()), RequisitionState::Draft,
            [&](auto& query) {
                query.Set("state", static_cast<int>(RequisitionState::Submitted))
                    .Set("submitted_by", principal)
                    .Set("submitted_at", now)
                    .Set("deadline_at", now + (hours * kMillisPerHour));
            },
            [&](auto& query) {
                (void)query.Where("total_num", "=", row.totalNum.Value()).Where("total_den", "=", row.totalDen.Value());
            })) {
        throw Conflict{"SubmitRequisition: requisition " + std::to_string(*action.requisitionId) +
                       " was submitted or changed meanwhile"};
    }
    sqlTxn.Commit();
    return inventory::requisitionView(mapper, static_cast<std::int64_t>(row.id.Value()));
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
RequisitionView ProcurementModel::execute(const ApproveRequisition& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"ApproveRequisition: a requisition is required"};
    }
    requireRoleSomewhere<ApproveRequisition>();
    auto const row = requisitionRecord(action.requisitionId, "ApproveRequisition");
    // Before the rule, so approving an approved requisition is an IllegalTransition for everyone and never a refusal
    // that names who raised it.
    if (row.state.Value() != static_cast<int>(RequisitionState::Submitted)) {
        throw IllegalTransition{"ApproveRequisition: only a submitted requisition is approved"};
    }
    requireAuthorized(action);
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    if (!moveRequisition(
            mapper, static_cast<std::int64_t>(row.id.Value()), RequisitionState::Submitted,
            [](auto& query) {
                query.Set("state", static_cast<int>(RequisitionState::Approved))
                    .Set("approved_by", ::morph::session::current()->principal);
            },
            [](auto&) {})) {
        throw Conflict{"ApproveRequisition: requisition " + std::to_string(*action.requisitionId) +
                       " was approved meanwhile"};
    }
    sqlTxn.Commit();
    return inventory::requisitionView(mapper, static_cast<std::int64_t>(row.id.Value()));
}
```

If the measured round of "two approvers at once" shows a raw `database is locked` error, wrap each transition's write in the `catch (Lightweight::SqlException const&)` that `InvoiceEditModel::execute(LeaseInvoice)` has: a busy or locked error becomes `Conflict` when the requisition's state is no longer the one read (re-read on a mapper of its own), and surfaces as itself otherwise. Do that in a helper beside `moveRequisition`, not per handler.

The rest of the file: `CreateRequisition` (the site must be the company's, through `OrgModel::findSite`; currency is the company's base currency; total zero; `requested_by` the principal), `AddRequisitionLine` (requester and draft as in `SubmitRequisition`; the quantity converted to the item's stock unit with `convertStock`, positive; the item of the requisition's company; the estimate engaged, positive and a whole number of the currency's minor units with `fitsCurrency`; the new total `checkedAdd`ed, `OutOfRange` when it does not fit; the transaction opens with the conditional update of the requisition's total — `Where` state Draft and the total read — and then inserts the line), `GetRequisition` (`requisitionRecord`, then the view), `ListRequisitions` (visible companies, by id), and `ListPendingRequisitions`, which is `ListPendingApprovals` from `invoice_approval.cpp` over requisitions: each submitted requisition of a visible company whose `authorizationVerdict(authorizationFactsOf(ApproveRequisition{…}, principal).facts)` is `Allowed`. Add `requisitionView` (lines with each item's code; `level` from `escalated`) and `bool inventory::requisitionEscalated(db::RequisitionRecord const&)` (true from `deadline_at` on, for a submitted requisition) to `inventory_store.hpp`/`.cpp`, and `static std::optional<InventorySettingsView> StockModel::settingsOf(CompanyId)` to the stock model.

- [ ] **Step 5: Declared authorization**

```cpp
/// @brief A requisition is raised by anyone holding a role at its site.
template <>
struct Authorization<CreateRequisition> {
    /// @brief Some role, whichever.
    static constexpr bool anyRole = true;

    /// @brief The site the payload names.
    /// @param action The action.
    /// @return It.
    [[nodiscard]] static Scope scopeOf(CreateRequisition const& action) { return Scope{action.site}; }
};

/// @brief A line is added by anyone holding a role at the requisition's site; the model adds that it is its requester.
template <>
struct Authorization<AddRequisitionLine> {
    /// @brief Some role, whichever.
    static constexpr bool anyRole = true;

    /// @brief The site of the requisition the payload names.
    /// @param action The action.
    /// @return It; an empty site when there is no such requisition.
    [[nodiscard]] static Scope scopeOf(AddRequisitionLine const& action);
};

/// @brief A requisition is submitted by anyone holding a role at its site; the model adds that it is its requester.
template <>
struct Authorization<SubmitRequisition> {
    /// @brief Some role, whichever.
    static constexpr bool anyRole = true;

    /// @brief The site of the requisition the payload names.
    /// @param action The action.
    /// @return It; an empty site when there is no such requisition.
    [[nodiscard]] static Scope scopeOf(SubmitRequisition const& action);
};

/// @brief A requisition is approved by a purchase approver who neither raised nor submitted it and whose limit for
///        that role covers its total: at its site before its deadline, at its company from the deadline on.
template <>
struct Authorization<ApproveRequisition> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::PurchaseApprover;

    /// @brief The pairs: whoever raised the requisition, and whoever submitted it, may not also approve it.
    static constexpr std::array<SeparatedFrom, 2> separations{
        SeparatedFrom{
            .action = ::morph::model::ActionTraits<CreateRequisition>::typeId(),
            .refusal = "requested this requisition and cannot also approve it",
        },
        SeparatedFrom{
            .action = ::morph::model::ActionTraits<SubmitRequisition>::typeId(),
            .refusal = "submitted this requisition and cannot also approve it",
        },
    };

    /// @brief The requisition's site before its deadline, its company from the deadline on.
    /// @param action The action.
    /// @return The scope; an empty company when there is no such requisition.
    [[nodiscard]] static Scope scopeOf(ApproveRequisition const& action);

    /// @brief The requisition's total in its company's currency, as submitted.
    /// @param action The action.
    /// @return The amount; zero when there is no such requisition.
    [[nodiscard]] static ::morph::math::Rational amountOf(ApproveRequisition const& action);

    /// @brief @p principal's limit for this role at the company of @p scope.
    /// @param principal The principal.
    /// @param scope The scope `scopeOf` gave.
    /// @return The limit, or nothing.
    [[nodiscard]] static std::optional<::morph::math::Rational> limitOf(std::string_view principal,
                                                                        Scope const& scope);

    /// @brief Who raised the requisition and who submitted it, in `separations`' order.
    /// @param action The action.
    /// @return The two; each empty when there is no such requisition or no submission.
    [[nodiscard]] static std::array<std::optional<std::string>, 2> firstPerformersOf(
        ApproveRequisition const& action);
};
```

Each out-of-line function reads the requisition on its own mapper, as billing's do for invoices; `limitOf` resolves a site scope to its company with `OrgModel::findSite` and calls `OrgModel::valueLimit(principal, company, LimsRole::PurchaseApprover)`. Add the four to `declaredRequirements()` (size raised by four) and the coverage case.

- [ ] **Step 6: Build and run**

Run: Build lims, Model suite `"[requisition]"`, `"[authorization]"`, `"[billing]"` (the billing suites set limits with a role now), Everything labelled for the rung, then the GCC shadow sweep. Expected (the h5 case inferred from decision 35's instance; the effects case inferred from effects being derived from executed actions only, which decision 35 measured once): all pass. If `CHECK_FALSE(told)` fails, the passing deadline was told: change it to `CHECK(told)`, read from the `changes` body what carried it, and use that in Step 7. If the two-approvers case reports other failures, read them: a raw SQLite error means Step 4's busy-to-Conflict helper is needed; add it and run again. Run each mutation named in the file once and restore it.

- [ ] **Step 7: File the deadline gap — its second place — and record decision 43**

Decision 35 recorded that an open inbox is not told when an invoice's deadline passes; this is the same gap in a second module, so `AGENTS.md` says file it against `include/morph`. Search first (`gh issue list --search "deadline effect" --state all`); if no issue covers it, file:

```bash
cd /home/yaraslau/repo/morph
gh issue create --label enhancement --title "A watcher is not told when a time-derived fact changes: nothing fans out when a deadline passes" --body "$(cat <<'BODY'
## What happens

A list a model filters by a deadline — an approver's pending invoices or requisitions, which move from the site's
approvers to the company's when the deadline passes — changes when the clock passes the deadline. No action runs at
that instant, so no effect is declared and nothing reaches a connection watching the list's record. The watcher sees
the change only when it asks again.

## Verification status

Measured on <commit>, twice, in two lims modules:

- `examples/lims/screens/tests/test_lims_approval_inbox.cpp`: a site approver's inbox mounted before an invoice's
  deadline still lists it after the deadline.
- `examples/lims/tests/test_requisitions.cpp`, "an approver's open inbox is not told when a deadline passes": a
  connection watching `Inventory_Requisition` hears nothing within 1 s of the clock passing a requisition's deadline,
  while a submission after the deadline is heard (the control).

Not verified: any non-lims consumer. The cause is inferred from reading `include/morph/core/remote.hpp`'s fan-out,
which derives changes from executed actions only.

## What would change the verdict

A public way for a model to say when a record it serves will next change by time alone, so that the server fans out
`changes<R>` at that instant; or a decision that time-derived facts are always re-asked for, with the reasoning.
BODY
)"
```

Replace `<commit>` with `git rev-parse --short HEAD`. Keep the issue number in the session's notes, not in the README (`AGENTS.md`). Then append to the README (and, beside the finding "A data migration that copies rows needs raw SQL", one line: "Dropping an index by its name is raw SQL too: the migration builder drops an index by column (`org/src/db/org_schema.cpp`, migration `20261120000003`)."):

```markdown
### 43. A requisition is approved as an invoice is, with its own limit, and nothing shared but the declared rule (hypothesis 5, second instance)

A requisition is raised at a site by anyone who holds a role there, and only its requester adds lines and submits
it. Each line is an item, a quantity converted to the item's unit, and an estimated line total in the company's
currency; the requisition's total is their exact sum, and an estimate or a total that does not fit is refused.
`SubmitRequisition` records the submitter, the deadline (now plus the company's approval hours) and requires the
total it read, so a line added meanwhile is a `Conflict` and not a stale amount for the approver's limit.
`ApproveRequisition` is a purchase approver's, at the requisition's site before the deadline and at its company from
it, within their limit for that role, and never the requester's or the submitter's. Each transition is a conditional
update on the state read; two approvers acting at once give one approval and one `Conflict` — measured over ten
rounds of two threads, with no other failure. A value limit belongs to a principal, a company and a role, because one
person approves invoices and requisitions up to different amounts; limits set before roles had them are the invoice
approver's.

**Hypothesis 5 is refuted for its second instance.** Measured in `tests/test_requisitions.cpp`: a requisition
submitted on one rig is approved on another a day later in `Local`, `LocalSingleThread` and `Socket` mode; four eyes
and the limit hold. The lifecycle needed no state outside the row, and shares no code with invoice approval but the
declared authorization: the two lifecycles each wrote their own transition helper, pending list and escalation
predicate (`requisitions.cpp`, [n] lines; `invoice_approval.cpp`, 220). Escalation is again a deadline the rule
reads, not scheduled work (decision 35); and again an approver's open inbox is not told when the deadline passes —
measured on a watching connection, with a submission after the deadline as the control that the connection hears a
change. That is the same gap in a second module, so it is filed against `include/morph`.

**Declared authorization, third module.** [If decision 41 kept it domain logic:] The rule is declared once beside the
action (`inventory_authorization.hpp`) and checked in the model by the same two calls lab and billing make; the
edge's table gains four entries by hand. [If the action guard landed:] The rule is declared once beside the action
and the framework's guard runs it; the handler makes no authorization call.
```

Fill `[n]` with `wc -l examples/lims/inventory/src/models/requisitions.cpp`, keep the one bracketed sentence that applies and drop the brackets. If Step 6 measured the deadline as told, replace the sentence about the open inbox with what carried it and do not file.

- [ ] **Step 8: Commit**

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/org examples/lims/billing/src/authorization/billing_authorization.cpp examples/lims/inventory \
        examples/lims/app/include/lims/app/declared_requirements.hpp examples/lims/tests examples/lims/README.md
git commit -m "Approve lims requisitions with four eyes, a value limit per role and deadline escalation

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 3: Suppliers and purchase orders in the supplier's currency (hypothesis 8, the orders half of instance 2)

**Files:**
- Create: `examples/lims/inventory/include/lims/inventory/dto/order_dto.hpp`, `…/core/stock_math.hpp`, `…/src/core/stock_math.cpp`, `examples/lims/inventory/src/models/purchase_orders.cpp`, `examples/lims/tests/test_purchase_orders.cpp`
- Modify: `…/core/types.hpp` (`SupplierId`, `PurchaseOrderId`, `OrderLineId`, `OrderState`), `…/dto/requisition_dto.hpp` (`RequisitionView::order`), `…/db/inventory_entity.hpp`, `…/src/db/inventory_schema.cpp` (migration `20261120000005`), `…/models/procurement_model.hpp`, `…/authorization/inventory_authorization.hpp`/`.cpp`, `examples/lims/tests/inventory_test_support.hpp` (`approvedRequisition`, `openOrder`), `declared_requirements.hpp`, `test_declared_authorization.cpp`, `examples/lims/README.md` (decision 44)

**Interfaces:**
- Consumes: `RequisitionView`, `moveRequisition`, `ExactSum`, `checkedMul`, `roundToCurrency`, `fitsCurrency`, `decimalsOf`, `morph::forms::schemaJson`.
- Produces: ids `SupplierId`, `PurchaseOrderId`, `OrderLineId` (`lims.supplierId`, `lims.purchaseOrderId`, `lims.orderLineId`); `enum class lims::OrderState { Open, PartlyReceived, Received }`; actions `CreateSupplier { CompanyId company; std::string name; CurrencyCode currency; }` → `SupplierView { SupplierId id; CompanyId company; std::string name; CurrencyCode currency; }`, `CreatePurchaseOrder { RequisitionId requisition; SupplierId supplier; std::vector<OrderLinePrice> prices; }` with `OrderLinePrice { RequisitionLineId line; Amount unitPrice; }` → `PurchaseOrderView` (mutating); `ListSuppliers { CompanyId company; }` → `ListSuppliersResult`, `GetPurchaseOrder { PurchaseOrderId orderId; }` → `PurchaseOrderView`, `ListPurchaseOrders { CompanyId company; }` → `ListPurchaseOrdersResult` (reads), on `ProcurementModel`; `OrderLineView { OrderLineId id; ItemId item; std::string itemCode; Measure ordered; Measure received; StockUnit unit; Amount unitPrice; Amount lineTotal; }`; `PurchaseOrderView { PurchaseOrderId id; CompanyId company; SiteId site; SupplierId supplier; std::string supplierName; RequisitionId requisition; CurrencyCode currency; OrderState state; std::vector<OrderLineView> lines; Amount total; std::string orderedBy; ::morph::time::Timestamp orderedAt; }`; records `Inventory_Supplier`, `Inventory_Order`; `Rational lims::orderLineTotal(Rational const& quantity, Rational const& unitPrice, CurrencyCode)` (half-up to the currency, checked; `OutOfRange` when it does not fit); test helpers `ApprovedRequisition lims::test::approvedRequisition()` (`{ RequisitionView requisition; StockSetUp stock; }`), `OpenOrder lims::test::openOrder(CurrencyCode supplierCurrency = CurrencyCode::EUR)` (`{ PurchaseOrderView order; StockSetUp stock; }`).

An approved requisition becomes one purchase order to one supplier, in the supplier's currency. The purchaser prices each requisition line per stock unit in that currency; a line total is quantity times unit price, rounded half-up once to the currency's decimals; the order total is the exact sum of the line totals. Ordering moves the requisition from approved to ordered by a conditional update in the order's transaction, so a requisition is ordered once. A yen order holds whole yen: a unit price or a line total with a fraction of a yen is refused by the model, not the type — the measurement of hypothesis 8's orders half.

- [ ] **Step 1: Write the failing test**

Append to `inventory_test_support.hpp`:

```cpp
/// @brief What `approvedRequisition` made.
struct ApprovedRequisition {
    /// @brief The requisition, approved.
    RequisitionView requisition;
    /// @brief The stock `stockSetUp` made.
    StockSetUp stock;
};

/// @brief What `openOrder` made.
struct OpenOrder {
    /// @brief The order, open.
    PurchaseOrderView order;
    /// @brief The stock `stockSetUp` made.
    StockSetUp stock;
};

/// @brief `requisitionSetUp`, submitted by rita and approved by anna at 10:00 on 10 November 2026; `paul` is a
///        purchaser at the seeded company.
/// @return The approved requisition and the stock.
inline ApprovedRequisition approvedRequisition() {
    auto const setUp = requisitionSetUp();
    ::morph::ladder::ScopedClockOverride const clock{at(2026, 11, 10, 10)};
    {
        ScopedPrincipal const rita{"rita"};
        static_cast<void>(ProcurementModel{}.execute(SubmitRequisition{.requisitionId = setUp.draft.id}));
    }
    grant("paul", LimsRole::Purchaser);
    ScopedPrincipal const anna{"anna"};
    return ApprovedRequisition{
        .requisition = ProcurementModel{}.execute(ApproveRequisition{.requisitionId = setUp.draft.id}),
        .stock = setUp.stock};
}

/// @brief An order by `paul` for the approved requisition's 2 L, from "Merck" in @p supplierCurrency, at 85.00 a litre
///        (8500 yen in JPY).
/// @param supplierCurrency The supplier's currency.
/// @return The open order and the stock.
inline OpenOrder openOrder(CurrencyCode supplierCurrency = CurrencyCode::EUR) {
    auto const approved = approvedRequisition();
    auto const& requisition = approved.requisition;
    ScopedPrincipal const paul{"paul"};
    ProcurementModel procurement;
    auto const supplier = procurement.execute(
        CreateSupplier{.company = seededCompany(), .name = "Merck", .currency = supplierCurrency});
    // 85.00 a litre is 8500/100; in yen, 8500/1.
    constexpr std::int64_t price = 8500;
    std::uint32_t const places = decimalsOf(supplierCurrency);
    std::int64_t const denominator = places == 0U ? 1 : 100;
    auto order = procurement.execute(CreatePurchaseOrder{
        .requisition = requisition.id,
        .supplier = supplier.id,
        .prices = {OrderLinePrice{
            .line = requisition.lines.front().id,
            .unitPrice = Amount{::morph::math::Rational{::morph::math::Numerator{price},
                                                        ::morph::math::Denominator{denominator},
                                                        ::morph::math::DecimalPlaces{places}}}}}});
    return OpenOrder{.order = std::move(order), .stock = approved.stock};
}
```


Create `examples/lims/tests/test_purchase_orders.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Hypothesis 8, the orders half of its second instance: a purchase order is in its supplier's currency, which only the
// supplier's row names; whole yen are kept by the model, and the served form still advertises the type's two places.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <morph/forms/forms.hpp>
#include <morph/util/rational.hpp>
#include <string>

#include "inventory_test_support.hpp"
#include "lims/inventory/core/stock_math.hpp"
#include "lims/inventory/models/procurement_model.hpp"
#include "lims/org/core/errors.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;
using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

// Mutation: sum line totals with operator+ in the order total (the over-range case answers a saturated total).
TEST_CASE("purchase orders: a line total is quantity times unit price, rounded half-up once", "[lims][inventory][order]") {
    Rational const twoAndAThird{Numerator{7}, Denominator{3}, DecimalPlaces{6}};
    Rational const price{Numerator{1001}, Denominator{100}, DecimalPlaces{2}};
    // 7/3 × 10.01 = 23.356666… → 23.36 EUR, → 23 JPY at a yen price of 10.
    CHECK(lims::orderLineTotal(twoAndAThird, price, lims::CurrencyCode::EUR) ==
          Rational{Numerator{2336}, Denominator{100}, DecimalPlaces{2}});
    CHECK(lims::orderLineTotal(twoAndAThird, Rational{10, DecimalPlaces{0}}, lims::CurrencyCode::JPY) ==
          Rational{23, DecimalPlaces{0}});
    Rational const huge{4'000'000'000'000'000'000, DecimalPlaces{0}};
    CHECK_THROWS_AS(lims::orderLineTotal(huge, Rational{3, DecimalPlaces{0}}, lims::CurrencyCode::EUR),
                    lims::OutOfRange);
}

// Mutation: drop the conditional update of the requisition's state from CreatePurchaseOrder (a second order of the
// same requisition succeeds).
TEST_CASE("purchase orders: an approved requisition is ordered once, in the supplier's currency",
          "[lims][inventory][order]") {
    DbFixture fixture;
    auto const order = lims::test::openOrder(lims::CurrencyCode::CHF).order;
    CHECK(order.currency == lims::CurrencyCode::CHF);
    CHECK(order.state == lims::OrderState::Open);
    CHECK(*order.total == Rational{Numerator{17000}, Denominator{100}, DecimalPlaces{2}});
    ScopedPrincipal const paul{"paul"};
    lims::ProcurementModel procurement;
    auto const requisition = procurement.execute(lims::GetRequisition{.requisitionId = order.requisition});
    CHECK(requisition.state == lims::RequisitionState::Ordered);
    CHECK(requisition.order == order.id);
    CHECK_THROWS_AS(procurement.execute(lims::CreatePurchaseOrder{.requisition = order.requisition,
                                                                  .supplier = order.supplier,
                                                                  .prices = {}}),
                    lims::IllegalTransition);
}

// Hypothesis 8, orders half. Mutation: remove the fitsCurrency check on a unit price (a fraction of a yen is stored).
TEST_CASE("Hypothesis 8: a yen order keeps whole yen by the model, not by the type", "[lims][inventory][order][h8]") {
    DbFixture fixture;
    auto const requisition = lims::test::approvedRequisition().requisition;
    ScopedPrincipal const paul{"paul"};
    lims::ProcurementModel procurement;
    auto const supplier = procurement.execute(
        lims::CreateSupplier{.company = lims::test::seededCompany(), .name = "Kanto", .currency = lims::CurrencyCode::JPY});
    CHECK_THROWS_WITH(
        procurement.execute(lims::CreatePurchaseOrder{
            .requisition = requisition.id,
            .supplier = supplier.id,
            .prices = {lims::OrderLinePrice{.line = requisition.lines.front().id,
                                            .unitPrice = lims::Amount{Rational{Numerator{85050}, Denominator{100},
                                                                               DecimalPlaces{2}}}}}}),
        Catch::Matchers::ContainsSubstring("whole number of JPY"));
    // The type still says two places: the served form of a yen order line advertises them.
    auto const schema = morph::forms::schemaJson<lims::CreatePurchaseOrder>();
    CHECK_THAT(schema, Catch::Matchers::ContainsSubstring(R"("x-decimalPlaces":2)"));
}
```

`schemaJson`'s exact name and header are `docs/spec/forms/forms.md`'s; `test_invoices.cpp` measured decision 33's half with it — read that case and call it exactly the same way, including how it reaches a nested field's decimals.

- [ ] **Step 2: Run it and see it fail**

Run: Build lims. Expected: compile error, `lims/inventory/core/stock_math.hpp` not found.

- [ ] **Step 3: Rows, DTOs, the arithmetic and the actions**

`core/types.hpp`: the three ids and `OrderState { Open, PartlyReceived, Received }` with `glz::meta`. `RequisitionView` gains `PurchaseOrderId order;` (empty until ordered); `RequisitionRecord` gains `Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"order_id"}> orderId;`. Entities:

- `SupplierRecord` (`inventory_suppliers`): `id`, `company_id`, `name` (`SqlAnsiString<128>`), `currency` (`int`).
- `PurchaseOrderRecord` (`inventory_orders`): `id`, `company_id`, `site_id`, `supplier_id`, `requisition_id`, `currency`, `state`, `total_num`, `total_den`, `ordered_by`, `ordered_at`.
- `OrderLineRecord` (`inventory_order_lines`): `id`, `order_id`, `item_id`, `ordered_num`, `ordered_den`, `received_num`, `received_den` (stock unit; 0 until received), `unit_price_num`, `unit_price_den`, `line_total_num`, `line_total_den`.

```cpp
LIGHTWEIGHT_SQL_MIGRATION(20261120000005, "Create inventory suppliers, orders and order lines") {
    plan.AlterTable("inventory_requisitions").AddNotRequiredColumn("order_id", Bigint());
    plan.CreateTableIfNotExists("inventory_suppliers")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("company_id", Bigint())
        .RequiredColumn("name", Varchar(128))
        .RequiredColumn("currency", Integer());
    plan.CreateUniqueIndex("idx_inventory_suppliers_company_name", "inventory_suppliers", {"company_id", "name"});
    plan.CreateTableIfNotExists("inventory_orders")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("company_id", Bigint())
        .RequiredColumn("site_id", Bigint())
        .RequiredColumn("supplier_id", Bigint())
        .RequiredColumn("requisition_id", Bigint())
        .RequiredColumn("currency", Integer())
        .RequiredColumn("state", Integer())
        .RequiredColumn("total_num", Bigint())
        .RequiredColumn("total_den", Bigint())
        .RequiredColumn("ordered_by", Varchar(64))
        .RequiredColumn("ordered_at", Bigint());
    // A requisition becomes one order: the index refuses the second of two orders that raced.
    plan.CreateUniqueIndex("idx_inventory_orders_requisition", "inventory_orders", {"requisition_id"});
    plan.CreateTableIfNotExists("inventory_order_lines")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("order_id", Bigint())
        .RequiredColumn("item_id", Bigint())
        .RequiredColumn("ordered_num", Bigint())
        .RequiredColumn("ordered_den", Bigint())
        .RequiredColumn("received_num", Bigint())
        .RequiredColumn("received_den", Bigint())
        .RequiredColumn("unit_price_num", Bigint())
        .RequiredColumn("unit_price_den", Bigint())
        .RequiredColumn("line_total_num", Bigint())
        .RequiredColumn("line_total_den", Bigint());
}
```

`core/stock_math.hpp`/`.cpp`:

```cpp
::morph::math::Rational orderLineTotal(::morph::math::Rational const& quantity, ::morph::math::Rational const& unitPrice,
                                       CurrencyCode currency) {
    auto const product = ::morph::math::checkedMul(quantity, unitPrice);
    if (!product.has_value()) {
        throw OutOfRange{"an order line's total is too large to hold exactly"};
    }
    return roundToCurrency(*product, currency);
}
```

`CreatePurchaseOrder` in `src/models/purchase_orders.cpp`: validate; `requireRoleSomewhere`; read (own mapper) the requisition and the supplier, `NotFound` for either missing or of a company the caller cannot see, `ValidationError` when the supplier is another company's; `requireAuthorized`; the requisition must be `Approved` (`IllegalTransition` otherwise — before any price check, so a second order of an ordered requisition is refused as such); every requisition line is priced exactly once (`ValidationError` naming a missing or extra line); each unit price is engaged, positive and `fitsCurrency(price, supplier.currency)` (`ValidationError` "a unit price of … is not a whole number of JPY", with the currency's name); each line total is `orderLineTotal`; the order total is an `ExactSum` (`OutOfRange` when `add` refuses). The transaction opens with the conditional update of the requisition (`moveRequisition` from `Approved` to `Ordered`, setting `order_id` after the insert — so: update state first, insert the order and its lines, then a second conditional update setting `order_id` where `id` and state `Ordered` match), and a `Conflict` when the first update matches no row. The unique index on `requisition_id` settles two orders that raced past the state read: the loser's insert fails, the transaction rolls back, and the `catch` turns it into `Conflict` when the requisition is now ordered (re-read on its own mapper). `CreateSupplier`: a purchaser at the company; a name taken is a `Conflict`. `ListSuppliers`, `ListPurchaseOrders` (visible companies, by id) and `GetPurchaseOrder` (`NotFound` outside the caller's companies) read.

Registration, appended to `procurement_model.hpp`'s block:

```cpp
BRIDGE_REGISTER_ACTION(lims::ProcurementModel, lims::CreateSupplier, "CreateSupplier")
BRIDGE_REGISTER_EFFECTS(lims::CreateSupplier, ::morph::effects::changes<lims::SupplierView>)
BRIDGE_REGISTER_ACTION(lims::ProcurementModel, lims::ListSuppliers, "ListSuppliers", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::ProcurementModel, lims::CreatePurchaseOrder, "CreatePurchaseOrder")
BRIDGE_REGISTER_EFFECTS(lims::CreatePurchaseOrder, ::morph::effects::changes<lims::PurchaseOrderView>,
                        ::morph::effects::changes<lims::RequisitionView>)
BRIDGE_REGISTER_ACTION(lims::ProcurementModel, lims::GetPurchaseOrder, "GetPurchaseOrder", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::ProcurementModel, lims::ListPurchaseOrders, "ListPurchaseOrders",
                       ::morph::model::Loggable::No)
```

`Authorization<CreateSupplier>` is `CompanyRule<CreateSupplier, LimsRole::Purchaser>`; `Authorization<CreatePurchaseOrder>` needs `Purchaser` at the requisition's company (`scopeOf` reads it). Both to `declaredRequirements()` (size raised by two) and the coverage case.

- [ ] **Step 4: Build and run**

Run: Build lims, Model suite `"[order]"`, `"[authorization]"`, then Everything labelled for the rung and the GCC shadow sweep. Expected (the h8 case inferred from decision 33's measurement of the same type): all pass. If the schema check fails because the served form follows the supplier's currency, hypothesis 8's orders half is refuted: change the check to `"x-decimalPlaces":0` for a yen supplier and say what served it in Step 5. Run the three mutations once each and restore them.

- [ ] **Step 5: Record decision 44 and commit**

```markdown
### 44. A purchase order is in its supplier's currency; the type does not know which (hypothesis 8, orders half of instance 2)

An approved requisition becomes one purchase order to one supplier, in the supplier's currency. The purchaser prices
every requisition line per stock unit; a line total is the quantity times the unit price, rounded half-up once to the
currency's decimals, and the order total is their exact sum — every product and sum checked, an order that does not
fit refused as `OutOfRange`. Ordering moves the requisition from approved to ordered by a conditional update in the
order's transaction, and a unique index on the requisition settles two orders that raced: a requisition is ordered
once.

**Hypothesis 8's runtime-currency half is confirmed for its second instance.** Measured in
`tests/test_purchase_orders.cpp`: a unit price of 850.50 for a yen supplier is refused by the model (`fitsCurrency`),
and the served form of `CreatePurchaseOrder` still advertises `x-decimalPlaces: 2`, because the declared precision is
the type's. The currency is known only from the supplier's row, as an invoice's is from its client's (decision 33),
and a stock quantity's unit from its item's (decision 42). Three places in two modules now carry an amount beside a
field naming its unit and check the pair by hand.
```

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/inventory examples/lims/app/include/lims/app/declared_requirements.hpp examples/lims/tests \
        examples/lims/README.md
git commit -m "Order approved lims requisitions from suppliers in the supplier's currency

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---
### Task 4: Goods receipt — lots and the posting that values them, under failure injection (hypothesis 1, second instance)

**Waits for Part B Task 14** (the posting port, `recordPostingIn`, `GetRateOn`, `RecordPosting`, the number series pattern, `lims::test::books()`, decision 38). Read decision 38 and `billing/src/models/invoice_posting.cpp` as Task 14 left them before Step 1: the server's posting mode, the port's shape and the fault point's name are theirs, and this task uses them unchanged.

**Files:**
- Create: `examples/lims/posting/include/lims/posting/ledger_port.hpp` (moved), `examples/lims/inventory/include/lims/inventory/dto/receipt_dto.hpp`, `examples/lims/inventory/src/models/goods_receipt.cpp`, `examples/lims/tests/test_goods_receipt.cpp`
- Delete: `examples/lims/billing/include/lims/billing/core/ledger_port.hpp`, `examples/lims/billing/src/core/ledger_port.cpp` (their content moves)
- Modify: every includer of the billing port header (`grep -rln 'billing/core/ledger_port.hpp' examples/lims`), `examples/lims/app/include/lims/app/ledger_wiring.hpp`/`.cpp` (one port for both posters), `…/core/stock_math.hpp`/`.cpp` (`receiptPostingFor`), `…/db/inventory_entity.hpp` (`ReceiptRecord`, `ReceiptNumberRecord`, `ReceiptLotRecord`), `…/src/db/inventory_schema.cpp` (migration `20261120000006`), `…/db/inventory_store.hpp`/`.cpp` (`createLot`), `…/models/procurement_model.hpp` (`ReceiveGoods`, `GetReceipt`, `ListReceipts`), `…/authorization/inventory_authorization.hpp`/`.cpp`, `examples/lims/tests/inventory_test_support.hpp` (`inventoryBooks`, `ReceiptRig`), `declared_requirements.hpp`, `test_declared_authorization.cpp`, `examples/lims/README.md` (Modules row for `posting`, decision 45)

**Interfaces:**
- Consumes: from Part B Task 14 — `lims::PostingMode`, `lims::LedgerPort { PostingMode mode; post; postIn; rateOn; }`, `lims::setLedgerPort`, `lims::ledgerPort`, `lims::RateQuery`, `lims::RateAnswer`, `lims::app::installLedgerPort(InternalClient&, PostingMode)`, `lims::test::books()`, `lims::test::PostingRig`; `PurchaseOrderView`, `orderLineTotal`, `inventory::moveLot`, `fiscalYearOf`, `roundToCurrency`, `ExactSum`, `inventoryFaultPoint`.
- Produces: `posting/include/lims/posting/ledger_port.hpp` holding `PostingMode`, `LedgerPort`, `setLedgerPort` and `ledgerPort` unchanged in name and shape (inline storage, since `posting` is header-only); `ReceiptId` (`lims.receiptId`); `ReceiptLineInput { OrderLineId orderLine; std::string lotNumber; ::morph::time::Timestamp expiresAt; Measure quantity; StockUnit unit; }`; `ReceiveGoods { PurchaseOrderId order; LocationId location; ::morph::time::Timestamp date; std::vector<ReceiptLineInput> lines; }` → `ReceiptView { ReceiptId id; std::string number; PurchaseOrderId order; CompanyId company; CurrencyCode currency; Amount value; Amount companyValue; std::vector<LotId> lots; JournalEntryId entry; ::morph::time::Timestamp date; std::string receivedBy; }` (a Task handler); `GetReceipt { ReceiptId receiptId; }`, `ListReceipts { CompanyId company; }` → `ListReceiptsResult`; record `Inventory_Receipt`; `struct lims::ReceiptToPost { CompanyId company; CurrencyCode companyCurrency; CurrencyCode currency; Rational rate; Timestamp date; std::string sourceKey; std::string sourceReference; std::vector<Rational> lineValues; std::string stockAccount; std::string grniAccount; std::string roundingAccount; }`; `PostingRequest lims::receiptPostingFor(ReceiptToPost const&)`; test helpers `void lims::test::inventoryBooks()`, `class lims::test::ReceiptRig`.

A goods receipt takes delivery against an open order: each line names an order line, a new lot (number and expiry), a location and a quantity in any unit of the item's dimension, converted to the stock unit; one order line may arrive as several lots. The receipt is valued at the order's prices in the supplier's currency — each line's value is its quantity times the line's unit price, rounded half-up once — and posted: a debit to the company's stock account per receipt line, valued in company currency at the ledger's rate on the receipt's date and rounded per line, against one credit to goods received not invoiced for the total, rounded once; where the per-line rounding leaves a company-currency difference, an explicit line to the rounding account carries it (the mockups' CHF receipt loses a cent this way). A lot's unit cost is its line's company-currency value divided by its quantity, exactly. The receipt's number, `GR-<fiscal year>-<six digits>`, is gap-free per company and fiscal year.

The atomic step is hypothesis 1's: the receipt (lots, movements, order lines, number) and the ledger's entry. It is built in the mode decision 38 put the server in, and measured both ways as Task 14 measured the invoice:

- **Dispatched**: the ledger records first, as its own `RecordPosting` dispatched by `system:posting`, under the source key `inventory.receipt:<order id>:<ordinal>` — the order's next receipt ordinal, read before the call, so a retry of the same delivery names the same key and the ledger answers the entry it recorded (a retry with other lines is the ledger's `Conflict`: "a source key is bound to one posting body"). Inventory then creates the lots, moves the order lines, assigns the number and commits. Between the two commits an entry exists for goods not on hand.
- **One shared transaction**: the port writes the entry through inventory's own mapper (`postIn`) inside inventory's transaction; one commit.

The port moves to the shared `posting` module because two posters now reach it: one port, installed once by the app, rather than two copies of the same struct in billing and inventory. It stays lims's — the posting contract is domain wiring — and its move is not a framework seam.

- [ ] **Step 1: Move the port**

Create `posting/include/lims/posting/ledger_port.hpp` with the content of `billing/include/lims/billing/core/ledger_port.hpp` (Task 14), the storage made inline because `posting` has no sources:

```cpp
/// @brief The port the app installs, empty until it does.
/// @return The process's one port.
[[nodiscard]] inline LedgerPort& ledgerPortStorage() {
    static LedgerPort port;
    return port;
}

/// @brief Installs @p port: called once by the app at start-up and by a test, before any poster runs.
/// @param port The port; an empty one uninstalls.
inline void setLedgerPort(LedgerPort port) { ledgerPortStorage() = std::move(port); }

/// @brief The installed port.
/// @return It; its functions are empty when none is installed.
[[nodiscard]] inline LedgerPort const& ledgerPort() { return ledgerPortStorage(); }
```

Keep Task 14's doc comments on `PostingMode` and `LedgerPort`, unchanged but for the word "billing" where the comment names the poster (say "a poster"). Delete the two billing files, point every includer at `lims/posting/ledger_port.hpp`, and remove `ledger_port.cpp` from nothing else (sources are globbed). `ledger_wiring.hpp`'s `installLedgerPort` keeps its signature; its doc says it installs the one port both posters use. If the `posting` target does not link Lightweight (the port's `postIn` names `Lightweight::DataMapper`), add the include path the way `morph_add_rung` gives it to `org` — read `cmake/morph_add_rung.cmake`'s handling of a shared module, and if it cannot, keep the port in `posting` but declare `postIn` over a forward-declared `Lightweight::DataMapper` (`namespace Lightweight { class DataMapper; }`), which a `std::function` parameter by reference allows.

Run: Build lims, Model suite `"[posting]"`. Expected: billing's posting cases pass unchanged. Commit:

```bash
cd /home/yaraslau/repo/morph
git add -A examples/lims/posting examples/lims/billing examples/lims/app examples/lims/src/server/main.cpp examples/lims/tests
git commit -m "Move lims's ledger port into the shared posting module for its second poster

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

- [ ] **Step 2: Write the failing test**

Append to `inventory_test_support.hpp` (include `billing_test_support.hpp` for `books()` and `PostingRig`, and `lims/ledger/models/ledger_model.hpp`):

```cpp
/// @brief `books()`, plus the stock (3100, asset), goods received not invoiced (1600, liability) and consumption
///        (5100, expense) accounts in the seeded company's chart, made by `acc`, an accountant there; and a CHF→EUR
///        rate of 0.95 from 1 November 2026.
inline void inventoryBooks() {
    books();
    grant("acc", LimsRole::Accountant);
    ScopedPrincipal const acc{"acc"};
    LedgerModel ledger;
    for (auto const& [code, name, type] :
         {std::tuple{"3100", "Stock", AccountType::Asset}, std::tuple{"1600", "Goods received not invoiced",
                                                                       AccountType::Liability},
          std::tuple{"5100", "Reagent consumption", AccountType::Expense}}) {
        static_cast<void>(ledger.execute(
            CreateAccount{.company = seededCompany(), .code = code, .name = name, .type = type}));
    }
    static_cast<void>(ledger.execute(SetExchangeRate{
        .company = seededCompany(),
        .from = CurrencyCode::CHF,
        .to = CurrencyCode::EUR,
        .validFrom = ::morph::time::Timestamp{at(2026, 11, 1, 0)},
        .rate = Rate{::morph::math::Rational{::morph::math::Numerator{95}, ::morph::math::Denominator{100},
                                             ::morph::math::DecimalPlaces{6}}}}));
}
```

If `books()` already creates any of the three codes, drop that one here. `ReceiptRig` is `PostingRig` (Task 14) with `system:posting` granted the service role and the port installed in a mode the constructor takes; if `PostingRig` already takes a mode, use it directly and write `using ReceiptRig = PostingRig;`.

Create `examples/lims/tests/test_goods_receipt.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Hypothesis 1, second instance: a goods receipt creates its lots, moves its order and takes the next number of its
// series, and the ledger records its value against goods received not invoiced. Measured in both posting modes under
// a failure injected between the halves.

#include <Lightweight/SqlConnection.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <glaze/glaze.hpp>
#include <morph/util/rational.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

#include "clock.hpp"
#include "inventory_test_support.hpp"
#include "lims/inventory/core/fault_hook.hpp"
#include "lims/inventory/core/stock_math.hpp"
#include "lims/inventory/models/procurement_model.hpp"
#include "lims/inventory/models/stock_model.hpp"
#include "lims/ledger/models/ledger_model.hpp"
#include "lims/posting/ledger_port.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::at;
using lims::test::litres;
using lims::test::millilitres;
using lims::test::ScopedPrincipal;
using morph::ladder::ScopedClockOverride;
using morph::ladder::testkit::DbFixture;
using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

namespace {

/// @brief Receives @p order's single line as two lots of 1 L each (the second entered as 1000 mL), as kim, on
///        9 November 2026, dispatched as a client sends it.
/// @param order The open order.
/// @param room The location.
/// @return The receipt.
lims::ReceiptView receiveTwoLots(lims::PurchaseOrderView const& order, lims::LocationId room) {
    ScopedClockOverride const clock{at(2026, 11, 9)};
    return lims::test::dispatchAs<lims::ProcurementModel>(
        "kim", lims::ReceiveGoods{
                   .order = order.id,
                   .location = room,
                   .date = morph::time::Timestamp{at(2026, 11, 9)},
                   .lines = {lims::ReceiptLineInput{.orderLine = order.lines.front().id,
                                                    .lotNumber = "MK-1101",
                                                    .expiresAt = morph::time::Timestamp{at(2027, 5, 31)},
                                                    .quantity = litres(1000),
                                                    .unit = lims::StockUnit::litre},
                             lims::ReceiptLineInput{.orderLine = order.lines.front().id,
                                                    .lotNumber = "MK-1102",
                                                    .expiresAt = morph::time::Timestamp{at(2027, 8, 31)},
                                                    .quantity = millilitres(1000),
                                                    .unit = lims::StockUnit::millilitre}}});
}

/// @brief How many lots of the reagent the seeded company holds.
/// @return The count.
std::size_t lotCount() {
    ScopedPrincipal const kim{"kim"};
    return lims::StockModel{}.execute(lims::StockOnHand{.company = lims::test::seededCompany(), .item = {}}).lots.size();
}

/// @brief How many journal entries the seeded company's books hold.
/// @return The count.
std::size_t entryCount() {
    ScopedPrincipal const acc{"acc"};
    return lims::LedgerModel{}.execute(lims::ListEntries{.company = lims::test::seededCompany()}).entries.size();
}

}  // namespace

// Mutation: credit goods received not invoiced with the sum of the rounded line values (the rounding line disappears,
// and the line-count check fails).
TEST_CASE("goods receipt: a CHF receipt posts each line rounded and carries the cent as a rounding line",
          "[lims][inventory][receipt]") {
    // 0.33, 0.33 and 0.34 CHF at 0.95 are 0.3135, 0.3135 and 0.323 EUR, rounded to 0.31, 0.31 and 0.32 (0.94); the
    // total of 1.00 CHF is 0.95 EUR. The cent between them is a debit to the rounding account.
    auto const request = lims::receiptPostingFor(lims::ReceiptToPost{
        .company = lims::test::seededCompany(),
        .companyCurrency = lims::CurrencyCode::EUR,
        .currency = lims::CurrencyCode::CHF,
        .rate = Rational{Numerator{95}, Denominator{100}, DecimalPlaces{6}},
        .date = morph::time::Timestamp{at(2026, 11, 9)},
        .sourceKey = "inventory.receipt:1:1",
        .sourceReference = "receipt",
        .lineValues = {Rational{Numerator{33}, Denominator{100}, DecimalPlaces{2}},
                       Rational{Numerator{33}, Denominator{100}, DecimalPlaces{2}},
                       Rational{Numerator{34}, Denominator{100}, DecimalPlaces{2}}},
        .stockAccount = "3100",
        .grniAccount = "1600",
        .roundingAccount = "6900"});
    REQUIRE(request.lines.size() == 5);  // three stock debits, one GRNI credit, one rounding line
    CHECK(request.lines.at(3).accountCode == "1600");
    CHECK(*request.lines.at(3).companyAmount == Rational{Numerator{95}, Denominator{100}, DecimalPlaces{2}});
    CHECK(request.lines.back().accountCode == "6900");
    CHECK(request.lines.back().side == lims::PostingSide::Debit);
    CHECK(*request.lines.back().amount == Rational{0, DecimalPlaces{2}});
    CHECK(*request.lines.back().companyAmount == Rational{Numerator{1}, Denominator{100}, DecimalPlaces{2}});
}

// Mutation: skip the unit conversion of a receipt line (the second lot holds 1000 L).
TEST_CASE("goods receipt: one order line arrives as two lots, one entered in millilitres", "[lims][inventory][receipt]") {
    DbFixture fixture;
    lims::test::inventoryBooks();
    auto const opened = lims::test::openOrder(lims::CurrencyCode::EUR);
    auto const& order = opened.order;
    lims::test::ReceiptRig const rig{lims::PostingMode::Dispatched};
    auto const receipt = receiveTwoLots(order, opened.stock.coldRoom);
    CHECK(receipt.number == "GR-2026-000001");
    CHECK(receipt.lots.size() == 2);
    CHECK(*receipt.value == Rational{Numerator{17000}, Denominator{100}, DecimalPlaces{2}});
    ScopedPrincipal const kim{"kim"};
    auto const stock = lims::StockModel{}.execute(lims::StockOnHand{.company = lims::test::seededCompany(), .item = {}});
    REQUIRE(stock.lots.size() == 2);
    CHECK(*stock.lots.at(1).onHand == Rational{Numerator{1}, Denominator{1}, DecimalPlaces{6}});
    CHECK(*stock.lots.at(1).unitCost == Rational{Numerator{8500}, Denominator{100}, DecimalPlaces{2}});
    auto const now = lims::ProcurementModel{}.execute(lims::GetPurchaseOrder{.orderId = order.id});
    CHECK(now.state == lims::OrderState::Received);
}

TEST_CASE("goods receipt: refused without a rate, for a duplicate lot, and beyond what was ordered",
          "[lims][inventory][receipt]") {
    DbFixture fixture;
    lims::test::books();  // no CHF rate
    auto const opened = lims::test::openOrder(lims::CurrencyCode::CHF);
    lims::test::ReceiptRig const rig{lims::PostingMode::Dispatched};
    CHECK_THROWS_WITH(receiveTwoLots(opened.order, opened.stock.coldRoom),
                      Catch::Matchers::ContainsSubstring("no CHF/EUR rate"));
    CHECK(lotCount() == 0);
}

// Mutation: drop the received-quantity condition from the order line's update (the second receipt of the same 2 L is
// accepted).
TEST_CASE("goods receipt: a duplicate lot and a receipt beyond what was ordered are refused",
          "[lims][inventory][receipt]") {
    DbFixture fixture;
    lims::test::inventoryBooks();
    auto const opened = lims::test::openOrder(lims::CurrencyCode::EUR);
    lims::test::ReceiptRig const rig{lims::PostingMode::Dispatched};
    auto const receiveOne = [&opened](std::string lotNumber, lims::Measure quantity) {
        ScopedClockOverride const clock{at(2026, 11, 9)};
        lims::ReceiptLineInput line{.orderLine = opened.order.lines.front().id,
                                    .lotNumber = std::move(lotNumber),
                                    .expiresAt = morph::time::Timestamp{at(2027, 5, 31)},
                                    .quantity = std::move(quantity),
                                    .unit = lims::StockUnit::litre};
        return lims::test::dispatchAs<lims::ProcurementModel>(
            "kim", lims::ReceiveGoods{.order = opened.order.id,
                                      .location = opened.stock.coldRoom,
                                      .date = morph::time::Timestamp{at(2026, 11, 9)},
                                      .lines = {std::move(line)}});
    };
    static_cast<void>(receiveOne("MK-1101", litres(1000)));
    CHECK_THROWS_WITH(receiveOne("MK-1101", litres(500)), Catch::Matchers::ContainsSubstring("MK-1101"));
    CHECK_THROWS_WITH(receiveOne("MK-1103", litres(1500)), Catch::Matchers::ContainsSubstring("more than was ordered"));
    CHECK(lotCount() == 1);
}
```

Then the hypothesis cases, which are Task 14's three `[h1]` cases over a receipt instead of an invoice — read `test_invoice_posting.cpp` and write each with the same structure and the same assertions:

- `Hypothesis 1: a dispatched receipt that fails after the ledger leaves an entry for goods not on hand, and its retry converges` (`[h1]`): `ReceiptRig{Dispatched}`; an inventory fault hook that throws at `receipt.afterLedger`; `receiveTwoLots` throws; `entryCount()` grew by one, `lotCount() == 0`; hook removed, `receiveTwoLots` again succeeds with `GR-2026-000001` and `entry` equal to the entry the failed attempt left; `entryCount()` unchanged by the retry. Mutation: compute the source key's ordinal after the ledger call (the retry records a second entry).
- `Hypothesis 1: a receipt in one shared transaction that fails leaves nothing and no hole` (`[h1]`): `ReceiptRig{SharedTransaction}`; the same fault point; nothing in the ledger, no lot, no number consumed — the next receipt is `GR-2026-000001`. Mutation: commit inventory's transaction before `postIn` (the failure leaves the receipt).
- `Hypothesis 1: what a receipt in one shared transaction gives up` (`[h1]`): over a Socket rig, an accountant's connection watches `Ledger_Entry`; in `Dispatched` it hears the receipt's entry, in `SharedTransaction` it does not (`eventually` with a 1 s budget, `CHECK_FALSE`); revoke `system:posting`'s service role and receive again: `Dispatched` is refused by the ledger's rule, `SharedTransaction` succeeds. Mutation: in `SharedTransaction`, dispatch a no-op `RecordPosting` repeat after the commit (the connection hears it — the control).
- `goods receipt: numbers are gap-free per company and fiscal year` (`[receipt]`): two receipts in November 2026 are `…000001` and `…000002`; with the seeded company's fiscal year starting in January, a receipt dated 2 January 2027 is `GR-2027-000001`. Mutation: take the number before the ledger call in Dispatched mode (a failed receipt consumes one, and the retry is `…000002`).
- `goods receipt: two receipts against the last of an order line give one receipt and one Conflict` (`[receipt]`): two threads, as Task 2's two-approvers case, each receiving the line's full 2 L; one succeeds, one is a `Conflict` (the order line's conditional update on `received` lost), nothing else; run 10 rounds over fresh orders.

- [ ] **Step 3: Run it and see it fail**

Run: Build lims. Expected: compile error, `lims/inventory/dto/receipt_dto.hpp` not found.

- [ ] **Step 4: Rows, the posting, and the action**

Entities: `ReceiptRecord` (`inventory_receipts`): `id`, `company_id`, `order_id`, `number` (`SqlAnsiString<32>`), `currency`, `value_num`, `value_den`, `company_value_num`, `company_value_den`, `entry_id` (`std::optional<std::int64_t>`), `received_at` (the receipt's date), `received_by`; `ReceiptLotRecord` (`inventory_receipt_lots`): `id`, `receipt_id`, `lot_id`, `order_line_id`; `ReceiptNumberRecord` (`inventory_receipt_numbers`): `id`, `company_id`, `fiscal_year`, `next` (`int64`).

```cpp
LIGHTWEIGHT_SQL_MIGRATION(20261120000006, "Create inventory receipts, their lots and number series") {
    plan.CreateTableIfNotExists("inventory_receipts")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("company_id", Bigint())
        .RequiredColumn("order_id", Bigint())
        .RequiredColumn("number", Varchar(32))
        .RequiredColumn("currency", Integer())
        .RequiredColumn("value_num", Bigint())
        .RequiredColumn("value_den", Bigint())
        .RequiredColumn("company_value_num", Bigint())
        .RequiredColumn("company_value_den", Bigint())
        .Column("entry_id", Bigint())
        .RequiredColumn("received_at", Bigint())
        .RequiredColumn("received_by", Varchar(64));
    // A number is issued once per company: the index refuses a second receipt with one number.
    plan.CreateUniqueIndex("idx_inventory_receipts_company_number", "inventory_receipts", {"company_id", "number"});
    plan.CreateTableIfNotExists("inventory_receipt_lots")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("receipt_id", Bigint())
        .RequiredColumn("lot_id", Bigint())
        .RequiredColumn("order_line_id", Bigint());
    plan.CreateTableIfNotExists("inventory_receipt_numbers")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("company_id", Bigint())
        .RequiredColumn("fiscal_year", Bigint())
        .RequiredColumn("next", Bigint());
    plan.CreateUniqueIndex("idx_inventory_receipt_numbers_year", "inventory_receipt_numbers",
                           {"company_id", "fiscal_year"});
}
```

`receiptPostingFor` in `stock_math.cpp`:

```cpp
PostingRequest receiptPostingFor(ReceiptToPost const& receipt) {
    PostingRequest request{.company = receipt.company,
                           .date = receipt.date,
                           .sourceKey = receipt.sourceKey,
                           .sourceReference = receipt.sourceReference,
                           .currency = receipt.currency,
                           .rate = Rate{receipt.rate},
                           .lines = {}};
    auto const inCompany = [&](::morph::math::Rational const& value) {
        auto const converted = ::morph::math::checkedMul(value, receipt.rate);
        if (!converted.has_value()) {
            throw OutOfRange{"a receipt line's value is too large to convert exactly"};
        }
        return roundToCurrency(*converted, receipt.companyCurrency);
    };
    ExactSum total;
    ExactSum companyLines;
    for (auto const& value : receipt.lineValues) {
        auto const company = inCompany(value);
        if (!total.add(value) || !companyLines.add(company)) {
            throw OutOfRange{"a receipt's total is too large to hold exactly"};
        }
        request.lines.push_back(PostingLine{.accountCode = receipt.stockAccount,
                                            .side = PostingSide::Debit,
                                            .amount = Amount{value},
                                            .companyAmount = Amount{company}});
    }
    auto const companyTotal = inCompany(total.total());
    request.lines.push_back(PostingLine{.accountCode = receipt.grniAccount,
                                        .side = PostingSide::Credit,
                                        .amount = Amount{total.total()},
                                        .companyAmount = Amount{companyTotal}});
    // The poster rounds line by line and the ledger never adjusts: what the per-line rounding left is a line of its
    // own, zero in the transaction currency, on whichever side balances the company currency.
    auto const difference = ::morph::math::checkedSub(companyTotal, companyLines.total());
    if (!difference.has_value()) {
        throw OutOfRange{"a receipt's rounding difference is too large to hold exactly"};
    }
    if (difference->numerator != 0) {
        bool const debit = difference->numerator > 0;
        auto const magnitude = debit ? *difference : ::morph::math::checkedSub(companyLines.total(), companyTotal).value();
        request.lines.push_back(PostingLine{
            .accountCode = receipt.roundingAccount,
            .side = debit ? PostingSide::Debit : PostingSide::Credit,
            .amount = Amount{::morph::math::Rational{0, ::morph::math::DecimalPlaces{decimalsOf(receipt.currency)}}},
            .companyAmount = Amount{magnitude}});
    }
    return request;
}
```

`.value()` on the second `checkedSub` cannot fail where the first did not (the same two operands reversed); keep it, with that reason in a comment above the line.

`ReceiveGoods` in `goods_receipt.cpp` is a Task handler, the shape of Task 14's `PostInvoice` (`core::async::Task<ReceiptView> execute(ReceiveGoods action);` in `procurement_model.hpp`, `#include <core/async/Task.hpp>`):

1. `requirePrincipal`; validate (an order, a location, a date, at least one line, each with an order line, a lot number of 1–32 bytes, an expiry and a quantity); `requireRoleSomewhere`; read the order, its lines, the location and the company on a mapper of their own; `requireAuthorized`; the order must be `Open` or `PartlyReceived` (`IllegalTransition`); the location must be at the order's site (`ValidationError`); each line's quantity is converted to the item's stock unit and must be positive; the received quantities per order line, added to what it has received (`checkedAdd`), must not exceed what was ordered (`ValidationError` "more than was ordered"); each line's value is `orderLineTotal(quantity, unitPrice, currency)`.
2. The rate: one when the order is in the company's currency; otherwise `co_await ledgerPort().rateOn(RateQuery{company, order currency, base, date})`, refusing with `ValidationError` "no CHF/EUR rate on 09-Nov-2026" (the date as the screens show it, `dd-Mon-yyyy`) when none is valid.
3. The ordinal: one plus the order's receipts so far, read on a mapper of its own; the source key `inventory.receipt:<order id>:<ordinal>`; the request is `receiptPostingFor` with the settings' accounts.
4. `PostingMode::SharedTransaction`: one mapper and transaction; the first write is the order lines' conditional updates (`received` as read, `Conflict` on zero rows); then `postIn`; `inventoryFaultPoint("receipt.afterLedger", …)`; then the lots (`createLot` + `moveLot(Receipt)`), the receipt row with its number (the series row's conditional update on `next`; a missing row inserted, the unique index settling two first receipts of a year), the receipt-lot rows, the order's state, commit.
5. `PostingMode::Dispatched`: `co_await ledgerPort().post(request)`; `inventoryFaultPoint("receipt.afterLedger", …)` on a fresh mapper's connection; then step 4's writes without `postIn`, in one transaction, with `entry_id` the answer's.

Factor step 4's writes into `std::int64_t recordReceipt(Lightweight::DataMapper&, ReceiptInput const&, std::optional<JournalEntryId>)` in the same file so both modes share them. Move the lot creation out of `RecordOpeningLot` into `inventory::createLot(mapper, item, location, number, expires, unitCost)` (the unique-index `Conflict` with it), and call it from both.

Registration:

```cpp
BRIDGE_REGISTER_ACTION(lims::ProcurementModel, lims::ReceiveGoods, "ReceiveGoods")
BRIDGE_REGISTER_EFFECTS(lims::ReceiveGoods, ::morph::effects::changes<lims::ReceiptView>,
                        ::morph::effects::changes<lims::LotStockView>,
                        ::morph::effects::changes<lims::StockMovementView>,
                        ::morph::effects::updates<lims::PurchaseOrderView>)
BRIDGE_REGISTER_ACTION(lims::ProcurementModel, lims::GetReceipt, "GetReceipt", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::ProcurementModel, lims::ListReceipts, "ListReceipts", ::morph::model::Loggable::No)
```

The entry a shared-transaction receipt makes is the ledger's `Ledger_Entry`, which inventory cannot name; that is decision 39's subject and is not worked around here. `Authorization<ReceiveGoods>`: `StockKeeper` at the order's company (`scopeOf` reads the order). Declared table and coverage case as before.

- [ ] **Step 5: Build and run**

Run: Build lims, Model suite `"[receipt]"`, `"[posting]"`, `"[authorization]"`, Everything labelled for the rung, the GCC shadow sweep. Expected (inferred from decision 38's measurement of the same two modes): every case passes. Run every mutation named in the file once and restore it. Then write down, as Task 14 Step 8 did, what (a) dispatched left between the halves, (b) the shared transaction left, and what (b) gave up of the three (the ledger's effect, the ledger's rule, the ledger's journal entry), each as measured.

- [ ] **Step 6: Record decision 45**

In the Modules table, `posting/`'s row becomes "the posting contract: `PostingRequest` and `PostingResult`, and the port a poster reaches the ledger through". Then, keeping the paragraph Step 5's measurement selects and filling its brackets:

```markdown
### 45. A goods receipt posts its value as an invoice does, and meets the same choice (hypothesis 1, second instance)

`ReceiveGoods` takes a delivery against an open order: each line a new lot at a location of the order's site, its
quantity converted to the item's unit, several lots to one order line allowed, and never more than was ordered. The
receipt is valued at the order's prices in the supplier's currency, each line rounded half-up once, and posted
through the one ledger port the app installs (which now lives in `posting`, because two posters reach it): a debit to
stock per line, converted at the ledger's rate on the receipt's date and rounded per line, against one credit to goods
received not invoiced for the total, rounded once, and a line to the rounding account for what the per-line rounding
left. A lot's unit cost is its line's company-currency value over its quantity, exactly. The number,
`GR-<fiscal year>-<six digits>`, is gap-free per company and fiscal year, assigned in inventory's commit. A receipt is
refused without a rate on its date, for a lot number the item already has, and beyond what was ordered; two receipts
against the last of an order line give one receipt and one `Conflict`.

[One of, the same three as decision 38:]

**Hypothesis 1 is confirmed for its second instance.** Measured in `tests/test_goods_receipt.cpp`: in one shared
transaction a failure between the halves leaves nothing and no hole, but the ledger's entry [is not heard by a watching
accountant / is not refused by the ledger's rule / is not journaled as the ledger's action]; dispatched, the ledger
keeps each of those and a failure between the halves leaves an entry for goods that are not on hand until the receipt
is retried, which converges on the same entry by its source key. The two postings needed the same thing and neither
has it: two models' actions committed as one fact, each keeping its authorization, effects and journal.

**Hypothesis 1 is refuted.** [As decision 38's refuted text, for the receipt.]

**Hypothesis 1 is mis-framed.** [What Step 5 measured that the hypothesis does not describe.]
```

- [ ] **Step 7: Commit**

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/inventory examples/lims/app examples/lims/tests examples/lims/README.md
git commit -m "Receive lims goods into new lots and post their value, measured in both posting modes

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---
### Task 5: `lab.ResultCaptured` becomes a consumption from the first-expiring lot (hypothesis 2, second instance)

Steps 1–7 wait only for Task 1. **Step 8 (posting the consumption's cost) waits for Task 4** and so for Part B Task 14.

**Files:**
- Create: `examples/lims/inventory/include/lims/inventory/dto/recipe_dto.hpp`, `…/dto/consumption_dto.hpp`, `examples/lims/inventory/src/models/consumption.cpp`, `examples/lims/tests/test_consumption.cpp`
- Modify: `examples/lims/lab/include/lims/lab/events/lab_events.hpp` (`kResultCaptured`, `ResultCaptured`), `examples/lims/lab/include/lims/lab/events/lab_outbox_store.hpp` and `examples/lims/lab/src/events/lab_events.cpp` (`writeResultCaptured`), `examples/lims/lab/src/models/sample_model.cpp` (`applyCapture` writes it), `examples/lims/app/include/lims/app/event_wiring.hpp`/`.cpp` (`InventoryEventSink`, `LabEventRouter`, `relayLabEvents` routes), `…/db/inventory_entity.hpp` (`RecipeRecord`, `ConsumedEventRecord`), `…/src/db/inventory_schema.cpp` (migration `20261120000007`), `…/models/stock_model.hpp`, `…/authorization/inventory_authorization.hpp`/`.cpp`, `examples/lims/tests/test_domain_events.cpp` (relay counts and the dying relay's sink), `examples/lims/tests/inventory_test_support.hpp` (`consumptionSetUp`), `declared_requirements.hpp`, `test_declared_authorization.cpp`, `examples/lims/README.md` (decision 46)

**Interfaces:**
- Consumes: `lims::app::InternalClient`, `lims::app::kEventsPrincipal`, `morph::journal::OutboxRelay`, `morph::journal::IActionLog`, `morph::offline::IReplayLedger`, `lims::drainLabOutbox`, `lims::markLabOutboxRelayed`, `inventory::moveLot`, `convertStock`, `requireServicePrincipal`, `lims::test::readyToPublish`, `lims::test::grantService`.
- Produces: `inline constexpr std::string_view lims::kResultCaptured = "lab.ResultCaptured"`; `struct lims::ResultCaptured { ResultId result; SampleId sample; AnalysisId analysis; SiteId site; ::morph::time::Timestamp capturedAt; }` (event id `lab.ResultCaptured:<result>:<sample version>`); `void lims::writeResultCaptured(Lightweight::DataMapper&, db::SampleRecord const&, db::ResultRecord const&, std::int64_t analysisId, std::string const& principal)`; `RecipeId` (`lims.recipeId`); `SetRecipe { CompanyId company; AnalysisId analysis; ItemId item; Measure quantity; StockUnit unit; }` → `RecipeView { RecipeId id; CompanyId company; AnalysisId analysis; ItemId item; std::string itemCode; Measure quantity; StockUnit unit; }`; `ListRecipes { CompanyId company; }` → `ListRecipesResult`; `RecordConsumption { std::string eventId; ResultId result; SampleId sample; AnalysisId analysis; SiteId site; ::morph::time::Timestamp capturedAt; }` → `ConsumptionView { std::string eventId; SampleId sample; std::vector<StockMovementView> movements; Measure shortfall; }`, all on `StockModel`; records `Inventory_Recipe`, `Inventory_Consumption` (keyed by `eventId`); `class lims::app::InventoryEventSink final : IActionLog`; `class lims::app::LabEventRouter final : IActionLog` (`LabEventRouter(std::vector<std::pair<std::string, std::shared_ptr<IActionLog>>> routes)`); `std::shared_ptr<LabEventRouter> lims::app::labEventRouter(InternalClient& events)`; `relayLabEvents` unchanged in signature, routing both event types; test helper `lims::test::ConsumptionSetUp lims::test::consumptionSetUp(bool withLateLot = true)`.

An analysis definition's reagent consumption is declared in inventory, per analysis and company: "a nitrate analysis uses 25 mL of the nitrate reagent". It lives in inventory and not on lab's analysis definition because lab cannot know items, and an item is inventory's; inventory refers to the analysis by `AnalysisId`, which is shared in `org/core/references.hpp`. When lab captures a result it writes `lab.ResultCaptured` into its outbox in the capture's transaction, as it writes `lab.AnalysisPublished` when it publishes (decision 32). The app's relay delivers each event to its consumer by type: analysis-published to billing, result-captured to inventory. Inventory's consumer, `RecordConsumption`, takes each recipe line of the analysis from the lots of the item at the event's site, first-expiring first, skipping expired lots; what the lots cannot cover is recorded as a shortfall movement rather than refused, because a consumer that refuses an event holds back every event behind it (decision 32). A redelivered event consumes nothing twice.

The second consumer is the measurement: decision 32 counted about 70 lines of consumer-side idempotence in billing (the `IReplayLedger` adapter over its own table, the lookup before the write, the race catch, and the unique index) and said a copy is a seam to promote. Inventory needs the same guarantee and is written the same honest way, and the lines are counted.

- [ ] **Step 1: Write the failing test**

Append to `inventory_test_support.hpp` (include `billing_test_support.hpp` for `readyToPublish` and `grantService`):

```cpp
/// @brief What `consumptionSetUp` made.
struct ConsumptionSetUp {
    /// @brief The stock.
    StockSetUp stock;
    /// @brief The lots: `L-EARLY` (0.040 L, expiring 31 January 2027) and `L-LATE` (1 L, expiring 30 June 2027).
    LotStockView early;
    /// @brief The later lot.
    LotStockView late;
};

/// @brief `stockSetUp`, lots of the reagent in the cold room, and a recipe: each analysis of id 1 to 20 uses 25 mL of
///        the reagent. The events principal holds the service role at the seeded company.
/// @param withLateLot Whether `L-LATE` is recorded; without it, `L-EARLY` is the last lot.
/// @return The ids; `late` is empty without the late lot.
inline ConsumptionSetUp consumptionSetUp(bool withLateLot = true) {
    ConsumptionSetUp setUp{.stock = stockSetUp(), .early = {}, .late = {}};
    setUp.early = openingLot(setUp.stock.reagent, setUp.stock.coldRoom, "L-EARLY", at(2027, 1, 31), litres(40),
                             StockUnit::litre);
    if (withLateLot) {
        setUp.late = openingLot(setUp.stock.reagent, setUp.stock.coldRoom, "L-LATE", at(2027, 6, 30), litres(1000),
                                StockUnit::litre);
    }
    ScopedPrincipal const kim{"kim"};
    StockModel stock;
    for (std::int64_t analysis = 1; analysis <= 20; ++analysis) {
        static_cast<void>(stock.execute(SetRecipe{.company = seededCompany(),
                                                  .analysis = AnalysisId{analysis},
                                                  .item = setUp.stock.reagent,
                                                  .quantity = millilitres(25),
                                                  .unit = StockUnit::millilitre}));
    }
    grantService(std::string{app::kEventsPrincipal});
    return setUp;
}
```

Create `examples/lims/tests/test_consumption.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Hypothesis 2, second instance: lab's result-captured event reaches inventory through the same outbox and relay as
// its published event reaches billing, routed by type, and consumes reagent from the first-expiring lot once, across a
// relay that dies before marking and a restart.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <memory>
#include <morph/journal/outbox.hpp>
#include <span>
#include <stdexcept>
#include <string>

#include "billing_test_support.hpp"
#include "inventory_test_support.hpp"
#include "lims/app/event_wiring.hpp"
#include "lims/app/internal_client.hpp"
#include "lims/app/service_principals.hpp"
#include "lims/inventory/models/stock_model.hpp"
#include "lims/lab/events/lab_events.hpp"
#include "testkit/backend_rig.hpp"

using lims::test::at;
using lims::test::litres;
using lims::test::ScopedPrincipal;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::Mode;
using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

namespace {

/// @brief The reagent's lots, by number, as kim sees them.
/// @return Each lot's stock on hand, by lot number.
std::map<std::string, Rational> onHand() {
    ScopedPrincipal const kim{"kim"};
    std::map<std::string, Rational> lots;
    for (auto const& lot :
         lims::StockModel{}.execute(lims::StockOnHand{.company = lims::test::seededCompany(), .item = {}}).lots) {
        lots.emplace(lot.lotNumber, *lot.onHand);
    }
    return lots;
}

/// @brief @p thousandths / 1000 at six places.
/// @param thousandths The amount.
/// @return It.
Rational milli(std::int64_t thousandths) {
    return Rational{Numerator{thousandths}, Denominator{1000}, DecimalPlaces{6}};
}

}  // namespace

// Mutation: write the outbox row after the capture's commit (a failure injected between them leaves a result with no
// event; the count check fails).
TEST_CASE("consumption: a capture writes one result-captured event in its transaction", "[lims][inventory][events]") {
    lims::test::LabFixture const fixture;
    static_cast<void>(lims::test::consumptionSetUp());
    auto const lab = lims::test::readyToPublish();
    auto const events = lims::drainLabOutbox();
    auto const captured = std::ranges::count_if(
        events, [](morph::journal::LogEntry const& entry) { return entry.actionType == lims::kResultCaptured; });
    CHECK(captured == 1);
    CHECK(events.front().idempotencyKey.starts_with("lab.ResultCaptured:" + std::to_string(*lab.results.front()) + ":"));
}

// Mutation: order lots by id instead of expiry (the later lot is consumed first).
TEST_CASE("consumption: reagent is taken from the first-expiring lot, then the next", "[lims][inventory][events]") {
    lims::test::LabFixture const fixture;
    static_cast<void>(lims::test::consumptionSetUp());
    static_cast<void>(lims::test::readyToPublish("WW-1"));
    static_cast<void>(lims::test::relayLabEvents());
    CHECK(onHand().at("L-EARLY") == milli(15));
    CHECK(onHand().at("L-LATE") == milli(1000));
    static_cast<void>(lims::test::readyToPublish("WW-2"));
    static_cast<void>(lims::test::relayLabEvents());
    // 15 mL left in L-EARLY, so 10 mL come from L-LATE.
    CHECK(onHand().at("L-EARLY") == milli(0));
    CHECK(onHand().at("L-LATE") == milli(990));
}

// Mutation: throw ValidationError when the lots cannot cover a recipe (the relay stops at the event, and the second
// event behind it is never consumed).
TEST_CASE("consumption: what the lots cannot cover is a shortfall, not a refusal", "[lims][inventory][events]") {
    lims::test::LabFixture const fixture;
    auto const setUp = lims::test::consumptionSetUp();
    {
        ScopedPrincipal const kim{"kim"};
        static_cast<void>(lims::StockModel{}.execute(lims::SetRecipe{.company = lims::test::seededCompany(),
                                                                     .analysis = lims::AnalysisId{1},
                                                                     .item = setUp.stock.reagent,
                                                                     .quantity = litres(2000),
                                                                     .unit = lims::StockUnit::litre}));
    }
    static_cast<void>(lims::test::readyToPublish("WW-1"));
    static_cast<void>(lims::test::readyToPublish("WW-2"));
    CHECK_NOTHROW(lims::test::relayLabEvents());
    CHECK(onHand().at("L-EARLY") == milli(0));
    CHECK(onHand().at("L-LATE") == milli(0));
    ScopedPrincipal const kim{"kim"};
    auto const moves =
        lims::StockModel{}.execute(lims::ListStockMovements{.item = setUp.stock.reagent, .query = {}}).rows;
    CHECK(std::ranges::count_if(moves, [](lims::StockMovementView const& move) {
              return move.kind == lims::MovementKind::Shortfall;
          }) >= 1);
}

// Hypothesis 2. Mutation: make the consumed-event lookup always empty (the redelivery consumes again: L-LATE drops by
// a further 10 mL).
TEST_CASE("Hypothesis 2: a capture consumes once, across a relay that dies before marking it and a restart",
          "[lims][inventory][events][h2]") {
    lims::test::LabFixture const fixture;
    static_cast<void>(lims::test::consumptionSetUp());
    static_cast<void>(lims::test::readyToPublish("WW-1"));
    static_cast<void>(lims::test::readyToPublish("WW-2"));
    lims::test::grantService(std::string{lims::app::kEventsPrincipal});
    {
        BackendRig rig{Mode::Socket, 0, lims::test::limsAuthorizer(lims::test::kServiceSecret)};
        lims::app::InternalClient events{rig.server(), lims::test::kServiceSecret,
                                         std::string{lims::app::kEventsPrincipal}};
        morph::journal::OutboxRelay dying;
        dying.drainOutbox = [] { return lims::drainLabOutbox(); };
        dying.markRelayed = [](std::span<morph::journal::LogEntry const>) {
            throw std::runtime_error{"the relay dies before marking"};
        };
        dying.sink = lims::app::labEventRouter(events);
        CHECK_THROWS(dying.relay());
    }
    auto const afterFirst = onHand();
    CHECK(afterFirst.at("L-LATE") == milli(990));
    CHECK_FALSE(lims::drainLabOutbox().empty());
    // A fresh server and relay over the same database: the restart.
    static_cast<void>(lims::test::relayLabEvents());
    CHECK(onHand() == afterFirst);
    CHECK(lims::drainLabOutbox().empty());
}

// Mutation: drop the unique index on inventory_consumed_events (the two racing deliveries both consume).
TEST_CASE("consumption: two deliveries of one event that race past the lookup consume once",
          "[lims][inventory][events]") {
    lims::test::LabFixture const fixture;
    static_cast<void>(lims::test::consumptionSetUp());
    static_cast<void>(lims::test::readyToPublish("WW-1"));
    auto const events = lims::drainLabOutbox();
    auto const captured = std::ranges::find_if(
        events, [](morph::journal::LogEntry const& entry) { return entry.actionType == lims::kResultCaptured; });
    REQUIRE(captured != events.end());
    lims::ResultCaptured event;
    REQUIRE_FALSE(glz::read<glz::opts{.error_on_unknown_keys = false}>(event, captured->payload));
    lims::RecordConsumption const delivery{.eventId = captured->idempotencyKey,
                                           .result = event.result,
                                           .sample = event.sample,
                                           .analysis = event.analysis,
                                           .site = event.site,
                                           .capturedAt = event.capturedAt};
    lims::test::grantService("system:test");
    ScopedPrincipal const service{"system:test"};
    auto const winner = lims::StockModel::storeConsumption(delivery, lims::test::seededCompany(), [&delivery] {
        // The winner commits between the loser's lookup and its write.
        static_cast<void>(lims::StockModel::storeConsumption(delivery, lims::test::seededCompany(), {}));
    });
    CHECK(winner.movements.size() == 1);
    CHECK(onHand().at("L-EARLY") == milli(15));
}

TEST_CASE("consumption: only the server's own client records a consumption", "[lims][inventory][events]") {
    lims::test::LabFixture const fixture;
    static_cast<void>(lims::test::consumptionSetUp());
    lims::test::grant("eve", lims::LimsRole::Service);
    ScopedPrincipal const eve{"eve"};
    CHECK_THROWS_WITH(lims::StockModel{}.execute(lims::RecordConsumption{.eventId = "lab.ResultCaptured:1:2",
                                                                         .result = lims::ResultId{1},
                                                                         .sample = lims::SampleId{1},
                                                                         .analysis = lims::AnalysisId{1},
                                                                         .site = lims::test::seededSite(),
                                                                         .capturedAt = morph::time::Timestamp{
                                                                             at(2026, 11, 10)}}),
                      Catch::Matchers::ContainsSubstring("only the server's own clients"));
}

// Mutation: make LabEventRouter skip an event type it has no route for (an unknown event is deleted unconsumed by the
// relay's mark; the check expects a throw).
TEST_CASE("consumption: the router refuses an event type it has no consumer for", "[lims][inventory][events]") {
    lims::test::LabFixture const fixture;
    lims::app::LabEventRouter router{{}};
    morph::journal::LogEntry entry;
    entry.actionType = "lab.SomethingElse";
    entry.idempotencyKey = "lab.SomethingElse:1:1";
    CHECK_THROWS_WITH(router.append(entry), Catch::Matchers::ContainsSubstring("no consumer"));
}
```

Add `#include <algorithm>`, `#include <map>` and `#include <glaze/glaze.hpp>`. Every case runs in a `LabFixture`, whose bench staff (`alice` among them) may capture; `readyToPublish` captures as alice.

- [ ] **Step 2: Run it and see it fail**

Run: Build lims. Expected: compile error, `RecordConsumption` and `kResultCaptured` undeclared.

- [ ] **Step 3: Lab writes `lab.ResultCaptured` in the capture's transaction**

Append to `lab_events.hpp`:

```cpp
/// @brief The event type of a captured result.
inline constexpr std::string_view kResultCaptured = "lab.ResultCaptured";

/// @brief A result was captured: a measurement was made, whatever it read.
struct ResultCaptured {
    /// @brief The result.
    ResultId result;
    /// @brief Its sample.
    SampleId sample;
    /// @brief The analysis definition it was captured under.
    AnalysisId analysis;
    /// @brief The sample's site.
    SiteId site;
    /// @brief When it was captured.
    ::morph::time::Timestamp capturedAt;
};
```

Declare `writeResultCaptured` in `lab_outbox_store.hpp` beside `writeAnalysisPublished`, with a doc comment saying it writes through the caller's mapper so the row commits with the capture or not at all. In `lab_events.cpp`:

```cpp
void writeResultCaptured(Lightweight::DataMapper& mapper, db::SampleRecord const& sample,
                         db::ResultRecord const& result, std::int64_t analysisId, std::string const& principal) {
    std::int64_t const now = nowMillis();
    ResultCaptured const event{
        .result = ResultId{static_cast<std::int64_t>(result.id.Value())},
        .sample = SampleId{static_cast<std::int64_t>(sample.id.Value())},
        .analysis = AnalysisId{analysisId},
        .site = SiteId::fromOptional(sample.siteId.Value()),
        .capturedAt = timestampFromMillis(result.capturedAt.Value()),
    };
    db::LabOutboxRecord row;
    // The sample's version, already moved by the capture, makes a re-capture of the same result a new event.
    row.eventId = Lightweight::SqlAnsiString<128>{std::string{kResultCaptured} + ":" + std::to_string(*event.result) +
                                                  ":" + std::to_string(sample.version.Value())};
    row.eventType = Lightweight::SqlAnsiString<64>{std::string{kResultCaptured}};
    row.entityKey = Lightweight::SqlAnsiString<64>{std::to_string(*event.sample)};
    row.payload = Lightweight::SqlDynamicAnsiString<4096>{glz::write_json(event).value_or(std::string{})};
    row.principal = Lightweight::SqlAnsiString<64>{principal};
    row.recordedAt = now;
    mapper.Create(row);
}
```

In `sample_model.cpp`'s `applyCapture`, after `mapper.Update(sampleRow);` and before `markDecided`, call `writeResultCaptured(mapper, sampleRow, row, static_cast<std::int64_t>(version.analysis.Value()), author);` (`version` is the analysis version row the function already read). Every path that captures — `CaptureConcentration`, a replayed `QueuedCapture` and a `ResolveConflict` that applies anyway — runs `applyCapture`, so each writes its event. A re-capture of one analysis version is a second measurement and a second event; it consumes again, as a repeated measurement uses reagent again.

- [ ] **Step 4: Recipes, the consumer and its idempotence**

Entities: `RecipeRecord` (`inventory_recipes`): `id`, `company_id`, `analysis_id`, `item_id`, `quantity_num`, `quantity_den` (in the item's unit); `ConsumedEventRecord` (`inventory_consumed_events`): `id`, `event_id` (`SqlAnsiString<128>`), `company_id`, `sample_id`, `shortfall_num`, `shortfall_den`, `cost_num`, `cost_den` (company currency, Step 8), `entry_id` (`std::optional<std::int64_t>`, Step 8).

```cpp
LIGHTWEIGHT_SQL_MIGRATION(20261120000007, "Create inventory recipes and consumed events") {
    plan.CreateTableIfNotExists("inventory_recipes")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("company_id", Bigint())
        .RequiredColumn("analysis_id", Bigint())
        .RequiredColumn("item_id", Bigint())
        .RequiredColumn("quantity_num", Bigint())
        .RequiredColumn("quantity_den", Bigint());
    // One quantity of an item per analysis and company; setting it again replaces it.
    plan.CreateUniqueIndex("idx_inventory_recipes_line", "inventory_recipes", {"company_id", "analysis_id", "item_id"});
    plan.CreateTableIfNotExists("inventory_consumed_events")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("event_id", Varchar(128))
        .RequiredColumn("company_id", Bigint())
        .RequiredColumn("sample_id", Bigint())
        .RequiredColumn("shortfall_num", Bigint())
        .RequiredColumn("shortfall_den", Bigint())
        .RequiredColumn("cost_num", Bigint())
        .RequiredColumn("cost_den", Bigint())
        .Column("entry_id", Bigint());
    // An event is consumed once: the index refuses the second of two deliveries that raced past the lookup.
    plan.CreateUniqueIndex("idx_inventory_consumed_events_event", "inventory_consumed_events", {"event_id"});
    plan.AlterTable("inventory_movements").AddNotRequiredColumn("event_id", Varchar(128));
}
```

Movements a consumption writes carry its event id in `event_id` too (and `source` names it, for the screens), so a consumption's movements are read back by the event that made them. `MovementRecord` gains `Light::Field<std::optional<Light::SqlAnsiString<128>>, Light::SqlRealName{"event_id"}> eventId;`.

The consumer's dedup record is its own row in `inventory_consumed_events`, not an `IReplayLedger` adapter over a separate table: decision 32 noted that billing's `billing_consumed_events` duplicates `billing_lines.event_id`, and inventory takes the cheaper form that note names — the unique index on the consumption's own event id is the dedup record. What it still hand-writes is the lookup before the write and the race catch; those are what Step 6 counts.

`StockModel::execute(const RecordConsumption&)` in `consumption.cpp`: `requirePrincipal`; validate (event id 1–128 bytes, every id and the date); `requireServicePrincipal("RecordConsumption")`; `requireRoleSomewhere`; the site's company through `OrgModel::findSite` (`NotFound`); `requireAuthorized`; then `storeConsumption(action, company, {})`. `storeConsumption`, a public static seam like `InvoiceModel::storeLine` (doc: it checks nothing; a test delivers a racing winner between its lookup and its write):

```cpp
ConsumptionView StockModel::storeConsumption(RecordConsumption const& action, CompanyId company,
                                             std::function<void()> const& betweenLookupAndWrite) {
    if (auto const earlier = inventory::consumptionOfEvent(action.eventId)) {
        return *earlier;
    }
    if (betweenLookupAndWrite) {
        betweenLookupAndWrite();
    }
    try {
        return inventory::writeConsumption(action, company);
    } catch (Lightweight::SqlException const&) {
        // A concurrent delivery of the same event recorded it between this call's lookup and its write, and the
        // unique index refused this one; the winner's consumption is the answer. A failure that is not that race finds
        // no such consumption and surfaces as itself.
        if (auto const winner = inventory::consumptionOfEvent(action.eventId)) {
            return *winner;
        }
        throw;
    } catch (Conflict const&) {
        // A lot moved under this delivery (another consumption of it committed first): the event is not consumed, so
        // the relay's next pass delivers it again and it is judged afresh.
        throw;
    }
}
```

`inventory::consumptionOfEvent(eventId)` reads on its own mapper (a read inside the write's transaction would hold a lock a concurrent writer's commit waits on): the consumed-event row and the movements with that event id, as a `ConsumptionView`; nothing when no delivery has committed. `inventory::writeConsumption(action, company)` is one transaction whose first write is the consumed-event row (so a racing duplicate is refused before any stock moves), then, per recipe line of `(company, action.analysis)`, the item's lots at the event's site (`site_id`) with stock on hand, not expired at `capturedAt`, by `expires_at` then `id`, each taken with `moveLot(Consumption, -taken, action.eventId, principal)` (and the movement's `event_id` set) until the recipe's quantity is covered, `checkedSub` throughout; what is left is one `Shortfall` movement with no lot (`moveLot` is not used for it: it writes only the movement row) and is added to the row's shortfall; the row's shortfall is updated before commit. No recipe for the analysis: the row alone is written, and the answer has no movements — the event is consumed and nothing is owed.

DTOs (`recipe_dto.hpp`, `consumption_dto.hpp`) as in Interfaces, with `validate()`s; `BRIDGE_REGISTER_RECORD(lims::RecipeView, "Inventory_Recipe")` and `BRIDGE_REGISTER_RECORD(lims::ConsumptionView, "Inventory_Consumption", ::morph::record::key<&lims::ConsumptionView::eventId>)`. Registration:

```cpp
BRIDGE_REGISTER_ACTION(lims::StockModel, lims::SetRecipe, "SetRecipe")
BRIDGE_REGISTER_EFFECTS(lims::SetRecipe, ::morph::effects::changes<lims::RecipeView>)
BRIDGE_REGISTER_ACTION(lims::StockModel, lims::ListRecipes, "ListRecipes", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::StockModel, lims::RecordConsumption, "RecordConsumption")
BRIDGE_REGISTER_EFFECTS(lims::RecordConsumption, ::morph::effects::changes<lims::ConsumptionView>,
                        ::morph::effects::changes<lims::LotStockView>,
                        ::morph::effects::changes<lims::StockMovementView>)
```

`Authorization<SetRecipe>` is `CompanyRule<SetRecipe, LimsRole::StockKeeper>`; `Authorization<RecordConsumption>` is `LimsRole::Service` with `scopeOf` the payload's site, as `RecordBillableLine`'s. Declared table and coverage case.

- [ ] **Step 5: The relay routes by type**

`event_wiring.hpp` gains, after `BillingEventSink`:

```cpp
/// @brief A `morph::journal::IActionLog` that delivers lab's result-captured events to inventory's consumer.
class InventoryEventSink final : public ::morph::journal::IActionLog {
public:
    /// @brief A sink dispatching through @p client.
    /// @param client The events principal's client. Borrowed: it must outlive this sink.
    explicit InventoryEventSink(InternalClient& client) : _client{client} {}

    /// @brief Delivers @p entry to inventory.
    /// @param entry An entry `drainLabOutbox` produced.
    /// @throws ValidationError if the entry is not a `lab.ResultCaptured` or its payload does not decode.
    /// @throws std::exception inventory's refusal.
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

/// @brief The relay's one sink: hands each event to the sink its type is routed to.
///
/// `OutboxRelay` has one sink and marks a drained batch whole, and lab's outbox holds every event type lab writes, so
/// one pass delivers several types; this routes each to its consumer. An event type with no route is refused, never
/// skipped: the relay deletes what it marks, so a skipped event would be lost to the consumer that should have had it.
class LabEventRouter final : public ::morph::journal::IActionLog {
public:
    /// @brief A router over @p routes.
    /// @param routes Each event type with the sink it goes to.
    explicit LabEventRouter(std::vector<std::pair<std::string, std::shared_ptr<::morph::journal::IActionLog>>> routes)
        : _routes{std::move(routes)} {}

    /// @brief Delivers @p entry to its type's sink.
    /// @param entry An entry `drainLabOutbox` produced.
    /// @throws ValidationError if no sink is routed for its type.
    /// @throws std::exception the sink's failure.
    void append(::morph::journal::LogEntry entry) override;

    /// @brief Flushes every routed sink.
    void flush() override;

    /// @brief The entries every routed sink delivered, for @p entityKey or all.
    /// @param entityKey A sample id, or empty for every entry.
    /// @return The entries, each sink's in its delivery order, sinks in route order.
    [[nodiscard]] std::vector<::morph::journal::LogEntry> entries(std::string_view entityKey = {}) const override;

private:
    std::vector<std::pair<std::string, std::shared_ptr<::morph::journal::IActionLog>>> _routes;
};

/// @brief The router the server relays lab's outbox into: analysis-published to billing, result-captured to
///        inventory, both through @p events.
/// @param events The events principal's client.
/// @return The router.
[[nodiscard]] std::shared_ptr<LabEventRouter> labEventRouter(InternalClient& events);
```

In `event_wiring.cpp`, `InventoryEventSink::append` is `BillingEventSink::append` with `kResultCaptured`, `ResultCaptured` and `_client.call<StockModel>(RecordConsumption{…})` (include `lims/inventory/models/stock_model.hpp`); `LabEventRouter::append` finds the route whose type equals `entry.actionType` and calls its sink, else `throw ValidationError{"no consumer is wired for " + entry.actionType}`; `labEventRouter` builds the two routes; `relayLabEvents` sets `relay.sink = labEventRouter(client)` instead of a `BillingEventSink`. `BillingEventSink` is unchanged.

Existing suites change only where they counted on lab's outbox holding one event type: in `test_domain_events.cpp`, `relayLabEvents()` now relays the capture's event too (a published sample with one result relays two events, then zero), and the dying relay of the Hypothesis 2 case uses `lims::app::labEventRouter(events)` as its sink. Change those counts to what is now relayed and keep every other assertion. A suite that relays without inventory settings is fine: a consumption with no recipe consumes nothing. The events principal needs the service role at the site's company for both consumers; `lims::test::grantService` grants it at the seeded company already.

- [ ] **Step 6: Build, run, and count the second consumer**

Run: Build lims, Model suite `"[events]"` (billing's and inventory's), `"[authorization]"`, Everything labelled for the rung, the GCC shadow sweep. Expected (the h2 case inferred from decision 32's measurement of the same relay): all pass. Run each mutation named in `test_consumption.cpp` once and restore it.

Then count, and write the numbers down for Step 7:

```bash
cd /home/yaraslau/repo/morph
wc -l examples/lims/app/src/event_wiring.cpp examples/lims/app/include/lims/app/event_wiring.hpp
grep -n 'consumptionOfEvent\|storeConsumption\|SqlException' examples/lims/inventory/src/models/consumption.cpp
grep -rn 'cursor\|lastId\|last_id\|offset\|position' examples/lims/app examples/lims/inventory/src | grep -v '//' || true
```

Count separately: (i) the wiring in `app/` for the second consumer (`InventoryEventSink`, the router, `labEventRouter`); (ii) the consumer-side idempotence inventory hand-wrote — `consumptionOfEvent`, the lookup-then-write-then-catch in `storeConsumption`, and the unique index in the migration — in lines, beside billing's 70 (decision 32's breakdown: adapter 36, `lineOfEvent` 14, race catch 11, table and index 8); (iii) whether either copies a delivery loop or a cursor (expected: neither).

- [ ] **Step 7: Record decision 46**

```markdown
### 46. A captured result consumes reagent from the first-expiring lot, through the same relay, routed by type (hypothesis 2, second instance)

An analysis's reagent use is declared in inventory, per company: a quantity of an item per analysis, referring to the
analysis by its shared id, because lab cannot know items. Lab writes `lab.ResultCaptured` into its outbox in the
capture's transaction (every capture path writes it: entry, replayed offline capture, a conflict applied anyway), with
the event id `lab.ResultCaptured:<result>:<sample version>`, so a re-capture is a new event and consumes again. The
relay's one sink is now `lims::app::LabEventRouter`, which hands each event to the consumer its type is routed to —
analysis-published to billing, result-captured to inventory — and refuses a type with no route rather than let the
relay delete it unconsumed. `RecordConsumption`, sent only by the server's own client, takes each recipe line from the
lots of the item at the event's site that have not expired, first-expiring first, each lot's movement conditional on
its stock as read; what the lots cannot cover is a shortfall movement, not a refusal, because a refused event holds
back every event behind it.

**Hypothesis 2 is [refuted | confirmed] for its second instance.** Measured in `tests/test_consumption.cpp`: a relay
that delivers and dies before marking, then a fresh relay over the same database, leaves each lot as the first
delivery left it, and the outbox empty. What `lims/app` gained for the second consumer is wiring: the sink and the
router, [n] lines, and no delivery loop or cursor. What inventory hand-wrote for idempotence is [n] lines — the lookup
before the write, the race catch, and the unique index — against billing's 70. Inventory took the cheaper form
decision 32 named (its consumption row is the dedup record, so it has no adapter and no second table), and still
[copied the lookup and the race catch verbatim in shape: the guarantee every consumer of an at-least-once source needs,
written twice | needed none of it, because …].

[If the lookup and the race catch were copied:] That is the same hand-rolled guarantee in a second module: a
consumer-side "consume once by event id over the consumer's own row" with the race settled by a unique index. It is
mechanism (bookmarks or a CRM consuming an outbox would need it unchanged) and it has now been needed twice, so it is a
seam to specify.
```

Choose by Step 6's count: **refuted** when (i) is wiring only and (iii) finds no loop or cursor, which is the design's criterion — and then add the bracketed paragraph about the copied guarantee if (ii) copied it; **confirmed** if the second consumer needed a delivery loop or cursor of its own. Fill every `[n]`.

- [ ] **Step 8: Post the consumption's cost (waits for Task 4)**

A consumption takes stock at each lot's unit cost; its cost is the sum, per lot taken, of the quantity times the unit cost, rounded half-up per lot to the company's currency (`checkedMul`, `roundToCurrency`, `ExactSum`), and it is posted as a debit to the consumption account against a credit to stock, through the one ledger port, with the source key `inventory.consumption:<event id>` and the company's currency (rate one). The cost is fixed by the committed consumption, never recomputed, so a repeated posting has the same body and the ledger answers the first entry:

- `PostingMode::SharedTransaction`: `writeConsumption` computes the cost and calls `ledgerPort().postIn(mapper, request)` before its commit, storing `entry_id`.
- `PostingMode::Dispatched`: `writeConsumption` stores the cost with `entry_id` empty and commits; `RecordConsumption`, now a Task handler (`core::async::Task<ConsumptionView>`), then `co_await`s `ledgerPort().post(request)` and stores `entry_id` by a conditional update (`entry_id IS NULL`). A redelivered event whose consumed row has no entry is posted then, from the stored cost: the consumer's dedup answer includes "the posting is still owed".

A consumption with no cost (no recipe, or only a shortfall) posts nothing. Add two cases to `test_consumption.cpp`, each in both modes over `ReceiptRig`: "consumption: its cost is posted once, at each lot's unit cost" (two lots at different unit costs, entry amounts checked; mutation: post at the first lot's cost for every lot) and "consumption: a dispatched cost posting that failed is posted when the event is redelivered" (fault point `consumption.afterCommit` throws once; the relay's next pass posts it; one entry; mutation: answer the dedup hit without checking `entry_id`). Add the cost and its posting to decision 46's first paragraph, one sentence each, and commit with the message "Post the cost of lims reagent consumption through the ledger port".

- [ ] **Step 9: Commit (Steps 1–7)**

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/lab examples/lims/inventory examples/lims/app examples/lims/tests examples/lims/README.md
git commit -m "Consume lims reagent from the first-expiring lot when lab captures a result, routed by event type

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---
### Task 6: Lot reservation per sample — the race for the last lot, and the lapse on disconnect (hypothesis 3, second instance)

**Files:**
- Create: `examples/lims/inventory/include/lims/inventory/dto/reservation_dto.hpp`, `…/models/reservation_model.hpp`, `examples/lims/inventory/src/models/reservation_model.cpp`, `examples/lims/tests/test_reservation.cpp`
- Modify: `…/core/types.hpp` (`ReservationId`), `…/db/inventory_entity.hpp` (`ReservationRecord`), `…/src/db/inventory_schema.cpp` (migration `20261120000008`), `…/db/inventory_store.hpp`/`.cpp` (`lotView`'s live reserved quantity; `availableOf`), `…/src/models/consumption.cpp` (a sample's reserved lots first; other samples' reservations excluded), `…/models/stock_model.hpp` (`GetSampleReservation`), `…/authorization/inventory_authorization.hpp`/`.cpp`, `declared_requirements.hpp` (`ReserveForSample`; `ReleaseReservation` open), `test_declared_authorization.cpp`, `examples/lims/README.md` (decision 47)

**Interfaces:**
- Consumes: `InvoiceEditModel`'s pattern (`billing/src/models/invoice_edit_model.cpp`: a token per instance, the destructor's release, `isBusy`, `leaseMovedOn`), `BackendRig::connect`, `ServerConnection`, `consumptionSetUp`, `inventory::changedRows`, `inventoryFaultPoint`.
- Produces: `ReservationId` (`lims.reservationId`); `ReserveForSample { SampleId sample; SiteId site; std::vector<AnalysisId> analyses; }` → `ReservationView`; `ReleaseReservation {}` → `ReservationView` (on `lims::ReservationModel`, `"ReservationModel"`, one private instance per bench); `GetSampleReservation { SampleId sample; }` → `ReservationView` (on `StockModel`, a read); `ReservedLotView { LotId lot; std::string lotNumber; ItemId item; std::string itemCode; Measure quantity; StockUnit unit; }`; `ReservationView { SampleId sample; std::string holder; std::vector<ReservedLotView> lots; ::morph::time::Timestamp expiresAt; }` (empty holder when nothing is reserved); record `Inventory_Reservation` (keyed by `sample`); `Rational lims::inventory::availableOf(Lightweight::DataMapper&, db::LotRecord const&, std::int64_t exceptSample)`.

Assigning analyses to a sample at the bench reserves what they will consume: for each recipe line of each analysis, the first-expiring lot at the sample's site whose available stock — on hand less every live reservation of another sample — covers the line. A reservation is rows (sample, lot, item, quantity, holder, the editing instance's token, an expiry an hour after the last reservation), and it is what `RecordConsumption` takes first for that sample. Two benches reserving the last lot for two samples race: the lot's `reservation_version` is the first write of each reservation's transaction, conditional on the version read, so the two are ordered; the one that finds the lot no longer covers its line is refused in words naming the holder of the reservations that took it. A reservation lapses when its instance goes — the bench's screen unmounts, or the server reclaims the instance when its connection closes — through the destructor, as the invoice lease does (decision 37), and after its hour, which needs no timer: a lapsed reservation counts for nothing.

Inventory cannot check that the sample exists or is at the site the payload names: no module reads another's tables, and lab tells inventory of a sample only when a result is captured. A reservation for a sample that does not exist reserves stock until it lapses; the bench's role at the named site is what authorizes it.

- [ ] **Step 1: Write the failing test**

Create `examples/lims/tests/test_reservation.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Hypothesis 3, second instance: a bench reserves the lots a sample's analyses will consume; two benches racing for
// the last lot give one reservation and a refusal naming its holder; the reservation lapses after its hour and when
// its holder's connection closes; what a watching connection is told of the lapse is measured.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <glaze/glaze.hpp>
#include <morph/session/session_auth.hpp>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "clock.hpp"
#include "inventory_test_support.hpp"
#include "lims/inventory/models/reservation_model.hpp"
#include "lims/inventory/models/stock_model.hpp"
#include "lims/org/core/errors.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::at;
using lims::test::ScopedPrincipal;
using morph::ladder::ScopedClockOverride;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::ServerConnection;

namespace {

constexpr std::string_view kSecret = "lims-reservation-test-secret-at-least-32";

/// @brief `consumptionSetUp` with only the early lot (40 mL, enough for one analysis of 25 mL), and two bench
///        analysts at the seeded site, `alice` and `bob`.
void lastLot() {
    static_cast<void>(lims::test::consumptionSetUp(false));
    lims::test::grant("alice", lims::LimsRole::Analyst, lims::Scope{lims::test::seededSite()});
    lims::test::grant("bob", lims::LimsRole::Analyst, lims::Scope{lims::test::seededSite()});
}

/// @brief The reservation for @p sample, nitrate analysis 1, at the seeded site.
/// @param sample The sample.
/// @return The action.
lims::ReserveForSample reserve(std::int64_t sample) {
    return lims::ReserveForSample{.sample = lims::SampleId{sample},
                                  .site = lims::test::seededSite(),
                                  .analyses = {lims::AnalysisId{1}}};
}

/// @brief Who holds @p sample's reservation, as kim sees it.
/// @param sample The sample.
/// @return The holder; empty when nothing is reserved.
std::string holderOf(std::int64_t sample) {
    ScopedPrincipal const kim{"kim"};
    return lims::StockModel{}.execute(lims::GetSampleReservation{.sample = lims::SampleId{sample}}).holder;
}

/// @brief A session signed with the case's secret.
/// @param principal The principal.
/// @return The session.
morph::session::Context as(std::string const& principal) {
    morph::session::TokenIssuer const issuer{std::string{kSecret}, morph::session::hmacSha256};
    return lims::test::signedAs(issuer, principal);
}

}  // namespace
```

Then the cases:

```cpp
// Mutation: skip the availability check against other samples' live reservations (bob reserves the 40 mL alice holds).
TEST_CASE("reservation: a second bench is told who holds the last lot, and takes it once it lapses",
          "[lims][inventory][reservation]") {
    DbFixture fixture;
    lastLot();
    ScopedClockOverride const nine{at(2026, 11, 10, 9)};
    lims::ReservationModel aliceBench;
    lims::ReservationModel bobBench;
    {
        ScopedPrincipal const alice{"alice"};
        auto const held = aliceBench.execute(reserve(101));
        CHECK(held.holder == "alice");
        REQUIRE(held.lots.size() == 1);
        CHECK(held.lots.front().lotNumber == "L-EARLY");
    }
    {
        ScopedPrincipal const bob{"bob"};
        CHECK_THROWS_WITH(bobBench.execute(reserve(102)), Catch::Matchers::ContainsSubstring("reserved by 'alice'"));
    }
    CHECK(holderOf(101) == "alice");
    ScopedClockOverride const later{at(2026, 11, 10, 11)};
    ScopedPrincipal const bob{"bob"};
    CHECK(bobBench.execute(reserve(102)).holder == "bob");
}

TEST_CASE("reservation: a bench that goes away releases its reservation", "[lims][inventory][reservation]") {
    DbFixture fixture;
    lastLot();
    {
        ScopedPrincipal const alice{"alice"};
        lims::ReservationModel bench;
        static_cast<void>(bench.execute(reserve(101)));
        CHECK(holderOf(101) == "alice");
    }
    CHECK(holderOf(101).empty());
}

// The lesson Part B paid for: two benches at once give one reservation and one refusal naming its holder, never a raw
// SQL error. Mutation: insert the reservation rows before bumping the lot's reservation_version (both benches reserve
// the same 40 mL in some round).
TEST_CASE("reservation: two benches racing for the last lot give one reservation and one named refusal",
          "[lims][inventory][reservation]") {
    DbFixture fixture;
    lastLot();
    for (int round = 0; round < 10; ++round) {
        std::atomic<int> reserved{0};
        std::atomic<int> named{0};
        std::vector<std::string> other;
        std::mutex otherMutex;
        auto const attempt = [&](std::string const& principal, std::int64_t sample) {
            try {
                ScopedPrincipal const who{principal};
                lims::ReservationModel bench;
                static_cast<void>(bench.execute(reserve(sample)));
                ++reserved;
                // The bench goes away at the end of the round, releasing its reservation for the next round.
            } catch (lims::Conflict const& error) {
                if (std::string_view{error.what()}.find("reserved by '") != std::string_view::npos) {
                    ++named;
                } else {
                    std::scoped_lock const lock{otherMutex};
                    other.emplace_back(error.what());
                }
            } catch (std::exception const& error) {
                std::scoped_lock const lock{otherMutex};
                other.emplace_back(error.what());
            }
        };
        std::thread first{attempt, std::string{"alice"}, std::int64_t{201 + (2 * round)}};
        std::thread second{attempt, std::string{"bob"}, std::int64_t{202 + (2 * round)}};
        first.join();
        second.join();
        INFO("round " << round << ": " << (other.empty() ? std::string{} : other.front()));
        CHECK(reserved + named == 2);
        CHECK(reserved >= 1);
        CHECK(other.empty());
    }
}
```

A bench that loses the race may also find the lot free again when the winner's thread has already ended and
released it, which is why that case checks `reserved + named == 2` with at least one reservation. The next case holds
both benches alive, so exactly one wins:

```cpp
// Mutation: as above (both benches hold the lot in some round).
TEST_CASE("reservation: while the winner's bench lives, the loser is refused naming it",
          "[lims][inventory][reservation]") {
    DbFixture fixture;
    lastLot();
    for (int round = 0; round < 10; ++round) {
        lims::ReservationModel aliceBench;
        lims::ReservationModel bobBench;
        std::latch start{2};
        std::atomic<int> reserved{0};
        std::atomic<int> named{0};
        auto const attempt = [&](std::string const& principal, lims::ReservationModel& bench, std::int64_t sample) {
            ScopedPrincipal const who{principal};
            start.arrive_and_wait();
            try {
                static_cast<void>(bench.execute(reserve(sample)));
                ++reserved;
            } catch (lims::Conflict const& error) {
                if (std::string_view{error.what()}.find("reserved by '") != std::string_view::npos) {
                    ++named;
                }
            }
        };
        std::thread first{attempt, std::string{"alice"}, std::ref(aliceBench), std::int64_t{301 + (2 * round)}};
        std::thread second{attempt, std::string{"bob"}, std::ref(bobBench), std::int64_t{302 + (2 * round)}};
        first.join();
        second.join();
        INFO("round " << round);
        CHECK(reserved == 1);
        CHECK(named == 1);
        // Both benches go out of scope here and release what they hold before the next round.
    }
}
```

Add `#include <functional>` and `#include <latch>` to the file's includes.

```cpp
// Consumption takes a sample's reserved lot first, and leaves other samples' reservations alone. Mutation: ignore
// reservations in writeConsumption's lot order (sample 102's capture consumes the lot reserved for 101).
TEST_CASE("reservation: a capture consumes its own sample's reservation and not another's",
          "[lims][inventory][reservation]") {
    lims::test::LabFixture const fixture;
    static_cast<void>(lims::test::consumptionSetUp());
    lims::test::grant("bob", lims::LimsRole::Analyst, lims::Scope{lims::test::seededSite()});
    auto const lab = lims::test::readyToPublish("WW-1");  // captures as alice; its event names lab.sample
    // Reserve the late lot for another sample first, then the capture's sample: its reservation takes the early lot.
    lims::ReservationModel other;
    {
        ScopedPrincipal const bob{"bob"};
        static_cast<void>(other.execute(lims::ReserveForSample{.sample = lims::SampleId{*lab.sample + 1000},
                                                               .site = lims::test::seededSite(),
                                                               .analyses = {lims::AnalysisId{1}, lims::AnalysisId{2}}}));
    }
    static_cast<void>(lims::test::relayLabEvents());
    ScopedPrincipal const kim{"kim"};
    auto const stock = lims::StockModel{}.execute(lims::StockOnHand{.company = lims::test::seededCompany(), .item = {}});
    // Bob's sample holds 50 mL: the early lot's 40 mL could cover only one line, so his two lines are in the late lot.
    // The capture took its 25 mL from the early lot, which no other sample holds.
    for (auto const& lot : stock.lots) {
        if (lot.lotNumber == "L-EARLY") {
            CHECK(*lot.onHand == morph::math::Rational{morph::math::Numerator{15}, morph::math::Denominator{1000},
                                                       morph::math::DecimalPlaces{6}});
        }
    }
}

// Hypothesis 3. Mutation: remove the release from ~ReservationModel (bob's reservation is refused until the hour
// passes).
TEST_CASE("Hypothesis 3: a reservation lapses when its holder's connection closes",
          "[lims][inventory][reservation][h3]") {
    DbFixture fixture;
    lastLot();
    BackendRig rig{Mode::Socket, 0, lims::test::limsAuthorizer(kSecret)};
    auto const aliceAction = glz::write_json(reserve(101)).value_or(std::string{});
    auto const bobAction = glz::write_json(reserve(102)).value_or(std::string{});
    auto const alice = rig.connect();
    auto const aliceBench = alice->registerModel("ReservationModel", as("alice"));
    REQUIRE(alice->execute(aliceBench, "ReservationModel", "ReserveForSample", aliceAction, as("alice")).kind == "ok");

    auto const bob = rig.connect();
    REQUIRE(bob->watch({"Inventory_Reservation", "Inventory_Lot"}, as("bob")).kind == "ok");
    auto const bobBench = bob->registerModel("ReservationModel", as("bob"));
    auto const refused = bob->execute(bobBench, "ReservationModel", "ReserveForSample", bobAction, as("bob"));
    CHECK(refused.kind == "err");
    CHECK_THAT(refused.message, Catch::Matchers::ContainsSubstring("reserved by 'alice'"));
    auto const heardBefore = bob->changes().size();

    alice->close();
    // What bob's connection is told of the lapse itself, before bob acts: the lapse runs no action, so the expected
    // outcome is nothing. If this CHECK_FALSE fails, the lapse was told: record what carried it in decision 47.
    bool const toldOfLapse =
        ServerConnection::eventually([&] { return bob->changes().size() > heardBefore; }, std::chrono::milliseconds{1000});
    CHECK_FALSE(toldOfLapse);

    bool const lapsed = ServerConnection::eventually(
        [&] { return bob->execute(bobBench, "ReservationModel", "ReserveForSample", bobAction, as("bob")).kind == "ok"; });
    CHECK(lapsed);
    CHECK(holderOf(102) == "bob");
}

// Mutation: declare ReleaseReservation with no effect (bob's connection hears nothing when alice lets go).
TEST_CASE("reservation: a release reaches a second bench's connection", "[lims][inventory][reservation]") {
    DbFixture fixture;
    lastLot();
    BackendRig rig{Mode::Socket, 0, lims::test::limsAuthorizer(kSecret)};
    auto const alice = rig.connect();
    auto const bob = rig.connect();
    auto const aliceBench = alice->registerModel("ReservationModel", as("alice"));
    REQUIRE(alice->execute(aliceBench, "ReservationModel", "ReserveForSample",
                           glz::write_json(reserve(101)).value_or(std::string{}), as("alice"))
                .kind == "ok");
    REQUIRE(bob->watch({"Inventory_Reservation"}, as("bob")).kind == "ok");
    auto const before = bob->changes().size();
    REQUIRE(alice->execute(aliceBench, "ReservationModel", "ReleaseReservation", "{}", as("alice")).kind == "ok");
    CHECK(ServerConnection::eventually(
        [&] { return bob->changes().size() > before && bob->heardOf("Inventory_Reservation"); }));
    CHECK(holderOf(101).empty());
}
```


- [ ] **Step 2: Run it and see it fail**

Run: Build lims. Expected: compile error, `lims/inventory/models/reservation_model.hpp` not found.

- [ ] **Step 3: The table, availability, and the model**

`ReservationRecord` (`inventory_reservations`): `id`, `sample_id`, `company_id`, `site_id`, `lot_id`, `item_id`, `quantity_num`, `quantity_den` (stock unit), `holder` (`SqlAnsiString<64>`), `token` (`SqlAnsiString<32>`), `expires_at`.

```cpp
LIGHTWEIGHT_SQL_MIGRATION(20261120000008, "Create inventory_reservations") {
    plan.CreateTableIfNotExists("inventory_reservations")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("sample_id", Bigint())
        .RequiredColumn("company_id", Bigint())
        .RequiredColumn("site_id", Bigint())
        .RequiredColumn("lot_id", Bigint())
        .RequiredColumn("item_id", Bigint())
        .RequiredColumn("quantity_num", Bigint())
        .RequiredColumn("quantity_den", Bigint())
        .RequiredColumn("holder", Varchar(64))
        .RequiredColumn("token", Varchar(32))
        .RequiredColumn("expires_at", Bigint());
    plan.CreateIndex("idx_inventory_reservations_lot", "inventory_reservations", {"lot_id"});
    plan.CreateIndex("idx_inventory_reservations_sample", "inventory_reservations", {"sample_id"});
}
```

`availableOf(mapper, lot, exceptSample)`: the lot's stock on hand less the `ExactSum` of the quantities of the reservations on it that are unexpired (`expires_at > nowMillis()`) and not of `exceptSample` (0 for none); `OutOfRange` when a sum does not fit. `lotView`'s `reserved` becomes the sum of the live reservations on the lot.

`reservation_model.hpp`:

```cpp
/// @brief One bench's reservations for one sample: the lots its analyses will consume.
///
/// One private instance per bench (a screen registers its own). The instance names its reservation rows with a token
/// of its own, so a second instance of the same principal is another bench. Destroying the instance deletes its rows:
/// that is what happens when its screen unmounts, and when the server reclaims the instance because the connection
/// that registered it closed.
class ReservationModel {
public:
    ReservationModel();
    ~ReservationModel();
    ReservationModel(ReservationModel const&) = delete;
    ReservationModel& operator=(ReservationModel const&) = delete;
    ReservationModel(ReservationModel&&) = delete;
    ReservationModel& operator=(ReservationModel&&) = delete;

    /// @brief Reserves, for one sample, what each recipe line of its analyses will consume, replacing this bench's
    ///        earlier reservation; renews it for an hour.
    /// @param action The sample, its site and its analyses.
    /// @return The reservation.
    /// @throws ValidationError if the sample, the site or the analyses are missing.
    /// @throws NotFound if the site does not exist.
    /// @throws Conflict if another bench holds this sample's reservation, or no lot at the site has enough available
    ///         for a line, naming who holds the reservations that took it.
    /// @throws Forbidden if the caller is not an analyst at the site.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    ReservationView execute(const ReserveForSample& action);

    /// @brief Gives this bench's reservation up.
    /// @param action Carries nothing.
    /// @return The sample's reservation as it now is; its holder empty unless another bench took it since.
    ReservationView execute(const ReleaseReservation& action);

private:
    std::string _token;
    std::optional<std::int64_t> _sample;
};
```

registered as `"ReservationModel"` with `"ReserveForSample"` and `"ReleaseReservation"`, each `changes<ReservationView>` and `changes<LotStockView>` (a reservation moves a lot's reserved quantity, and a sample's row newly enters a list of reserved samples). `GetSampleReservation` on `StockModel` is a read.

`execute(const ReserveForSample&)` in `reservation_model.cpp`, in this order:

1. `requirePrincipal`; validate; `requireRoleSomewhere`; `OrgModel::findSite` (`NotFound`); `requireAuthorized`.
2. On a reader of its own: the site's company; another token's live reservation for this sample is a `Conflict` "sample N is reserved by '<holder>'"; the recipe lines of the analyses at the company, summed per item (`ExactSum`); for each item, its lots at the site with stock on hand, unexpired, by expiry then id, each with its `reservation_version` and its `availableOf(reader, lot, sample)`; the plan: for each item, the first lot whose available stock covers the item's need. An item with no such lot is a `Conflict` naming the holders of the live reservations on that item's lots at the site ("lot L-EARLY of NO3-R is reserved by 'alice'"; several holders joined with " and "), or "not enough NO3-R at site N" when no reservation is in the way.
3. One transaction, whose first writes are the planned lots' conditional updates, `reservation_version = v + 1 WHERE id = ? AND reservation_version = v` (`inventoryFaultPoint("reservation.beforeVersion", …)` before them, so a test can make a concurrent bench win): a lot whose update matches no row was reserved meanwhile; the transaction rolls back and the refusal re-reads who holds that lot, on its own mapper, and names them. After the versions, delete this token's earlier rows, insert the new rows (the quantities, the holder, the token, `expires_at = now + 60 min`), commit, remember the sample.
4. A `Lightweight::SqlException` during step 3 is turned into a `Conflict` naming the lot's holders when the lot's `reservation_version` is no longer the one read (re-read on a mapper of its own) and surfaces as itself otherwise — `invoice_edit_model.cpp`'s `isBusy` and `leaseMovedOn`, written for the lot's version. Copy the two functions into `reservation_model.cpp`'s anonymous namespace, adapted; Step 6 counts them as the copy they are.

The destructor is `~InvoiceEditModel`'s: nothing to do without a sample; otherwise delete this token's rows on a fresh mapper inside `try`, logging a failure with `morph::log::logError`. `ReleaseReservation` deletes this token's rows and answers what is left for the sample (`GetSampleReservation`'s view).

- [ ] **Step 4: Consumption takes the sample's reservation first**

In `writeConsumption`, before the first-expiring order: the live reservation rows of `action.sample` for the recipe's item, oldest first; each is consumed from its lot (up to its quantity and the line's remaining need) and deleted in the same transaction. Then the first-expiring order continues for what is left, each lot's take capped at `availableOf(mapper, lot, sample)` — never into another sample's live reservation. The movement's `source` stays the event id.

- [ ] **Step 5: Declared authorization**

`Authorization<ReserveForSample>`: `LimsRole::Analyst`, `scopeOf` the payload's site. `ReleaseReservation` declares no role: it releases only the instance's own rows; add it to `openMutatingActions()` (size raised by one) with that reason added to the function's doc comment, beside the invoice editor's edits. Both to the coverage case.

- [ ] **Step 6: Build, run, and count the copy**

Run: Build lims, Model suite `"[reservation]"`, `"[events]"`, `"[authorization]"`, Everything labelled for the rung, the GCC shadow sweep. Expected (the h3 lapse inferred from decision 37's measurement through the same hook; the race from the version write): all pass. If `CHECK(lapsed)` fails, the destructor did not run on close or ran without a connection: the hypothesis is confirmed for this instance — change it to `CHECK_FALSE(lapsed)`, assert bob reserves only after the hour, and use the confirmed text in Step 7. If `CHECK_FALSE(toldOfLapse)` fails, the lapse was told: change it to `CHECK(toldOfLapse)` and read from the `changes` body what carried it. Run the mutations named in the file once each and restore them.

Count what the second lease copied from the first, for Step 7 and Task 8: the token generator, the destructor's release, `isBusy`, the moved-on check, and the "an expired row is free" rule, in lines, in `reservation_model.cpp` beside `invoice_edit_model.cpp`:

```bash
cd /home/yaraslau/repo/morph
grep -n 'newToken\|isBusy\|MovedOn\|~ReservationModel\|~InvoiceEditModel\|expires_at\|expiresAt' \
    examples/lims/inventory/src/models/reservation_model.cpp examples/lims/billing/src/models/invoice_edit_model.cpp
```

- [ ] **Step 7: Record decision 47 and commit**

```markdown
### 47. A sample's lots are reserved by the bench that will use them, and the reservation lapses with the bench's instance (hypothesis 3, second instance)

`ReservationModel` is one private instance per bench. `ReserveForSample` reserves, for one sample, what each recipe
line of its analyses will consume: per item, the first-expiring lot at the sample's site whose available stock (on
hand less every other sample's live reservation) covers the need. The rows carry the holder, the instance's token and
an expiry an hour after the last reservation; a lapsed row counts for nothing, so the hour needs no timer. Contention
for a lot is ordered by its `reservation_version`, the first write of each reservation's transaction and conditional
on the version read: two benches racing for the last lot give one reservation and a `Conflict` naming the holder —
measured over ten rounds of two threads with both benches alive, with no other failure. A capture consumes its
sample's reservation first and never another sample's. Inventory cannot check that a reserved sample exists: no module
reads another's tables, and the bench's role at the named site is what authorizes the reservation.

**Hypothesis 3 is [refuted | confirmed] for its second instance, [through the same hook].** Measured in
`tests/test_reservation.cpp` over a real `RemoteServer`: closing the holder's connection [frees the reservation, and
the second bench takes the lot without waiting for the hour]. The hook is again the instance's destructor, with again
its costs (decision 37): database work on whatever thread drops the last reference, a failure that can only be
logged, nothing released by a crash. And again the lapse is not told: a bench watching `Inventory_Reservation` and
`Inventory_Lot` hears of a release but not of a lapse on disconnect, measured on the same connection. The second lease
copied the first's mechanism: [n] lines — the instance token, the release in the destructor, the busy and moved-on
checks, the expiry rule — of which none is about stock. That is the same hand-rolled guarantee in a second module: a
lease tied to a connection, released on close, and told to watchers as a change.
```

Fill the brackets from Step 6 (if confirmed, "Hypothesis 3 is confirmed for its second instance. … the reservation lapsed only at its hour."). Then:

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/inventory examples/lims/app/include/lims/app/declared_requirements.hpp examples/lims/tests \
        examples/lims/README.md
git commit -m "Reserve lims lots per sample at the bench, measured against the race for the last lot and the lapse

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---
### Task 7: Does a company-B stock change reach a company-A connection? (hypothesis 7, second instance)

**Waits for Part B Task 16** (decision 40, the first instance over invoices). Read `tests/test_two_company_effects.cpp` and decision 40 first: this case is the same measurement over stock, and its verdict is compared with that one.

**Files:**
- Create: `examples/lims/tests/test_two_company_stock.cpp`
- Modify: `examples/lims/README.md` (decision 48)

**Interfaces:**
- Consumes: `BackendRig::connect`, `ServerConnection`, `StockModel`, `OrgModel`, `RecordOpeningLot`, `stockSetUp`, `openingLot`.
- Produces: a measured verdict on hypothesis 7 for stock.

- [ ] **Step 1: Write the test**

Create `examples/lims/tests/test_two_company_stock.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Hypothesis 7, second instance: two companies' stock keepers watch lots and movements on one server; a company-B lot
// is recorded. Does the company-A connection hear of it, and what does it learn?

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <morph/session/session_auth.hpp>
#include <string>
#include <string_view>
#include <utility>

#include "inventory_test_support.hpp"
#include "lims/inventory/models/stock_model.hpp"
#include "lims/org/models/org_model.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::at;
using lims::test::litres;
using lims::test::ScopedPrincipal;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::ServerConnection;

namespace {

constexpr std::string_view kSecret = "lims-two-company-stock-secret-at-least-32";

/// @brief A session for @p principal, signed with the case's secret.
/// @param principal The principal.
/// @return The session.
morph::session::Context as(std::string const& principal) {
    morph::session::TokenIssuer const issuer{std::string{kSecret}, morph::session::hmacSha256};
    return lims::test::signedAs(issuer, principal);
}

/// @brief The Osaka company (JPY) with one site, a reagent stocked in litres and a cold room, set up by sam; `kenji`
///        its stock keeper.
/// @return The reagent and the room.
lims::test::StockSetUp osaka() {
    ScopedPrincipal const sam{"sam"};
    lims::OrgModel org;
    auto const company = org.execute(lims::CreateCompany{
        .name = "Rheinlab Japan", .baseCurrency = lims::CurrencyCode::JPY, .fiscalYearStart = lims::Month::April});
    auto const site = org.execute(lims::CreateSite{.companyId = company.id, .name = "Osaka"});
    for (auto const* principal : {"kenji", "sam"}) {
        static_cast<void>(org.execute(lims::AssignRole{
            .principal = principal, .role = lims::LimsRole::StockKeeper, .company = company.id, .site = {}}));
    }
    lims::StockModel stock;
    auto const item = stock.execute(
        lims::CreateItem{.company = company.id, .code = "NO3-R", .name = "Nitrate reagent", .unit = lims::StockUnit::litre});
    auto const room = stock.execute(lims::CreateLocation{.site = site.id, .name = "Cold room"});
    return lims::test::StockSetUp{.reagent = item.id, .coldRoom = room.id};
}

/// @brief Records an opening lot of @p item at @p room through @p connection, as @p principal: dispatched to the
///        server, so the server derives and fans out its effects, as a client's call is.
/// @param connection The connection.
/// @param principal Who records it.
/// @param item The item.
/// @param room The location.
/// @param lotNumber The lot number.
/// @return The reply's kind.
std::string recordThrough(ServerConnection& connection, std::string const& principal, lims::ItemId item,
                          lims::LocationId room, std::string lotNumber) {
    auto const model = connection.registerModel("StockModel", as(principal));
    lims::RecordOpeningLot const action{
        .item = item,
        .location = room,
        .lotNumber = std::move(lotNumber),
        .expiresAt = morph::time::Timestamp{at(2027, 3, 31)},
        .quantity = litres(1000),
        .unit = lims::StockUnit::litre,
        .unitCost = lims::Amount{morph::math::Rational{1500, morph::math::DecimalPlaces{0}}}};
    return connection.execute(model, "StockModel", "RecordOpeningLot", glz::write_json(action).value_or(""),
                              as(principal))
        .kind;
}

}  // namespace

// Hypothesis 7. The control: kim's connection hears a lot recorded at her own company, so the check can see a change.
TEST_CASE("Hypothesis 7: a company-B lot and what a company-A connection hears of it", "[lims][inventory][h7]") {
    DbFixture fixture;
    auto const rhein = lims::test::stockSetUp();
    auto const japan = osaka();
    BackendRig rig{Mode::Socket, 0, lims::test::limsAuthorizer(kSecret)};
    auto const kim = rig.connect();
    auto const kenji = rig.connect();
    auto const keeper = rig.connect();
    REQUIRE(kim->watch({"Inventory_Lot", "Inventory_Movement"}, as("kim")).kind == "ok");

    auto const before = kim->changes().size();
    REQUIRE(recordThrough(*kenji, "kenji", japan.reagent, japan.coldRoom, "OS-1") == "ok");
    bool const heardOfB =
        ServerConnection::eventually([&] { return kim->changes().size() > before; }, std::chrono::milliseconds{1000});
    INFO("company-A connection heard of the company-B lot: " << heardOfB);
    if (heardOfB) {
        auto const bodies = kim->changes();
        for (auto const& change : bodies.back().records) {
            UNSCOPED_INFO("it carried record " << change.record);
        }
    }
    // The expected outcome, a leak. If decision 40 measured none for invoices, expect none here: CHECK_FALSE.
    CHECK(heardOfB);

    auto const beforeOwn = kim->changes().size();
    REQUIRE(recordThrough(*keeper, "kim", rhein.reagent, rhein.coldRoom, "RH-1") == "ok");
    CHECK(ServerConnection::eventually([&] { return kim->changes().size() > beforeOwn; }));
}
```

Add `#include <glaze/glaze.hpp>` and `#include <morph/util/rational.hpp>`. While the case runs, read what the `INFO` lines print for the company-B change: whether the leak names the lot's key (the existence of a lot in another company) or only that `Inventory_Lot` changed. The field names of a changes body (`records`, `record`) are `ServerConnection::heardOf`'s; use what that function reads.

- [ ] **Step 2: Run it**

Run: Build lims, Model suite `"[h7]"` (both instances). Expected: the stock case behaves as decision 40's invoice case did. If it does not, the two instances disagree: run each once more, read what each body carries, and record both in Step 3 rather than choosing one.

- [ ] **Step 3: Record decision 48 and commit**

```markdown
### 48. A stock change in one company [reaches | does not reach] another company's watching stock keeper (hypothesis 7, second instance)

**Hypothesis 7 is [confirmed | refuted] for its second instance.** Measured in `tests/test_two_company_stock.cpp`
over a real `RemoteServer`, with company A's stock keeper watching `Inventory_Lot` and `Inventory_Movement`: a lot
recorded by company B's stock keeper [is heard on company A's connection, as the record name and the lot's key / is not
heard]; a lot recorded at company A is heard (the control). The fan-out re-authorizes each watcher at the model and
action level, which a stock keeper of either company passes, and does not ask whether the record is in a scope the
watcher can see. Invoices (decision 40) measured the same; two modules now need row-level scoping of the fan-out.
```

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/tests/test_two_company_stock.cpp examples/lims/README.md
git commit -m "Measure whether a lims stock change in one company reaches another company's connection

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 8: The seams both instances call for — decide, specify, review, and plan each

**Waits for** Tasks 2–7 and Part B Tasks 14–17 (decisions 38–41).

**Files:**
- Create (each only if Step 1 selects it): `docs/spec/core/cross_model_work.md` (hypothesis 1), `docs/spec/journal/consumer_dedup.md` (hypothesis 2), `docs/spec/core/connection_leases.md` (hypothesis 3), `docs/spec/core/scoped_fanout.md` (hypothesis 7), `docs/spec/util/runtime_units.md` (hypothesis 8); one implementation plan per spec, `docs/superpowers/plans/<date>-morph-<seam>.md`
- Modify: `docs/spec/README.md` (the index), the spec each new one names as its neighbour, `examples/lims/README.md` (decision 49)

**Interfaces:**
- Consumes: decisions 32–48 and their measured numbers; `include/morph/core/remote.hpp`, `include/morph/core/registry.hpp`, `include/morph/core/bridge.hpp`, `include/morph/core/backend.hpp`, `include/morph/journal/outbox.hpp`, `include/morph/offline/replay_ledger.hpp`, `include/morph/core/shared_instances.hpp` (or wherever `docs/spec/core/shared_instances.md` points), `include/morph/util/quantity.hpp`, `include/morph/forms/forms.hpp`.
- Produces: for each hypothesis whose two instances needed the same hand-rolled guarantee, a reviewed spec under `docs/spec/` and its implementation plan; for each that did not, its disposition in decision 49.

The design's rule: "Each confirmed hypothesis is designed (a spec under `docs/spec/`, one independent review) and implemented in `include/morph` in the same branch, before the part holding its second instance is called done." Each seam is a framework change across the dispatch path, the fan-out or the unit type, each the size of Part B's action-guard tasks or larger, so each gets a plan of its own, written once its spec is reviewed, and Task 9 does not call Part C done until those plans are executed. The domain stays in lims: a seam runs a mechanism and knows no account, lot, invoice, currency or role.

- [ ] **Step 1: Decide from what was measured**

For each row, read the two decisions and apply the condition. Write the outcome, with the measured numbers it rests on, to `/tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/plan_c_seams.txt`.

| Hypothesis | Instances | A seam is specified when | Otherwise |
|---|---|---|---|
| 1 | decisions 38, 45 | both confirmed: a shared transaction gives up the second model's authorization, effects or journal, and dispatch leaves a window | refuted or mis-framed: decision 49 says what carried the atomic step |
| 2 | decisions 32, 46 | the second consumer copied the lookup-before-write and the race catch (or needed a loop or cursor of its own) | the second consumer needed only wiring and its own row: no seam |
| 3 | decisions 37, 47 | the second lease copied the first's token, destructor release and busy/moved-on checks, or either instance's lapse was untold where a watcher waited | refuted with no copy: no seam |
| 7 | decisions 40, 48 | both confirmed: a company-B change reached a company-A connection | refuted: no seam |
| 8 | decisions 33, 42, 44 | confirmed for invoices and orders: an amount's unit or currency is a row's fact the type cannot carry, checked by hand in each place | refuted: what served the record's currency |

The deadline effect Task 2 filed (an open inbox not told when a deadline passes) is Part D's to specify and build: its consumer is the approvals inbox, and Part D's plan holds that task.

- [ ] **Step 2: Write each selected spec**

Each spec states, in present tense and without history (`AGENTS.md`), with complete reasoning and no size limit (`AGENTS.md`, "Design specs"):

- **What it is**, in morph's own terms, and the guarantee it gives; the public API with every type and function named; where it runs on each dispatch path (the server's `ActionDispatcher`, `Bridge`'s local paths) and on which strand or thread; what it journals and what it fans out.
- **Its two consumers**, by file: the lims code each replaces, and the lines that go.
- **What it does not do**, and the domain it does not know.
- **Alternatives considered**, each with why not, including "leave it in each module" with the measured cost of the copy.
- **How it is tested** in `tests/` (framework) and how lims's two instances prove it by deleting their hand-rolled code with every lims test still passing.

What each must decide, at least:

- *Cross-model work (hypothesis 1).* How two models' actions — billing's `PostInvoice` and the ledger's `RecordPosting`; inventory's `ReceiveGoods` and the same — commit as one fact while each keeps its own authorization, declared effects and journal entry; that `IMPLEMENTATION.md` rule 4's one database is what makes it possible, and what a deployment with two databases gets (a refusal at registration, not a silent second commit); how it orders against each model's gate (`docs/spec/core/coroutines.md`, "the action gate") without deadlock; what a failure between the halves leaves (nothing); how gap-free numbering is expressed with it (the number in the same unit). Read `docs/spec/core/backend.md` and `docs/spec/core/coroutines.md` first.
- *Consumer dedup (hypothesis 2).* A consume-once primitive over a consumer's own table: the lookup outside the write's transaction (the rollback-journal lesson), the record inside it, the race settled by a unique index and answered with the winner's result, generic over the consumer's result; where it sits beside `IReplayLedger` and `OutboxRelay`; and whether the routing sink (`LabEventRouter`) belongs with it (an outbox with several event types and several consumers is a morph shape, not a lims one).
- *Connection leases (hypothesis 3).* A lease a model takes for a connection: released when the connection closes as an action the server dispatches on the model's strand (so its effects fan out and the lapse is told), not as database work in a destructor on the server strand; the timeout as a fact the lease's reader evaluates, as both instances do now; what a crash releases (the timeout only).
- *Scoped fan-out (hypothesis 7).* A record's scope declared beside it (`BRIDGE_REGISTER_RECORD`) and asked of each watcher before a change is sent, so a change in a scope the watcher cannot see reaches no connection; what "a scope the watcher can see" is as a callback the application gives (lims answers from `OrgModel::visibleCompanies`), never a role or a company in morph; its cost per change and per watcher.
- *Runtime units (hypothesis 8).* An amount whose unit (a currency, a stock unit) is a value known at run time: how it is carried on the wire and in a form (`morph::forms::InstanceConstraints` already serves per-instance decimals: read it first and say what it lacks), how two amounts of different units refuse to combine, how a dated conversion fits (`UnitTraits::convert` takes no date), and what `Quantity`'s compile-time unit keeps doing.

Add each spec to `docs/spec/README.md`'s index, and one paragraph to the neighbouring spec it extends (`core/remote.md` for scoped fan-out; `core/backend.md` for cross-model work; `journal/journal.md` for consumer dedup; `core/shared_instances.md` for leases; `util/quantity_type.md` for runtime units) pointing to it.

- [ ] **Step 3: One independent review per spec**

For each spec, dispatch one review subagent on the default model (never Fable), giving it the spec, the `include/morph` headers it changes, and the two lims instances' code and README decisions — the code and the evidence, not this plan's conclusion. Ask it whether the seam is mechanism (would bookmarks, bank or a CRM need it unchanged), whether its two consumers are covered without lims-specific knowledge, which dispatch path it misses, what it leaves undecided, and whether the measured cost justifies it. Address every finding in the spec. If a review shows the guarantee is domain logic that morph cannot run without knowing the domain, drop the spec and record the reasoning in decision 49 instead.

- [ ] **Step 4: Commit the reviewed specs**

```bash
cd /home/yaraslau/repo/morph
git add docs/spec
git commit -m "Specify the morph seams lims's ERP modules needed twice, each reviewed

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

- [ ] **Step 5: Write each seam's implementation plan**

For each reviewed spec, use superpowers:writing-plans to write `docs/superpowers/plans/<today>-morph-<seam>.md`: the framework change with its tests in `tests/`, then one task per lims instance moving onto it and deleting the hand-rolled code (each lims instance's measuring test stays and must still pass, now asserting the refuted outcome where the seam refutes the hypothesis), then the README decision's update. Follow this plan's Global Constraints. Commit the plans:

```bash
cd /home/yaraslau/repo/morph
git add docs/superpowers/plans
git commit -m "Plan the morph seams lims's ERP modules needed twice

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

- [ ] **Step 6: Record decision 49 and commit**

```markdown
### 49. What morph gains from Part C's second instances

| Hypothesis | Instances | Outcome | morph |
|---|---|---|---|
| 1 | 38, 45 | [confirmed / refuted / mis-framed] | [spec `docs/spec/core/cross_model_work.md` / none, because …] |
| 2 | 32, 46 | [as measured] | [spec `docs/spec/journal/consumer_dedup.md` / none, because …] |
| 3 | 37, 47 | [as measured] | [spec `docs/spec/core/connection_leases.md` / none, because …] |
| 5 | 35, 43 | refuted twice | none: two lifecycles shared only the declared rule |
| 7 | 40, 48 | [as measured] | [spec `docs/spec/core/scoped_fanout.md` / none, because …] |
| 8 | 33, 42, 44 | [as measured] | [spec `docs/spec/util/runtime_units.md` / none, because …] |

Each spec was reviewed once, independently, before its implementation; each is built and each lims instance moved onto
it before Part C is called done. [The reasoning for every "none".]
```

Fill each bracket from Step 1 and the reviews; then:

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/README.md
git commit -m "Record what morph gains from lims's second instances

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

### Task 9: Part C's definition of done, measured

**Waits for** every task above and the execution of every plan Task 8 wrote.

**Files:**
- Modify: `examples/lims/README.md` (Status, "What to implement", Modules), `codecov.yml` (the lims component's inventory paths), `examples/LADDER.md` (lims's size, if it states one)

**Interfaces:**
- Consumes: everything above. Produces: nothing new in code.

- [ ] **Step 1: Full rung run and the boundary**

```
flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --preset linux-everything
flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock cmake --build build/linux-everything --target ladder_lims_tests ladder_lims_screen_tests ladder_lims_render_smoke ladder_lims_server morph_examples_client -j10
flock /tmp/claude-1000/-home-yaraslau-repo-morph/b1b73e49-c41d-4e27-bb68-b8c0cfacf145/scratchpad/morph-build.lock ctest --test-dir build/linux-everything -j6 -L ladder-lims --output-on-failure
bash scripts/check_ctest_name_collisions.sh build/linux-everything
```

Expected: `100% tests passed`, with the five inventory `lims.module_boundary.*` tests among them (`inventory_includes_billing` is the design's named control); no duplicate test names.

- [ ] **Step 2: Every Part C hypothesis has a recorded outcome**

Run `grep -n '^### \(4[2-9]\)\.' examples/lims/README.md` and `grep -n 'Hypothesis [0-9] is ' examples/lims/README.md`. Expected: decisions 42–49, an outcome sentence for hypotheses 1 (45), 2 (46), 3 (47), 5 (43), 7 (48) and 8 (42, 44), and decision 49's table with no bracket left. Every `[n]` in decisions 42–49 is a measured number.

- [ ] **Step 3: Every module follows the rules**

```
cd /home/yaraslau/repo/morph
grep -rn 'sqlite3_\|"SELECT \|"INSERT \|"UPDATE \|"DELETE ' examples/lims/inventory/
grep -rnE '^\s+(int|bool|double|float|std::int64_t|std::uint64_t) [a-z][A-Za-z]*( =|\{|;)' examples/lims/inventory/include/lims/inventory/dto/
grep -rn 'ScopedContext' examples/lims --include='*.cpp' --include='*.hpp' | grep -v '/tests/'
grep -rn 'operator+\|operator\*\| + \*\| \* \*' examples/lims/inventory/src | grep -i 'rational\|amount\|measure' || true
```

Expected: no raw SQL in inventory's sources (the one raw `DROP INDEX` is org's migration, with its finding); no plain numeric or `bool` DTO field; no session installed by hand outside the tests; no unchecked arithmetic on money or stock (read each hit of the last search: a hit is acceptable only when it is not a `Rational`).

- [ ] **Step 4: Size**

```
cd /home/yaraslau/repo/morph
grep -rhn 'BRIDGE_REGISTER_MODEL(lims::' examples/lims/*/include | wc -l
grep -rhn 'BRIDGE_REGISTER_ACTION(lims::' examples/lims/*/include | wc -l
grep -c 'ui::screen(' examples/lims/screens/*.cpp
```

Record the three numbers in the README's Status paragraph beside the design's estimate (~16 models, ~115 actions after all four parts).

- [ ] **Step 5: Model coverage**

Follow `coverage-local-repro.md` (`scripts/coverage.sh` over the `clang-coverage` preset, every build in the lock at `-j10`) and read the `lims` component's covered and total for `examples/lims/inventory/src/models/**` and `examples/lims/inventory/include/lims/inventory/models/**`; add both paths to `codecov.yml`'s `lims` component. Expected: at or above decision 20's ceiling; every uncovered line tested now or listed, in decision 20's style, with why it cannot be reached.

- [ ] **Step 6: Update the README and commit**

"What to implement" names `StockModel`, `ProcurementModel` and `ReservationModel` in inventory; the Modules table's `app` row adds "the event router"; the Status paragraph adds: "Part C of the ERP design is built: inventory (items, units, locations, lots, movements, recipes, reservations) and procurement (requisitions with approval, suppliers, purchase orders, goods receipts posted to the ledger), with lab's captures consumed through the relay (decisions 42–49). Models [n], actions [n], screens [n]."

```bash
cd /home/yaraslau/repo/morph
git add examples/lims/README.md codecov.yml examples/LADDER.md
git commit -m "Record lims ERP Part C as built, with its measured coverage and size

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01No1NznqxEBvjhZBaBRLCd6"
```

---

## What Part C hands to Part D

Not tasks; what the Part D plan starts from.

- **The inventory screens' models:** `StockModel` (stock on hand with reserved quantities, a lot's movements as a `table::TableQuery` page — hypothesis 10's second instance, which Part D measures), `ProcurementModel` (requisitions with their approval route and deadline, suppliers, purchase orders, goods receipts) and `ReservationModel` (the sample detail's reservations, with the refusal naming the holder).
- **The goods-receipt dialog's action** is one inventory action (`ReceiveGoods`) over one model; its posting preview is the ledger's, read through `GetRateOn` and the receipt's own lines. Whether the dialog needs ui/1 to span several models is Part D's measurement of hypothesis 9.
- **The approvals inbox across modules** lists invoices (`ListPendingApprovals`) and requisitions (`ListPendingRequisitions`); a deadline passing is not told to an open inbox in either (decisions 35, 43; filed), which Part D's deadline-effects task builds.
- **The lapse of a reservation and of an invoice lease** is not told to a waiting bench or clerk (decisions 37, 47) unless Task 8's connection-lease seam was specified and built.
- **A yen order's form** advertises two decimals (decision 44), as a yen price's does (decision 33), unless Task 8's runtime-units seam was built.

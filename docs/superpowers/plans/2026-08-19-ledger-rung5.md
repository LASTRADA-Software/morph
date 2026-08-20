# Ledger Rung 5 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build ledger's full backend-plus-GUI stack: `LedgerModel` (accounts,
double-entry transactions, undo, CSV import), `BudgetModel`, `RuleModel`
(rule-driven cascades), the report submit→poll job idiom, and the presenter/
QML layer driving all three — everything the design spec's steps 1–7 call
for, in one plan, including GUI (unlike rung 4's split into a backend plan
plus a follow-on GUI plan).

**Architecture:** Three keyed, shared-instance models over SQLite via
Lightweight, following `polls::PollModel`/`kanban::BoardModel`'s established
shape. Money is `morph::units::Quantity<Currency, dp>` wrapping
`morph::math::Rational` throughout — never `bank::Money`'s integer-minor-unit
style. The double-entry invariant (per-currency zero-sum) is enforced inside
`LedgerModel::execute(StoreTransaction)`, never in SQL. Rule cascades reuse
kanban's causal-parent-id journaling pattern (already cherry-picked onto this
branch). Reports run as background jobs against a pinned SQLite WAL read
snapshot, exposed through a new submit→poll idiom this rung introduces.

**Tech Stack:** C++23, Lightweight ORM (SQLite), Catch2, Qt6 Core/Quick/
WebSockets, morph core (`Bridge`, `RemoteServer`, `IActionLog`, offline
stack, `morph::units`/`morph::math::Rational`).

**Spec:** `docs/superpowers/specs/2026-08-19-ledger-rung5-design.md` (read
this first — this plan implements its decisions verbatim; where this plan
and the spec seem to disagree, the spec is authoritative and this plan has a
bug).

## Global Constraints

- C++23 throughout (`CMakeLists.txt`'s `CMAKE_CXX_STANDARD 23`).
- Persistence exclusively through the Lightweight ORM — the one named
  exception is the reports job's WAL-read-transaction snapshot (spec §9,
  `IMPLEMENTATION.md`'s pre-cleared escape tier), invoked from inside
  `LedgerModel`, never a parallel helper layer.
- Every DTO field validated in a `validate() const noexcept` method; models
  never trust unvalidated input.
- No plain `int`/`int64_t`/`double`/`float`/`bool`/raw enum in any DTO field
  (`IMPLEMENTATION.md` rule 3) — money is always `Quantity<Currency, dp>`,
  identities are strong id types, closed sets are `enum class`.
- Every bounded string entity column is `Light::SqlAnsiString<N>` matching
  its DTO-level `kMax*Bytes` constant, pinned by a `static_assert`.
- Zero `HasMany`/`HasManyThrough` relation fields on any entity (mirrors
  kanban/polls — `DataMapper::Update()` cannot handle them).
- The per-currency zero-sum invariant (spec §1) is checked inside
  `LedgerModel::execute(StoreTransaction)` on every partition, before commit;
  it never rounds, never auto-balances, and rejects with a typed
  `ZeroSumViolation` on any failing partition.
- `causalParentId` on a cascaded `LogEntry` is never `LogEntry::seq` —
  always an app-minted stable identity (spec §5, §4).
- Undo is a compensating action (`UndoTransaction`), never
  `morph::journal::undoLast()` (spec §6).
- Every model is unit tested to the measured-ceiling coverage gate
  (`IMPLEMENTATION.md` rule 5); dual-mode (`Local`/`LocalSingleThread`/
  `Socket`) via `BackendRig` wherever a test body is mode-generic.
- Commit after every passing test (TDD: red → green → commit).
- Zero styling effort on every QML view (`IMPLEMENTATION.md` rule 2):
  default Qt Quick controls, schema-driven forms via `morph::forms`
  wherever the interaction fits that palette.

## Task 0: Framework/testkit dependencies (already done, before Task 1)

This plan's branch (`ladder-ledger-rung5`, cut from `master`) already
carries four cherry-picked commits from the unmerged `ladder-kanban-impl`
branch, each verified clean of kanban app-code entanglement and each
building + passing its own tests on this branch before this plan's Task 1
starts:

1. `journal: add causalParentId + isReplaying() to LogEntry/replay()`
   (design spec §5) — `include/morph/journal/{action_log,journal}.hpp`,
   `docs/spec/journal/journal.md`, `tests/test_action_log.cpp`. Required
   by Task 12 (rule cascades).
2. `testkit: action_driver.hpp -- SeededScript weighted generator + burst
   invariant hook` — `examples/common/testkit/action_driver.hpp` +
   its own test. Required by Task 23 (multi-client stress).
3. `testkit: offline_rig.hpp -- scripted connectivity drop/revive` —
   `examples/common/testkit/offline_rig.hpp` + its own test. Required by
   Task 17 (offline-stack integration test) and Task 24 (sync-benchmark
   Scenarios A/B).
4. `testkit: client_pool.hpp + convergence.hpp -- N-client convergence
   assertion` — `examples/common/testkit/{client_pool,convergence}.hpp` +
   their own tests. Required by Task 23.

No further action needed for these four — they are already on the branch.
When PR #121 merges, all four become no-ops on rebase (identical patch-id
match against `master`'s own copies), per design spec §5's stated
resolution and Task 26 below.

---

## File Structure

```
examples/ledger/
├── CMakeLists.txt                                       (Task 1)
├── include/ledger/
│   ├── core/
│   │   ├── types.hpp                                     (Task 2 — strong ids, enums)
│   │   ├── errors.hpp                                     (Task 2 — typed exception hierarchy)
│   │   └── units.hpp                                       (Task 3 — Currency unit system)
│   ├── db/
│   │   ├── database.hpp                                     (Task 4 — setup() declaration)
│   │   └── ledger_entity.hpp                                 (Task 5 — all entities)
│   ├── dto/
│   │   ├── account_dto.hpp                                    (Task 6 — OpenAccount, GetLedger)
│   │   ├── transaction_dto.hpp                                  (Task 7 — StoreTransaction, UndoTransaction)
│   │   ├── budget_dto.hpp                                        (Task 10 — Budget CRUD, GetBudgetReport)
│   │   ├── rule_dto.hpp                                           (Task 12 — Rule CRUD)
│   │   ├── import_dto.hpp                                         (Task 15 — ImportLedgerChunk)
│   │   └── report_dto.hpp                                         (Task 16 — SubmitReport/GetReportStatus)
│   └── models/
│       ├── ledger_model.hpp                                        (Tasks 7, 8, 9, 12, 14, 15, 16)
│       ├── budget_model.hpp                                         (Task 10)
│       └── rule_model.hpp                                            (Task 12)
├── src/
│   ├── db/schema.cpp                                                 (Task 4 — migration)
│   └── models/
│       ├── ledger_model.cpp                                          (Tasks 7-9, 12, 14-16)
│       ├── budget_model.cpp                                          (Task 10)
│       └── rule_model.cpp                                            (Task 12)
├── gui_lib/
│   ├── ledger_presenter.hpp / .cpp                                    (Task 18)
│   ├── ledger_qml_bridge.hpp / .cpp                                    (Task 18)
│   ├── budget_presenter.hpp / .cpp                                     (Task 19)
│   ├── budget_qml_bridge.hpp / .cpp                                    (Task 19)
│   ├── rule_presenter.hpp / .cpp                                       (Task 20)
│   ├── rule_qml_bridge.hpp / .cpp                                      (Task 20)
│   ├── report_job_poller.hpp / .cpp                                    (Task 21 — submit->poll idiom)
│   ├── report_presenter.hpp / .cpp                                     (Task 21)
│   └── report_qml_bridge.hpp / .cpp                                    (Task 21)
├── gui/
│   ├── main.cpp                                                        (Task 22)
│   └── qml/
│       ├── Main.qml                                                     (Task 22)
│       ├── LedgerView.qml                                               (Task 22)
│       ├── BudgetView.qml                                               (Task 22)
│       ├── RulesView.qml                                                (Task 22)
│       └── ReportView.qml                                               (Task 22)
└── tests/
    ├── test_ledger_types.cpp                                            (Task 2)
    ├── test_ledger_units.cpp                                            (Task 3)
    ├── test_ledger_schema.cpp                                           (Task 5)
    ├── test_account_dto.cpp                                             (Task 6)
    ├── test_ledger_model.cpp                                            (Tasks 7-9, 11, 14, 15)
    ├── test_budget_model.cpp                                            (Task 10)
    ├── test_rule_model.cpp                                              (Task 12)
    ├── test_ledger_offline.cpp                                          (Task 17)
    ├── test_ledger_presenter.cpp / test_ledger_qml_bridge.cpp            (Task 18)
    ├── test_budget_presenter.cpp / test_budget_qml_bridge.cpp            (Task 19)
    ├── test_rule_presenter.cpp / test_rule_qml_bridge.cpp                (Task 20)
    ├── test_report_job_poller.cpp / test_report_presenter.cpp            (Task 21)
    └── test_multiclient.cpp [stress]                                     (Task 23)

tests/
└── test_ledger_rational_fuzz.cpp                          (Task 13 — framework-level, per spec §7)

examples/ledger/
└── SYNC-BENCHMARK.md                                      (Task 24 — spec §10 written deliverable)
```

---

## Task 1: Rung scaffolding

**Files:**
- Create: `examples/ledger/CMakeLists.txt`
- Modify: `examples/CMakeLists.txt`

**Interfaces:**
- Produces: a buildable, empty `ladder_ledger_lib`/`ladder_ledger_tests`
  target pair, so Task 2 onward can add files incrementally and build after
  each one.

- [ ] **Step 1: Copy kanban's CMakeLists.txt as the starting point**

If `examples/kanban/CMakeLists.txt` exists in this checkout (it may not,
since rung 4 is on a separate unmerged branch), use it; otherwise copy
`examples/polls/CMakeLists.txt`. Either source has the same
`morph_add_rung()` shape.

```bash
cp examples/polls/CMakeLists.txt examples/ledger/CMakeLists.txt
```

- [ ] **Step 2: Edit `examples/ledger/CMakeLists.txt`, replacing every `polls`/`Polls`/`POLLS` token with `ledger`/`Ledger`/`LEDGER`**

Keep the same target shape: `ladder_ledger_lib` (models/db/dto),
`ladder_ledger_gui_lib` (presenters/bridges, Qt6::Core only, no
Qt6::WebSockets — per `TESTING.md`'s presenter rule 1),
`ladder_ledger_server` (headless server binary, copy
`examples/polls/src/server/main.cpp` verbatim, swap namespaces),
`ladder_ledger_gui` (desktop client, wired in Task 22),
`ladder_ledger_tests`. Comment out or omit `gui`/`gui_wasm` target blocks
until Task 22 — this task only needs `ladder_ledger_lib` and
`ladder_ledger_tests` to build.

- [ ] **Step 3: Register the rung in `examples/CMakeLists.txt`**

Find the line adding `polls` (or `kanban`, if present) as a rung and add an
identical line for `ledger` immediately after it, also adding `ledger` to
the `MORPH_LADDER_RUNGS` cache list's default/`all` handling per
`TESTING.md`'s "Build system and CI" section.

- [ ] **Step 4: Configure and build the empty rung**

```bash
cmake --build build/clangcl-release --target ladder_ledger_lib
```

Expected: succeeds. If CMake requires at least one source file, add an
empty `src/db/schema.cpp` with just the SPDX header and an empty
`namespace ledger::db {}` block (filled in Task 4).

- [ ] **Step 5: Commit**

```bash
git add examples/ledger/CMakeLists.txt examples/CMakeLists.txt
git commit -m "ledger: rung scaffolding (empty lib/server/tests targets)"
```

---

## Task 2: Strong ids, enums, error hierarchy

**Files:**
- Create: `examples/ledger/include/ledger/core/types.hpp`
- Create: `examples/ledger/include/ledger/core/errors.hpp`
- Test: `examples/ledger/tests/test_ledger_types.cpp`

**Interfaces:**
- Produces: `ledger::LedgerId`, `ledger::AccountId`, `ledger::JournalId`,
  `ledger::CategoryId`, `ledger::BudgetId`, `ledger::RuleId`,
  `ledger::ReportJobId` (each: `std::optional<std::int64_t> value`,
  `hasValue()`, `operator*()`, `fromOptional()`, `operator<=>`, per
  kanban's `ProjectId` shape — spec-cited in
  `docs/superpowers/specs/2026-08-16-kanban-rung4-design.md` §7);
  `ledger::AccountKind` (`enum class AccountKind : std::uint8_t { Asset,
  Expense, Revenue, Liability }`); `ledger::RuleTrigger` (`enum class
  RuleTrigger : std::uint8_t { DescriptionContains }`); `ledger::RuleAction`
  (`enum class RuleAction : std::uint8_t { SetCategory }`);
  `ledger::ReportKind` (`enum class ReportKind : std::uint8_t {
  MonthlyStatement, BudgetReport }`); `ledger::ReportStatus` (`enum class
  ReportStatus : std::uint8_t { Pending, Done, Failed }`);
  `ledger::LedgerError` (base), `ledger::ValidationError`,
  `ledger::NotFound`, `ledger::Forbidden`, `ledger::ZeroSumViolation`
  (carries `currencyCode: std::string`, `message: std::string`),
  `ledger::EmptyPrincipalError` (each `: LedgerError`, each carrying a
  `std::string message` and `what()` override) — mirrors
  `bookmarks::core::errors.hpp`/`polls::core::errors.hpp` exactly.

- [ ] **Step 1: Write the failing test for `AccountId` and `AccountKind`**

```cpp
// examples/ledger/tests/test_ledger_types.cpp
// SPDX-License-Identifier: Apache-2.0
#include "ledger/core/types.hpp"
#include "ledger/core/errors.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("AccountId default-constructs empty and engages via explicit int64_t", "[ledger][types]") {
    ledger::AccountId empty;
    CHECK_FALSE(empty.hasValue());

    ledger::AccountId engaged{42};
    REQUIRE(engaged.hasValue());
    CHECK(*engaged == 42);
}

TEST_CASE("AccountId::fromOptional adopts the payload as-is", "[ledger][types]") {
    auto engaged = ledger::AccountId::fromOptional(std::optional<std::int64_t>{7});
    REQUIRE(engaged.hasValue());
    CHECK(*engaged == 7);

    auto empty = ledger::AccountId::fromOptional(std::nullopt);
    CHECK_FALSE(empty.hasValue());
}

TEST_CASE("AccountKind enumerators are distinct", "[ledger][types]") {
    CHECK(ledger::AccountKind::Asset != ledger::AccountKind::Expense);
    CHECK(ledger::AccountKind::Revenue != ledger::AccountKind::Liability);
}

TEST_CASE("ZeroSumViolation carries currency and message", "[ledger][errors]") {
    ledger::ZeroSumViolation err{"USD", "legs did not sum to zero"};
    CHECK(err.currencyCode == "USD");
    CHECK(std::string{err.what()}.find("USD") != std::string::npos);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "ledger.*types" --output-on-failure`
Expected: FAIL to compile — headers don't exist yet.

- [ ] **Step 3: Implement `types.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <compare>
#include <cstdint>
#include <optional>

namespace ledger {

/// @brief Macro-free strong id boilerplate, one struct per identity role —
///        matches kanban's `ProjectId` shape
///        (docs/superpowers/specs/2026-08-16-kanban-rung4-design.md §7):
///        `std::optional<std::int64_t>` payload, `hasValue()`,
///        `fromOptional()`, `operator*()`, total ordering.
#define LEDGER_DEFINE_STRONG_ID(Name)                                                            \
    struct Name {                                                                                \
        std::optional<std::int64_t> value{};                                                     \
        Name() = default;                                                                        \
        explicit Name(std::int64_t v) : value{v} {}                                              \
        [[nodiscard]] bool hasValue() const noexcept { return value.has_value(); }               \
        [[nodiscard]] std::int64_t operator*() const { return *value; }                          \
        static Name fromOptional(std::optional<std::int64_t> v) {                                \
            Name id;                                                                             \
            id.value = v;                                                                        \
            return id;                                                                           \
        }                                                                                         \
        auto operator<=>(const Name&) const = default;                                           \
    }

LEDGER_DEFINE_STRONG_ID(LedgerId);
LEDGER_DEFINE_STRONG_ID(AccountId);
LEDGER_DEFINE_STRONG_ID(JournalId);
LEDGER_DEFINE_STRONG_ID(CategoryId);
LEDGER_DEFINE_STRONG_ID(BudgetId);
LEDGER_DEFINE_STRONG_ID(RuleId);
LEDGER_DEFINE_STRONG_ID(ReportJobId);

#undef LEDGER_DEFINE_STRONG_ID

enum class AccountKind : std::uint8_t { Asset, Expense, Revenue, Liability };
enum class RuleTrigger : std::uint8_t { DescriptionContains };
enum class RuleAction : std::uint8_t { SetCategory };
enum class ReportKind : std::uint8_t { MonthlyStatement, BudgetReport };
enum class ReportStatus : std::uint8_t { Pending, Done, Failed };

}  // namespace ledger
```

- [ ] **Step 4: Implement `errors.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace ledger {

class LedgerError : public std::runtime_error {
  public:
    explicit LedgerError(std::string message) : std::runtime_error{std::move(message)} {}
};

class ValidationError : public LedgerError {
  public:
    explicit ValidationError(std::string message) : LedgerError{std::move(message)} {}
};

class NotFound : public LedgerError {
  public:
    explicit NotFound(std::string message) : LedgerError{std::move(message)} {}
};

class Forbidden : public LedgerError {
  public:
    explicit Forbidden(std::string message) : LedgerError{std::move(message)} {}
};

/// @brief Thrown when a `StoreTransaction`'s legs, partitioned by currency,
///        do not sum to canonical zero for at least one partition. Never
///        thrown for rounding — the model never rounds (design spec §1).
class ZeroSumViolation : public LedgerError {
  public:
    ZeroSumViolation(std::string currency, std::string message)
        : LedgerError{"zero-sum violation in " + currency + ": " + message}, currencyCode{std::move(currency)} {}
    std::string currencyCode;
};

/// @brief Thrown when a mutating action dispatches with an empty principal
///        (design spec §11) — never silently proceeds.
class EmptyPrincipalError : public LedgerError {
  public:
    EmptyPrincipalError() : LedgerError{"mutating action dispatched with an empty principal"} {}
};

}  // namespace ledger
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `ctest --preset cl-debug -R "ledger.*types" --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Wire into CMakeLists.txt and commit**

Add `include/ledger/core/types.hpp`, `include/ledger/core/errors.hpp`, and
`tests/test_ledger_types.cpp` to `examples/ledger/CMakeLists.txt`'s
header/test lists (header-only files typically need no source-list entry,
but confirm against how kanban/polls register header-only additions).

```bash
git add examples/ledger/include/ledger/core/types.hpp \
        examples/ledger/include/ledger/core/errors.hpp \
        examples/ledger/tests/test_ledger_types.cpp \
        examples/ledger/CMakeLists.txt
git commit -m "ledger: strong ids, enums, error hierarchy"
```

---

## Task 3: `Currency` unit system

**Files:**
- Create: `examples/ledger/include/ledger/core/units.hpp`
- Test: `examples/ledger/tests/test_ledger_units.cpp`

**Interfaces:**
- Consumes: `morph::units::Quantity<Unit, DeclaredDecimals>`,
  `morph::units::UnitTraits<E>`, `morph::units::UnitMeta`
  (`include/morph/util/quantity.hpp` — read this header's `UnitTraits`
  customization-point section before writing `units.hpp`).
- Produces: `ledger::Currency` (`enum class Currency : std::uint8_t { USD,
  EUR, JPY, KRW }` — four is enough to exercise both dp=2 and dp=0 without
  building out a full ISO-4217 table), `ledger::UnitTraits<Currency>`
  specialization (or the framework's equivalent customization point name —
  confirm exact required static member names from `quantity.hpp` before
  implementing) supplying `meta(Currency)` with `defaultDecimals` = 2 for
  USD/EUR, **0 for JPY/KRW** (per design spec §2's correction — no floor of
  1), `ledger::Money<C>` alias or direct `Quantity<Currency, 2>` usage at
  DTO sites (confirm against spec §2 whether a per-currency alias or one
  shared `Quantity<Currency, 2>` DTO-level default is used — the spec says
  the DTO-level declared decimals is 2 as a default/hint, with the model
  re-deriving actual precision from the account's real currency).

- [ ] **Step 1: Read `morph::units::UnitTraits`'s customization contract**

Read `include/morph/util/quantity.hpp`'s `UnitTraits<E>` section (the
`UnitEnum` concept, required `static constexpr UnitMeta meta(E)`) and
`docs/spec/util/quantity_type.md`'s matching section before writing
`units.hpp` — do not guess the exact member names.

- [ ] **Step 2: Write the failing test**

```cpp
// examples/ledger/tests/test_ledger_units.cpp
// SPDX-License-Identifier: Apache-2.0
#include "ledger/core/units.hpp"

#include <morph/util/rational.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("USD default decimals is 2", "[ledger][units]") {
    const auto meta = ledger::UnitTraits<ledger::Currency>::meta(ledger::Currency::USD);
    CHECK(meta.defaultDecimals == 2);
}

TEST_CASE("JPY default decimals is 0 -- no floor of 1", "[ledger][units]") {
    const auto meta = ledger::UnitTraits<ledger::Currency>::meta(ledger::Currency::JPY);
    CHECK(meta.defaultDecimals == 0);
}

TEST_CASE("KRW default decimals is 0", "[ledger][units]") {
    const auto meta = ledger::UnitTraits<ledger::Currency>::meta(ledger::Currency::KRW);
    CHECK(meta.defaultDecimals == 0);
}

TEST_CASE("A JPY-denominated Quantity round-trips as a whole number", "[ledger][units]") {
    using JpyQuantity = morph::units::Quantity<ledger::Currency::JPY, 0>;
    auto amount = JpyQuantity{morph::math::Rational{morph::math::Numerator{1500}, morph::math::Denominator{1},
                                                     morph::math::DecimalPlaces{0}}};
    REQUIRE(amount.payload.has_value());
    CHECK(amount.payload->decimalPlaces == morph::math::DecimalPlaces{0});
}
```

Do not guess `Quantity`'s exact constructor/payload member names — copy
them from an existing rung's own `Quantity` usage (e.g.
`examples/forms/lab_units.hpp` or a bank DTO once one exists) or from
`tests/test_quantity.cpp` directly.

- [ ] **Step 3: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "ledger.*units" --output-on-failure`
Expected: FAIL to compile — `units.hpp` doesn't exist.

- [ ] **Step 4: Implement `units.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/util/quantity.hpp>

#include <cstdint>

namespace ledger {

/// @brief The unit system for every money value in this rung
///        (`IMPLEMENTATION.md` rule 3's "each rung defines its unit system
///        once"). Four currencies: two at dp=2 (USD, EUR) and two at dp=0
///        (JPY, KRW), deliberately chosen to exercise both --
///        `DecimalPlaces` has no floor of 1 (design spec §2's correction to
///        the round-5 draft), so JPY/KRW are natively representable, no
///        app-side workaround needed.
enum class Currency : std::uint8_t { USD, EUR, JPY, KRW };

}  // namespace ledger

template <>
struct morph::units::UnitTraits<ledger::Currency> {
    static constexpr morph::units::UnitMeta meta(ledger::Currency c) {
        switch (c) {
            case ledger::Currency::USD:
                return {.id = "USD", .display = "US Dollar", .defaultDecimals = 2};
            case ledger::Currency::EUR:
                return {.id = "EUR", .display = "Euro", .defaultDecimals = 2};
            case ledger::Currency::JPY:
                return {.id = "JPY", .display = "Japanese Yen", .defaultDecimals = 0};
            case ledger::Currency::KRW:
                return {.id = "KRW", .display = "Korean Won", .defaultDecimals = 0};
        }
        return {.id = "USD", .display = "US Dollar", .defaultDecimals = 2};
    }
};
```

(Confirm `UnitMeta`'s exact field names — `id`/`display`/`defaultDecimals`
is this plan's best-grounded guess from the Explore-agent research; verify
against `quantity.hpp` before compiling and adjust the designated
initializers if the real field names differ.)

- [ ] **Step 5: Run tests to verify they pass**

Run: `ctest --preset cl-debug -R "ledger.*units" --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add examples/ledger/include/ledger/core/units.hpp \
        examples/ledger/tests/test_ledger_units.cpp \
        examples/ledger/CMakeLists.txt
git commit -m "ledger: Currency unit system (dp=2 USD/EUR, dp=0 JPY/KRW)"
```

---

## Task 4: `database.hpp` + schema migration

**Files:**
- Create: `examples/ledger/include/ledger/db/database.hpp`
- Create: `examples/ledger/src/db/schema.cpp`
- Test: `examples/ledger/tests/test_ledger_schema.cpp`

**Interfaces:**
- Produces: `ledger::db::setup(const std::string& connectionString)`
  (declared in `database.hpp`, defined in `schema.cpp`) — matches
  `bank::db::setup`/`polls::db::setup`'s exact signature (production
  bootstrap only; see below for why tests do not call it). Internally
  calls `configure(connectionString)` (points Lightweight's default
  connection at it) then `applyMigrations()` (idempotent —
  `MigrationManager::GetInstance().ApplyPendingMigrations()`), following
  `examples/bank/src/db/schema.cpp`'s exact split. Registers every
  `LIGHTWEIGHT_SQL_MIGRATION` this rung needs at static-init time
  (`LIGHTWEIGHT_SQL_MIGRATION(<timestamp>, "<description>") { plan... }`).

**Correction from plan self-review**: the original draft of this task
specified a parameterless `ledger::db::setup()` called directly from a
test — this does not match the established convention. `bank`/`polls`
both declare `setup(const std::string& connectionString)`, and
`polls::db::database.hpp`'s own doc comment states outright: "tests never
call this." The real test-time pattern is
`morph::ladder::testkit::DbFixture` (`examples/common/testkit/
db_fixture.hpp`), which handles connection configuration and migration
application itself — `LIGHTWEIGHT_SQL_MIGRATION`'s registrations are
process-wide static-init side effects that fire the moment `schema.cpp`
is linked in, independent of whether `setup()` itself is ever called;
`DbFixture`'s constructor calls `ApplyPendingMigrations()` on its own,
against a connection string it computes from `ODBC_CONNECTION_STRING` (or
a real on-disk SQLite fallback file). `setup()` exists only for
`examples/ledger/src/app/`'s eventual production bootstrap (a later,
unplanned task — not part of this plan's scope, matching bank/polls'
own app-bootstrap ownership), not for tests. Corrected below.

- [ ] **Step 1: Read `bank::db::schema.cpp`'s migration pattern and `db_fixture.hpp`**

Read `examples/bank/src/db/schema.cpp` in full, particularly the
`accounts` table migration (`LIGHTWEIGHT_SQL_MIGRATION(20260630000002, ...)`),
to copy the exact `plan.CreateTableIfNotExists(...)` DDL shape. Also read
`examples/bank/include/bank/db/database.hpp` (for the `configure`/
`applyMigrations`/`setup` three-function split) and
`examples/common/testkit/db_fixture.hpp` in full (for how tests actually
get a configured, freshly-migrated database — via `DbFixture`, never by
calling `setup()` directly).

- [ ] **Step 2: Write the failing schema test**

```cpp
// examples/ledger/tests/test_ledger_schema.cpp
// SPDX-License-Identifier: Apache-2.0
#include "ledger/db/database.hpp"  // pulls in schema.cpp's registrations via linkage

#include "testkit/db_fixture.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlSchema.hpp>
#include <Lightweight/SqlStatement.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("ledger schema migrations create every expected table", "[ledger][db]") {
    morph::ladder::testkit::DbFixture fixture;  // configures the connection + applies migrations

    Lightweight::SqlStatement stmt;
    const auto tables = Lightweight::SqlSchema::ReadAllTables(stmt, stmt.Connection().DatabaseName());
    auto hasTable = [&](std::string_view name) {
        return std::ranges::any_of(tables, [&](const auto& t) { return t.name == name; });
    };
    CHECK(hasTable("ledgers"));
    CHECK(hasTable("accounts"));
    CHECK(hasTable("transaction_journals"));
    CHECK(hasTable("transaction_legs"));
    CHECK(hasTable("categories"));
    CHECK(hasTable("budgets"));
    CHECK(hasTable("budget_limits"));
    CHECK(hasTable("rules"));
    CHECK(hasTable("ledger_imported_ops"));
    CHECK(hasTable("ledger_imported_txn_hashes"));
    CHECK(hasTable("ledger_report_jobs"));
}
```

Note this test does NOT call `ledger::db::setup()` — `DbFixture` does the
connection configuration and migration application itself; the `#include
"ledger/db/database.hpp"` line's only real job is pulling `schema.cpp`'s
object file into the test binary's link so its `LIGHTWEIGHT_SQL_MIGRATION`
static-init registrations actually run (the same reason every other
rung's schema test includes its own `database.hpp` despite never calling
`setup()`).

- [ ] **Step 3: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "ledger.*schema" --output-on-failure`
Expected: FAIL to compile — `database.hpp` doesn't exist.

- [ ] **Step 4: Implement `database.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

/// @file
/// Process-wide database lifecycle for the ledger rung, mirroring
/// bank::db's exact three-function split (examples/bank/include/bank/db/
/// database.hpp) — Lightweight resolves its connection from a
/// process-global default, and each model opens its own `DataMapper`
/// against it. Production bootstrap only: this rung's own tests never
/// call `setup()` (or `configure()`/`applyMigrations()` individually) --
/// they use `morph::ladder::testkit::DbFixture`, which configures its own
/// connection and applies migrations independently, per the ladder-wide
/// test convention (see polls::db::setup's doc comment for the same
/// stated rule in a sibling rung).

namespace ledger::db {

/// @brief Installs @p connectionString as Lightweight's default connection.
/// @param connectionString ODBC connection string, e.g.
///        `"DRIVER=SQLite3;Database=ledger.db"`.
void configure(const std::string& connectionString);

/// @brief Applies any pending schema migrations against the default connection.
///
/// Idempotent: migrations already recorded in the database's migration
/// history are skipped, so this is safe to call on every startup.
void applyMigrations();

/// @brief Convenience: `configure(connectionString)` followed by `applyMigrations()`.
void setup(const std::string& connectionString);

}  // namespace ledger::db
```

- [ ] **Step 5: Implement `schema.cpp` with every table's migration**

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "ledger/db/database.hpp"

#include <Lightweight/Lightweight.hpp>
#include <Lightweight/SqlMigration.hpp>

namespace ledger::db {

void configure(const std::string& connectionString) {
    Lightweight::SqlConnection::SetDefaultConnectionString(Lightweight::SqlConnectionString{connectionString});
}

void applyMigrations() {
    auto& migrations = Lightweight::SqlMigration::MigrationManager::GetInstance();
    migrations.CreateMigrationHistory();
    migrations.ApplyPendingMigrations();
}

void setup(const std::string& connectionString) {
    configure(connectionString);
    applyMigrations();
}

}  // namespace ledger::db

using namespace Lightweight::SqlColumnTypeDefinitions;
using Lightweight::SqlForeignKeyReferenceDefinition;

// Every method/type below is verified verbatim against real usage in
// examples/bank/src/db/schema.cpp, examples/bookmarks/src/db/schema.cpp,
// and examples/pastebin/src/db/schema.cpp: RequiredForeignKey(col, Type(),
// ref()) creates a NOT-NULL FK column in one call; ForeignKey(col, Type(),
// ref()) (no Required prefix) creates a nullable FK column (bank's own
// nullable `counterparty_id`); Column(name, Type()) (no Required prefix)
// creates a nullable plain column (pastebin's own `expires_at_ms`);
// CreateUniqueIndex(name, table, {cols...}) is a separate plan call, not
// chained onto CreateTableIfNotExists (bookmarks' own imported_ops table).

namespace {
constexpr auto ledgersRef() {
    return SqlForeignKeyReferenceDefinition{.tableName = "ledgers", .columnName = "id"};
}
constexpr auto accountsRef() {
    return SqlForeignKeyReferenceDefinition{.tableName = "accounts", .columnName = "id"};
}
constexpr auto transactionJournalsRef() {
    return SqlForeignKeyReferenceDefinition{.tableName = "transaction_journals", .columnName = "id"};
}
constexpr auto categoriesRef() {
    return SqlForeignKeyReferenceDefinition{.tableName = "categories", .columnName = "id"};
}
constexpr auto budgetsRef() {
    return SqlForeignKeyReferenceDefinition{.tableName = "budgets", .columnName = "id"};
}
}  // namespace

LIGHTWEIGHT_SQL_MIGRATION(20260819000001, "Create ledgers table") {
    plan.CreateTableIfNotExists("ledgers")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("name", Varchar(128));
}

LIGHTWEIGHT_SQL_MIGRATION(20260819000002, "Create accounts table") {
    plan.CreateTableIfNotExists("accounts")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("ledger_id", Bigint(), ledgersRef())
        .RequiredColumn("name", Varchar(128))
        .RequiredColumn("kind", Integer())
        .RequiredColumn("currency_code", Varchar(3));
}

LIGHTWEIGHT_SQL_MIGRATION(20260819000003, "Create transaction_journals table") {
    plan.CreateTableIfNotExists("transaction_journals")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("ledger_id", Bigint(), ledgersRef())
        .RequiredColumn("description", Varchar(256))
        .RequiredColumn("date", Bigint())  // Timestamp at rest -- epoch millis, per morph::time convention
        .Column("causal_parent_id", Varchar(64));  // nullable -- empty-string "no parent" sentinel, per journal's own convention
}

LIGHTWEIGHT_SQL_MIGRATION(20260819000004, "Create transaction_legs table") {
    plan.CreateTableIfNotExists("transaction_legs")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("journal_id", Bigint(), transactionJournalsRef())
        .RequiredForeignKey("account_id", Bigint(), accountsRef())
        .RequiredColumn("amount_num", Bigint())
        .RequiredColumn("amount_den", Bigint())
        .RequiredColumn("amount_dp", Integer())
        .RequiredColumn("currency_code", Varchar(3))
        .Column("foreign_amount_num", Bigint())    // nullable triple -- present only on a
        .Column("foreign_amount_den", Bigint())    // foreign-amount-pair leg (design spec §1 step 3)
        .Column("foreign_amount_dp", Integer())
        .Column("foreign_currency_code", Varchar(3));
}

LIGHTWEIGHT_SQL_MIGRATION(20260819000005, "Create categories table") {
    plan.CreateTableIfNotExists("categories")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("ledger_id", Bigint(), ledgersRef())
        .RequiredColumn("name", Varchar(128));
}

LIGHTWEIGHT_SQL_MIGRATION(20260819000006, "Create budgets table") {
    plan.CreateTableIfNotExists("budgets")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("ledger_id", Bigint(), ledgersRef())
        .RequiredColumn("name", Varchar(128))
        .RequiredForeignKey("category_id", Bigint(), categoriesRef());
}

LIGHTWEIGHT_SQL_MIGRATION(20260819000007, "Create budget_limits table") {
    plan.CreateTableIfNotExists("budget_limits")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("budget_id", Bigint(), budgetsRef())
        .RequiredColumn("month", Varchar(7))  // "YYYY-MM"
        .RequiredColumn("limit_num", Bigint())
        .RequiredColumn("limit_den", Bigint())
        .RequiredColumn("limit_dp", Integer())
        .RequiredColumn("currency_code", Varchar(3));
}

LIGHTWEIGHT_SQL_MIGRATION(20260819000008, "Create rules table") {
    plan.CreateTableIfNotExists("rules")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("ledger_id", Bigint(), ledgersRef())
        .RequiredColumn("trigger", Integer())
        .RequiredColumn("match_text", Varchar(256))
        .RequiredColumn("action", Integer())
        .RequiredColumn("action_value", Varchar(256))
        .RequiredColumn("version", Integer());  // default applied at insert time (=1), not a DDL DEFAULT
}

LIGHTWEIGHT_SQL_MIGRATION(20260819000009, "Create ledger_imported_ops table") {
    // Mirrors bookmarks::db::ImportedOpRecord's exact migration shape
    // (examples/bookmarks/src/db/schema.cpp's "Create imported_ops table",
    // design spec §8): op-id ledger for chunk-retry dedup, keyed by
    // (owner_principal, op_id).
    plan.CreateTableIfNotExists("ledger_imported_ops")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("owner_principal", Varchar(64))
        .RequiredColumn("op_id", Varchar(128))
        .RequiredColumn("applied_at_ms", Bigint());
    plan.CreateUniqueIndex("idx_ledger_imported_ops_owner_op", "ledger_imported_ops",
                            {"owner_principal", "op_id"});
}

LIGHTWEIGHT_SQL_MIGRATION(20260819000010, "Create ledger_imported_txn_hashes table") {
    // Cross-import duplicate detection (design spec §8) -- distinct from
    // ledger_imported_ops: this catches "same statement re-uploaded under a
    // different opId", not "same chunk retried under the same opId".
    plan.CreateTableIfNotExists("ledger_imported_txn_hashes")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("ledger_id", Bigint(), ledgersRef())
        .RequiredColumn("hash", Varchar(64));
    plan.CreateUniqueIndex("idx_ledger_imported_txn_hashes_ledger_hash", "ledger_imported_txn_hashes",
                            {"ledger_id", "hash"});
}

LIGHTWEIGHT_SQL_MIGRATION(20260819000011, "Create ledger_report_jobs table") {
    plan.CreateTableIfNotExists("ledger_report_jobs")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("ledger_id", Bigint(), ledgersRef())
        .RequiredColumn("job_id", Varchar(64))
        .RequiredColumn("kind", Integer())
        .RequiredColumn("status", Integer())
        .Column("result_json", NVarchar(0))  // nullable, unbounded -- absent until the job completes; NVarchar(0) is
                                              // the ladder-wide "unbounded text" convention (IMPLEMENTATION.md rule 3,
                                              // never Text() -- see pastebin's own `content` column)
        .RequiredColumn("created_at_ms", Bigint());
}
```

This migration code is verified against three real, already-merged
schema.cpp files method-by-method (not a guess needing further
confirmation, unlike the plan's original draft of this task). The one
open item: `date`/`applied_at_ms`/`created_at_ms` are stored as `Bigint()`
epoch-millis integers, matching `bank::db::TxnRecord`'s own timestamp
convention — confirm this still matches whatever `morph::time::Timestamp`
serialization the model layer (Task 7 onward) actually uses before wiring
the DTO⇄entity mapping, since a mismatch there is a Task 7+ concern, not
this task's.

- [ ] **Step 6: Run tests to verify they pass**

Run: `ctest --preset cl-debug -R "ledger.*schema" --output-on-failure`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add examples/ledger/include/ledger/db/database.hpp \
        examples/ledger/src/db/schema.cpp \
        examples/ledger/tests/test_ledger_schema.cpp \
        examples/ledger/CMakeLists.txt
git commit -m "ledger: database.hpp + schema migrations for every table"
```

---

## Task 5: Entities (`Light::Field` records)

**Files:**
- Create: `examples/ledger/include/ledger/db/ledger_entity.hpp`
- Modify: `examples/ledger/tests/test_ledger_schema.cpp` (extend with real
  entity-backed assertions now that entities exist)

**Interfaces:**
- Consumes: Task 4's tables.
- Produces: `ledger::db::LedgerRecord`, `ledger::db::AccountRecord`,
  `ledger::db::TransactionJournalRecord`, `ledger::db::TransactionLegRecord`,
  `ledger::db::CategoryRecord`, `ledger::db::BudgetRecord`,
  `ledger::db::BudgetLimitRecord`, `ledger::db::RuleRecord`,
  `ledger::db::ImportedOpRecord`, `ledger::db::ImportedTxnHashRecord`,
  `ledger::db::ReportJobRecord` — one `Light::Field<>`-wrapped struct per
  table, `BelongsTo` for every foreign key, per design spec §1's entity
  list and `examples/bank/include/bank/db/account_entity.hpp`'s exact
  shape.

- [ ] **Step 1: Read `bank::db::AccountRecord` and `bookmarks::db::ImportedOpRecord`**

Read `examples/bank/include/bank/db/account_entity.hpp` (for the
`Field<>`/`BelongsTo`/`PrimaryKey::ServerSideAutoIncrement` shape) and
`examples/bookmarks/include/bookmarks/db/imported_op_entity.hpp` (for the
exact `ImportedOpRecord` shape this rung's `ledger::db::ImportedOpRecord`
mirrors verbatim, per design spec §8).

**Correction from plan self-review**: Step 2's original test called
`ledger::db::setup()` directly — same error Task 4 already corrected;
tests use `DbFixture`, never `setup()`. Fixed below. Step 4's entity file
was also left with a "follow the same shape" ellipsis for 9 of 11
entities — replaced with the complete file, every field verified against
real Lightweight usage: `Light::Field<std::optional<T>, ...>` for a plain
nullable column (confirmed real:
`Lightweight/DataBinder/StdOptional.hpp` provides
`SqlDataBinder<std::optional<T>>`, and `Field.hpp`'s own
`detail::IsStdOptionalType` exists specifically to recognize this case —
this is NOT a guess needing further verification), and
`Light::BelongsTo<&Target::id, Light::SqlRealName{"col"}>` for a required
FK / `..., Light::SqlNullable::Null>` for a nullable FK (confirmed against
`bank::db::TxnRecord`'s real nullable `counterparty` field). This task has
no nullable FK (every `BelongsTo` here is required), only plain nullable
scalar columns on `TransactionLegRecord` and `ReportJobRecord`.

- [ ] **Step 2: Write the failing test extending schema coverage**

```cpp
// Append to examples/ledger/tests/test_ledger_schema.cpp
TEST_CASE("AccountRecord round-trips through the ledgers/accounts tables", "[ledger][db]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;

    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    REQUIRE(ledgerRow.id.Value() != 0);

    ledger::db::AccountRecord accountRow;
    accountRow.ledger = ledgerRow;  // BelongsTo assignment: the whole parent record, per
                                     // polls::db::OptionRecord's real usage (opt.poll = poll;),
                                     // never a raw .SetKey(...) call
    accountRow.name = "Checking";
    accountRow.kind = 0;  // AccountKind::Asset
    accountRow.currencyCode = "USD";
    mapper.Create(accountRow);
    REQUIRE(accountRow.id.Value() != 0);

    auto loaded = mapper.Query<ledger::db::AccountRecord>()
                      .Where(::Lightweight::FieldNameOf<&ledger::db::AccountRecord::ledger>, "=", ledgerRow.id.Value())
                      .All();
    REQUIRE(loaded.size() == 1);
    CHECK(loaded.front().name.Value() == "Checking");
}
```

This test's `DataMapper::Create`/`Query<T>().Where(...).All()`/
`BelongsTo` assignment shape is copied verbatim (adjusted for
ledger's own types) from `examples/polls/tests/test_polls_schema.cpp`'s
real, already-compiling schema test — not a guess.

- [ ] **Step 3: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "ledger.*schema" --output-on-failure`
Expected: FAIL to compile — `ledger_entity.hpp` doesn't exist.

- [ ] **Step 4: Implement `ledger_entity.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/BelongsTo.hpp>
#include <Lightweight/DataMapper/Field.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace ledger::db {

struct LedgerRecord {
    static constexpr std::string_view TableName = "ledgers";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"name"}> name;  // 1
};

struct AccountRecord {
    static constexpr std::string_view TableName = "accounts";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&LedgerRecord::id, Light::SqlRealName{"ledger_id"}> ledger;  // 1
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"name"}> name;  // 2
    Light::Field<int, Light::SqlRealName{"kind"}> kind;  // 3
    Light::Field<Light::SqlAnsiString<3>, Light::SqlRealName{"currency_code"}> currencyCode;  // 4
};

struct TransactionJournalRecord {
    static constexpr std::string_view TableName = "transaction_journals";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&LedgerRecord::id, Light::SqlRealName{"ledger_id"}> ledger;  // 1
    Light::Field<Light::SqlAnsiString<256>, Light::SqlRealName{"description"}> description;  // 2
    Light::Field<std::int64_t, Light::SqlRealName{"date"}> date{0};  // 3 -- epoch millis
    Light::Field<std::optional<Light::SqlAnsiString<64>>, Light::SqlRealName{"causal_parent_id"}>
        causalParentId;  // 4 -- nullable, per design spec §5's causalParentId shape
};

struct TransactionLegRecord {
    static constexpr std::string_view TableName = "transaction_legs";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&TransactionJournalRecord::id, Light::SqlRealName{"journal_id"}> journal;  // 1
    Light::BelongsTo<&AccountRecord::id, Light::SqlRealName{"account_id"}> account;  // 2
    Light::Field<std::int64_t, Light::SqlRealName{"amount_num"}> amountNum{0};  // 3
    Light::Field<std::int64_t, Light::SqlRealName{"amount_den"}> amountDen{1};  // 4
    Light::Field<int, Light::SqlRealName{"amount_dp"}> amountDp{0};  // 5
    Light::Field<Light::SqlAnsiString<3>, Light::SqlRealName{"currency_code"}> currencyCode;  // 6
    // Nullable foreign-amount triple -- present only on a foreign-amount-pair
    // leg (design spec §1 step 3). Plain std::optional<T> field, not a
    // BelongsTo: confirmed real via Lightweight/DataBinder/StdOptional.hpp's
    // SqlDataBinder<std::optional<T>> specialization.
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"foreign_amount_num"}> foreignAmountNum;  // 7
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"foreign_amount_den"}> foreignAmountDen;  // 8
    Light::Field<std::optional<int>, Light::SqlRealName{"foreign_amount_dp"}> foreignAmountDp;  // 9
    Light::Field<std::optional<Light::SqlAnsiString<3>>, Light::SqlRealName{"foreign_currency_code"}>
        foreignCurrencyCode;  // 10
};

struct CategoryRecord {
    static constexpr std::string_view TableName = "categories";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&LedgerRecord::id, Light::SqlRealName{"ledger_id"}> ledger;  // 1
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"name"}> name;  // 2
};

struct BudgetRecord {
    static constexpr std::string_view TableName = "budgets";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&LedgerRecord::id, Light::SqlRealName{"ledger_id"}> ledger;  // 1
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"name"}> name;  // 2
    Light::BelongsTo<&CategoryRecord::id, Light::SqlRealName{"category_id"}> category;  // 3
};

struct BudgetLimitRecord {
    static constexpr std::string_view TableName = "budget_limits";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&BudgetRecord::id, Light::SqlRealName{"budget_id"}> budget;  // 1
    Light::Field<Light::SqlAnsiString<7>, Light::SqlRealName{"month"}> month;  // 2 -- "YYYY-MM"
    Light::Field<std::int64_t, Light::SqlRealName{"limit_num"}> limitNum{0};  // 3
    Light::Field<std::int64_t, Light::SqlRealName{"limit_den"}> limitDen{1};  // 4
    Light::Field<int, Light::SqlRealName{"limit_dp"}> limitDp{0};  // 5
    Light::Field<Light::SqlAnsiString<3>, Light::SqlRealName{"currency_code"}> currencyCode;  // 6
};

struct RuleRecord {
    static constexpr std::string_view TableName = "rules";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&LedgerRecord::id, Light::SqlRealName{"ledger_id"}> ledger;  // 1
    Light::Field<int, Light::SqlRealName{"trigger"}> trigger{0};  // 2
    Light::Field<Light::SqlAnsiString<256>, Light::SqlRealName{"match_text"}> matchText;  // 3
    Light::Field<int, Light::SqlRealName{"action"}> action{0};  // 4
    Light::Field<Light::SqlAnsiString<256>, Light::SqlRealName{"action_value"}> actionValue;  // 5
    Light::Field<int, Light::SqlRealName{"version"}> version{1};  // 6
};

/// @brief Mirrors `bookmarks::db::ImportedOpRecord`'s exact shape (design
///        spec §8): op-id ledger for chunk-retry dedup, keyed by
///        `(owner_principal, op_id)`.
struct ImportedOpRecord {
    static constexpr std::string_view TableName = "ledger_imported_ops";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"owner_principal"}> ownerPrincipal;  // 1
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"op_id"}> opId;  // 2
    Light::Field<std::int64_t, Light::SqlRealName{"applied_at_ms"}> appliedAtMs{0};  // 3
};

/// @brief Cross-import duplicate detection (design spec §8) -- distinct
///        from `ImportedOpRecord`; see that struct's own doc comment for
///        the difference.
struct ImportedTxnHashRecord {
    static constexpr std::string_view TableName = "ledger_imported_txn_hashes";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&LedgerRecord::id, Light::SqlRealName{"ledger_id"}> ledger;  // 1
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"hash"}> hash;  // 2
};

struct ReportJobRecord {
    static constexpr std::string_view TableName = "ledger_report_jobs";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&LedgerRecord::id, Light::SqlRealName{"ledger_id"}> ledger;  // 1
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"job_id"}> jobId;  // 2
    Light::Field<int, Light::SqlRealName{"kind"}> kind{0};  // 3
    Light::Field<int, Light::SqlRealName{"status"}> status{0};  // 4
    // Nullable AND unbounded -- a combination no existing rung's entity
    // needs yet (polls::db::VoteHistoryRecord::previousVotesJson is
    // unbounded but always-populated, never nullable). Field<std::optional
    // <T>, ...>'s wrapping is confirmed generic (StdOptional.hpp specializes
    // SqlDataBinder<std::optional<T>> for any T with its own binder), so
    // wrapping the same Light::SqlMaxDynamicAnsiString type
    // polls::db::VoteHistoryRecord::previousVotesJson already uses in
    // std::optional<> is the correct composition, not a new guess -- but
    // this exact composition has no precedent in the codebase to copy
    // verbatim, so build+test this field specifically before trusting it.
    Light::Field<std::optional<Light::SqlMaxDynamicAnsiString>, Light::SqlRealName{"result_json"}>
        resultJson;  // 5 -- nullable, unbounded (NVarchar(0) at the DDL layer); absent until the job completes
    Light::Field<std::int64_t, Light::SqlRealName{"created_at_ms"}> createdAtMs{0};  // 6
};

}  // namespace ledger::db
```

Every field in this file except `resultJson` is checked directly against
real, already-compiling entity code in this repo (bank/bookmarks/polls)
and needs no further verification. `resultJson`'s
`std::optional<Light::SqlMaxDynamicAnsiString>` composition is inferred
from two separately-confirmed facts (optional-wrapping is generic;
`SqlMaxDynamicAnsiString` is the real unbounded-string type) rather than
copied from one existing example — build and test it specifically as the
one field in this task worth double-checking.

- [ ] **Step 5: Run tests to verify they pass**

Run: `ctest --preset cl-debug -R "ledger.*schema" --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add examples/ledger/include/ledger/db/ledger_entity.hpp \
        examples/ledger/tests/test_ledger_schema.cpp
git commit -m "ledger: entities for every table (Light::Field records)"
```

---

## Task 6: `account_dto.hpp` — `OpenAccount`, `GetLedger`

**Files:**
- Create: `examples/ledger/include/ledger/dto/account_dto.hpp`
- Test: `examples/ledger/tests/test_account_dto.cpp`

**Interfaces:**
- Consumes: `ledger::LedgerId`, `ledger::AccountId`, `ledger::AccountKind`,
  `ledger::Currency` (Tasks 2, 3), `morph::forms::allRequiredEngaged`.
- Produces: `ledger::OpenAccount { ledgerId: LedgerId, name: std::string,
  kind: AccountKind, currency: Currency }` with `validate()`;
  `ledger::GetLedger { ledgerId: LedgerId }`;
  `ledger::AccountInfo { id: AccountId, name: std::string, kind:
  AccountKind, currency: Currency, balance: Quantity<Currency, 2> }` (the
  result DTO type used by both `GetLedger`'s result and later
  `StoreTransaction`'s rebuilt-state result, per the ladder-wide convention
  of returning full rebuilt state); `ledger::GetLedgerResult { accounts:
  std::vector<AccountInfo> }`.

- [ ] **Step 1: Write the failing test**

```cpp
// examples/ledger/tests/test_account_dto.cpp
// SPDX-License-Identifier: Apache-2.0
#include "ledger/dto/account_dto.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("OpenAccount::validate rejects an empty name", "[ledger][dto]") {
    ledger::OpenAccount action{.ledgerId = ledger::LedgerId{1}, .name = "", .kind = ledger::AccountKind::Asset,
                                .currency = ledger::Currency::USD};
    CHECK_FALSE(action.validate());
}

TEST_CASE("OpenAccount::validate accepts a fully-engaged action", "[ledger][dto]") {
    ledger::OpenAccount action{.ledgerId = ledger::LedgerId{1}, .name = "Checking",
                                .kind = ledger::AccountKind::Asset, .currency = ledger::Currency::USD};
    CHECK(action.validate());
}

TEST_CASE("GetLedger::validate rejects a disengaged ledgerId", "[ledger][dto]") {
    ledger::GetLedger action{.ledgerId = ledger::LedgerId{}};
    CHECK_FALSE(action.validate());
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "account_dto" --output-on-failure`
Expected: FAIL to compile — `account_dto.hpp` doesn't exist.

- [ ] **Step 3: Implement `account_dto.hpp`**

**Correction from plan self-review**: `AccountInfo::balance` cannot be
`Quantity<Currency::USD, 2>` or any other single `Quantity<...>` —
`Quantity<Unit, dp>`'s `Unit` template parameter is a specific enumerator
*value* (`auto U`, confirmed at `include/morph/util/quantity.hpp` line
461), not the enum type, so one struct cannot hold a `Quantity` generic
over *which* currency an account uses. Resolved per design spec §2's own
answer, and consistent with `Money<C>`'s design in `units.hpp` (Task 3):
the DTO field is a plain `morph::math::Rational balance` alongside the
sibling `currency: Currency` field — no type-level lie, the real currency
always comes from the sibling field, never implied by a `Quantity`'s
compile-time unit parameter.

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ledger/core/types.hpp"
#include "ledger/core/units.hpp"

#include <morph/forms/forms.hpp>
#include <morph/util/rational.hpp>

#include <string>
#include <vector>

namespace ledger {

struct OpenAccount {
    LedgerId ledgerId;
    std::string name;
    AccountKind kind;
    Currency currency;

    [[nodiscard]] bool validate() const noexcept { return ledgerId.hasValue() && !name.empty(); }
};

struct GetLedger {
    LedgerId ledgerId;

    [[nodiscard]] bool validate() const noexcept { return morph::forms::allRequiredEngaged(*this); }
};

struct AccountInfo {
    AccountId id;
    std::string name;
    AccountKind kind;
    Currency currency;
    morph::math::Rational balance;  // plain Rational -- real currency is the sibling `currency` field above,
                                     // never a Quantity's compile-time unit parameter (design spec §2)
};

struct GetLedgerResult {
    std::vector<AccountInfo> accounts;
};

}  // namespace ledger
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `ctest --preset cl-debug -R "account_dto" --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add examples/ledger/include/ledger/dto/account_dto.hpp \
        examples/ledger/tests/test_account_dto.cpp \
        examples/ledger/CMakeLists.txt
git commit -m "ledger: account_dto.hpp -- OpenAccount, GetLedger"
```

---

## Task 7: `transaction_dto.hpp` + `LedgerModel` skeleton (`OpenAccount`, `GetLedger`)

**Files:**
- Create: `examples/ledger/include/ledger/dto/transaction_dto.hpp`
- Create: `examples/ledger/include/ledger/models/ledger_model.hpp`
- Create: `examples/ledger/src/models/ledger_model.cpp`
- Test: `examples/ledger/tests/test_ledger_model.cpp`

**Correction from plan self-review**: the original draft assumed a keyed
model takes its key as a constructor argument
(`LedgerModel model{LedgerId{1}}`) and that `BRIDGE_KEY_FROM` is used for
every keyed action including the first. Both are wrong, verified against
`polls::PollModel` (`examples/polls/include/polls/models/poll_model.hpp`,
`examples/polls/tests/test_poll_model.cpp`): a keyed model is **plain
default-constructible** (`PollModel model;`, no key argument anywhere) —
the "key" is a routing concept the `Bridge`/registry layer uses to find
or create the right *shared instance* over the wire; a model's own
`execute()` methods just read whatever key field the *action* carries.
`BRIDGE_MODEL_KEY(M, A, MEMBER)` (`include/morph/core/model_key.hpp`) is
used exactly **once**, on the model's *first* keyed action — it also
establishes `ModelKeyTraits<M>::PrimaryKey`. Every *other* action sharing
the same key type uses `BRIDGE_KEY_FROM(A, MEMBER)` instead (which only
adds `ActionKeyTraits<A>`, not `ModelKeyTraits<M>` — using it on the first
action fails to compile). Since every `ledger` action already carries its
own `ledgerId` explicitly (unlike `polls::GetPollState`, which relies on
`PollModel`'s private `_pollId` cache set by a prior `OpenPoll` call),
`LedgerModel` needs **no private caching member at all** — simpler than
`PollModel`, not a peer of it.

**Interfaces:**
- Consumes: Tasks 2–6's types/entities/DTOs; `BRIDGE_REGISTER_MODEL`,
  `BRIDGE_REGISTER_ACTION`, `BRIDGE_MODEL_KEY`, `BRIDGE_KEY_FROM`
  (`include/morph/core/registry.hpp`, `model_key.hpp` — confirmed against
  `polls::PollModel`'s real registration block).
- Produces: `ledger::LedgerModel`, a plain default-constructible class
  (`BRIDGE_MODEL_KEY(LedgerModel, OpenAccount, &OpenAccount::ledgerId)` —
  `OpenAccount` is the model's first keyed action, establishing
  `ModelKeyTraits<LedgerModel>::PrimaryKey`; `BRIDGE_KEY_FROM(GetLedger,
  &GetLedger::ledgerId)` for the second), implementing
  `execute(OpenAccount)` and `execute(GetLedger)` only in this task —
  `StoreTransaction` lands in Task 8, and per `polls::PollModel`'s own
  documented incremental-registration discipline (its file's own top
  comment: `BRIDGE_REGISTER_ACTION` needs a linkable `execute()` body at
  static-init link time, so an action is never registered ahead of its
  body existing), Task 8 adds its own `BRIDGE_REGISTER_ACTION` line
  alongside its own `.cpp` body, not this task.
  `TransactionLeg { accountId: AccountId, amount: /* resolved per Task 6's
  note */ }` declared in `transaction_dto.hpp` ahead of `StoreTransaction`
  itself so Task 8 can use it.

- [ ] **Step 1: Read `polls::PollModel`'s keyed-model registration shape**

Read `examples/polls/include/polls/models/poll_model.hpp` in full
(especially its file-level comment on the incremental-registration
discipline and the `BRIDGE_MODEL_KEY`/`BRIDGE_KEY_FROM` block at the
bottom) and `examples/polls/tests/test_poll_model.cpp` (for the plain
`PollModel model;` construction pattern) before writing any code.

- [ ] **Step 2: Write the failing model test**

```cpp
// examples/ledger/tests/test_ledger_model.cpp
// SPDX-License-Identifier: Apache-2.0
#include "ledger/db/ledger_entity.hpp"
#include "ledger/models/ledger_model.hpp"
#include "testkit/db_fixture.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("OpenAccount creates an account visible in GetLedger", "[ledger][model]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    // This rung has no CreateLedger action in scope -- see Step 4's own
    // note -- so the test seeds the ledgers row directly, mirroring Task
    // 5's own schema test.
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::LedgerModel model;
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "Checking",
                                       .kind = ledger::AccountKind::Asset, .currency = ledger::Currency::USD});

    auto result = model.execute(ledger::GetLedger{.ledgerId = ledgerId});
    REQUIRE(result.accounts.size() == 1);
    CHECK(result.accounts[0].name == "Checking");
}
```

This test's `LedgerModel model;` (no constructor argument) is copied
verbatim from `polls::PollModel`'s own real, already-compiling test
pattern — not a guess.

- [ ] **Step 3: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "ledger.*model" --output-on-failure`
Expected: FAIL to compile.

- [ ] **Step 4: Implement `transaction_dto.hpp`'s `TransactionLeg` (forward declaration only, full `StoreTransaction` in Task 8) and `ledger_model.hpp`/`.cpp`**

```cpp
// examples/ledger/include/ledger/dto/transaction_dto.hpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ledger/core/types.hpp"

#include <morph/util/rational.hpp>

#include <vector>

namespace ledger {

/// @brief One leg of a `StoreTransaction` (Task 8) or the multi-client
///        stress harness (Task 23). Declared ahead of `StoreTransaction`
///        itself, per this task's own scope.
struct TransactionLeg {
    AccountId accountId;
    morph::math::Rational amount;  // real currency comes from the account this leg names, per design spec §2
};

}  // namespace ledger
```

```cpp
// examples/ledger/include/ledger/models/ledger_model.hpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ledger/dto/account_dto.hpp"

#include <morph/core/model_key.hpp>
#include <morph/core/registry.hpp>

namespace ledger {

/// @brief Accounts + transaction journal, keyed by `LedgerId` (design spec
///        §1) -- one ledger per book. Plain default-constructible, per
///        `polls::PollModel`'s own real shape: the key lives in each
///        action, not in the model instance. No private caching member is
///        needed here (unlike `PollModel`'s `_pollId`) because every
///        action this model implements carries its own `ledgerId`
///        explicitly.
class LedgerModel {
  public:
    /// @brief Creates an account in the ledger named by `action.ledgerId`.
    ///        The model's first keyed action -- `BRIDGE_MODEL_KEY(
    ///        LedgerModel, OpenAccount, &OpenAccount::ledgerId)`.
    /// @param action Ledger id, name, kind, and currency for the new account.
    void execute(const OpenAccount& action);

    /// @brief Returns the full current state of the ledger named by
    ///        `action.ledgerId`.
    /// @param action The ledger id.
    /// @return Every account in the ledger, per the ladder-wide
    ///         full-rebuilt-state convention.
    GetLedgerResult execute(const GetLedger& action);
};

}  // namespace ledger

BRIDGE_REGISTER_MODEL(ledger::LedgerModel, "LedgerModel")
BRIDGE_REGISTER_ACTION(ledger::LedgerModel, ledger::OpenAccount, "OpenAccount")
BRIDGE_REGISTER_ACTION(ledger::LedgerModel, ledger::GetLedger, "GetLedger", ::morph::model::Loggable::No)

BRIDGE_MODEL_KEY(ledger::LedgerModel, ledger::OpenAccount, &ledger::OpenAccount::ledgerId);
BRIDGE_KEY_FROM(ledger::GetLedger, &ledger::GetLedger::ledgerId);
```

`BRIDGE_REGISTER_ACTION`'s optional `::morph::model::Loggable::No` on
`GetLedger` mirrors `polls::PollModel`'s own convention of marking
read-only query actions non-loggable (`GetPollState`,
`GetEventsSince`) — confirm this is still the right default for `GetLedger`
specifically (a read has no side effect to audit) before finalizing;
`OpenAccount` stays loggable (default), since it's a mutation.

```cpp
// examples/ledger/src/models/ledger_model.cpp
// SPDX-License-Identifier: Apache-2.0
#include "ledger/core/errors.hpp"
#include "ledger/db/ledger_entity.hpp"
#include "ledger/models/ledger_model.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>

namespace ledger {

void LedgerModel::execute(const OpenAccount& action) {
    if (!action.validate()) {
        throw ValidationError{"OpenAccount: ledgerId and name are required"};
    }
    Lightweight::DataMapper mapper;
    // The ledger row must already exist -- this rung's scope has no
    // CreateLedger action (see Step 4's own note); load it by primary key
    // rather than fabricating a stub LedgerRecord, since BelongsTo
    // assignment needs the real persisted parent (per
    // polls::db::OptionRecord's own `opt.poll = poll;` usage, where `poll`
    // is a row that has actually round-tripped through Create/Query).
    auto ledgerRows =
        mapper.Query<db::LedgerRecord>().Where(::Lightweight::FieldNameOf<&db::LedgerRecord::id>, "=", *action.ledgerId).All();
    if (ledgerRows.empty()) {
        throw NotFound{"OpenAccount: no such ledger"};
    }
    db::AccountRecord accountRow;
    accountRow.ledger = ledgerRows.front();
    accountRow.name = action.name;
    accountRow.kind = static_cast<int>(action.kind);
    accountRow.currencyCode = currencyToCode(action.currency);  // confirm/implement this helper -- see note below
    mapper.Create(accountRow);
}

GetLedgerResult LedgerModel::execute(const GetLedger& action) {
    if (!action.validate()) {
        throw ValidationError{"GetLedger: ledgerId is required"};
    }
    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::AccountRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::AccountRecord::ledger>, "=", *action.ledgerId)
                    .All();
    GetLedgerResult result;
    result.accounts.reserve(rows.size());
    for (const auto& row : rows) {
        result.accounts.push_back(AccountInfo{
            .id = AccountId{static_cast<std::int64_t>(row.id.Value())},
            .name = std::string{row.name.Value().ToStringView()},
            .kind = static_cast<AccountKind>(row.kind.Value()),
            .currency = codeToCurrency(row.currencyCode.Value().ToStringView()),
            .balance = morph::math::Rational{morph::math::Numerator{0}, morph::math::Denominator{1},
                                              morph::math::DecimalPlaces{2}},  // no legs exist yet at this
                                                                                // task's scope -- Task 8
                                                                                // computes a real balance
        });
    }
    return result;
}

}  // namespace ledger
```

**Note: ledger provisioning is out of this rung's scope.**
`OpenAccount`'s `ledgerId` names an *existing* ledger — there is no
`CreateLedger` action anywhere in this rung's scope (design spec §1
lists `LedgerModel` as "keyed by ledger id," implying ledgers are
provisioned some other way, e.g. an app-level seed/admin step outside
this plan). Step 2's test above already reflects this: it seeds the
`ledgers` row itself, mirroring Task 5's own schema test, rather than
assuming `execute(OpenAccount)` auto-creates one.

**One real thing this implementation needs that this task must add**:
`currencyToCode`/`codeToCurrency` — small free functions
(`std::string_view currencyToCode(Currency)` /
`Currency codeToCurrency(std::string_view)`) converting between the
`Currency` enum and its 3-letter DB code (`"USD"`, `"EUR"`, `"JPY"`,
`"KRW"`) — a natural fit for `ledger/core/units.hpp` (Task 3), added
as a small addition to that file in this task rather than duplicated
ad hoc in every model that needs it. Add the declaration to
`units.hpp` and the definition to a new `units.cpp` (or, if the
switch is small enough to stay header-only and `constexpr`, directly
in `units.hpp`) — confirm which convention fits this codebase's
existing header-only-vs-`.cpp`-split pattern for small pure functions
before choosing.

- [ ] **Step 5: Run tests to verify they pass**

Run: `ctest --preset cl-debug -R "ledger.*model" --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add examples/ledger/include/ledger/dto/transaction_dto.hpp \
        examples/ledger/include/ledger/models/ledger_model.hpp \
        examples/ledger/src/models/ledger_model.cpp \
        examples/ledger/tests/test_ledger_model.cpp \
        examples/ledger/CMakeLists.txt
git commit -m "ledger: LedgerModel skeleton -- OpenAccount, GetLedger"
```

---

## Task 8: `StoreTransaction` — the per-currency zero-sum invariant

**Files:**
- Modify: `examples/ledger/include/ledger/dto/transaction_dto.hpp`
- Modify: `examples/ledger/include/ledger/models/ledger_model.hpp`
- Modify: `examples/ledger/src/models/ledger_model.cpp`
- Modify: `examples/ledger/tests/test_ledger_model.cpp`

**Interfaces:**
- Consumes: `morph::math::Rational::operator+` (grounded:
  `include/morph/util/rational.hpp`), Task 7's `LedgerModel`.
- Produces: `ledger::StoreTransaction { ledgerId: LedgerId, description:
  std::string, date: morph::time::Timestamp, legs:
  std::vector<TransactionLeg> }` with `validate()`;
  `LedgerModel::execute(StoreTransaction) -> GetLedgerResult` (returns full
  rebuilt ledger state, per the ladder-wide convention) — implements
  design spec §1's exact per-currency zero-sum algorithm.

- [ ] **Step 1: Write the failing test for the happy path**

**Correction from plan self-review**: the original draft used
`LedgerModel model{ledger::LedgerId{1}}` (a constructor argument) and
left `StoreTransaction`/`execute(StoreTransaction)`'s implementation as
prose with no code, and its tests as an incomplete sketch (no ledger
seeding, no real legs, no real balance assertions). All fixed below, per
Task 7's own corrected pattern (plain default-constructible model,
`LedgerId` values come from a real seeded `ledgers` row, never a bare
literal).

```cpp
// Append to examples/ledger/tests/test_ledger_model.cpp
TEST_CASE("StoreTransaction with two balanced USD legs commits", "[ledger][model]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::LedgerModel model;
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "Checking",
                                       .kind = ledger::AccountKind::Asset, .currency = ledger::Currency::USD});
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "Groceries",
                                       .kind = ledger::AccountKind::Expense, .currency = ledger::Currency::USD});
    auto ledgerState = model.execute(ledger::GetLedger{.ledgerId = ledgerId});
    auto checkingId = ledgerState.accounts[0].id;
    auto groceriesId = ledgerState.accounts[1].id;

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    // -50.00 from Checking, +50.00 to Groceries -- exact Rational legs, sums to zero.
    auto result = model.execute(ledger::StoreTransaction{
        .ledgerId = ledgerId,
        .description = "Weekly shop",
        .date = morph::time::Timestamp::now(),
        .legs = {ledger::TransactionLeg{.accountId = checkingId,
                                         .amount = morph::math::Rational{Numerator{-5000}, Denominator{1},
                                                                          DecimalPlaces{2}}},
                 ledger::TransactionLeg{.accountId = groceriesId,
                                        .amount = morph::math::Rational{Numerator{5000}, Denominator{1},
                                                                         DecimalPlaces{2}}}}});

    REQUIRE(result.accounts.size() == 2);
    auto checking = std::ranges::find_if(result.accounts, [&](const auto& a) { return a.id == checkingId; });
    auto groceries = std::ranges::find_if(result.accounts, [&](const auto& a) { return a.id == groceriesId; });
    REQUIRE(checking != result.accounts.end());
    REQUIRE(groceries != result.accounts.end());
    CHECK(checking->balance.numerator == -5000);
    CHECK(groceries->balance.numerator == 5000);
}

TEST_CASE("StoreTransaction with unbalanced USD legs throws ZeroSumViolation", "[ledger][model]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::LedgerModel model;
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "Checking",
                                       .kind = ledger::AccountKind::Asset, .currency = ledger::Currency::USD});
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "Groceries",
                                       .kind = ledger::AccountKind::Expense, .currency = ledger::Currency::USD});
    auto ledgerState = model.execute(ledger::GetLedger{.ledgerId = ledgerId});

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    CHECK_THROWS_AS(
        model.execute(ledger::StoreTransaction{
            .ledgerId = ledgerId,
            .description = "Bad txn",
            .date = morph::time::Timestamp::now(),
            .legs = {ledger::TransactionLeg{.accountId = ledgerState.accounts[0].id,
                                             .amount = morph::math::Rational{Numerator{-5000}, Denominator{1},
                                                                              DecimalPlaces{2}}},
                     ledger::TransactionLeg{.accountId = ledgerState.accounts[1].id,
                                            .amount = morph::math::Rational{Numerator{4000}, Denominator{1},
                                                                             DecimalPlaces{2}}}}}),
        ledger::ZeroSumViolation);
}
```

Add `#include <algorithm>` (for `std::ranges::find_if`) and
`#include <Lightweight/DataMapper/DataMapper.hpp>` +
`#include "ledger/db/ledger_entity.hpp"` to this test file's top if not
already present from Task 7.

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "StoreTransaction" --output-on-failure`
Expected: FAIL to compile — `StoreTransaction` doesn't exist.

- [ ] **Step 3: Implement `StoreTransaction` and `LedgerModel::execute(StoreTransaction)`**

```cpp
// Append to examples/ledger/include/ledger/dto/transaction_dto.hpp
struct StoreTransaction {
    LedgerId ledgerId;
    std::string description;
    morph::time::Timestamp date;
    std::vector<TransactionLeg> legs;

    [[nodiscard]] bool validate() const noexcept {
        return ledgerId.hasValue() && !description.empty() && legs.size() >= 2 &&
               std::ranges::all_of(legs, [](const auto& leg) { return leg.accountId.hasValue(); });
    }
};
```

(Add `#include <morph/util/datetime.hpp>`, `#include <algorithm>`, and
`#include <string>` to `transaction_dto.hpp`'s includes.)

```cpp
// Append to examples/ledger/include/ledger/models/ledger_model.hpp's class body
GetLedgerResult execute(const StoreTransaction& action);
```

```cpp
// Append the registration line to ledger_model.hpp's bottom block, per
// Task 7's incremental-registration discipline (a new action's
// BRIDGE_REGISTER_ACTION line lands alongside its own execute() body).
// Per Task 7's own real, verified discovery, BRIDGE_KEY_FROM does not
// compile against LedgerId (a LEDGER_DEFINE_STRONG_ID type fails
// morph::model::ModelKey's std::integral/std::string constraint) -- add
// a hand-written ActionKeyTraits<StoreTransaction> specialization
// instead, matching ledger_model.hpp's existing ActionKeyTraits<
// OpenAccount>/<GetLedger> pattern exactly (unwrap *action.ledgerId to
// the raw std::int64_t PrimaryKey Task 7 already declared on
// ModelKeyTraits<LedgerModel> -- do not declare it again, it is
// specialized exactly once):
BRIDGE_REGISTER_ACTION(ledger::LedgerModel, ledger::StoreTransaction, "StoreTransaction")

template <>
struct morph::model::ActionKeyTraits<ledger::StoreTransaction> {
    static constexpr bool hasKey = true;
    static constexpr bool fromResult = false;
    static std::string key(const ledger::StoreTransaction& action) {
        return morph::model::keyToString(*action.ledgerId);
    }
};
```

```cpp
// Append to examples/ledger/src/models/ledger_model.cpp
GetLedgerResult LedgerModel::execute(const StoreTransaction& action) {
    if (!action.validate()) {
        throw ValidationError{"StoreTransaction: description and at least two legs with engaged accountIds are required"};
    }
    Lightweight::DataMapper mapper;

    // Partition legs by the account's OWN currency, never a client-supplied
    // field (design spec §1) -- look up every referenced account first.
    std::map<std::string, morph::math::Rational> sumsByCurrency;
    std::vector<db::AccountRecord> legAccounts;
    legAccounts.reserve(action.legs.size());
    for (const auto& leg : action.legs) {
        auto rows = mapper.Query<db::AccountRecord>()
                        .Where(::Lightweight::FieldNameOf<&db::AccountRecord::id>, "=", *leg.accountId)
                        .All();
        if (rows.empty()) {
            throw NotFound{"StoreTransaction: no such account"};
        }
        legAccounts.push_back(rows.front());
        const std::string currency{legAccounts.back().currencyCode.Value().ToStringView()};
        auto it = sumsByCurrency.find(currency);
        if (it == sumsByCurrency.end()) {
            sumsByCurrency.emplace(currency, leg.amount);
        } else {
            it->second = it->second + leg.amount;
        }
    }
    for (const auto& [currency, sum] : sumsByCurrency) {
        if (sum.numerator != 0) {
            throw ZeroSumViolation{currency, "legs did not sum to zero"};
        }
    }

    // Constructor/commit shape copied verbatim from
    // bank::LoanModel::execute(const dto::TakeLoan&) (examples/bank/src/
    // models/loan_model.cpp:77-80) -- the exact multi-row-commit pattern
    // this rung's own StoreTransaction (journal + N legs) needs.
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    db::TransactionJournalRecord journalRow;
    journalRow.description = action.description;
    // DateTime->epoch-millis conversion copied verbatim from
    // bookmarks::db's own nowMs()/fromEpochMs() helpers
    // (bookmark_model.cpp:61-63): Timestamp::value is
    // std::optional<DateTime>, DateTime::value is
    // std::chrono::sys_time<milliseconds> -- .time_since_epoch().count()
    // gives the raw millisecond integer this entity column stores.
    // action.date is a client-supplied "when did this happen" field
    // (design spec §1) -- not a server audit stamp, so this does NOT go
    // through morph::ladder::now() (see this rung's own note on that
    // convention, which binds server-stamped timestamps like
    // ImportedOpRecord::appliedAtMs/ReportJobRecord::createdAtMs in later
    // tasks, not a client-supplied journal date).
    journalRow.date = action.date.value.has_value() ? (*action.date.value).value.time_since_epoch().count() : 0;
    auto ledgerRows = mapper.Query<db::LedgerRecord>()
                           .Where(::Lightweight::FieldNameOf<&db::LedgerRecord::id>, "=", *action.ledgerId)
                           .All();
    if (ledgerRows.empty()) {
        throw NotFound{"StoreTransaction: no such ledger"};
    }
    journalRow.ledger = ledgerRows.front();
    mapper.Create(journalRow);

    for (std::size_t i = 0; i < action.legs.size(); ++i) {
        db::TransactionLegRecord legRow;
        legRow.journal = journalRow;
        legRow.account = legAccounts[i];
        legRow.amountNum = action.legs[i].amount.numerator;
        legRow.amountDen = action.legs[i].amount.denominator;
        legRow.amountDp = static_cast<int>(action.legs[i].amount.decimalPlaces.value);
        legRow.currencyCode = legAccounts[i].currencyCode.Value();
        mapper.Create(legRow);
    }
    sqlTxn.Commit();

    return execute(GetLedger{.ledgerId = action.ledgerId});
}
```

`Lightweight::SqlTransaction`'s constructor/`Commit()` and `DateTime`'s
epoch-millis conversion are both verified above against real,
already-compiling code (`bank::LoanModel`, `bookmarks::db`'s `nowMs()`/
`fromEpochMs()`) — no further confirmation needed for those two. The one
remaining real implementation item this task must complete: **this
task's own `execute(GetLedger)` must be extended to compute each
account's balance as the real sum of its own legs**, not the hardcoded
zero Task 7 left it at (Task 7's own doc comment on that placeholder
says exactly this — "Task 8 computes a real balance"). Add a
per-account leg-sum query (`Query<db::TransactionLegRecord>().Where(...
account ...).All()`, summed via `Rational::operator+` in a loop, in-model
per design spec §3's own "never a raw SQL `SUM()`" rule, which applies
here too even though §3 is nominally about budgets — the same reason
holds: `Rational`'s per-row denominators can't be combined by SQL) inside
`execute(GetLedger)`'s existing per-account loop, replacing the hardcoded
zero. This is required for this task's own tests (above) to pass, since
they assert real non-zero balances.

- [ ] **Step 4: Run tests to verify they pass**

Run: `ctest --preset cl-debug -R "StoreTransaction" --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add examples/ledger/include/ledger/dto/transaction_dto.hpp \
        examples/ledger/include/ledger/models/ledger_model.hpp \
        examples/ledger/src/models/ledger_model.cpp \
        examples/ledger/tests/test_ledger_model.cpp
git commit -m "ledger: StoreTransaction -- per-currency zero-sum invariant"
```

---

## Task 9: Foreign-amount pairs (multi-currency)

**Files:**
- Modify: `examples/ledger/include/ledger/dto/transaction_dto.hpp`
- Modify: `examples/ledger/src/models/ledger_model.cpp`
- Modify: `examples/ledger/tests/test_ledger_model.cpp`

**Interfaces:**
- Produces: `TransactionLeg` gains an optional `foreignAmount:
  std::optional<Rational>` + `foreignCurrency: std::optional<Currency>`
  pair; `LedgerModel::execute(StoreTransaction)` excludes any leg's
  foreign-amount annotation from both partitions' zero-sum checks (design
  spec §1, step 3).

- [ ] **Step 1: Write the failing test**

**Correction from plan self-review**: rewritten with a concrete 4-leg
scenario satisfying the invariant's real wording ("legs sum to zero
*within each currency*") instead of an unbalanceable 2-leg sketch, and
using this plan's now-corrected model-construction/ledger-seeding
pattern.

```cpp
// Append to examples/ledger/tests/test_ledger_model.cpp
TEST_CASE("A foreign-amount pair balances USD and EUR partitions independently", "[ledger][model]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::LedgerModel model;
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "USD Checking",
                                       .kind = ledger::AccountKind::Asset, .currency = ledger::Currency::USD});
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "USD Travel Expense",
                                       .kind = ledger::AccountKind::Expense, .currency = ledger::Currency::USD});
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "EUR Wallet",
                                       .kind = ledger::AccountKind::Asset, .currency = ledger::Currency::EUR});
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "EUR Merchant Payable",
                                       .kind = ledger::AccountKind::Liability, .currency = ledger::Currency::EUR});
    auto ledgerState = model.execute(ledger::GetLedger{.ledgerId = ledgerId});
    auto usdChecking = ledgerState.accounts[0].id;
    auto usdExpense = ledgerState.accounts[1].id;
    auto eurWallet = ledgerState.accounts[2].id;
    auto eurPayable = ledgerState.accounts[3].id;

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    // A real 4-leg transaction: USD partition legs sum to zero on their
    // own (a -50.00/+50.00 pair), EUR partition legs sum to zero on their
    // own (a -45.23/+45.23 pair) -- the foreign-amount annotation on the
    // USD leg is display metadata only, never entering either check
    // (design spec §1 step 3).
    auto result = model.execute(ledger::StoreTransaction{
        .ledgerId = ledgerId,
        .description = "Travel expense with EUR receipt",
        .date = morph::time::Timestamp::now(),
        .legs = {ledger::TransactionLeg{.accountId = usdChecking,
                                         .amount = morph::math::Rational{Numerator{-5000}, Denominator{1},
                                                                          DecimalPlaces{2}},
                                         .foreignAmount = morph::math::Rational{Numerator{4523}, Denominator{1},
                                                                                 DecimalPlaces{2}},
                                         .foreignCurrency = ledger::Currency::EUR},
                 ledger::TransactionLeg{.accountId = usdExpense,
                                        .amount = morph::math::Rational{Numerator{5000}, Denominator{1},
                                                                         DecimalPlaces{2}}},
                 ledger::TransactionLeg{.accountId = eurWallet,
                                        .amount = morph::math::Rational{Numerator{-4523}, Denominator{1},
                                                                         DecimalPlaces{2}}},
                 ledger::TransactionLeg{.accountId = eurPayable,
                                        .amount = morph::math::Rational{Numerator{4523}, Denominator{1},
                                                                         DecimalPlaces{2}}}}});

    // No ZeroSumViolation thrown (implicit -- the call above would have
    // thrown otherwise); assert both currencies' balances landed correctly.
    auto findBalance = [&](ledger::AccountId id) {
        return std::ranges::find_if(result.accounts, [&](const auto& a) { return a.id == id; })->balance.numerator;
    };
    CHECK(findBalance(usdChecking) == -5000);
    CHECK(findBalance(usdExpense) == 5000);
    CHECK(findBalance(eurWallet) == -4523);
    CHECK(findBalance(eurPayable) == 4523);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "foreign.amount"  --output-on-failure`
Expected: FAIL — foreign-amount fields don't exist on `TransactionLeg` yet.

- [ ] **Step 3: Implement the foreign-amount fields and exclusion logic**

```cpp
// Modify TransactionLeg in transaction_dto.hpp:
struct TransactionLeg {
    AccountId accountId;
    morph::math::Rational amount;
    std::optional<morph::math::Rational> foreignAmount;    // display/audit metadata only --
    std::optional<Currency> foreignCurrency;                // never enters a zero-sum check (design spec §1 step 3)
};
```

In `LedgerModel::execute(StoreTransaction)`'s partitioning loop (Task 8),
the sum accumulation already reads only `leg.amount`/the account's own
`currencyCode` — no change needed there, since `foreignAmount`/
`foreignCurrency` were never read by that loop to begin with. Extend the
`TransactionLegRecord` creation loop to persist the optional triple
(mapping `std::nullopt` to Lightweight's own null representation for
each of `foreignAmountNum`/`Den`/`Dp`/`foreignCurrencyCode`,
unconditionally — always execute this assignment, never branch on
whether the leg has a foreign amount, since `std::optional`'s own empty
state already expresses "no foreign amount" through the column).

- [ ] **Step 4: Run tests to verify they pass**

Run: `ctest --preset cl-debug -R "foreign.amount" --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add examples/ledger/include/ledger/dto/transaction_dto.hpp \
        examples/ledger/src/models/ledger_model.cpp \
        examples/ledger/tests/test_ledger_model.cpp
git commit -m "ledger: foreign-amount pairs -- multi-currency, per-currency zero-sum stays intact"
```

---

## Task 10: `BudgetModel` — budgets and spent-so-far aggregation

**Files:**
- Create: `examples/ledger/include/ledger/dto/budget_dto.hpp`
- Create: `examples/ledger/include/ledger/models/budget_model.hpp`
- Create: `examples/ledger/src/models/budget_model.cpp`
- Test: `examples/ledger/tests/test_budget_model.cpp`

**Interfaces:**
- Consumes: `ledger::db::TransactionLegRecord`/`AccountRecord`/
  `CategoryRecord` (Task 5), `Lightweight::DataMapper::Query<T>`.
- Produces: `ledger::CreateBudget { ledgerId, name, categoryId }`,
  `ledger::SetBudgetLimit { budgetId, month, limit: Rational, currency:
  Currency }`, `ledger::GetBudgetReport { budgetId, month } ->
  GetBudgetReportResult { limit: Rational, spent: Rational, currency:
  Currency }`; `ledger::BudgetModel` keyed by `LedgerId`, implementing
  in-model summation per design spec §3 (never a raw SQL `SUM()` over the
  `Rational` columns).

**Correction from plan self-review**: the original draft's test was pure
prose with no code, and `budget_dto.hpp`/`budget_model.hpp`/`.cpp` had no
implementation at all. Written out fully below.

- [ ] **Step 1: Write the failing test**

```cpp
// examples/ledger/tests/test_budget_model.cpp
// SPDX-License-Identifier: Apache-2.0
#include "ledger/models/budget_model.hpp"
#include "ledger/models/ledger_model.hpp"
#include "testkit/db_fixture.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("GetBudgetReport sums matching legs in-model, exactly", "[ledger][budget]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::LedgerModel ledgerModel;
    ledgerModel.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "Checking",
                                             .kind = ledger::AccountKind::Asset, .currency = ledger::Currency::USD});
    ledgerModel.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "Groceries",
                                             .kind = ledger::AccountKind::Expense,
                                             .currency = ledger::Currency::USD});
    auto ledgerState = ledgerModel.execute(ledger::GetLedger{.ledgerId = ledgerId});
    auto checkingId = ledgerState.accounts[0].id;
    auto groceriesId = ledgerState.accounts[1].id;

    ledger::BudgetModel budgetModel;
    auto categoryId = budgetModel.execute(ledger::CreateCategory{.ledgerId = ledgerId, .name = "Food"});
    budgetModel.execute(ledger::LinkAccountToCategory{.accountId = groceriesId, .categoryId = categoryId});
    auto budgetId = budgetModel.execute(
        ledger::CreateBudget{.ledgerId = ledgerId, .name = "Monthly groceries", .categoryId = categoryId});
    budgetModel.execute(ledger::SetBudgetLimit{
        .budgetId = budgetId, .month = "2026-01",
        .limit = morph::math::Rational{morph::math::Numerator{20000}, morph::math::Denominator{1},
                                        morph::math::DecimalPlaces{2}},
        .currency = ledger::Currency::USD});

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    // Two StoreTransaction calls against Groceries, both dated in
    // January 2026 -- -30.00 and -45.50, summing to -75.50 spent.
    ledgerModel.execute(ledger::StoreTransaction{
        .ledgerId = ledgerId,
        .description = "Groceries 1",
        .date = morph::time::Timestamp::now(),  // confirm this actually lands in "2026-01" against the real
                                                  // system clock at implementation time, or construct an explicit
                                                  // January 2026 DateTime instead -- see Task 17's time_util.hpp
                                                  // for the eventual real month-boundary machinery this task can
                                                  // borrow from if `now()` doesn't reliably land in the test's
                                                  // expected month
        .legs = {ledger::TransactionLeg{.accountId = checkingId,
                                         .amount = morph::math::Rational{Numerator{-3000}, Denominator{1},
                                                                          DecimalPlaces{2}}},
                 ledger::TransactionLeg{.accountId = groceriesId,
                                        .amount = morph::math::Rational{Numerator{3000}, Denominator{1},
                                                                         DecimalPlaces{2}}}}});
    ledgerModel.execute(ledger::StoreTransaction{
        .ledgerId = ledgerId,
        .description = "Groceries 2",
        .date = morph::time::Timestamp::now(),
        .legs = {ledger::TransactionLeg{.accountId = checkingId,
                                         .amount = morph::math::Rational{Numerator{-4550}, Denominator{1},
                                                                          DecimalPlaces{2}}},
                 ledger::TransactionLeg{.accountId = groceriesId,
                                        .amount = morph::math::Rational{Numerator{4550}, Denominator{1},
                                                                         DecimalPlaces{2}}}}});

    auto report = budgetModel.execute(ledger::GetBudgetReport{.budgetId = budgetId, .month = "2026-01"});
    CHECK(report.spent.numerator == 7550);
    CHECK(report.limit.numerator == 20000);
}
```

This test assumes `CreateCategory`/`LinkAccountToCategory` actions exist
on `BudgetModel` for wiring an account to a category — the brief's own
Interfaces block (below) did not originally name these, but
`GetBudgetReport`'s "matching legs" concept (design spec §3) is undefined
without some way to say "this account belongs to this category." Added
to this task's own scope rather than left implicit.

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "budget" --output-on-failure`
Expected: FAIL to compile.

- [ ] **Step 3: Implement `budget_dto.hpp`/`budget_model.hpp`/`.cpp`**

```cpp
// examples/ledger/include/ledger/dto/budget_dto.hpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ledger/core/types.hpp"
#include "ledger/core/units.hpp"

#include <morph/forms/forms.hpp>
#include <morph/util/rational.hpp>

#include <string>

namespace ledger {

struct CreateCategory {
    LedgerId ledgerId;
    std::string name;

    [[nodiscard]] bool validate() const noexcept { return ledgerId.hasValue() && !name.empty(); }
};

struct LinkAccountToCategory {
    AccountId accountId;
    CategoryId categoryId;

    [[nodiscard]] bool validate() const noexcept { return accountId.hasValue() && categoryId.hasValue(); }
};

struct CreateBudget {
    LedgerId ledgerId;
    std::string name;
    CategoryId categoryId;

    [[nodiscard]] bool validate() const noexcept {
        return ledgerId.hasValue() && !name.empty() && categoryId.hasValue();
    }
};

struct SetBudgetLimit {
    BudgetId budgetId;
    std::string month;  // "YYYY-MM"
    morph::math::Rational limit;
    Currency currency;

    [[nodiscard]] bool validate() const noexcept { return budgetId.hasValue() && month.size() == 7; }
};

struct GetBudgetReport {
    BudgetId budgetId;
    std::string month;

    [[nodiscard]] bool validate() const noexcept { return budgetId.hasValue() && month.size() == 7; }
};

struct GetBudgetReportResult {
    morph::math::Rational limit;
    morph::math::Rational spent;
    Currency currency;
};

}  // namespace ledger
```

**Schema addition this task must make first**: `AccountRecord` (Task 5)
has no way to record which category an account belongs to.
`LinkAccountToCategory` needs one. Add a migration and an entity field:

```cpp
// Append to examples/ledger/src/db/schema.cpp -- additive-only per
// IMPLEMENTATION.md rule 4, a later timestamp than Task 4's own highest
// (20260819000011):
LIGHTWEIGHT_SQL_MIGRATION(20260819000012, "Add category_id to accounts") {
    plan.AlterTable("accounts").AddNotRequiredForeignKeyColumn("category_id", Bigint(), categoriesRef());
}
```

`AlterTable(std::string_view)` returns a `SqlAlterTableQueryBuilder`
(`Lightweight/SqlQuery/Migrate.hpp`), whose
`AddNotRequiredForeignKeyColumn(columnName, columnType,
referencedColumn)` is the verified real method for exactly this shape (a
nullable FK column added via `ALTER TABLE`) — confirmed directly against
the header rather than guessed; no existing rung's `schema.cpp` has an
`ALTER TABLE` migration to copy from, so this is the first one in the
codebase, but the method itself is real and unambiguous. `categoriesRef()`
is the same `SqlForeignKeyReferenceDefinition` helper Task 4's own
`schema.cpp` already declares in its anonymous namespace — reuse it, do
not redeclare.

```cpp
// Modify AccountRecord in examples/ledger/include/ledger/db/ledger_entity.hpp:
struct AccountRecord {
    static constexpr std::string_view TableName = "accounts";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&LedgerRecord::id, Light::SqlRealName{"ledger_id"}> ledger;  // 1
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"name"}> name;  // 2
    Light::Field<int, Light::SqlRealName{"kind"}> kind{0};  // 3
    Light::Field<Light::SqlAnsiString<3>, Light::SqlRealName{"currency_code"}> currencyCode;  // 4
    Light::BelongsTo<&CategoryRecord::id, Light::SqlRealName{"category_id"}, Light::SqlNullable::Null>
        category;  // 5 -- nullable, per bank::db::TxnRecord's own nullable-BelongsTo shape
};
```

This changes `AccountRecord`'s member-index comments (a new member at
index 5) — confirm this doesn't break anything relying on the old
4-member layout (nothing should, since Lightweight's `DataMapper` matches
columns by name via `Light::SqlRealName`, not by ordinal position, except
for `HasMany` resolution — this entity has none). Also note
`CategoryRecord` must now be forward-declared or fully declared before
`AccountRecord` in this header — confirm the file's declaration order
still works (it should, since `CategoryRecord` doesn't depend on
`AccountRecord`) or reorder the structs if not.

```cpp
// examples/ledger/include/ledger/models/budget_model.hpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ledger/dto/budget_dto.hpp"

#include <morph/core/registry.hpp>

namespace ledger {

/// @brief Budgets, limits, and in-model spent-so-far aggregation (design
///        spec §3). Plain default-constructible, per LedgerModel's own
///        corrected shape (Task 7) -- every action carries its own key.
class BudgetModel {
  public:
    CategoryId execute(const CreateCategory& action);
    AccountId execute(const LinkAccountToCategory& action);
    BudgetId execute(const CreateBudget& action);
    BudgetId execute(const SetBudgetLimit& action);
    GetBudgetReportResult execute(const GetBudgetReport& action);
};

}  // namespace ledger

BRIDGE_REGISTER_MODEL(ledger::BudgetModel, "BudgetModel")
BRIDGE_REGISTER_ACTION(ledger::BudgetModel, ledger::CreateCategory, "CreateCategory")
BRIDGE_REGISTER_ACTION(ledger::BudgetModel, ledger::LinkAccountToCategory, "LinkAccountToCategory")
BRIDGE_REGISTER_ACTION(ledger::BudgetModel, ledger::CreateBudget, "CreateBudget")
BRIDGE_REGISTER_ACTION(ledger::BudgetModel, ledger::SetBudgetLimit, "SetBudgetLimit")
BRIDGE_REGISTER_ACTION(ledger::BudgetModel, ledger::GetBudgetReport, "GetBudgetReport", ::morph::model::Loggable::No)

// Hand-written ActionKeyTraits per action, exactly as Task 7's real,
// verified discovery established for LedgerModel: LEDGER_DEFINE_STRONG_ID
// types (LedgerId, BudgetId, AccountId, CategoryId) all fail
// morph::model::ModelKey's std::integral/std::string constraint, so
// BRIDGE_MODEL_KEY/BRIDGE_KEY_FROM cannot be used for any of them.
// BudgetModel's actions carry genuinely different key TYPES
// (LedgerId for CreateCategory/CreateBudget, BudgetId for
// SetBudgetLimit/GetBudgetReport) -- since ModelKeyTraits<BudgetModel>
// declares one PrimaryKey type for the whole model (see Task 7's own
// ModelKeyTraits<LedgerModel> -- std::int64_t, specialized exactly once),
// and every one of these ids already unwraps to the same underlying
// std::int64_t, PrimaryKey = std::int64_t here too; each action's own
// key() just unwraps whichever field it carries. LinkAccountToCategory
// carries two ids (accountId, categoryId) and no single natural
// "the" key -- confirm against ActionKeyTraits::hasKey's actual
// contract (include/morph/core/model_key.hpp) whether hasKey = false
// (this action doesn't route to an existing shared instance the way a
// keyed action does) is the correct answer for it, rather than picking
// one of its two ids arbitrarily.
template <>
struct morph::model::ModelKeyTraits<ledger::BudgetModel> {
    using PrimaryKey = std::int64_t;
};
template <>
struct morph::model::ActionKeyTraits<ledger::CreateCategory> {
    static constexpr bool hasKey = true;
    static constexpr bool fromResult = false;
    static std::string key(const ledger::CreateCategory& action) {
        return morph::model::keyToString(*action.ledgerId);
    }
};
template <>
struct morph::model::ActionKeyTraits<ledger::CreateBudget> {
    static constexpr bool hasKey = true;
    static constexpr bool fromResult = false;
    static std::string key(const ledger::CreateBudget& action) {
        return morph::model::keyToString(*action.ledgerId);
    }
};
template <>
struct morph::model::ActionKeyTraits<ledger::SetBudgetLimit> {
    static constexpr bool hasKey = true;
    static constexpr bool fromResult = false;
    static std::string key(const ledger::SetBudgetLimit& action) {
        return morph::model::keyToString(*action.budgetId);
    }
};
template <>
struct morph::model::ActionKeyTraits<ledger::GetBudgetReport> {
    static constexpr bool hasKey = true;
    static constexpr bool fromResult = false;
    static std::string key(const ledger::GetBudgetReport& action) {
        return morph::model::keyToString(*action.budgetId);
    }
};
```

`LinkAccountToCategory`'s own `ActionKeyTraits` is intentionally omitted
above — confirm against `include/morph/core/model_key.hpp`'s real
`hasKey = false` contract (an unkeyed action, dispatched without routing
to a specific shared instance) before writing it, rather than picking
one of its two ids as "the" key arbitrarily. Both `LinkAccountToCategory`
and `SetBudgetLimit` changed from `void` to returning an id (`AccountId`/
`BudgetId` respectively), per Task 7's own real, verified discovery that
`BRIDGE_REGISTER_ACTION`'s `Result` deduction cannot bind a `void`
`execute()` — return the affected row's own id (already available from
the lookup each body performs) rather than inventing a new return value.

```cpp
// examples/ledger/src/models/budget_model.cpp
// SPDX-License-Identifier: Apache-2.0
#include "ledger/core/errors.hpp"
#include "ledger/db/ledger_entity.hpp"
#include "ledger/models/budget_model.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>

namespace ledger {

CategoryId BudgetModel::execute(const CreateCategory& action) {
    if (!action.validate()) {
        throw ValidationError{"CreateCategory: ledgerId and name are required"};
    }
    Lightweight::DataMapper mapper;
    auto ledgerRows = mapper.Query<db::LedgerRecord>()
                           .Where(::Lightweight::FieldNameOf<&db::LedgerRecord::id>, "=", *action.ledgerId)
                           .All();
    if (ledgerRows.empty()) {
        throw NotFound{"CreateCategory: no such ledger"};
    }
    db::CategoryRecord categoryRow;
    categoryRow.ledger = ledgerRows.front();
    categoryRow.name = action.name;
    mapper.Create(categoryRow);
    return CategoryId{static_cast<std::int64_t>(categoryRow.id.Value())};
}

AccountId BudgetModel::execute(const LinkAccountToCategory& action) {
    if (!action.validate()) {
        throw ValidationError{"LinkAccountToCategory: accountId and categoryId are required"};
    }
    Lightweight::DataMapper mapper;
    auto accountRows = mapper.Query<db::AccountRecord>()
                            .Where(::Lightweight::FieldNameOf<&db::AccountRecord::id>, "=", *action.accountId)
                            .All();
    auto categoryRows = mapper.Query<db::CategoryRecord>()
                             .Where(::Lightweight::FieldNameOf<&db::CategoryRecord::id>, "=", *action.categoryId)
                             .All();
    if (accountRows.empty() || categoryRows.empty()) {
        throw NotFound{"LinkAccountToCategory: no such account or category"};
    }
    accountRows.front().category = categoryRows.front();
    mapper.Update(accountRows.front());
    return AccountId{static_cast<std::int64_t>(accountRows.front().id.Value())};
}

BudgetId BudgetModel::execute(const CreateBudget& action) {
    if (!action.validate()) {
        throw ValidationError{"CreateBudget: ledgerId, name, and categoryId are required"};
    }
    Lightweight::DataMapper mapper;
    auto ledgerRows = mapper.Query<db::LedgerRecord>()
                           .Where(::Lightweight::FieldNameOf<&db::LedgerRecord::id>, "=", *action.ledgerId)
                           .All();
    auto categoryRows = mapper.Query<db::CategoryRecord>()
                             .Where(::Lightweight::FieldNameOf<&db::CategoryRecord::id>, "=", *action.categoryId)
                             .All();
    if (ledgerRows.empty() || categoryRows.empty()) {
        throw NotFound{"CreateBudget: no such ledger or category"};
    }
    db::BudgetRecord budgetRow;
    budgetRow.ledger = ledgerRows.front();
    budgetRow.name = action.name;
    budgetRow.category = categoryRows.front();
    mapper.Create(budgetRow);
    return BudgetId{static_cast<std::int64_t>(budgetRow.id.Value())};
}

BudgetId BudgetModel::execute(const SetBudgetLimit& action) {
    if (!action.validate()) {
        throw ValidationError{"SetBudgetLimit: budgetId and a YYYY-MM month are required"};
    }
    Lightweight::DataMapper mapper;
    auto budgetRows = mapper.Query<db::BudgetRecord>()
                           .Where(::Lightweight::FieldNameOf<&db::BudgetRecord::id>, "=", *action.budgetId)
                           .All();
    if (budgetRows.empty()) {
        throw NotFound{"SetBudgetLimit: no such budget"};
    }
    db::BudgetLimitRecord limitRow;
    limitRow.budget = budgetRows.front();
    limitRow.month = action.month;
    limitRow.limitNum = action.limit.numerator;
    limitRow.limitDen = action.limit.denominator;
    limitRow.limitDp = static_cast<int>(action.limit.decimalPlaces.value);
    limitRow.currencyCode = currencyToCode(action.currency);  // Task 7's helper
    mapper.Create(limitRow);
    return action.budgetId;
}

GetBudgetReportResult BudgetModel::execute(const GetBudgetReport& action) {
    if (!action.validate()) {
        throw ValidationError{"GetBudgetReport: budgetId and a YYYY-MM month are required"};
    }
    Lightweight::DataMapper mapper;
    auto budgetRows = mapper.Query<db::BudgetRecord>()
                           .Where(::Lightweight::FieldNameOf<&db::BudgetRecord::id>, "=", *action.budgetId)
                           .All();
    if (budgetRows.empty()) {
        throw NotFound{"GetBudgetReport: no such budget"};
    }
    auto limitRows = mapper.Query<db::BudgetLimitRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::BudgetLimitRecord::budget>, "=", *action.budgetId)
                          .Where(::Lightweight::FieldNameOf<&db::BudgetLimitRecord::month>, "=", action.month)
                          .All();
    // In-model summation, never a raw SQL SUM() over the Rational columns
    // (design spec §3 -- SQL cannot combine differing per-row denominators
    // meaningfully). This task's scope: sum every leg whose account is
    // linked to this budget's category and whose journal falls in the
    // named month -- deferred to a real query once LinkAccountToCategory's
    // schema addition (above) lands; for now, fetch all
    // TransactionLegRecord rows for the category's accounts within the
    // month's date range and sum with Rational::operator+ in a loop
    // (bounded fetch -- see this task's own headroom note once Task 13's
    // fuzz test exists).
    morph::math::Rational spent{morph::math::Numerator{0}, morph::math::Denominator{1},
                                 morph::math::DecimalPlaces{2}};
    // for (const auto& leg : mapper.Query<db::TransactionLegRecord>()...) { spent = spent + ...; }
    Currency currency = Currency::USD;
    morph::math::Rational limit = spent;
    if (!limitRows.empty()) {
        limit = morph::math::Rational{morph::math::Numerator{limitRows.front().limitNum.Value()},
                                       morph::math::Denominator{limitRows.front().limitDen.Value()},
                                       morph::math::DecimalPlaces{
                                           static_cast<std::uint32_t>(limitRows.front().limitDp.Value())}};
        currency = codeToCurrency(limitRows.front().currencyCode.Value().ToStringView());
    }
    return GetBudgetReportResult{.limit = limit, .spent = spent, .currency = currency};
}

}  // namespace ledger
```

The `spent` aggregation's actual leg-summing loop is deliberately left as
a commented-out sketch rather than guessed further: it depends on the
`LinkAccountToCategory` schema addition landing first (a real,
non-optional part of this task, not a stretch goal), and on a concrete
decision for how "the journal falls in the named month" translates to a
date-range query against `TransactionJournalRecord.date` (stored as
epoch millis) — resolve both, then complete the loop, before this task's
tests can pass. This is this task's real remaining work, not something
to leave unfinished.

- [ ] **Step 4: Run tests to verify they pass**

Run: `ctest --preset cl-debug -R "budget" --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add examples/ledger/include/ledger/dto/budget_dto.hpp \
        examples/ledger/include/ledger/models/budget_model.hpp \
        examples/ledger/src/models/budget_model.cpp \
        examples/ledger/tests/test_budget_model.cpp \
        examples/ledger/CMakeLists.txt
git commit -m "ledger: BudgetModel -- budgets, limits, in-model spent-so-far aggregation"
```

---

## Task 11: Empty-principal refusal

**Files:**
- Modify: `examples/ledger/src/models/ledger_model.cpp`
- Modify: `examples/ledger/src/models/budget_model.cpp`
- Modify: `examples/ledger/tests/test_ledger_model.cpp`
- Modify: `examples/ledger/tests/test_budget_model.cpp`

**Correction from plan self-review**: `morph::session::Context::principal`
(`include/morph/session/session.hpp`) is a plain `std::string` — empty
string means "no principal," never `std::optional`/`.hasValue()`. The
accessor is `morph::session::current()`, returning `const Context*`
(`nullptr` outside any dispatch/test scope, per
`tests/test_coverage_push95.cpp`'s own "returns nullptr outside any
ScopedContext" test). The real test-time mechanism to drive a scenario
under a specific (or empty) principal is `morph::session::detail::
ScopedContext` (a RAII context-installer), following the exact pattern
`bookmarks::tests::test_bookmark_model.cpp`'s own `ScopedPrincipal`
helper already establishes (`contextFor(principal)` builds a `Context`,
`ScopedContext{ctx}` installs it for the guard's lifetime) — copied
verbatim below rather than reinvented.

**Interfaces:**
- Consumes: `morph::session::current()` (returns `const Context*`,
  `nullptr` outside any dispatch), `Context::principal` (a plain
  `std::string`, empty means unauthenticated).
- Produces: every mutating `execute()` overload on `LedgerModel` and
  `BudgetModel` throws `EmptyPrincipalError` as its first statement when
  `session::current()` is `nullptr` or its `principal` is empty (design
  spec §11).

- [ ] **Step 1: Write the failing test**

```cpp
// Append to examples/ledger/tests/test_ledger_model.cpp -- add near the
// top of the file, in an anonymous namespace, mirroring
// bookmarks::tests::test_bookmark_model.cpp's own contextFor/ScopedPrincipal:
namespace {
[[nodiscard]] morph::session::Context contextFor(std::string principal) {
    morph::session::Context ctx;
    ctx.principal = std::move(principal);
    return ctx;
}

class ScopedPrincipal {
  public:
    explicit ScopedPrincipal(std::string principal) : _ctx{contextFor(std::move(principal))}, _scope{_ctx} {}

  private:
    morph::session::Context _ctx;
    morph::session::detail::ScopedContext _scope;
};
}  // namespace

TEST_CASE("StoreTransaction refuses an empty principal", "[ledger][model][security]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::LedgerModel model;
    ScopedPrincipal empty{""};  // installs a Context with an empty principal for this scope
    CHECK_THROWS_AS(
        model.execute(ledger::StoreTransaction{.ledgerId = ledgerId, .description = "Should be refused",
                                                 .date = morph::time::Timestamp::now(), .legs = {}}),
        ledger::EmptyPrincipalError);
}
```

Note `legs = {}` (empty) here would ALSO fail `validate()`'s own
"at least two legs" check with `ValidationError`, not
`EmptyPrincipalError` — since the empty-principal check must run FIRST,
before `validate()`, this test's assertion is only meaningful if the
principal check genuinely happens before validation. If it does not, this
test would pass for the wrong reason (throwing `ValidationError`, which
`CHECK_THROWS_AS` would fail to match against `EmptyPrincipalError`
anyway — so this particular test shape is self-checking on that point,
not a false-positive risk).

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "empty.principal" --output-on-failure`
Expected: FAIL — no such check exists yet.

- [ ] **Step 3: Add the principal check as the first statement of every mutating `execute()`**

```cpp
// At the top of LedgerModel::execute(StoreTransaction) and every other
// mutating overload (OpenAccount too -- it is also a mutation):
const auto* ctx = morph::session::current();
if (ctx == nullptr || ctx->principal.empty()) {
    throw EmptyPrincipalError{};
}
```

Add `#include <morph/session/session.hpp>` to `ledger_model.cpp` if not
already transitively included. Apply the identical check (first
statement, before `validate()`) to every mutating `execute()` overload
on both `LedgerModel` (`OpenAccount`, `StoreTransaction`) and
`BudgetModel` (`CreateCategory`, `LinkAccountToCategory`, `CreateBudget`,
`SetBudgetLimit`) — `GetLedger`/`GetBudgetReport` are reads and stay
exempt (design spec §11 only binds mutations).

Add the equivalent test for `BudgetModel`:

```cpp
// Append to examples/ledger/tests/test_budget_model.cpp -- reuse the
// same contextFor/ScopedPrincipal helper pattern, added to this file's
// own anonymous namespace (or a small shared testkit header if the
// duplication across test_ledger_model.cpp/test_budget_model.cpp
// bothers the implementer -- not required by this task, since both
// rungs' own tests, e.g. bookmarks' three separate test files, each
// declare their own local copy rather than share one).
TEST_CASE("CreateCategory refuses an empty principal", "[ledger][budget][security]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::BudgetModel model;
    ScopedPrincipal empty{""};
    CHECK_THROWS_AS(model.execute(ledger::CreateCategory{.ledgerId = ledgerId, .name = "Food"}),
                     ledger::EmptyPrincipalError);
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `ctest --preset cl-debug -R "empty.principal" --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add examples/ledger/src/models/ledger_model.cpp \
        examples/ledger/src/models/budget_model.cpp \
        examples/ledger/tests/test_ledger_model.cpp \
        examples/ledger/tests/test_budget_model.cpp
git commit -m "ledger: refuse empty-principal writes at the model (design spec §11)"
```

---

## Task 11a: `LedgerModel`/`BudgetModel` self-journaling infrastructure (retrofit)

**Inserted during SDD execution, before Task 12**: Task 12's cascade-
journaling requires `LedgerModel` to append a manually-constructed
`LogEntry` for a rule cascade, with `causalParentId` set. This is only
possible if the model already journals its *own* triggering actions —
kanban's real, already-implemented pattern
(`ladder-kanban-impl:examples/kanban/src/models/board_model.{hpp,cpp}`,
unmerged) shows why: a plain-constructed model (`LedgerModel model;`, as
every test in this plan already does) is never wrapped by the
framework's registry `IModelHolder`, so `IModelHolder::attachActionLog`/
`recordIfAttached`'s automatic per-call journaling never fires for it —
`model.execute(action)` calls `LedgerModel::execute` directly, bypassing
the dispatcher entirely. `BoardModel` therefore keeps its own
`shared_ptr<IActionLog>`, attached explicitly via a model-level
`attachActionLog(log, entityKey)` method, and calls a private
`logAction(action, result, causalParentId = {})` helper at the end of
**every** successful mutating `execute()` — not just the ones that might
cascade. This task retrofits that same infrastructure into
`LedgerModel`/`BudgetModel` before Task 12 needs to append a cascade
entry on top of it.

**Files:**
- Modify: `examples/ledger/include/ledger/models/ledger_model.hpp`
- Modify: `examples/ledger/src/models/ledger_model.cpp`
- Modify: `examples/ledger/include/ledger/models/budget_model.hpp`
- Modify: `examples/ledger/src/models/budget_model.cpp`
- Test: `examples/ledger/tests/test_ledger_model.cpp`,
  `examples/ledger/tests/test_budget_model.cpp`

**Interfaces:**
- Consumes: `morph::journal::IActionLog`, `morph::journal::LogEntry`,
  `morph::model::ActionTraits<Action>::typeId()`/`toJson()`/
  `resultToJson()` (`include/morph/journal/action_log.hpp`,
  `include/morph/core/registry.hpp`).
- Produces: `LedgerModel::attachActionLog(shared_ptr<IActionLog>,
  std::string entityKey)` and a private `logAction(action, result,
  causalParentId = {})` template, called unconditionally (no-op when no
  log is attached) at the end of every mutating `execute()`
  (`OpenAccount`, `StoreTransaction`). Identical shape on `BudgetModel`
  for `CreateCategory`, `LinkAccountToCategory`, `CreateBudget`,
  `SetBudgetLimit`. No behavior change for any existing test — none of
  them call `attachActionLog`, so `_log` stays null and `logAction`
  no-ops exactly as before this task.

- [ ] **Step 1: Read kanban's real `attachActionLog`/`logAction` pair in full**

Read `attachActionLog`'s doc comment and `logAction`'s implementation
(cited above) in full before writing anything — the doc comment's own
explanation of *why* a plain-constructed model needs this (not the
framework's automatic path) is the fact this task exists to apply.

- [ ] **Step 2: Write the failing test proving `logAction` fires (and no-ops without an attached log)**

```cpp
// Append to examples/ledger/tests/test_ledger_model.cpp
#include <morph/journal/action_log.hpp>

TEST_CASE("OpenAccount records a LogEntry once a log is attached, and is a no-op without one", "[ledger][model][journal]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::LedgerModel model;

    // No log attached: succeeds, no crash, nothing recorded anywhere to
    // check against -- this half of the test exists to prove the no-op
    // path doesn't throw or misbehave when _log is null.
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "Checking",
                                       .kind = ledger::AccountKind::Asset, .currency = ledger::Currency::USD});

    // Attach a log, then repeat -- this call must be recorded.
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    model.attachActionLog(log, std::to_string(*ledgerId));
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "Savings",
                                       .kind = ledger::AccountKind::Asset, .currency = ledger::Currency::USD});

    auto entries = log->entries();
    REQUIRE(entries.size() == 1);  // only the second call was journaled -- the first ran before attachActionLog
    CHECK(entries[0].actionType == "OpenAccount");
    CHECK(entries[0].outcome == morph::journal::Outcome::Succeeded);
    CHECK(entries[0].entityKey == std::to_string(*ledgerId));
}
```

Confirm `morph::journal::InMemoryActionLog`'s exact constructor/`entries()`
signature against `tests/test_action_log.cpp`'s own usage before
finalizing — this plan has not independently verified that specific type
the way it has verified other framework surfaces in this session; if the
constructor or `entries()` shape differs from what's written above,
match the real header rather than guessing further.

- [ ] **Step 3: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "records a LogEntry" --output-on-failure`
Expected: FAIL to compile — `attachActionLog` doesn't exist on
`LedgerModel` yet.

- [ ] **Step 4: Implement `attachActionLog`/`logAction` on `LedgerModel`**

```cpp
// Append to LedgerModel's class body in ledger_model.hpp:
public:
    /// @brief Attaches a durable action log and this instance's stable
    ///        identity, so every subsequent mutating `execute()` records
    ///        a `morph::journal::LogEntry`. Model-level mirror of
    ///        `morph::model::detail::IModelHolder::attachActionLog` for a
    ///        plain-constructed instance that never goes through the
    ///        framework's registry/dispatcher path (see this class's own
    ///        file-level doc comment, or Task 11a's own plan text, for
    ///        why that path never fires for a directly-constructed
    ///        LedgerModel).
    /// @param log Sink entries are forwarded to.
    /// @param entityKey Stable identity stamped onto every LogEntry this
    ///        instance produces (this rung's ledger id, as a string).
    void attachActionLog(std::shared_ptr<::morph::journal::IActionLog> log, std::string entityKey);

private:
    /// @brief Records @p action/@p result as a LogEntry if a log is
    ///        attached; no-op otherwise.
    /// @tparam Action Concrete action type.
    /// @tparam Result Concrete result type.
    /// @param action The executed action.
    /// @param result The action's result.
    /// @param causalParentId Empty (the default) for every ordinary call
    ///        site; Task 12's evaluateRules is the only caller that
    ///        passes a non-empty value.
    template <typename Action, typename Result>
    void logAction(const Action& action, const Result& result, std::string causalParentId = {}) const;

    std::optional<std::string> _entityKeyStr;
    std::shared_ptr<::morph::journal::IActionLog> _log;
```

```cpp
// Append to ledger_model.cpp:
void LedgerModel::attachActionLog(std::shared_ptr<::morph::journal::IActionLog> log, std::string entityKey) {
    _log = std::move(log);
    _entityKeyStr = std::move(entityKey);
}

template <typename Action, typename Result>
void LedgerModel::logAction(const Action& action, const Result& result, std::string causalParentId) const {
    if (!_log) {
        return;
    }
    ::morph::journal::LogEntry entry;
    entry.modelType = "LedgerModel";
    entry.entityKey = _entityKeyStr.value_or(std::string{});
    entry.actionType = std::string{::morph::model::ActionTraits<Action>::typeId()};
    entry.payload = ::morph::model::ActionTraits<Action>::toJson(action);
    entry.result = ::morph::model::ActionTraits<Action>::resultToJson(result);
    entry.outcome = ::morph::journal::Outcome::Succeeded;
    if (const auto* ctx = ::morph::session::current()) {
        entry.principal = ctx->principal;
    }
    entry.timestampMs = (*morph::ladder::now().value).value.time_since_epoch().count();  // server-stamped audit
                                                                                            // timestamp -- goes
                                                                                            // through the ladder's
                                                                                            // injectable clock
                                                                                            // convention, unlike
                                                                                            // StoreTransaction's own
                                                                                            // client-supplied date
    entry.causalParentId = std::move(causalParentId);
    _log->append(std::move(entry));
    // See kanban's own identical comment (design spec §5's citation) for
    // why this flush is load-bearing, not optional: append() writes
    // through buffered C stdio with no implicit flush for FileActionLog,
    // and entries() reads through a separate stream that cannot see
    // unflushed bytes. InMemoryActionLog::flush() is a no-op, so this
    // costs nothing for the log type most tests attach.
    _log->flush();
}

// Add explicit instantiations for every (Action, Result) pair this file
// actually calls logAction with, at the bottom of the file (templates
// defined in a .cpp need this, since nothing outside this TU calls
// logAction directly) -- confirm the exact instantiation-declaration
// syntax against an existing rung's own template method defined in a
// .cpp if this doesn't compile as a bare member-function-template
// definition; ledger_model.cpp already has the includes logAction's own
// body needs (registry.hpp via ledger_model.hpp, journal headers added
// here).
```

Add `logAction(action, result);` as the last statement before each
`return` in `execute(OpenAccount)` and `execute(StoreTransaction)` (after
the mutation has genuinely committed — for `StoreTransaction`, after
`sqlTxn.Commit()`, mirroring kanban's own placement).

Add `#include <morph/journal/action_log.hpp>` and
`#include <morph/session/session.hpp>` to `ledger_model.hpp`/`.cpp` as
needed (Task 11 already added the session include if this branch already
has it).

- [ ] **Step 5: Run tests to verify they pass**

Run: `ctest --preset cl-debug -R "records a LogEntry" --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Repeat Steps 2-5 for `BudgetModel`**

Identical shape, `modelType = "BudgetModel"`, `logAction` called at the
end of `CreateCategory`, `LinkAccountToCategory`, `CreateBudget`,
`SetBudgetLimit` (never the two read-only actions). Add the equivalent
test to `test_budget_model.cpp`.

- [ ] **Step 7: Run the full ledger test suite to confirm no regressions**

Run: `ctest --preset cl-debug -L ladder-ledger --output-on-failure`
Expected: PASS — every existing test still passes unchanged, since none
of them attach a log (this task is additive-only in effect).

- [ ] **Step 8: Commit**

```bash
git add examples/ledger/include/ledger/models/ledger_model.hpp \
        examples/ledger/src/models/ledger_model.cpp \
        examples/ledger/include/ledger/models/budget_model.hpp \
        examples/ledger/src/models/budget_model.cpp \
        examples/ledger/tests/test_ledger_model.cpp \
        examples/ledger/tests/test_budget_model.cpp
git commit -m "ledger: self-journaling infrastructure on LedgerModel/BudgetModel (attachActionLog/logAction, retrofit for Task 12)"
```

---

## Task 11b: `StoreTransaction` exactly-once (opId + applied-ops ledger)

**Inserted during SDD execution, before Task 12**: pre-verifying Task
12's divergence test surfaced a real, pre-existing gap in Task 8's
`execute(StoreTransaction)`: unlike kanban's `MoveTaskPosition`
(naturally idempotent — replaying it just re-sets a task to the same
position), `StoreTransaction` is a pure insert (a new
`TransactionJournalRecord` + legs every call). `morph::journal::
replay()` re-dispatches every recorded entry against a fresh model
instance — including the trigger `StoreTransaction` entry itself, not
just the cascade — so without an idempotency mechanism, replay would
insert a *second* journal+legs row and double every affected balance.
This is exactly the "exactly-once" cross-cutting strain LADDER.md names
(`kanban` establishes the pattern at rung 4; `ledger` was always meant
to "re-test it with money," per LADDER.md's own recurring-strains list)
— not a new invention, a scope item this plan should have picked up
earlier and didn't. Fixed now, before Task 12 needs replay to actually
converge.

**Files:**
- Modify: `examples/ledger/include/ledger/dto/transaction_dto.hpp`
- Modify: `examples/ledger/include/ledger/db/ledger_entity.hpp`
- Modify: `examples/ledger/src/db/schema.cpp`
- Modify: `examples/ledger/src/models/ledger_model.cpp`
- Test: `examples/ledger/tests/test_ledger_model.cpp`

**Interfaces:**
- Consumes: the exact pattern kanban's real, verified
  `execute(MoveTaskPosition)` already establishes
  (`ladder-kanban-impl:examples/kanban/src/models/board_model.cpp`):
  client-supplied `opId`, a server-side applied-ops table storing the
  full serialized result, checked (after any auth/role gate — none
  applies to `StoreTransaction` beyond Task 11's empty-principal check,
  already the first statement) before any re-validation or mutation. A
  hit returns the stored result verbatim, with **no re-journaling** (a
  ledger hit performed nothing new, so nothing new is logged — confirmed
  against kanban's own doc comment on this exact point, itself verified
  against a live capture: the framework's own auto-append does not
  double-log this path, so skipping `logAction` on a ledger hit is
  correct, not a workaround).
- Produces: `StoreTransaction` gains an `opId: ImportOpId` field
  (reusing bookmarks' own `ImportOpId` type/shape per design spec §8's
  already-established rule-of-three tracking — this is the *second*
  occurrence of the same op-id-ledger pattern in this rung, after §8's
  own import dedup; a third occurrence anywhere in the ladder would
  trigger `IMPLEMENTATION.md`'s promotion rule, noted for whoever builds
  the next rung needing it). `ledger_applied_ops` table + `AppliedOpRecord`
  entity, keyed by `(ledger_id, op_id)`.

- [ ] **Step 1: Write the failing test for opId replay-safety**

```cpp
// Append to examples/ledger/tests/test_ledger_model.cpp
TEST_CASE("StoreTransaction with a repeated opId is a safe no-op, not a second insert", "[ledger][model][exactly-once]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ScopedPrincipal principal{"alice"};
    ledger::LedgerModel model;
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "Checking",
                                       .kind = ledger::AccountKind::Asset, .currency = ledger::Currency::USD});
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "Groceries",
                                       .kind = ledger::AccountKind::Expense, .currency = ledger::Currency::USD});
    auto ledgerState = model.execute(ledger::GetLedger{.ledgerId = ledgerId});

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    const auto opId = ledger::ImportOpId::fromOptional(std::optional<std::string>{"txn-op-1"});
    const auto txn = ledger::StoreTransaction{
        .ledgerId = ledgerId, .description = "Groceries", .date = morph::time::Timestamp::now(), .opId = opId,
        .legs = {ledger::TransactionLeg{.accountId = ledgerState.accounts[0].id,
                                         .amount = morph::math::Rational{Numerator{-3000}, Denominator{1},
                                                                          DecimalPlaces{2}}},
                 ledger::TransactionLeg{.accountId = ledgerState.accounts[1].id,
                                        .amount = morph::math::Rational{Numerator{3000}, Denominator{1},
                                                                         DecimalPlaces{2}}}}};

    auto first = model.execute(txn);
    auto second = model.execute(txn);  // identical opId -- must be a no-op replay, not a second insert

    auto findBalance = [&](ledger::AccountId id, const ledger::GetLedgerResult& r) {
        return std::ranges::find_if(r.accounts, [&](const auto& a) { return a.id == id; })->balance.numerator;
    };
    CHECK(findBalance(ledgerState.accounts[0].id, first) == -3000);
    CHECK(findBalance(ledgerState.accounts[0].id, second) == -3000);  // still -3000, not -6000
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "exactly-once" --output-on-failure`
Expected: FAIL — `opId` field doesn't exist on `StoreTransaction` yet.

- [ ] **Step 3: Add `opId` to `StoreTransaction`, the `ledger_applied_ops` table/entity**

```cpp
// Modify StoreTransaction in transaction_dto.hpp -- reuse bookmarks'
// ImportOpId shape verbatim (per design spec §8's own citation):
#include "ledger/core/import_op_id.hpp"  // or wherever this task declares ImportOpId -- see note below

struct StoreTransaction {
    LedgerId ledgerId;
    std::string description;
    morph::time::Timestamp date;
    std::vector<TransactionLeg> legs;
    ImportOpId opId;

    [[nodiscard]] bool validate() const noexcept {
        return ledgerId.hasValue() && !description.empty() && legs.size() >= 2 &&
               std::ranges::all_of(legs, [](const auto& leg) { return leg.accountId.hasValue(); });
    }
};
```

`ImportOpId` was originally scoped to Task 15 (import dedup) in this
plan's own design spec §8 discussion, but this task needs it first —
declare it now, in a small shared header both this task and Task 15 can
include (e.g. `ledger/core/import_op_id.hpp`), rather than duplicating
the type. Shape copied verbatim from
`examples/bookmarks/include/bookmarks/core/types.hpp`'s `ImportOpId`
(`std::optional<std::string> value`, `hasValue()`, `fromOptional()`).

```cpp
// Append to examples/ledger/src/db/schema.cpp:
LIGHTWEIGHT_SQL_MIGRATION(20260819000013, "Create ledger_applied_ops table") {
    plan.CreateTableIfNotExists("ledger_applied_ops")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("ledger_id", Bigint(), ledgersRef())
        .RequiredColumn("op_id", Varchar(128))
        .RequiredColumn("result_json", NVarchar(0))
        .RequiredColumn("created_at_ms", Bigint());
    plan.CreateUniqueIndex("idx_ledger_applied_ops_ledger_op", "ledger_applied_ops", {"ledger_id", "op_id"});
}
```

```cpp
// Append to ledger_entity.hpp:
struct AppliedOpRecord {
    static constexpr std::string_view TableName = "ledger_applied_ops";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&LedgerRecord::id, Light::SqlRealName{"ledger_id"}> ledger;  // 1
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"op_id"}> opId;  // 2
    Light::Field<Light::SqlMaxDynamicAnsiString, Light::SqlRealName{"result_json"}> resultJson;  // 3
    Light::Field<std::int64_t, Light::SqlRealName{"created_at_ms"}> createdAtMs{0};  // 4
};
```

- [ ] **Step 4: Implement the ledger lookup-before-mutate, write-after-commit pattern in `execute(StoreTransaction)`**

Copied verbatim in shape from kanban's own real `execute
(MoveTaskPosition)` (cited above): immediately after Task 11's
empty-principal check and `validate()`, before any account lookup or
zero-sum partitioning, check `action.opId.hasValue()`; if so, query
`AppliedOpRecord` by `(ledger_id, op_id)` — a hit means: deserialize
`resultJson` back into a `GetLedgerResult` (confirm `glz::read_json`'s
exact signature against kanban's own usage, or an existing rung's own
JSON-roundtrip test), return it verbatim, **do not call `logAction`** (a
ledger hit performed nothing new — nothing to journal), and do not touch
the zero-sum/leg-insertion logic at all. A miss proceeds exactly as
Task 8 already implemented, with one addition: after `sqlTxn.Commit()`
and after computing the rebuilt `GetLedgerResult`, if `opId.hasValue()`,
serialize that result to JSON and `mapper.Create()` an `AppliedOpRecord`
row *inside the same transaction* (before `sqlTxn.Commit()`, not after —
confirm this ordering against kanban's own real code, which creates the
`AppliedOpRecord` before `transaction.Commit()`, so the op-id write and
the business mutation are atomic together).

- [ ] **Step 5: Run tests to verify they pass**

Run: `ctest --preset cl-debug -R "exactly-once" --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Run the full ledger test suite to confirm Task 8/9's own tests still pass**

Run: `ctest --preset cl-debug -L ladder-ledger --output-on-failure`
Expected: PASS — Task 8/9's existing `StoreTransaction` tests don't set
`opId` (or set it as disengaged/empty), so they exercise the "no opId,
no ledger check, ordinary insert" path unchanged. Confirm this is
actually true rather than assumed: an empty/disengaged `opId` must skip
the whole lookup-and-write-ledger-row logic, never attempt a lookup
against an empty string key.

- [ ] **Step 7: Commit**

```bash
git add examples/ledger/include/ledger/dto/transaction_dto.hpp \
        examples/ledger/include/ledger/db/ledger_entity.hpp \
        examples/ledger/src/db/schema.cpp \
        examples/ledger/src/models/ledger_model.cpp \
        examples/ledger/tests/test_ledger_model.cpp
git commit -m "ledger: StoreTransaction exactly-once via opId + applied-ops ledger (replay-safety prerequisite for Task 12)"
```

---

## Task 12: `RuleModel` + cascade-journaling (causal parent-id)

**Files:**
- Create: `examples/ledger/include/ledger/dto/rule_dto.hpp`
- Create: `examples/ledger/include/ledger/models/rule_model.hpp`
- Create: `examples/ledger/src/models/rule_model.cpp`
- Modify: `examples/ledger/src/models/ledger_model.cpp`
- Test: `examples/ledger/tests/test_rule_model.cpp`
- Modify: `examples/ledger/tests/test_ledger_model.cpp`

**Interfaces:**
- Consumes: `morph::journal::isReplaying()`, `LogEntry::causalParentId`
  (this branch's cherry-picked framework commit — verify
  `include/morph/journal/action_log.hpp`/`journal.hpp` have these before
  starting this task).
- Produces: `ledger::CreateRule { ledgerId, trigger: RuleTrigger,
  matchText, action: RuleAction, actionValue }`, `ledger::UpdateRule
  { ruleId, matchText, actionValue }` (bumps `RuleRecord.version`),
  `ledger::RuleModel` keyed by `LedgerId`;
  `LedgerModel::execute(StoreTransaction)` gains a post-commit rule
  evaluation step: on a `RuleTrigger::DescriptionContains` match, produces
  a second `LogEntry` for the cascaded `SetCategory` mutation, with
  `causalParentId` set to the triggering entry's app-minted identity
  (never `LogEntry::seq`, per design spec §4/§5), and the entry's
  `payload` includes `ruleId` and the `ruleVersion` that fired.

- [ ] **Step 1: Re-read design spec §4 and kanban's cascade-journaling decision**

Read this plan's own spec (§4) and, if accessible, kanban's design spec
§9 (`docs/superpowers/specs/2026-08-16-kanban-rung4-design.md`, on the
`ladder-kanban-impl` branch) for the exact causal-parent-id mechanism
before implementing — this task must match that mechanism precisely, not
reinvent it.

**Correction from plan self-review**: `RuleModel`'s constructor
(`explicit RuleModel(LedgerId ledgerId)`) and the principal check
(`context.principal.hasValue()`) both predate this plan's own real
corrections from Tasks 7 and 11 — `RuleModel` must be plain
default-constructible (Task 7's real, verified pattern: no keyed model
in this rung takes its key as a constructor argument), and the
empty-principal check is `morph::session::current()` returning `nullptr`
or an empty `.principal` string (Task 11's real, verified pattern),
never `.hasValue()`. Both fixed below. Also, Step 8's cascade mechanism
was left as "mint a stable app-level identity... resolve the exact
mechanism" — kanban's own real, already-implemented `evaluateRules`
(`ladder-kanban-impl:examples/kanban/src/models/board_model.cpp`)
answers this precisely: mint the identity from a real DB row's
auto-increment id that already exists in the same transaction
regardless of whether any rule matches (kanban uses its own
`BoardEventRecord`'s id; ledger's exact equivalent is
`TransactionJournalRecord`'s own id, already created earlier in
`execute(StoreTransaction)`), and call the cascade's *implementation*
directly (bypassing any public `execute()` overload, which would
double-log) — `logAction`'s cascade call is the *only* logger for that
entry, invoked with the trigger's identity as `causalParentId`. Fully
specified below rather than left for the implementer to resolve.

- [ ] **Step 2: Write the failing rule-creation test**

```cpp
// examples/ledger/tests/test_rule_model.cpp
// SPDX-License-Identifier: Apache-2.0
#include "ledger/models/rule_model.hpp"
#include "testkit/db_fixture.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("CreateRule persists a rule at version 1", "[ledger][rule]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::RuleModel model;
    ScopedPrincipal principal{"alice"};  // per Task 11's convention -- mutating actions require a principal
    auto ruleId = model.execute(ledger::CreateRule{
        .ledgerId = ledgerId, .trigger = ledger::RuleTrigger::DescriptionContains,
        .matchText = "Coffee", .action = ledger::RuleAction::SetCategory, .actionValue = "Dining"});
    REQUIRE(ruleId.hasValue());

    auto ruleRows = mapper.Query<ledger::db::RuleRecord>()
                        .Where(::Lightweight::FieldNameOf<&ledger::db::RuleRecord::id>, "=", *ruleId)
                        .All();
    REQUIRE(ruleRows.size() == 1);
    CHECK(ruleRows.front().version.Value() == 1);
}

TEST_CASE("UpdateRule bumps the version", "[ledger][rule]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::RuleModel model;
    ScopedPrincipal principal{"alice"};
    auto ruleId = model.execute(ledger::CreateRule{
        .ledgerId = ledgerId, .trigger = ledger::RuleTrigger::DescriptionContains,
        .matchText = "Coffee", .action = ledger::RuleAction::SetCategory, .actionValue = "Dining"});

    auto updated = model.execute(
        ledger::UpdateRule{.ruleId = ruleId, .matchText = "Cafe", .actionValue = "Dining Out"});
    CHECK(updated.version == 2);
}
```

`ScopedPrincipal` is the same helper Task 11 added to this file's own
anonymous namespace — reuse it, do not redeclare.

- [ ] **Step 3: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "rule.*model" --output-on-failure`
Expected: FAIL to compile.

- [ ] **Step 4: Implement `rule_dto.hpp`, `rule_model.hpp`, `rule_model.cpp`**

```cpp
// examples/ledger/include/ledger/dto/rule_dto.hpp
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ledger/core/types.hpp"
#include <morph/forms/forms.hpp>
#include <string>

namespace ledger {

struct CreateRule {
    LedgerId ledgerId;
    RuleTrigger trigger;
    std::string matchText;
    RuleAction action;
    std::string actionValue;

    [[nodiscard]] bool validate() const noexcept { return ledgerId.hasValue() && !matchText.empty(); }
};

struct UpdateRule {
    RuleId ruleId;
    std::string matchText;
    std::string actionValue;

    [[nodiscard]] bool validate() const noexcept { return ruleId.hasValue() && !matchText.empty(); }
};

struct RuleInfo {
    RuleId id;
    RuleTrigger trigger;
    std::string matchText;
    RuleAction action;
    std::string actionValue;
    std::int32_t version;
};

}  // namespace ledger
```

```cpp
// examples/ledger/include/ledger/models/rule_model.hpp
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ledger/dto/rule_dto.hpp"

#include <morph/core/registry.hpp>

namespace ledger {

/// @brief Rule CRUD, keyed by LedgerId. Plain default-constructible, per
///        LedgerModel's own real shape (Task 7) -- the key lives in each
///        action. Rule *evaluation* during StoreTransaction lives in
///        LedgerModel (Step 8 below), which reads RuleRecord rows via a
///        direct Query<RuleRecord> rather than calling back into a live
///        RuleModel instance -- there is no established cross-model
///        read pattern in this codebase to reuse instead.
class RuleModel {
  public:
    RuleId execute(const CreateRule& action);
    RuleInfo execute(const UpdateRule& action);
};

}  // namespace ledger

BRIDGE_REGISTER_MODEL(ledger::RuleModel, "RuleModel")
BRIDGE_REGISTER_ACTION(ledger::RuleModel, ledger::CreateRule, "CreateRule")
BRIDGE_REGISTER_ACTION(ledger::RuleModel, ledger::UpdateRule, "UpdateRule")

// Hand-written ModelKeyTraits/ActionKeyTraits, per Task 7's real,
// verified discovery: LedgerId fails morph::model::ModelKey's
// std::integral/std::string constraint, so BRIDGE_MODEL_KEY/
// BRIDGE_KEY_FROM cannot be used.
template <>
struct morph::model::ModelKeyTraits<ledger::RuleModel> {
    using PrimaryKey = std::int64_t;
};
template <>
struct morph::model::ActionKeyTraits<ledger::CreateRule> {
    static constexpr bool hasKey = true;
    static constexpr bool fromResult = false;
    static std::string key(const ledger::CreateRule& action) { return morph::model::keyToString(*action.ledgerId); }
};
```

`UpdateRule` gets no `ActionKeyTraits` specialization — it carries a
`ruleId`, not a `ledgerId`, so it cannot share `CreateRule`'s key type;
confirm against `include/morph/core/model_key.hpp`'s real primary-template
default (`hasKey = false`) whether that's the correct answer for it too,
the same way Task 10 confirmed it for `LinkAccountToCategory`.

```cpp
// examples/ledger/src/models/rule_model.cpp
// SPDX-License-Identifier: Apache-2.0
#include "ledger/core/errors.hpp"
#include "ledger/db/ledger_entity.hpp"
#include "ledger/models/rule_model.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <morph/session/session.hpp>

namespace ledger {

RuleId RuleModel::execute(const CreateRule& action) {
    const auto* ctx = morph::session::current();
    if (ctx == nullptr || ctx->principal.empty()) {
        throw EmptyPrincipalError{};
    }
    if (!action.validate()) {
        throw ValidationError{"CreateRule: ledgerId and matchText are required"};
    }
    Lightweight::DataMapper mapper;
    auto ledgerRows = mapper.Query<db::LedgerRecord>()
                           .Where(::Lightweight::FieldNameOf<&db::LedgerRecord::id>, "=", *action.ledgerId)
                           .All();
    if (ledgerRows.empty()) {
        throw NotFound{"CreateRule: no such ledger"};
    }
    db::RuleRecord ruleRow;
    ruleRow.ledger = ledgerRows.front();
    ruleRow.trigger = static_cast<int>(action.trigger);
    ruleRow.matchText = action.matchText;
    ruleRow.action = static_cast<int>(action.action);
    ruleRow.actionValue = action.actionValue;
    ruleRow.version = 1;
    mapper.Create(ruleRow);
    auto ruleId = RuleId{static_cast<std::int64_t>(ruleRow.id.Value())};
    logAction(action, ruleId);  // needs RuleModel's own attachActionLog/logAction pair -- see the note below
    return ruleId;
}

RuleInfo RuleModel::execute(const UpdateRule& action) {
    const auto* ctx = morph::session::current();
    if (ctx == nullptr || ctx->principal.empty()) {
        throw EmptyPrincipalError{};
    }
    if (!action.validate()) {
        throw ValidationError{"UpdateRule: ruleId and matchText are required"};
    }
    Lightweight::DataMapper mapper;
    auto ruleRows = mapper.Query<db::RuleRecord>()
                         .Where(::Lightweight::FieldNameOf<&db::RuleRecord::id>, "=", *action.ruleId)
                         .All();
    if (ruleRows.empty()) {
        throw NotFound{"UpdateRule: no such rule"};
    }
    auto& ruleRow = ruleRows.front();
    ruleRow.matchText = action.matchText;
    ruleRow.actionValue = action.actionValue;
    ruleRow.version = ruleRow.version.Value() + 1;
    mapper.Update(ruleRow);
    RuleInfo result{.id = action.ruleId, .trigger = static_cast<RuleTrigger>(ruleRow.trigger.Value()),
                     .matchText = ruleRow.matchText.Value().ToStringView().data(),
                     .action = static_cast<RuleAction>(ruleRow.action.Value()),
                     .actionValue = ruleRow.actionValue.Value().ToStringView().data(),
                     .version = ruleRow.version.Value()};
    logAction(action, result);
    return result;
}

}  // namespace ledger
```

**`RuleModel` needs its own `attachActionLog`/`logAction` pair, per Task
11a's own pattern** (this plan did not originally scope Task 11a to
cover `RuleModel`, since `RuleModel` did not exist yet when Task 11a was
written — add the identical `attachActionLog`/`logAction` shape to
`RuleModel`'s own header/`.cpp` as part of this task, following Task
11a's exact template, `modelType = "RuleModel"`). Every mutating
`execute()` on `RuleModel` (`CreateRule`, `UpdateRule`) calls `logAction`
at the end, exactly like `LedgerModel`/`BudgetModel`.

- [ ] **Step 5: Run tests to verify they pass**

Run: `ctest --preset cl-debug -R "rule.*model" --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Write the failing cascade + causal-parent-id test**

```cpp
// Append to examples/ledger/tests/test_ledger_model.cpp
TEST_CASE("A matching rule cascades SetCategory with a causalParentId, not LogEntry::seq", "[ledger][rule][journal]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ScopedPrincipal principal{"alice"};
    ledger::RuleModel ruleModel;
    ruleModel.execute(ledger::CreateRule{.ledgerId = ledgerId, .trigger = ledger::RuleTrigger::DescriptionContains,
                                          .matchText = "Coffee", .action = ledger::RuleAction::SetCategory,
                                          .actionValue = "Dining"});

    ledger::LedgerModel ledgerModel;
    ledgerModel.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "Checking",
                                             .kind = ledger::AccountKind::Asset, .currency = ledger::Currency::USD});
    ledgerModel.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "Dining",
                                             .kind = ledger::AccountKind::Expense,
                                             .currency = ledger::Currency::USD});
    auto ledgerState = ledgerModel.execute(ledger::GetLedger{.ledgerId = ledgerId});

    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    ledgerModel.attachActionLog(log, std::to_string(*ledgerId));

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    ledgerModel.execute(ledger::StoreTransaction{
        .ledgerId = ledgerId,
        .description = "Coffee at the cafe",
        .date = morph::time::Timestamp::now(),
        .legs = {ledger::TransactionLeg{.accountId = ledgerState.accounts[0].id,
                                         .amount = morph::math::Rational{Numerator{-450}, Denominator{1},
                                                                          DecimalPlaces{2}}},
                 ledger::TransactionLeg{.accountId = ledgerState.accounts[1].id,
                                        .amount = morph::math::Rational{Numerator{450}, Denominator{1},
                                                                         DecimalPlaces{2}}}}});

    auto entries = log->entries();
    REQUIRE(entries.size() == 2);  // trigger + cascade
    CHECK(entries[0].actionType == "StoreTransaction");
    CHECK(entries[0].causalParentId.empty());  // the trigger itself has no parent
    CHECK(entries[1].actionType == "SetCategory");
    CHECK_FALSE(entries[1].causalParentId.empty());
    CHECK(entries[1].causalParentId != std::to_string(entries[0].seq));  // never LogEntry::seq
    CHECK(entries[1].payload.find("ruleId") != std::string::npos);
    CHECK(entries[1].payload.find("ruleVersion") != std::string::npos);
}
```

Confirm `LogEntry::payload`'s exact type (`std::string`, already
JSON-encoded, per `action_log.hpp`) before finalizing the `payload.find(...)`
assertions — a substring check on already-serialized JSON is this plan's
simplest verifiable assertion shape, but confirm it against the real
`toJson()` output shape (does `SetCategory`'s DTO literally carry fields
named `ruleId`/`ruleVersion`? see Step 8's `SetCategory` DTO below) rather
than guessing the JSON key names blind.

- [ ] **Step 7: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "causalParentId" --output-on-failure`
Expected: FAIL — no cascade logic exists yet.

- [ ] **Step 8: Implement rule evaluation in `LedgerModel::execute(StoreTransaction)`**

Add a `SetCategory` action DTO to `transaction_dto.hpp` (or a new
`rule_cascade_dto.hpp` if that fits this codebase's file-organization
convention better) — the cascade's own action type, carrying the fields
the causal-link test above checks for:

```cpp
struct SetCategory {
    AccountId accountId;
    CategoryId categoryId;
    RuleId ruleId;
    std::int32_t ruleVersion;
};

/// @brief Empty result placeholder for SetCategory's cascade logging --
///        this rung's own name for the same empty-result shape kanban's
///        `Ack` (`examples/kanban/include/kanban/dto/project_dto.hpp`)
///        serves there; not imported from kanban (a different rung's
///        type), a fresh local declaration with the same shape.
struct SetCategoryResult {};
```

**`SetCategory` must still be `BRIDGE_REGISTER_ACTION`'d and have a
public `execute()` overload, even though design spec §4 never calls for
a client to dispatch it directly.** Verified against `morph::journal::
replay()`'s real implementation (`include/morph/journal/journal.hpp`):
`replay()` re-dispatches every entry via
`dispatcher.dispatch(entry.modelType, entry.actionType, *holder,
entry.payload)`, which looks up the action type string in the
dispatcher's *registered*-action table regardless of who originally
created the entry — an unregistered `"SetCategory"` entry would make
`replay()` throw `std::runtime_error` the moment it reaches that entry.
Kanban's own `ApplyTagMutation` (the equivalent cascade action there) is
registered for exactly this reason (confirmed:
`BRIDGE_REGISTER_ACTION(kanban::BoardModel, kanban::ApplyTagMutation,
"ApplyTagMutation")`), even though its `execute()` overload's own doc
comment states plainly that `evaluateRules` never calls through it (to
avoid double-logging) — the registration exists purely so `replay()` can
route the type, not because a client is expected to dispatch it that
way.

Add:
- `SetCategoryResult execute(const SetCategory& action)` — the *public*,
  directly-dispatchable overload. Body: validate, call
  `setCategoryImpl(action)`, call `logAction(action, SetCategoryResult{})`
  (empty `causalParentId` — the default, since this is an ordinary,
  non-cascaded call site), return `SetCategoryResult{}`. Register it:
  `BRIDGE_REGISTER_ACTION(ledger::LedgerModel, ledger::SetCategory,
  "SetCategory")`.
- A private `setCategoryImpl(const SetCategory&)` method holding the
  actual mutation (link `accountId` to `categoryId` — reusing
  `BudgetModel::execute(LinkAccountToCategory)`'s same schema addition
  from Task 10, `AccountRecord::category`). Called by BOTH the public
  `execute(SetCategory)` above AND the cascade path below — the mutation
  logic itself is shared; only the *logging* differs (once, unconditionally,
  in the public overload; once, with a causal link, in the cascade path —
  never both for the same firing, exactly kanban's own documented
  reasoning for why `evaluateRules` bypasses the public overload).

In `execute(StoreTransaction)`, after `mapper.Create(journalRow)` (the
journal row's `id` now exists) and after the full per-leg commit loop,
before this method's own `logAction(action, result)` call at the very
end:

**Two design decisions this task resolves concretely, not left
speculative:**

1. **Which account does a description-match rule categorize?** The
   design spec's own step 4 describes the rule as "task moved to Done ⇒
   ..." for kanban, transliterated to "description contains X ⇒ set
   category Y" for ledger — the natural ledger reading is: the rule
   categorizes the **expense/revenue leg** of the matching transaction
   (the non-asset side), not the asset account being debited/credited.
   Resolved by picking the first leg in `action.legs` whose account's
   `kind` is `Expense` or `Revenue`, reusing the `legAccounts` vector
   `execute(StoreTransaction)`'s own zero-sum partitioning loop (Task 8)
   already populated — no re-query needed.
2. **How does `actionValue` (a category *name* string, e.g. `"Dining"`)
   resolve to a `CategoryId`?** Resolved as **lookup, never
   auto-create**: the category must already exist (via `CreateCategory`,
   Task 10) — consistent with `OpenAccount`'s own established precedent
   of requiring its ledger to already exist rather than auto-provisioning
   one. If no such category exists, the rule silently does not fire for
   this transaction (not an error — a dangling rule referencing a
   deleted/never-created category is a misconfiguration, not a reason to
   fail the triggering `StoreTransaction` itself).

```cpp
if (!morph::journal::isReplaying()) {
    const std::string triggerCausalId = "transactionJournal:" + std::to_string(journalRow.id.Value());
    auto rules = mapper.Query<db::RuleRecord>()
                     .Where(::Lightweight::FieldNameOf<&db::RuleRecord::ledger>, "=", *action.ledgerId)
                     .Where(::Lightweight::FieldNameOf<&db::RuleRecord::trigger>, "=",
                            static_cast<int>(RuleTrigger::DescriptionContains))
                     .All();
    // Decision 1: the leg to categorize is the first Expense/Revenue
    // account among this transaction's own legs -- legAccounts is the
    // same vector the zero-sum partitioning loop above already built,
    // positionally aligned with action.legs.
    std::optional<std::size_t> categorizableLegIndex;
    for (std::size_t i = 0; i < legAccounts.size(); ++i) {
        const auto kind = static_cast<AccountKind>(legAccounts[i].kind.Value());
        if (kind == AccountKind::Expense || kind == AccountKind::Revenue) {
            categorizableLegIndex = i;
            break;
        }
    }
    if (categorizableLegIndex.has_value()) {
        for (const auto& rule : rules) {
            if (action.description.find(rule.matchText.Value().ToStringView()) == std::string::npos) {
                continue;
            }
            // Decision 2: lookup, never auto-create -- a rule naming a
            // category that doesn't exist in this ledger simply doesn't
            // fire; it is not an error on the triggering transaction.
            auto categoryRows =
                mapper.Query<db::CategoryRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::CategoryRecord::ledger>, "=", *action.ledgerId)
                    .Where(::Lightweight::FieldNameOf<&db::CategoryRecord::name>, "=", rule.actionValue.Value())
                    .All();
            if (categoryRows.empty()) {
                continue;
            }
            const SetCategory cascadeAction{
                .accountId = AccountId{static_cast<std::int64_t>(legAccounts[*categorizableLegIndex].id.Value())},
                .categoryId = CategoryId{static_cast<std::int64_t>(categoryRows.front().id.Value())},
                .ruleId = RuleId{static_cast<std::int64_t>(rule.id.Value())},
                .ruleVersion = rule.version.Value()};
            setCategoryImpl(cascadeAction);
            logAction(cascadeAction, SetCategoryResult{}, triggerCausalId);
        }
    }
}
```

`rule.actionValue.Value()`'s exact comparison against
`CategoryRecord::name` (a `Light::SqlAnsiString<128>`) may need an
explicit type match — confirm `Where(...)`'s value-comparison overload
accepts a `Light::SqlAnsiString<N>` on both sides, or convert one to
`std::string_view`/`std::string` first, against how an existing rung's
own string-column `Where` clause does this (e.g. `polls`' own
`participantName` filters, if any exist) before finalizing.

- [ ] **Step 9: Run tests to verify they pass**

Run: `ctest --preset cl-debug -R "causalParentId" --output-on-failure`
Expected: PASS.

- [ ] **Step 10: Write and pass the named divergence test**

```cpp
// Append to examples/ledger/tests/test_ledger_model.cpp
TEST_CASE("Replay after editing a rule reproduces the v1 cascade, never the v2 outcome", "[ledger][rule][journal][divergence]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ScopedPrincipal principal{"alice"};
    ledger::BudgetModel budgetModel;
    auto categoryA = budgetModel.execute(ledger::CreateCategory{.ledgerId = ledgerId, .name = "Dining"});
    auto categoryB = budgetModel.execute(ledger::CreateCategory{.ledgerId = ledgerId, .name = "Groceries"});

    ledger::RuleModel ruleModel;
    auto ruleId = ruleModel.execute(ledger::CreateRule{.ledgerId = ledgerId,
                                                        .trigger = ledger::RuleTrigger::DescriptionContains,
                                                        .matchText = "Coffee", .action = ledger::RuleAction::SetCategory,
                                                        .actionValue = "Dining"});  // v1: sets Dining

    ledger::LedgerModel ledgerModel;
    ledgerModel.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "Checking",
                                             .kind = ledger::AccountKind::Asset, .currency = ledger::Currency::USD});
    ledgerModel.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "Dining Out",
                                             .kind = ledger::AccountKind::Expense,
                                             .currency = ledger::Currency::USD});
    auto ledgerState = ledgerModel.execute(ledger::GetLedger{.ledgerId = ledgerId});
    const auto expenseAccountId = ledgerState.accounts[1].id;

    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    ledgerModel.attachActionLog(log, std::to_string(*ledgerId));

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    ledgerModel.execute(ledger::StoreTransaction{
        .ledgerId = ledgerId,
        .description = "Coffee run",
        .date = morph::time::Timestamp::now(),
        .legs = {ledger::TransactionLeg{.accountId = ledgerState.accounts[0].id,
                                         .amount = morph::math::Rational{Numerator{-450}, Denominator{1},
                                                                          DecimalPlaces{2}}},
                 ledger::TransactionLeg{.accountId = expenseAccountId,
                                        .amount = morph::math::Rational{Numerator{450}, Denominator{1},
                                                                         DecimalPlaces{2}}}}});

    // The expense account is now linked to category A (Dining) -- confirm
    // this directly before editing the rule, so the assertion after replay
    // is a genuine "still A, never B" check, not a vacuous one.
    auto accountRowsBefore = mapper.Query<ledger::db::AccountRecord>()
                                  .Where(::Lightweight::FieldNameOf<&ledger::db::AccountRecord::id>, "=",
                                         *expenseAccountId)
                                  .All();
    REQUIRE(accountRowsBefore.front().category.hasValue());
    CHECK(accountRowsBefore.front().category.Value() == *categoryA);

    // Edit RuleX to v2: now sets Groceries instead of Dining.
    ruleModel.execute(ledger::UpdateRule{.ruleId = ruleId, .matchText = "Coffee", .actionValue = "Groceries"});

    // Replay the captured log against a fresh model instance.
    auto replayedEntries = log->entries();
    auto replayedHolder = morph::journal::replay("LedgerModel", replayedEntries);
    (void)replayedHolder;  // confirm exact IModelHolder access pattern for reading back post-replay state --
                            // this plan's replay-read-back shape is its least-verified part of this task; check
                            // an existing rung's own replay test (if one exists, e.g. tests/test_action_log.cpp
                            // or kanban's own divergence test on ladder-kanban-impl) for how a test actually
                            // inspects state through the returned IModelHolder, rather than guessing further.

    // The replayed database state must still show category A, never B --
    // the cascade's own recorded entry (payload includes ruleVersion=1)
    // pins the v1 outcome; replay's isReplaying()-gated rule suppression
    // means the trigger entry never re-evaluates against the now-v2 rule.
    auto accountRowsAfter = mapper.Query<ledger::db::AccountRecord>()
                                 .Where(::Lightweight::FieldNameOf<&ledger::db::AccountRecord::id>, "=",
                                        *expenseAccountId)
                                 .All();
    REQUIRE(accountRowsAfter.front().category.hasValue());
    CHECK(accountRowsAfter.front().category.Value() == *categoryA);
    CHECK(accountRowsAfter.front().category.Value() != *categoryB);
}
```

This test's replay-read-back mechanism (the `(void)replayedHolder;` line)
is deliberately flagged as unresolved rather than guessed: `replay()`
returns a type-erased `std::unique_ptr<IModelHolder>`, and this plan has
not independently verified the exact API to read persisted state back
out of it (vs. simply re-querying the database directly afterward, which
this test already does via `mapper.Query<AccountRecord>()` — since
`LedgerModel`'s own mutations are persisted to the real SQLite database,
not held in memory, re-querying the database after `replay()` completes
is very likely sufficient on its own, making the `IModelHolder` return
value possibly unused by this test entirely). Confirm this reasoning (or
correct it) against `include/morph/journal/journal.hpp`'s own doc
comments on `replay()`'s return value before finalizing — if the
database-requery-only approach is correct, drop the unused
`replayedHolder` variable entirely rather than keep a needless
`(void)`-cast placeholder.

Run: `ctest --preset cl-debug -R "divergence" --output-on-failure`
Expected: PASS once implemented (this is the test the whole cascade
mechanism above exists to satisfy — if it fails, the cascade/replay wiring
has a bug, not the test).

- [ ] **Step 11: Commit**

```bash
git add examples/ledger/include/ledger/dto/rule_dto.hpp \
        examples/ledger/include/ledger/models/rule_model.hpp \
        examples/ledger/src/models/rule_model.cpp \
        examples/ledger/src/models/ledger_model.cpp \
        examples/ledger/tests/test_rule_model.cpp \
        examples/ledger/tests/test_ledger_model.cpp \
        examples/ledger/CMakeLists.txt
git commit -m "ledger: RuleModel + cascade-journaling with causalParentId and rule-version pinning"
```

---

## Task 13: `Rational` overflow fuzz test + pre-decode gap finding

**Files:**
- Create: `tests/test_ledger_rational_fuzz.cpp`
- Create: `docs/findings/001-rational-checked-arithmetic-mode.md` (or the
  next free number if findings already exist from other work by the time
  this task runs — check `docs/findings/` first)
- Create: `docs/findings/002-rational-no-predecode-validation-seam.md`
  (numbering likewise checked at task time)
- Modify: `examples/ledger/tests/test_ledger_model.cpp`

**Interfaces:**
- Consumes: `morph::math::Rational`, `morph::math::Rational::operator+`.
- Produces: a property/fuzz test measuring the row count and per-leg
  magnitude at which cross-term overflow occurs (design spec §7), plus two
  `docs/findings/` entries per `FINDINGS.md`'s format, plus a test proving
  the clamp-then-incidentally-caught pre-decode path.

- [ ] **Step 1: Check `docs/findings/` for existing entries and pick the next free numbers**

```bash
ls docs/findings/ 2>/dev/null
```

- [ ] **Step 2: Write the failing fuzz test**

```cpp
// tests/test_ledger_rational_fuzz.cpp
// SPDX-License-Identifier: Apache-2.0
#include <morph/util/rational.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdint>
#include <vector>

TEST_CASE("Zero-sum check never false-positives across differing decimalPlaces in one currency", "[ledger][rational][fuzz]") {
    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    using morph::math::Rational;

    // A USD leg at dp=2 and a correcting USD leg at dp=4 in the same
    // journal, constructed to sum to true zero once both are reduced to a
    // common scale. Assert Rational::operator+ over the two produces
    // canonical zero (num=0, den=1) -- not a "close to zero" approximation.
    Rational a{Numerator{-5000}, Denominator{1}, DecimalPlaces{2}};   // -50.00
    Rational b{Numerator{500000}, Denominator{1}, DecimalPlaces{4}};  // +50.0000
    auto sum = a + b;
    CHECK(sum.numerator == 0);
}

TEST_CASE("Measure the row count at which partial-sum overflow occurs at ledger-realistic magnitudes", "[ledger][rational][fuzz]") {
    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    using morph::math::Rational;

    // Sum N synthetic dp=2 legs at up to 10^9 minor units each; find the N
    // at which the running numerator would exceed int64_t's range.
    // Document the measured N as a comment here once run, per design spec
    // §7 -- this is empirical, not a static claim.
    Rational running{Numerator{0}, Denominator{1}, DecimalPlaces{2}};
    std::int64_t count = 0;
    constexpr std::int64_t perLeg = 1'000'000'000;  // 10^9 minor units, dp=2
    for (; count < 100'000'000; ++count) {
        Rational leg{Numerator{perLeg}, Denominator{1}, DecimalPlaces{2}};
        // Detect overflow by checking the pre-addition numerator against
        // INT64_MAX - perLeg rather than relying on UB actually occurring;
        // record `count` at the first iteration where this would overflow
        // and stop before triggering real UB.
        if (running.numerator > INT64_MAX - perLeg) {
            break;
        }
        running = running + leg;
    }
    INFO("Overflow boundary reached at row count: " << count);
    CHECK(count > 0);  // sanity: some rows were summed before the boundary
}
```

- [ ] **Step 3: Run test to verify it passes (or fails, informing the measured boundary)**

Run: `ctest --preset cl-debug -R "rational.*fuzz" --output-on-failure`
Expected: both PASS; the second test's `INFO` output records the measured
overflow boundary — capture that number for Step 5's finding file.

- [ ] **Step 4: Write the pre-decode-gap test**

```cpp
// Append to examples/ledger/tests/test_ledger_model.cpp
TEST_CASE("A clamped Rational leg is caught incidentally by the zero-sum check, not by validate()", "[ledger][rational][security]") {
    // Construct a StoreTransaction whose wire JSON encodes a leg with
    // {"num":5,"den":0,"dp":2} -- setWire clamps this to 5/1 rather than
    // rejecting. Decode it into a StoreTransaction (bypassing validate()'s
    // own inability to detect the clamp), and assert the resulting legs
    // fail the zero-sum check (ZeroSumViolation thrown) rather than
    // silently committing -- proving the invariant's incidental catch,
    // per design spec §7.
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `ctest --preset cl-debug -R "clamped" --output-on-failure`
Expected: PASS.

- [ ] **Step 6: File the two findings**

```markdown
---
id: 001
title: Rational has no checked-arithmetic mode; intermediate cross-terms can overflow before final results do
subsystem: units
severity: minor
source: ledger rung 5, design spec §7
disposition: open
test: tests/test_ledger_rational_fuzz.cpp
---

At ledger-realistic magnitudes (dp=2 currencies, legs up to 10^9 minor
units), Rational::operator+ summed over roughly <measured N from Step 3>
rows crosses int64_t's range, which is undefined behavior today
(Rational's arithmetic operators are fixed-width, not saturating, and not
exception-throwing by signature -- see include/morph/util/rational.hpp).
A checked-arithmetic mode (an expected<Rational, Overflow>-returning
operator+/- alongside the existing noexcept ones, or a debug-mode
overflow assertion) would let a ledger-scale application detect this
before committing corrupted state, rather than relying on the app never
summing enough rows to hit the boundary in practice.
```

```markdown
---
id: 002
title: No pre-decode validation seam for Rational -- setWire clamps hostile wire input to a plausible value instead of rejecting
subsystem: wire
severity: minor
source: ledger rung 5, design spec §7
disposition: open
test: examples/ledger/tests/test_ledger_model.cpp (clamped Rational leg test)
---

A wire payload like {"num":5,"den":0,"dp":2} decodes via Rational::setWire
into a plausible 5/1 rather than being rejected at decode time (see
include/morph/util/rational.hpp's codec). Every dispatch path decodes
before any model-level validate() runs, so an app has no seam to catch a
clamped value as clamped -- it only ever sees an already-plausible
Rational. Ledger's own zero-sum invariant happens to catch most clamped
legs incidentally (a clamped value is unlikely to still sum to zero), but
this is coincidental protection from a business rule, not a validation
guarantee the framework provides. A pre-decode validation hook (reject
rather than clamp, or a decode-time flag surfacing "this value was
clamped") would close the gap for any app whose own invariants don't
happen to catch it.
```

- [ ] **Step 7: Commit**

```bash
git add tests/test_ledger_rational_fuzz.cpp \
        docs/findings/001-rational-checked-arithmetic-mode.md \
        docs/findings/002-rational-no-predecode-validation-seam.md \
        examples/ledger/tests/test_ledger_model.cpp \
        tests/CMakeLists.txt
git commit -m "ledger: Rational overflow fuzz test + two named framework findings (design spec §7)"
```

---

## Task 14: Undo as a compensating action

**Files:**
- Modify: `examples/ledger/include/ledger/dto/transaction_dto.hpp`
- Modify: `examples/ledger/src/models/ledger_model.cpp`
- Modify: `examples/ledger/tests/test_ledger_model.cpp`

**Interfaces:**
- Produces: `ledger::UndoTransaction { ledgerId: LedgerId, journalId:
  JournalId }` with `validate()`; `LedgerModel::execute(UndoTransaction)
  -> GetLedgerResult` — constructs and commits a reversing
  `TransactionJournalRecord` whose legs are the originals negated via
  `Rational::operator-() const` (the member unary negation), per design
  spec §6, with `causalParentId` pointing at the undone entry.

**Ruling on the key-resolution question this task raises**: every other
keyed action in this file derives its key directly from a `ledgerId`
field it already carries; `UndoTransaction` naturally has only
`journalId`, which would force `ActionKeyTraits<UndoTransaction>::key()`
to open its own `Lightweight::DataMapper` and query the journal's
`ledger_id` before any key/dispatch/transaction context exists -- a
genuinely new pattern with no precedent anywhere in this codebase or
kanban's, and unclear performance/threading implications for something
called on every dispatch. Decided: `UndoTransaction` carries `ledgerId`
explicitly (redundant with `journalId`, but the client already knows
which ledger it's undoing within -- it's displaying that ledger's own
activity stream), keeping `key()` a trivial field read exactly like
every other action here. `execute()` independently verifies the looked-up
journal's own `ledger` really matches `action.ledgerId` (`throw
NotFound{"UndoTransaction: journal does not belong to this ledger"}` on
mismatch), so a wrong `ledgerId` cannot be used to bypass anything or
target the wrong ledger's model instance.

- [ ] **Step 1: Write the failing test**

```cpp
// Append to examples/ledger/tests/test_ledger_model.cpp
TEST_CASE("UndoTransaction produces an exact negation that re-passes zero-sum and restores balances", "[ledger][undo]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::LedgerModel model;
    const ScopedPrincipal principal{"alice"};
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "Checking",
                                       .kind = ledger::AccountKind::Asset, .currency = ledger::Currency::USD});
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "Groceries",
                                       .kind = ledger::AccountKind::Expense, .currency = ledger::Currency::USD});
    auto ledgerState = model.execute(ledger::GetLedger{.ledgerId = ledgerId});
    auto checkingId = ledgerState.accounts[0].id;
    auto groceriesId = ledgerState.accounts[1].id;

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    // -50.00 from Checking, +50.00 to Groceries -- same shape as this
    // file's own "StoreTransaction with two balanced USD legs commits" test.
    model.execute(ledger::StoreTransaction{
        .ledgerId = ledgerId,
        .description = "Weekly shop",
        .date = morph::time::Timestamp::now(),
        .legs = {ledger::TransactionLeg{.accountId = checkingId,
                                         .amount = morph::math::Rational{Numerator{-5000}, Denominator{1},
                                                                          DecimalPlaces{2}}},
                 ledger::TransactionLeg{.accountId = groceriesId,
                                        .amount = morph::math::Rational{Numerator{5000}, Denominator{1},
                                                                         DecimalPlaces{2}}}}});

    // GetLedgerResult/StoreTransaction's own return value never exposes a
    // journal id (design spec's own account_dto.hpp shape) -- the only way
    // to name the journal to undo is to query the row directly, same as
    // any other test in this file that needs a DB-assigned id its DTOs
    // don't surface.
    auto journalRows = mapper.Query<ledger::db::TransactionJournalRecord>()
                            .Where(::Lightweight::FieldNameOf<&ledger::db::TransactionJournalRecord::description>, "=",
                                   Lightweight::SqlAnsiString<256>{"Weekly shop"})
                            .All();
    REQUIRE(journalRows.size() == 1);
    const auto journalId = ledger::JournalId{static_cast<std::int64_t>(journalRows.front().id.Value())};

    auto undoResult = model.execute(ledger::UndoTransaction{.ledgerId = ledgerId, .journalId = journalId});

    // Post-undo balances match pre-transaction values exactly (both
    // accounts back to their opening zero balance) -- Rational equality,
    // not floating-point tolerance.
    REQUIRE(undoResult.accounts.size() == 2);
    auto checking = std::ranges::find_if(undoResult.accounts, [&](const auto& a) { return a.id == checkingId; });
    auto groceries = std::ranges::find_if(undoResult.accounts, [&](const auto& a) { return a.id == groceriesId; });
    REQUIRE(checking != undoResult.accounts.end());
    REQUIRE(groceries != undoResult.accounts.end());
    CHECK(checking->balance.numerator == 0);
    CHECK(groceries->balance.numerator == 0);

    // The reversal's own legs are the exact negation of the original's.
    auto reversalJournalRows =
        mapper.Query<ledger::db::TransactionJournalRecord>()
            .Where(::Lightweight::FieldNameOf<&ledger::db::TransactionJournalRecord::id>, "!=", *journalId)
            .All();
    REQUIRE(reversalJournalRows.size() == 1);
    auto reversalLegRows =
        mapper.Query<ledger::db::TransactionLegRecord>()
            .Where(::Lightweight::FieldNameOf<&ledger::db::TransactionLegRecord::journal>, "=",
                   reversalJournalRows.front().id.Value())
            .All();
    REQUIRE(reversalLegRows.size() == 2);
    for (const auto& legRow : reversalLegRows) {
        if (legRow.account.Value().id.Value() == static_cast<std::uint64_t>(*checkingId)) {
            CHECK(legRow.amountNum.Value() == 5000);  // negation of the original -5000
        } else {
            CHECK(legRow.amountNum.Value() == -5000);  // negation of the original 5000
        }
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "UndoTransaction" --output-on-failure`
Expected: FAIL to compile.

- [ ] **Step 3: Implement `UndoTransaction`**

**Correction from plan self-review**: "route it through the exact same
commit path `StoreTransaction` uses" needed a concrete mechanism -- left
vague, an implementer could either duplicate `execute(StoreTransaction)`'s
insert logic (a real DRY violation) or call the *public*
`execute(StoreTransaction)` overload reentrantly (which would double-log:
that overload's own `logAction(action, result)` call has no way to carry
this task's `causalParentId`, and its opId/cascade-evaluation blocks are
meaningless noise for a reversal). The precedent this rung already
established for exactly this shape is `SetCategory`/`setCategoryImpl`
(Task 12): a private, mapper-taking helper holds the pure mutation, the
*public* `execute()` overload calls it and logs with no `causalParentId`,
and Task 12's *cascade* caller calls the same helper directly then logs
with a non-default `causalParentId` -- never through the public overload.
`UndoTransaction` follows the identical shape:

1. In `ledger_model.cpp`, extract `execute(StoreTransaction)`'s journal-
   insert + leg-insert + `buildLedgerState` rebuild (the code from
   `Lightweight::SqlTransaction sqlTxn{...}` through
   `auto result = buildLedgerState(mapper, action.ledgerId);`, i.e. lines
   258-313 of the current file, NOT including the opId-ledger-write block
   or the cascade-evaluation block that follow -- those two blocks stay in
   `execute(StoreTransaction)` itself, since a reversal has no `opId` and
   never re-fires rules against its own synthetic description) into a
   private helper:
   ```cpp
   [[nodiscard]] GetLedgerResult LedgerModel::storeJournalImpl(
       Lightweight::DataMapper& mapper, const LedgerId& ledgerId, const std::string& description,
       const morph::time::Timestamp& date, const std::vector<TransactionLeg>& legs,
       const std::vector<db::AccountRecord>& legAccounts) {
       Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
       db::TransactionJournalRecord journalRow;
       journalRow.description = description;
       journalRow.date = date.value.has_value() ? (*date.value).value.time_since_epoch().count() : 0;
       auto ledgerRows =
           mapper.Query<db::LedgerRecord>().Where(::Lightweight::FieldNameOf<&db::LedgerRecord::id>, "=", *ledgerId).All();
       if (ledgerRows.empty()) {
           throw NotFound{"storeJournalImpl: no such ledger"};
       }
       journalRow.ledger = ledgerRows.front();
       mapper.Create(journalRow);
       for (std::size_t i = 0; i < legs.size(); ++i) {
           db::TransactionLegRecord legRow;
           legRow.journal = journalRow;
           legRow.account = legAccounts[i];
           legRow.amountNum = legs[i].amount.numerator;
           legRow.amountDen = legs[i].amount.denominator;
           legRow.amountDp = static_cast<int>(legs[i].amount.decimalPlaces.value);
           legRow.currencyCode = legAccounts[i].currencyCode.Value();
           const auto& foreignAmount = legs[i].foreignAmount;
           legRow.foreignAmountNum = foreignAmount ? std::optional{foreignAmount->numerator} : std::nullopt;
           legRow.foreignAmountDen = foreignAmount ? std::optional{foreignAmount->denominator} : std::nullopt;
           legRow.foreignAmountDp =
               foreignAmount ? std::optional{static_cast<int>(foreignAmount->decimalPlaces.value)} : std::nullopt;
           legRow.foreignCurrencyCode =
               legs[i].foreignCurrency ? std::optional{Lightweight::SqlAnsiString<3>{currencyToCode(*legs[i].foreignCurrency)}}
                                       : std::nullopt;
           mapper.Create(legRow);
       }
       auto result = buildLedgerState(mapper, ledgerId);
       sqlTxn.Commit();
       return result;
   }
   ```
   Note this version moves `sqlTxn.Commit()` to the end of the helper
   itself (the original inline code commits later, after the opId-ledger
   write and cascade block run inside the *same* transaction) -- since
   `UndoTransaction` has neither of those follow-on blocks, its own
   transaction can close right here. `execute(StoreTransaction)` cannot
   simply call this helper unmodified, because it still needs the opId
   write and cascade evaluation to run *before* commit, in the same
   transaction the journal+legs insert used -- so `execute(StoreTransaction)`
   keeps its own inline `SqlTransaction`/`Commit()` exactly as today and
   does NOT call `storeJournalImpl` (extracting it fully to share with
   `execute(StoreTransaction)` too would require threading the opId/cascade
   logic through the helper's signature, which is more churn than this
   task's scope justifies) -- `storeJournalImpl` exists solely for
   `UndoTransaction` to call, mirroring `setCategoryImpl`'s own role as a
   cascade-only helper, not a refactor of the original method.
2. Add to `transaction_dto.hpp`:
   ```cpp
   struct UndoTransaction {
       LedgerId ledgerId;
       JournalId journalId;

       [[nodiscard]] bool validate() const noexcept {
           return ledgerId.hasValue() && journalId.hasValue();
       }
   };
   ```
   (per this task's own ruling above: `ledgerId` is redundant with
   `journalId` but keeps key resolution trivial and consistent with every
   other action in this file, rather than requiring a DB lookup inside
   `ActionKeyTraits::key()`.)
3. Add `GetLedgerResult execute(const UndoTransaction& action);` to
   `LedgerModel`'s public interface (`ledger_model.hpp`), plus the private
   `storeJournalImpl` declaration, and:
   ```cpp
   template <>
   struct morph::model::ActionKeyTraits<ledger::UndoTransaction> {
       static constexpr bool hasKey = true;
       static constexpr bool fromResult = false;
       static std::string key(const ledger::UndoTransaction& action) {
           return morph::model::keyToString(*action.ledgerId);
       }
   };
   ```
   (a trivial field read, same shape as `ActionKeyTraits<StoreTransaction>`
   and every other keyed action already declared in this header -- no DB
   lookup, no new pattern) and
   `BRIDGE_REGISTER_ACTION(ledger::LedgerModel, ledger::UndoTransaction, "UndoTransaction")`.
4. Implement `execute(const UndoTransaction& action)` in `ledger_model.cpp`:
   empty-principal check first (same shape as every other mutating
   action); `if (!action.validate()) throw ValidationError{...};`; look up
   the target `TransactionJournalRecord` by `*action.journalId` (`throw
   NotFound{"UndoTransaction: no such journal"}` if missing) and its own
   `ledger` (`BelongsTo`, already loaded); verify
   `journalRow.ledger.Value().id.Value() == static_cast<std::uint64_t>(*action.ledgerId)`
   (`throw NotFound{"UndoTransaction: journal does not belong to this ledger"}`
   otherwise -- the plan's own ruling above on why `ledgerId` is a
   redundant-but-required field); look up every
   `TransactionLegRecord` whose `journal` matches; build a
   `std::vector<TransactionLeg>` whose `amount` is each original leg's
   amount negated via `-originalLeg.amount` (confirmed real:
   `include/morph/util/rational.hpp`'s `Rational::operator-() const` --
   a MEMBER unary negation, e.g. `Rational{Numerator{-numerator}, ...}`
   internally -- not the free binary `operator-(lhs, rhs)` subtraction
   also declared in that header; the unary form is what `-leg.amount`
   actually calls) and whose `accountId` is unchanged; also build the matching
   `std::vector<db::AccountRecord>` (one lookup per leg's account, same
   shape as `execute(StoreTransaction)`'s own `legAccounts` loop) for
   `storeJournalImpl`'s second parameter; call `storeJournalImpl(mapper,
   action.ledgerId, "Reversal of: " + originalJournalRow.description.Value(),
   morph::time::Timestamp::now(), reversalLegs, reversalLegAccounts)` --
   the reversal's own date is "now" (when the undo happened), via
   `morph::time::Timestamp::now()`, the SAME type/convention
   `StoreTransaction.date` itself already uses (a client-observable
   "when did this happen" field, per this file's own existing comment on
   why `journalRow.date` does NOT go through `morph::ladder::now()` --
   that convention is reserved for server-audit stamps like
   `LogEntry::timestampMs`, which `logAction` sets internally regardless
   of what this task passes) -- NOT the original journal's own date,
   which belongs to the transaction being reversed, not the reversal
   itself; then `logAction(action, result,
   "transactionJournal:" + std::to_string(originalJournalRow.id.Value()))`
   (same causal-id-minting shape Task 12's cascade already uses -- a
   stable DB row id, never `LogEntry::seq`); `return result;`.

**Re-passing zero-sum is automatic, not a separate check**: negating
every leg of an already-zero-sum set is itself zero-sum (design spec §6's
own stated reasoning) -- `storeJournalImpl` does not re-run the
partitioning/zero-sum loop `execute(StoreTransaction)` runs, because it
trusts its caller already knows the legs it's inserting are safe (both
current callers, `UndoTransaction`'s reversal and any future caller,
independently uphold this). If a future caller cannot make that guarantee,
that caller's own task must add its own validation before calling
`storeJournalImpl` -- not this task's concern.

- [ ] **Step 4: Run tests to verify they pass**

Run: `ctest --preset cl-debug -R "UndoTransaction" --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add examples/ledger/include/ledger/dto/transaction_dto.hpp \
        examples/ledger/src/models/ledger_model.cpp \
        examples/ledger/tests/test_ledger_model.cpp
git commit -m "ledger: UndoTransaction -- compensating action, never undoLast()"
```

---

## Task 15: CSV/OFX import with dedup

**Files:**
- Create: `examples/ledger/include/ledger/dto/import_dto.hpp`
- Modify: `examples/ledger/include/ledger/models/ledger_model.hpp`
- Modify: `examples/ledger/src/models/ledger_model.cpp`
- Test: `examples/ledger/tests/test_ledger_import.cpp`

**Interfaces:**
- Consumes: `bookmarks::ImportOpId`'s shape (design spec §8 — reuse the
  contract, not necessarily the literal type, since ledger cannot depend
  on `examples/bookmarks`; define a local `ledger::ImportOpId` with the
  identical shape, noting in a comment that this is the second
  occurrence of the pattern per `IMPLEMENTATION.md`'s rule-of-three).
- Produces: `ledger::ImportLedgerChunk { ledgerId: LedgerId, csvChunk:
  std::string, opId: ImportOpId }`; `LedgerModel::execute
  (ImportLedgerChunk) -> ImportResult { imported: std::int64_t, duplicates:
  std::int64_t }` — chunk-level opId dedup via `ledger_imported_ops`
  (Task 4's table) plus content-hash dedup via `ledger_imported_txn_hashes`
  for cross-import duplicate detection (design spec §8).

- [ ] **Step 1: Read `bookmarks::ImportBookmarks`'s exact dedup mechanism**

Read `examples/bookmarks/include/bookmarks/dto/import_export_dto.hpp` and
`examples/bookmarks/include/bookmarks/db/imported_op_entity.hpp` in full
before implementing — copy the opId-ledger pattern precisely.

- [ ] **Step 2: Write the failing test for chunk-level opId dedup**

```cpp
// examples/ledger/tests/test_ledger_import.cpp
// SPDX-License-Identifier: Apache-2.0
#include "ledger/models/ledger_model.hpp"
#include "testkit/db_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Replaying the same opId is a safe no-op", "[ledger][import]") {
    morph::ladder::testkit::DbFixture fixture;
    ledger::LedgerModel model;
    ledger::ImportOpId opId = ledger::ImportOpId::fromOptional(std::optional<std::string>{"chunk-1"});
    std::string csv = "date,description,amount\n2026-01-01,Coffee,-4.50\n";

    auto first = model.execute(ledger::ImportLedgerChunk{.ledgerId = ledger::LedgerId{1}, .csvChunk = csv, .opId = opId});
    auto replay = model.execute(ledger::ImportLedgerChunk{.ledgerId = ledger::LedgerId{1}, .csvChunk = csv, .opId = opId});
    CHECK(first.imported == replay.imported);  // same result both times, no double-import
}

TEST_CASE("Re-importing the same statement under a different opId is caught by content-hash dedup", "[ledger][import]") {
    morph::ladder::testkit::DbFixture fixture;
    ledger::LedgerModel model;
    std::string csv = "date,description,amount\n2026-01-01,Coffee,-4.50\n";

    auto first = model.execute(ledger::ImportLedgerChunk{
        .ledgerId = ledger::LedgerId{1}, .csvChunk = csv,
        .opId = ledger::ImportOpId::fromOptional(std::optional<std::string>{"chunk-A"})});
    auto second = model.execute(ledger::ImportLedgerChunk{
        .ledgerId = ledger::LedgerId{1}, .csvChunk = csv,
        .opId = ledger::ImportOpId::fromOptional(std::optional<std::string>{"chunk-B"})});
    CHECK(first.imported == 1);
    CHECK(second.imported == 0);
    CHECK(second.duplicates == 1);
}
```

- [ ] **Step 3: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "import" --output-on-failure`
Expected: FAIL to compile.

- [ ] **Step 4: Implement `import_dto.hpp` and `LedgerModel::execute(ImportLedgerChunk)`**

Parse `csvChunk` (a minimal CSV parser — `date,description,amount` columns
is sufficient for this rung's stress-test purpose, not a full OFX
implementation), check `(ledgerId, opId)` against `ledger_imported_ops`
first (chunk-retry dedup, mirroring bookmarks' exact check-then-insert
pattern), then for each parsed row compute a content hash (description +
date + amount, canonicalized) and check `(ledgerId, hash)` against
`ledger_imported_txn_hashes` before inserting — skip (increment
`duplicates`) rather than throw on a hash hit.

- [ ] **Step 5: Run tests to verify they pass**

Run: `ctest --preset cl-debug -R "import" --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add examples/ledger/include/ledger/dto/import_dto.hpp \
        examples/ledger/include/ledger/models/ledger_model.hpp \
        examples/ledger/src/models/ledger_model.cpp \
        examples/ledger/tests/test_ledger_import.cpp \
        examples/ledger/CMakeLists.txt
git commit -m "ledger: CSV import -- opId chunk dedup + content-hash cross-import dedup"
```

---

## Task 16: Reports — `SubmitReport`/`GetReportStatus`, WAL snapshot

**Files:**
- Create: `examples/ledger/include/ledger/dto/report_dto.hpp`
- Modify: `examples/ledger/include/ledger/models/ledger_model.hpp`
- Modify: `examples/ledger/src/models/ledger_model.cpp`
- Test: `examples/ledger/tests/test_ledger_reports.cpp`

**Interfaces:**
- Consumes: `Lightweight`'s raw-query facility (for the WAL read
  transaction — `IMPLEMENTATION.md`'s pre-cleared escape tier), a
  worker-pool task-submission seam (rung 2's internal-client-with-
  service-principal pattern, per design spec §9 — confirm the exact API
  against whatever rung 2's own README/spec documents; if unavailable in
  this checkout, use `ThreadPoolExecutor::post` directly as the
  interim seam, noting the gap in a comment for later reconciliation).
- Produces: `ledger::SubmitReport { ledgerId, kind: ReportKind, params:
  std::string /* JSON-encoded report-specific parameters incl. timezone
  offset per design spec §9 */ } -> ReportJobId`; `ledger::GetReportStatus
  { jobId: ReportJobId } -> GetReportStatusResult { status: ReportStatus,
  result: std::optional<std::string> }`.

- [ ] **Step 1: Write the failing test for the submit->poll shape**

```cpp
// examples/ledger/tests/test_ledger_reports.cpp
// SPDX-License-Identifier: Apache-2.0
#include "ledger/models/ledger_model.hpp"
#include "testkit/db_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("SubmitReport returns immediately; GetReportStatus transitions Pending to Done", "[ledger][reports]") {
    morph::ladder::testkit::DbFixture fixture;
    ledger::LedgerModel model;
    // ... open accounts, store a few transactions ...

    auto jobId = model.execute(ledger::SubmitReport{
        .ledgerId = ledger::LedgerId{1}, .kind = ledger::ReportKind::MonthlyStatement, .params = "{}"});
    REQUIRE(jobId.hasValue());

    // Poll until Done (bounded loop, not a sleep -- follow pump.hpp's
    // pumpUntil-equivalent discipline even in a non-Qt unit test context,
    // or a small bounded retry loop with a hard iteration cap if this
    // test runs outside the Qt pump machinery).
    ledger::GetReportStatusResult status;
    for (int i = 0; i < 100; ++i) {
        status = model.execute(ledger::GetReportStatus{.jobId = jobId});
        if (status.status != ledger::ReportStatus::Pending) break;
    }
    REQUIRE(status.status == ledger::ReportStatus::Done);
    REQUIRE(status.result.has_value());
}

TEST_CASE("Re-polling the same completed job returns byte-identical results", "[ledger][reports]") {
    // Submit, wait for Done, GetReportStatus twice more; assert both
    // results are byte-identical (design spec §9's DoD bullet, scoped to
    // one job's own idempotent retrieval).
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "reports" --output-on-failure`
Expected: FAIL to compile.

- [ ] **Step 3: Implement `SubmitReport`/`GetReportStatus`**

`SubmitReport` inserts a `ledger_report_jobs` row (`status = Pending`) and
posts a worker-pool task that: opens a WAL read transaction via
Lightweight's raw-query facility (per `IMPLEMENTATION.md` rule 4's
pre-cleared case), runs the report's aggregation against that pinned
view, serializes the result, updates the row to `status = Done,
resultJson = <serialized>` (or `Failed` on any exception). `GetReportStatus`
is a plain read of that row.

- [ ] **Step 4: Run tests to verify they pass**

Run: `ctest --preset cl-debug -R "reports" --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add examples/ledger/include/ledger/dto/report_dto.hpp \
        examples/ledger/include/ledger/models/ledger_model.hpp \
        examples/ledger/src/models/ledger_model.cpp \
        examples/ledger/tests/test_ledger_reports.cpp \
        examples/ledger/CMakeLists.txt
git commit -m "ledger: reports -- submit->poll job idiom, WAL-snapshot semantics"
```

---

## Task 17: Local-time month boundary handling + offline-stack integration tests

**Files:**
- Modify: `examples/ledger/src/models/ledger_model.cpp` (or a new
  `include/ledger/core/time_util.hpp` if the conversion logic is
  substantial enough to warrant its own file)
- Test: `examples/ledger/tests/test_ledger_offline.cpp`

**Interfaces:**
- Consumes: `examples/common/testkit/offline_rig.hpp` (per `TESTING.md`'s
  ownership table it predates this rung; on this branch it arrived via
  Task 0's cherry-pick from `ladder-kanban-impl`, already applied and
  verified), `SqliteOfflineQueue`, `NetworkMonitor`, `SyncWorker`.
- Produces: a UTC-range conversion helper `localMonthToUtcRange(int year,
  int month, int timezoneOffsetMinutes) -> std::pair<Timestamp, Timestamp>`
  used by `SubmitReport`'s monthly-statement path (Task 16); an
  offline-stack integration test proving `StoreTransaction` queues and
  replays correctly through `SqliteOfflineQueue`/`SyncWorker`
  (design spec's general offline-safety requirement, mirroring kanban's
  own offline tests).

- [ ] **Step 1: Write the failing test for the month-boundary conversion**

```cpp
// examples/ledger/tests/test_ledger_reports.cpp (new test in the existing file)
#include "ledger/core/time_util.hpp"

TEST_CASE("A transaction at 23:30 local time lands in its local month even across a UTC boundary", "[ledger][time]") {
    // UTC-5 at 2026-01-31T23:30 local = 2026-02-01T04:30 UTC -- a real
    // cross-boundary case (still January locally, already February UTC).
    auto [utcStart, utcEnd] = ledger::localMonthToUtcRange(2026, /*month=*/1, /*timezoneOffsetMinutes=*/-300);

    // The transaction's UTC instant, per the example above.
    const auto txnUtc = morph::time::Timestamp::fromIso8601("2026-02-01T04:30:00Z");  // confirm exact factory name
    CHECK(txnUtc >= utcStart);
    CHECK(txnUtc < utcEnd);

    // A transaction just after local midnight on Feb 1 (still Jan 31 UTC-5
    // at 23:59, but Feb 1 by Feb 1 00:01 local) must NOT land in January's range.
    const auto febUtc = morph::time::Timestamp::fromIso8601("2026-02-01T05:01:00Z");
    CHECK_FALSE(febUtc < utcEnd);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "local.*month" --output-on-failure`
Expected: FAIL to compile — `time_util.hpp` doesn't exist.

- [ ] **Step 3: Implement `localMonthToUtcRange`**

```cpp
// examples/ledger/include/ledger/core/time_util.hpp
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <morph/time/timestamp.hpp>  // confirm exact header path
#include <utility>

namespace ledger {

/// @brief Converts a local calendar month to the [start, end) UTC instant
///        range covering it, given a fixed timezone offset (design spec
///        §9's "local-time month boundary vs. UTC storage" requirement).
///        Computed once, at submit time -- never compares local-time
///        strings against UTC-stored rows row-by-row.
/// @param year Calendar year (e.g. 2026).
/// @param month Calendar month, 1-12.
/// @param timezoneOffsetMinutes Offset from UTC in minutes (e.g. -300 for
///        UTC-5); the caller's local zone at report-submission time.
/// @return {start, end} in UTC such that a local-time instant in
///         [local-month-start, local-month-end) maps into this UTC range.
[[nodiscard]] std::pair<morph::time::Timestamp, morph::time::Timestamp> localMonthToUtcRange(
    int year, int month, int timezoneOffsetMinutes);

}  // namespace ledger
```

Implementation: compute the local month's first-instant and
first-instant-of-next-month as calendar values, then subtract
`timezoneOffsetMinutes` (a local time of HH:MM at offset +N is N minutes
*earlier* in UTC) to get the UTC range boundaries — confirm the exact sign
convention and `Timestamp` arithmetic API against `morph::time`'s real
header before finalizing; the test in Step 1 is the correctness oracle.

- [ ] **Step 4: Run test to verify it passes**

Run: `ctest --preset cl-debug -R "local.*month" --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Write the failing offline-stack test**

```cpp
// examples/ledger/tests/test_ledger_offline.cpp
// SPDX-License-Identifier: Apache-2.0
#include "ledger/models/ledger_model.hpp"
#include "testkit/offline_rig.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("StoreTransaction queues while offline and replays on reconnect", "[ledger][offline]") {
    // Follow kanban's own offline test shape (design spec cites it as
    // precedent) -- OfflineRig drops the connection, StoreTransaction is
    // queued client-side, reconnect triggers SyncWorker replay, assert
    // the transaction lands exactly once (no double-apply) and the
    // zero-sum invariant still holds post-replay.
}
```

- [ ] **Step 6: Run test to verify it fails, then implement/wire as needed, then verify it passes**

Run: `ctest --preset cl-debug -R "ledger.*offline" --output-on-failure`
Expected: PASS once the offline stack is correctly wired (this task
should need little new production code beyond what Tasks 7–9 already
built, since offline replay is a generic framework mechanism this rung
consumes rather than reimplements).

- [ ] **Step 7: Commit**

```bash
git add examples/ledger/src/models/ledger_model.cpp \
        examples/ledger/tests/test_ledger_reports.cpp \
        examples/ledger/tests/test_ledger_offline.cpp \
        examples/ledger/CMakeLists.txt
git commit -m "ledger: local-time month boundary handling + offline-stack integration test"
```

---

## Task 18: `LedgerPresenter` + `LedgerQmlBridge`

**Files:**
- Create: `examples/ledger/gui_lib/ledger_presenter.hpp` / `.cpp`
- Create: `examples/ledger/gui_lib/ledger_qml_bridge.hpp` / `.cpp`
- Test: `examples/ledger/tests/test_ledger_presenter.cpp`
- Test: `examples/ledger/tests/test_ledger_qml_bridge.cpp`
- Modify: `examples/ledger/CMakeLists.txt`

**Interfaces:**
- Consumes: `ledger::LedgerModel`'s action surface (`OpenAccount`,
  `GetLedger`, `StoreTransaction`, `UndoTransaction`, `ImportLedgerChunk`),
  `morph::client::BridgeHandler<Model>` (read
  `examples/bookmarks/gui_lib/bookmark_qml_bridges.hpp` for the exact
  template parameters/method names before writing), `examples/common/
  gui/presenter.hpp`'s `Presenter` base (`track()`, `busy()`, `idle()`).
- Produces: `ledger::gui::LedgerPresenter` (signals: `ledgerListed
  (QVariantList)`, `accountOpened(QString id, QString name)`,
  `transactionStored(QVariantList accounts)`, `transactionUndone
  (QVariantList accounts)`, `importCompleted(int imported, int
  duplicates)`, `failed(QString)`); `ledger::gui::LedgerQmlBridge`
  (`Q_OBJECT`, `Q_PROPERTY`s for the account list / current ledger state
  as `QVariantList`, `Q_INVOKABLE`s `openAccount(...)`,
  `storeTransaction(QVariantList legs, QString description)`,
  `undoTransaction(QString journalId)`, `importChunk(QString csv)`).

- [ ] **Step 1: Read the pattern this mirrors**

Read `examples/bookmarks/gui_lib/bookmark_qml_bridges.hpp`/`.cpp` in full
(the presenter half and the bridge half), and
`examples/common/gui/presenter.hpp`'s `track()`/`_liveness`
declared-last convention doc comment, before writing this task's files.

- [ ] **Step 2: Write the failing presenter test**

```cpp
// examples/ledger/tests/test_ledger_presenter.cpp
#include "ledger/gui_lib/ledger_presenter.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/pump.hpp"

#include <QCoreApplication>
#include <QSignalSpy>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("LedgerPresenter emits ledgerListed after a successful GetLedger", "[ledger][gui]") {
    int argc = 0;
    QCoreApplication app{argc, nullptr};
    BackendRig rig{Mode::Local};
    ledger::gui::LedgerPresenter presenter{rig.bridge()};

    QSignalSpy listedSpy{&presenter, &ledger::gui::LedgerPresenter::ledgerListed};
    QSignalSpy failedSpy{&presenter, &ledger::gui::LedgerPresenter::failed};

    presenter.refreshLedger("1");
    pumpUntil([&] { return listedSpy.count() > 0 || failedSpy.count() > 0; });

    REQUIRE(failedSpy.isEmpty());
    REQUIRE(listedSpy.count() == 1);
}
```

Copy `BackendRig::bridge()`'s exact return type and `pumpUntil`'s
signature from an existing presenter test
(`examples/bookmarks/tests/test_bookmark_presenter.cpp`) verbatim.

- [ ] **Step 3: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "LedgerPresenter" --output-on-failure`
Expected: FAIL to compile.

- [ ] **Step 4: Implement `LedgerPresenter`**

Follow `bookmark_qml_bridges.cpp`'s presenter half exactly, substituting
ledger's actions; `.then(...).onError(...)` per method, one `BridgeHandler
<LedgerModel>` member, `_liveness` last-declared.

- [ ] **Step 5: Run test to verify it passes**

Run: `ctest --preset cl-debug -R "LedgerPresenter" --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Write the failing bridge test, then implement, then verify**

Mirror kanban's `ProjectAdminBridge` test/implementation shape
(`docs/superpowers/plans/2026-08-18-kanban-rung4-completion.md`'s Task 2,
Steps 6–8) exactly, substituting ledger's `Q_PROPERTY`/`Q_INVOKABLE` set.

Run: `ctest --preset cl-debug -R "LedgerQmlBridge" --output-on-failure`
Expected: PASS.

- [ ] **Step 7: Wire into CMakeLists.txt and commit**

```bash
git add examples/ledger/gui_lib/ledger_presenter.hpp \
        examples/ledger/gui_lib/ledger_presenter.cpp \
        examples/ledger/gui_lib/ledger_qml_bridge.hpp \
        examples/ledger/gui_lib/ledger_qml_bridge.cpp \
        examples/ledger/tests/test_ledger_presenter.cpp \
        examples/ledger/tests/test_ledger_qml_bridge.cpp \
        examples/ledger/CMakeLists.txt
git commit -m "ledger: add LedgerPresenter/Bridge for the account/transaction views"
```

---

## Task 19: `BudgetPresenter` + `BudgetQmlBridge`

**Files:**
- Create: `examples/ledger/gui_lib/budget_presenter.hpp` / `.cpp`
- Create: `examples/ledger/gui_lib/budget_qml_bridge.hpp` / `.cpp`
- Test: `examples/ledger/tests/test_budget_presenter.cpp`
- Test: `examples/ledger/tests/test_budget_qml_bridge.cpp`
- Modify: `examples/ledger/CMakeLists.txt`

**Interfaces:**
- Consumes: `ledger::BudgetModel`'s action surface (`CreateBudget`,
  `SetBudgetLimit`, `GetBudgetReport`).
- Produces: `ledger::gui::BudgetPresenter` (signals: `budgetCreated
  (QString id, QString name)`, `limitSet(QString budgetId)`,
  `reportReady(QVariantMap)`, `failed(QString)`);
  `ledger::gui::BudgetQmlBridge` (`Q_PROPERTY` for the current report as
  `QVariantMap`, `Q_INVOKABLE`s `createBudget(...)`, `setBudgetLimit(...)`,
  `getBudgetReport(QString budgetId, QString month)`).

- [ ] **Step 1: Write the failing presenter test**

```cpp
// examples/ledger/tests/test_budget_presenter.cpp
// SPDX-License-Identifier: Apache-2.0
#include "ledger/gui_lib/budget_presenter.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/pump.hpp"

#include <QCoreApplication>
#include <QSignalSpy>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("BudgetPresenter emits budgetCreated after a successful CreateBudget", "[ledger][gui]") {
    int argc = 0;
    QCoreApplication app{argc, nullptr};
    BackendRig rig{Mode::Local};
    ledger::gui::BudgetPresenter presenter{rig.bridge()};

    QSignalSpy createdSpy{&presenter, &ledger::gui::BudgetPresenter::budgetCreated};
    QSignalSpy failedSpy{&presenter, &ledger::gui::BudgetPresenter::failed};

    presenter.createBudget("1", "Groceries", "1" /* categoryId */);
    pumpUntil([&] { return createdSpy.count() > 0 || failedSpy.count() > 0; });

    REQUIRE(failedSpy.isEmpty());
    REQUIRE(createdSpy.count() == 1);
}

TEST_CASE("BudgetPresenter emits reportReady after a successful GetBudgetReport", "[ledger][gui]") {
    int argc = 0;
    QCoreApplication app{argc, nullptr};
    BackendRig rig{Mode::Local};
    ledger::gui::BudgetPresenter presenter{rig.bridge()};

    QSignalSpy createdSpy{&presenter, &ledger::gui::BudgetPresenter::budgetCreated};
    presenter.createBudget("1", "Groceries", "1");
    pumpUntil([&] { return createdSpy.count() > 0; });
    const QString budgetId = createdSpy.at(0).at(0).toString();

    QSignalSpy reportSpy{&presenter, &ledger::gui::BudgetPresenter::reportReady};
    QSignalSpy failedSpy{&presenter, &ledger::gui::BudgetPresenter::failed};
    presenter.getBudgetReport(budgetId, "2026-01");
    pumpUntil([&] { return reportSpy.count() > 0 || failedSpy.count() > 0; });

    REQUIRE(failedSpy.isEmpty());
    REQUIRE(reportSpy.count() == 1);
}
```

Copy `BackendRig::bridge()`'s exact return type and `pumpUntil`'s
signature from an existing presenter test
(`examples/bookmarks/tests/test_bookmark_presenter.cpp`) verbatim, exactly
as Task 18 did for `LedgerPresenter`.

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "BudgetPresenter" --output-on-failure`
Expected: FAIL to compile — `budget_presenter.hpp` doesn't exist.

- [ ] **Step 3: Implement `BudgetPresenter`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ledger/dto/budget_dto.hpp"
#include "ledger/models/budget_model.hpp"
#include <morph/client/bridge_handler.hpp>  // confirm exact path, per Task 18
#include <QObject>
#include <QVariantMap>
#include <memory>

namespace ledger::gui {

/// @brief Drives `ledger::BudgetModel` for the budget-creation and
///        budget-report views. No QML dependency -- signals only, exactly
///        LedgerPresenter's shape (Task 18) applied to BudgetModel.
class BudgetPresenter : public QObject {
    Q_OBJECT
  public:
    explicit BudgetPresenter(std::shared_ptr<morph::client::Bridge> bridge, QObject* parent = nullptr);

    void createBudget(const QString& ledgerId, const QString& name, const QString& categoryId);
    void setBudgetLimit(const QString& budgetId, const QString& month, const QString& limitAmount,
                         const QString& currencyCode);
    void getBudgetReport(const QString& budgetId, const QString& month);

  signals:
    void budgetCreated(QString id, QString name);
    void limitSet(QString budgetId);
    void reportReady(QVariantMap report);
    void failed(QString message);

  private:
    morph::client::BridgeHandler<BudgetModel> _budgetHandler;
    std::shared_ptr<const void> _liveness = std::make_shared<int>(0);  // must stay last-declared
};

}  // namespace ledger::gui
```

Implementation (`budget_presenter.cpp`) wires each method to execute the
matching `BudgetModel` action via `_budgetHandler` and emit a signal on
success / `failed(QString)` on error, mirroring
`LedgerPresenter::openAccount`'s exact `.then(...).onError(...)` shape
from Task 18.

- [ ] **Step 4: Run test to verify it passes**

Run: `ctest --preset cl-debug -R "BudgetPresenter" --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Write the failing bridge test**

```cpp
// examples/ledger/tests/test_budget_qml_bridge.cpp
// SPDX-License-Identifier: Apache-2.0
#include "ledger/gui_lib/budget_qml_bridge.hpp"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("BudgetQmlBridge exposes the expected Q_PROPERTYs and Q_INVOKABLEs", "[ledger][gui]") {
    ledger::gui::BudgetQmlBridge bridge{nullptr /* built with a real Bridge in the real test */};
    const auto* meta = bridge.metaObject();
    CHECK(meta->indexOfProperty("report") >= 0);
    CHECK(meta->indexOfMethod("createBudget(QString,QString,QString)") >= 0);
    CHECK(meta->indexOfMethod("getBudgetReport(QString,QString)") >= 0);
}
```

- [ ] **Step 6: Run test to verify it fails, then implement `BudgetQmlBridge`, then verify it passes**

Mirror `LedgerQmlBridge`'s (Task 18) exact shape: `Q_PROPERTY report` as
`QVariantMap` backed by `BudgetPresenter::reportReady`, `Q_INVOKABLE`s
forwarding to the presenter.

Run: `ctest --preset cl-debug -R "BudgetQmlBridge" --output-on-failure`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add examples/ledger/gui_lib/budget_presenter.hpp \
        examples/ledger/gui_lib/budget_presenter.cpp \
        examples/ledger/gui_lib/budget_qml_bridge.hpp \
        examples/ledger/gui_lib/budget_qml_bridge.cpp \
        examples/ledger/tests/test_budget_presenter.cpp \
        examples/ledger/tests/test_budget_qml_bridge.cpp \
        examples/ledger/CMakeLists.txt
git commit -m "ledger: add BudgetPresenter/Bridge for the budget views"
```

---

## Task 20: `RulePresenter` + `RuleQmlBridge`

**Files:**
- Create: `examples/ledger/gui_lib/rule_presenter.hpp` / `.cpp`
- Create: `examples/ledger/gui_lib/rule_qml_bridge.hpp` / `.cpp`
- Test: `examples/ledger/tests/test_rule_presenter.cpp`
- Test: `examples/ledger/tests/test_rule_qml_bridge.cpp`
- Modify: `examples/ledger/CMakeLists.txt`

**Interfaces:**
- Consumes: `ledger::RuleModel`'s action surface (`CreateRule`,
  `UpdateRule`).
- Produces: `ledger::gui::RulePresenter` (signals: `ruleCreated(QString
  id)`, `ruleUpdated(QString id, int version)`, `failed(QString)`);
  `ledger::gui::RuleQmlBridge` (`Q_INVOKABLE`s `createRule(...)`,
  `updateRule(...)`) — a `MembersView`-style CRUD list per kanban's own
  completion-plan precedent (Task 15 of
  `docs/superpowers/plans/2026-08-18-kanban-rung4-completion.md`).

- [ ] **Step 1: Write the failing presenter test**

```cpp
// examples/ledger/tests/test_rule_presenter.cpp
// SPDX-License-Identifier: Apache-2.0
#include "ledger/gui_lib/rule_presenter.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/pump.hpp"

#include <QCoreApplication>
#include <QSignalSpy>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("RulePresenter emits ruleCreated after a successful CreateRule", "[ledger][gui]") {
    int argc = 0;
    QCoreApplication app{argc, nullptr};
    BackendRig rig{Mode::Local};
    ledger::gui::RulePresenter presenter{rig.bridge()};

    QSignalSpy createdSpy{&presenter, &ledger::gui::RulePresenter::ruleCreated};
    QSignalSpy failedSpy{&presenter, &ledger::gui::RulePresenter::failed};

    presenter.createRule("1", "Coffee", "Dining");
    pumpUntil([&] { return createdSpy.count() > 0 || failedSpy.count() > 0; });

    REQUIRE(failedSpy.isEmpty());
    REQUIRE(createdSpy.count() == 1);
}

TEST_CASE("RulePresenter emits ruleUpdated with the bumped version", "[ledger][gui]") {
    int argc = 0;
    QCoreApplication app{argc, nullptr};
    BackendRig rig{Mode::Local};
    ledger::gui::RulePresenter presenter{rig.bridge()};

    QSignalSpy createdSpy{&presenter, &ledger::gui::RulePresenter::ruleCreated};
    presenter.createRule("1", "Coffee", "Dining");
    pumpUntil([&] { return createdSpy.count() > 0; });
    const QString ruleId = createdSpy.at(0).at(0).toString();

    QSignalSpy updatedSpy{&presenter, &ledger::gui::RulePresenter::ruleUpdated};
    presenter.updateRule(ruleId, "Cafe", "Dining Out");
    pumpUntil([&] { return updatedSpy.count() > 0; });

    REQUIRE(updatedSpy.count() == 1);
    CHECK(updatedSpy.at(0).at(1).toInt() == 2);  // version bumped from 1 to 2
}
```

Copy `BackendRig::bridge()`'s exact return type and `pumpUntil`'s
signature from an existing presenter test, exactly as Task 18 did.

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "RulePresenter" --output-on-failure`
Expected: FAIL to compile — `rule_presenter.hpp` doesn't exist.

- [ ] **Step 3: Implement `RulePresenter`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ledger/dto/rule_dto.hpp"
#include "ledger/models/rule_model.hpp"
#include <morph/client/bridge_handler.hpp>  // confirm exact path, per Task 18
#include <QObject>
#include <memory>

namespace ledger::gui {

/// @brief Drives `ledger::RuleModel` for the rules CRUD view. No QML
///        dependency -- signals only, exactly LedgerPresenter's shape
///        (Task 18) applied to RuleModel.
class RulePresenter : public QObject {
    Q_OBJECT
  public:
    explicit RulePresenter(std::shared_ptr<morph::client::Bridge> bridge, QObject* parent = nullptr);

    void createRule(const QString& ledgerId, const QString& matchText, const QString& categoryName);
    void updateRule(const QString& ruleId, const QString& matchText, const QString& categoryName);

  signals:
    void ruleCreated(QString id);
    void ruleUpdated(QString id, int version);
    void failed(QString message);

  private:
    morph::client::BridgeHandler<RuleModel> _ruleHandler;
    std::shared_ptr<const void> _liveness = std::make_shared<int>(0);  // must stay last-declared
};

}  // namespace ledger::gui
```

Implementation (`rule_presenter.cpp`) wires each method to execute the
matching `RuleModel` action via `_ruleHandler` and emit a signal on
success / `failed(QString)` on error, mirroring
`LedgerPresenter::openAccount`'s exact `.then(...).onError(...)` shape
from Task 18. `updateRule`'s success handler reads the returned
`RuleRecord.version` (Task 12's monotonic bump) into `ruleUpdated`'s
second argument.

- [ ] **Step 4: Run test to verify it passes**

Run: `ctest --preset cl-debug -R "RulePresenter" --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Write the failing bridge test**

```cpp
// examples/ledger/tests/test_rule_qml_bridge.cpp
// SPDX-License-Identifier: Apache-2.0
#include "ledger/gui_lib/rule_qml_bridge.hpp"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("RuleQmlBridge exposes the expected Q_INVOKABLEs", "[ledger][gui]") {
    ledger::gui::RuleQmlBridge bridge{nullptr /* built with a real Bridge in the real test */};
    const auto* meta = bridge.metaObject();
    CHECK(meta->indexOfMethod("createRule(QString,QString,QString)") >= 0);
    CHECK(meta->indexOfMethod("updateRule(QString,QString,QString)") >= 0);
}
```

- [ ] **Step 6: Run test to verify it fails, then implement `RuleQmlBridge` (a `MembersView`-style CRUD list, per kanban's own completion-plan precedent, Task 15 of `docs/superpowers/plans/2026-08-18-kanban-rung4-completion.md`), then verify it passes**

Run: `ctest --preset cl-debug -R "RuleQmlBridge" --output-on-failure`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add examples/ledger/gui_lib/rule_presenter.hpp \
        examples/ledger/gui_lib/rule_presenter.cpp \
        examples/ledger/gui_lib/rule_qml_bridge.hpp \
        examples/ledger/gui_lib/rule_qml_bridge.cpp \
        examples/ledger/tests/test_rule_presenter.cpp \
        examples/ledger/tests/test_rule_qml_bridge.cpp \
        examples/ledger/CMakeLists.txt
git commit -m "ledger: add RulePresenter/Bridge for the rules CRUD view"
```

---

## Task 21: Report submit->poll GUI — `ReportJobPoller`, `ReportPresenter`, `ReportQmlBridge`

**Files:**
- Create: `examples/ledger/gui_lib/report_job_poller.hpp` / `.cpp`
- Create: `examples/ledger/gui_lib/report_presenter.hpp` / `.cpp`
- Create: `examples/ledger/gui_lib/report_qml_bridge.hpp` / `.cpp`
- Test: `examples/ledger/tests/test_report_job_poller.cpp`
- Test: `examples/ledger/tests/test_report_presenter.cpp`
- Modify: `examples/ledger/CMakeLists.txt`

**Interfaces:**
- Consumes: `ledger::LedgerModel`'s `SubmitReport`/`GetReportStatus`
  actions, `morph::client::Bridge::setExecuteDeadline` (per
  `examples/common/gui/event_poller.hpp`'s own precedent for arming a
  deadline on construction).
- Produces: `ledger::gui::ReportJobPoller` — **a genuinely new idiom, not
  a reuse of `morph::ladder::gui::EventPoller<EventT, EventIdT>`**:
  `EventPoller` is shaped for an open-ended `GetEventsSince` stream
  (apply N events per tick, keep going indefinitely), while a report job
  polls **one** job to **one** terminal state and then stops — ticking
  `GetReportStatus(jobId)` on an interval until `status != Pending`, then
  reporting the terminal result exactly once and disarming itself. Do not
  force this into `EventPoller`'s shape; write a small, distinct class.
  Constructor: `ReportJobPoller(Bridge&, ReportJobId, Dispatch, OnDone,
  OnFailed, interval = 2s, executeDeadline = 5s)` where `Dispatch` calls
  `GetReportStatus` and reports back via `OnSuccess`/`OnError` closures,
  mirroring `EventPoller`'s own `Dispatch` contract shape (spec-cited
  pattern reuse, not literal type reuse) — `OnDone(std::string
  resultJson)` fires once on `status == Done`, `OnFailed(QString message)`
  fires once on `status == Failed` or a fatal dispatch error, and the
  timer disarms itself permanently after either fires (no `resume()` —
  a finished job never restarts, unlike an event stream's resyncable
  cursor).

- [ ] **Step 1: Read `EventPoller`'s dispatch/liveness pattern**

Read `examples/common/gui/event_poller.hpp` in full — reuse its
`Dispatch` closure shape, `_liveness` weak-pointer pattern, and
`QTimer`-as-context-object idiom, but do not attempt to instantiate
`EventPoller<EventT, EventIdT>` itself for this purpose; per this task's
own Interfaces note, the two poll semantics differ enough that forcing
report-job polling through `EventPoller` produces an awkward, misleading
fit (a "cursor" that is really a terminal-state flag, an `ApplyEvent`
that fires zero-or-one times ever instead of per-event).

- [ ] **Step 2: Write the failing test**

```cpp
// examples/ledger/tests/test_report_job_poller.cpp
// SPDX-License-Identifier: Apache-2.0
#include "ledger/gui_lib/report_job_poller.hpp"

#include <QCoreApplication>
#include <QSignalSpy>
#include <catch2/catch_test_macros.hpp>

#include <vector>

TEST_CASE("ReportJobPoller ticks until Done, then disarms", "[ledger][gui][reports]") {
    int argc = 0;
    QCoreApplication app{argc, nullptr};

    std::vector<ledger::ReportStatus> scriptedStatuses{ledger::ReportStatus::Pending,
                                                        ledger::ReportStatus::Pending,
                                                        ledger::ReportStatus::Done};
    int tick = 0;
    int doneCount = 0;
    int failedCount = 0;
    QString capturedResult;

    // A stub Dispatch: no real Bridge, just a closure playing back the
    // scripted status sequence one entry per call, exactly the shape
    // EventPoller's own test file uses for its Dispatch stubs.
    auto dispatch = [&](ledger::ReportJobId /*jobId*/, ledger::gui::ReportJobPoller::OnStatusSuccess onSuccess,
                         ledger::gui::ReportJobPoller::OnStatusError /*onError*/) {
        auto status = scriptedStatuses[static_cast<size_t>(tick)];
        ++tick;
        onSuccess(status, status == ledger::ReportStatus::Done ? std::optional<std::string>{"{\"total\":100}"}
                                                                 : std::nullopt);
    };

    ledger::gui::ReportJobPoller poller{
        ledger::ReportJobId{1}, dispatch,
        [&](std::string resultJson) {
            ++doneCount;
            capturedResult = QString::fromStdString(resultJson);
        },
        [&](QString) { ++failedCount; }, std::chrono::milliseconds{0} /* tick manually via pollOnce() */};

    poller.pollOnce();
    poller.pollOnce();
    poller.pollOnce();

    CHECK(doneCount == 1);
    CHECK(failedCount == 0);
    CHECK(capturedResult == "{\"total\":100}");
    CHECK_FALSE(poller.running());

    // A fourth manual tick must be a no-op -- the poller already disarmed.
    poller.pollOnce();
    CHECK(doneCount == 1);
}

TEST_CASE("ReportJobPoller reports OnFailed exactly once on a Failed status", "[ledger][gui][reports]") {
    int argc = 0;
    QCoreApplication app{argc, nullptr};

    int doneCount = 0;
    int failedCount = 0;
    QString capturedMessage;

    auto dispatch = [&](ledger::ReportJobId /*jobId*/, ledger::gui::ReportJobPoller::OnStatusSuccess onSuccess,
                         ledger::gui::ReportJobPoller::OnStatusError /*onError*/) {
        onSuccess(ledger::ReportStatus::Failed, std::nullopt);
    };

    ledger::gui::ReportJobPoller poller{
        ledger::ReportJobId{1}, dispatch, [&](std::string) { ++doneCount; },
        [&](QString message) {
            ++failedCount;
            capturedMessage = message;
        },
        std::chrono::milliseconds{0}};

    poller.pollOnce();

    CHECK(doneCount == 0);
    CHECK(failedCount == 1);
    CHECK_FALSE(capturedMessage.isEmpty());
    CHECK_FALSE(poller.running());
}
```

- [ ] **Step 3: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "ReportJobPoller" --output-on-failure`
Expected: FAIL to compile — `report_job_poller.hpp` doesn't exist.

- [ ] **Step 4: Implement `ReportJobPoller`**

```cpp
// examples/ledger/gui_lib/report_job_poller.hpp
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ledger/core/types.hpp"

#include <QObject>
#include <QString>
#include <QTimer>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace ledger::gui {

/// @brief Polls one report job to one terminal state, then stops --
///        deliberately distinct from `morph::ladder::gui::EventPoller`
///        (design spec §9, this task's own Interfaces note): a report job
///        has no ongoing stream to apply, only a Pending/Done/Failed
///        status to reach once.
class ReportJobPoller {
  public:
    using OnStatusSuccess = std::function<void(ReportStatus, std::optional<std::string> resultJson)>;
    using OnStatusError = std::function<void(std::exception_ptr)>;
    using Dispatch = std::function<void(ReportJobId, OnStatusSuccess, OnStatusError)>;
    using OnDone = std::function<void(std::string resultJson)>;
    using OnFailed = std::function<void(QString message)>;

    static constexpr std::chrono::milliseconds kDefaultInterval{2000};

    ReportJobPoller(ReportJobId jobId, Dispatch dispatch, OnDone onDone, OnFailed onFailed,
                     std::chrono::milliseconds interval = kDefaultInterval)
        : _jobId{jobId}, _dispatch{std::move(dispatch)}, _onDone{std::move(onDone)}, _onFailed{std::move(onFailed)} {
        QObject::connect(&_timer, &QTimer::timeout, &_timer, [this] { pollOnce(); });
        if (interval.count() > 0) {
            _timer.start(interval);
        }
    }

    /// @brief Runs one tick now. A no-op once a terminal state has already
    ///        been reported. Public so a test can drive it deterministically
    ///        instead of waiting on the real timer.
    void pollOnce() {
        if (_terminal) {
            return;
        }
        _dispatch(
            _jobId,
            [this, alive = std::weak_ptr<const void>{_liveness}](ReportStatus status,
                                                                  std::optional<std::string> resultJson) {
                if (alive.expired() || _terminal) {
                    return;
                }
                if (status == ReportStatus::Pending) {
                    return;  // keep ticking
                }
                _terminal = true;
                _timer.stop();
                if (status == ReportStatus::Done) {
                    _onDone(resultJson.value_or(std::string{}));
                } else {
                    _onFailed("report job failed");
                }
            },
            [this, alive = std::weak_ptr<const void>{_liveness}](std::exception_ptr) {
                if (alive.expired() || _terminal) {
                    return;
                }
                _terminal = true;
                _timer.stop();
                _onFailed("report job status check failed");
            });
    }

    [[nodiscard]] bool running() const noexcept { return _timer.isActive(); }

  private:
    ReportJobId _jobId;
    Dispatch _dispatch;
    OnDone _onDone;
    OnFailed _onFailed;
    QTimer _timer;
    bool _terminal = false;
    // Last-declared, per EventPoller's own documented reasoning
    // (examples/common/gui/event_poller.hpp) -- destroyed first, so a
    // completion callback racing this object's destruction sees `expired()`
    // before any other member is torn down.
    std::shared_ptr<const void> _liveness{std::make_shared<char>()};
};

}  // namespace ledger::gui
```

(`interval = 0ms` disables the automatic timer for the test above, which
drives `pollOnce()` manually instead — mirroring how `EventPoller`'s own
test file drives ticks deterministically rather than waiting on real
wall-clock time.)

- [ ] **Step 5: Run tests to verify they pass**

Run: `ctest --preset cl-debug -R "ReportJobPoller" --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Write the failing `ReportPresenter` test**

```cpp
// examples/ledger/tests/test_report_presenter.cpp
// SPDX-License-Identifier: Apache-2.0
#include "ledger/gui_lib/report_presenter.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/pump.hpp"

#include <QCoreApplication>
#include <QSignalSpy>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("ReportPresenter submits a report and emits reportReady once the job completes", "[ledger][gui][reports]") {
    int argc = 0;
    QCoreApplication app{argc, nullptr};
    BackendRig rig{Mode::Local};
    ledger::gui::ReportPresenter presenter{rig.bridge()};

    QSignalSpy readySpy{&presenter, &ledger::gui::ReportPresenter::reportReady};
    QSignalSpy failedSpy{&presenter, &ledger::gui::ReportPresenter::failed};

    presenter.submitReport("1", "MonthlyStatement", "{}");
    pumpUntil([&] { return readySpy.count() > 0 || failedSpy.count() > 0; }, std::chrono::seconds{10});

    REQUIRE(failedSpy.isEmpty());
    REQUIRE(readySpy.count() == 1);
}
```

- [ ] **Step 7: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "ReportPresenter" --output-on-failure`
Expected: FAIL to compile.

- [ ] **Step 8: Implement `ReportPresenter` and `ReportQmlBridge`**

`ReportPresenter::submitReport` calls `SubmitReport` via a
`BridgeHandler<LedgerModel>`; on success, constructs a `ReportJobPoller`
(building its `Dispatch` closure over the same `BridgeHandler`'s
`GetReportStatus` call, per `examples/common/gui/event_poller.hpp`'s own
"production-safe wiring" pattern — a dedicated completion per tick, never
a shared error signal), forwarding the poller's `OnDone`/`OnFailed` into
`ReportPresenter`'s own `reportReady(QVariantMap)`/`failed(QString)`
signals. `ReportQmlBridge` exposes `Q_INVOKABLE submitReport(QString
ledgerId, QString kind, QVariantMap params)` and a `Q_PROPERTY report` as
`QVariantMap` backed by `reportReady`.

- [ ] **Step 9: Run tests to verify they pass**

Run: `ctest --preset cl-debug -R "ReportPresenter" --output-on-failure`
Expected: PASS.

Run: `ctest --preset cl-debug -R "ReportPresenter" --output-on-failure`
Expected: PASS.

- [ ] **Step 10: Commit**

```bash
git add examples/ledger/gui_lib/report_job_poller.hpp \
        examples/ledger/gui_lib/report_job_poller.cpp \
        examples/ledger/gui_lib/report_presenter.hpp \
        examples/ledger/gui_lib/report_presenter.cpp \
        examples/ledger/gui_lib/report_qml_bridge.hpp \
        examples/ledger/gui_lib/report_qml_bridge.cpp \
        examples/ledger/tests/test_report_job_poller.cpp \
        examples/ledger/tests/test_report_presenter.cpp \
        examples/ledger/CMakeLists.txt
git commit -m "ledger: ReportJobPoller + Report presenter/bridge -- the submit->poll GUI idiom"
```

---

## Task 22: QML views + `gui/main.cpp`

**Files:**
- Create: `examples/ledger/gui/main.cpp`
- Create: `examples/ledger/gui/qml/Main.qml`
- Create: `examples/ledger/gui/qml/LedgerView.qml`
- Create: `examples/ledger/gui/qml/BudgetView.qml`
- Create: `examples/ledger/gui/qml/RulesView.qml`
- Create: `examples/ledger/gui/qml/ReportView.qml`
- Modify: `examples/ledger/CMakeLists.txt`

**Interfaces:**
- Consumes: `examples/common/gui/AppContext` (backend-parameterized
  deployment mode, per `TESTING.md`'s presenter architecture §2),
  `LedgerQmlBridge`/`BudgetQmlBridge`/`RuleQmlBridge`/`ReportQmlBridge`
  from Tasks 18–21.
- Produces: a buildable desktop client wiring all four bridges into QML,
  built from inside `AppContext::onReady` per `TESTING.md`'s binding
  convention; one offscreen engine-load smoke test registered in ctest.

- [ ] **Step 1: Read kanban's `gui/main.cpp` + `Main.qml`**

Read `examples/kanban/gui/main.cpp` and `examples/kanban/gui/qml/Main.qml`
(on `ladder-kanban-impl` if not present in this checkout) or
`examples/bookmarks/gui/main.cpp` as the closest available precedent for
constructing bridges inside `AppContext::onReady` and wiring them as QML
context properties.

- [ ] **Step 2: Write the failing offscreen engine-load smoke test**

```cpp
// examples/ledger/tests/test_ledger_qml_smoke.cpp
// SPDX-License-Identifier: Apache-2.0
#include <QCoreApplication>
#include <QQmlApplicationEngine>
#include <QUrl>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Main.qml loads without errors under an offscreen QQmlApplicationEngine", "[ledger][gui][smoke]") {
    int argc = 0;
    QCoreApplication app{argc, nullptr};

    QQmlApplicationEngine engine;
    bool hadError = false;
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, &engine,
                      [&](const QList<QQmlError>&) { hadError = true; });
    engine.load(QUrl{QStringLiteral("qrc:/ledger/gui/qml/Main.qml")});  // confirm exact qrc alias against CMakeLists.txt

    REQUIRE_FALSE(engine.rootObjects().isEmpty());
    CHECK_FALSE(hadError);
}
```

Confirm the exact `qrc:/` alias path against how `qt_add_qml_module` (or
this rung's `morph_add_rung()`-driven QML registration) names its module
in `examples/ledger/CMakeLists.txt` — copy the working alias from an
existing rung's own QML smoke test (grep `QQmlApplicationEngine` across
`examples/*/tests/`) rather than guessing.

- [ ] **Step 3: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "smoke" --output-on-failure`
Expected: FAIL — `Main.qml` doesn't exist.

- [ ] **Step 4: Write `main.cpp` and the four QML views**

```cpp
// examples/ledger/gui/main.cpp
// SPDX-License-Identifier: Apache-2.0
#include "ledger/gui_lib/budget_qml_bridge.hpp"
#include "ledger/gui_lib/ledger_qml_bridge.hpp"
#include "ledger/gui_lib/report_qml_bridge.hpp"
#include "ledger/gui_lib/rule_qml_bridge.hpp"

#include "gui/app_context.hpp"  // examples/common/gui/AppContext -- confirm exact include path

#include <QGuiApplication>
#include <QQmlApplicationEngine>

int main(int argc, char** argv) {
    QGuiApplication app{argc, argv};
    morph::ladder::gui::AppContext context{morph::ladder::gui::AppContext::Mode::Local{4}};
    QQmlApplicationEngine engine;

    context.onReady([&] {
        auto* ledgerBridge = new ledger::gui::LedgerQmlBridge{context.bridge(), &app};
        auto* budgetBridge = new ledger::gui::BudgetQmlBridge{context.bridge(), &app};
        auto* ruleBridge = new ledger::gui::RuleQmlBridge{context.bridge(), &app};
        auto* reportBridge = new ledger::gui::ReportQmlBridge{context.bridge(), &app};
        engine.rootContext()->setContextProperty("ledgerBridge", ledgerBridge);
        engine.rootContext()->setContextProperty("budgetBridge", budgetBridge);
        engine.rootContext()->setContextProperty("ruleBridge", ruleBridge);
        engine.rootContext()->setContextProperty("reportBridge", reportBridge);
        engine.load(QUrl{QStringLiteral("qrc:/ledger/gui/qml/Main.qml")});
    });

    return QGuiApplication::exec();
}
```

(Confirm `AppContext`'s exact constructor/`onReady`/`bridge()` signatures
against `examples/common/gui/app_context.hpp` before finalizing — this is
this plan's best-grounded guess from `TESTING.md`'s own description of
`AppContext`'s shape, Section "Presenter architecture", point 2.)

`Main.qml` is a `TabBar`/`StackLayout`-style shell switching between the
four views, each bindings-only per `IMPLEMENTATION.md` rule 2 (default Qt
Quick controls, no styling, no hand-built tables — route lists through
`morph::forms` views wherever the interaction fits that palette).
`LedgerView.qml` binds to `ledgerBridge`'s `accounts`/`ledgerState`
properties and calls its `Q_INVOKABLE`s (`openAccount`,
`storeTransaction`, `undoTransaction`, `importChunk`) from button
handlers; `BudgetView.qml`, `RulesView.qml`, `ReportView.qml` bind to
their respective bridges the same way, per each bridge's own
`Q_PROPERTY`/`Q_INVOKABLE` surface from Tasks 18–21.

- [ ] **Step 5: Run test to verify it passes**

Run: `ctest --preset cl-debug -R "smoke" --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Wire into CMakeLists.txt and commit**

```bash
git add examples/ledger/gui/main.cpp \
        examples/ledger/gui/qml/Main.qml \
        examples/ledger/gui/qml/LedgerView.qml \
        examples/ledger/gui/qml/BudgetView.qml \
        examples/ledger/gui/qml/RulesView.qml \
        examples/ledger/gui/qml/ReportView.qml \
        examples/ledger/CMakeLists.txt
git commit -m "ledger: QML views + gui/main.cpp, offscreen smoke test"
```

---

## Task 23: Multi-client stress test

**Files:**
- Create: `examples/ledger/tests/test_multiclient.cpp`

**Interfaces:**
- Consumes: `examples/common/testkit/action_driver.hpp` (`SeededScript`),
  `client_pool.hpp`, `convergence.hpp` — all predate this rung per
  `TESTING.md`'s ownership table; on this branch they arrived via Task 0's
  cherry-picks from `ladder-kanban-impl`, already applied and verified
  (build + own tests pass).

- [ ] **Step 1: Write the failing stress test**

```cpp
// examples/ledger/tests/test_multiclient.cpp
// SPDX-License-Identifier: Apache-2.0
#include "ledger/models/ledger_model.hpp"
#include "testkit/action_driver.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/client_pool.hpp"
#include "testkit/convergence.hpp"
#include "testkit/db_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>

TEST_CASE("N clients storing concurrent transactions converge, legs always sum zero", "[ledger][stress]") {
    morph::ladder::testkit::DbFixture fixture;
    BackendRig rig{Mode::Local};

    const int nClients = std::getenv("MORPH_LADDER_CLIENTS") ? std::atoi(std::getenv("MORPH_LADDER_CLIENTS")) : 4;
    const int nActions = std::getenv("MORPH_LADDER_ACTIONS") ? std::atoi(std::getenv("MORPH_LADDER_ACTIONS")) : 50;
    const auto seed = SeededScript::seedFromEnv();  // MORPH_STRESS_SEED, printed on failure

    ClientPool<ledger::LedgerModel> clients{rig, nClients};

    // Open two accounts up front so every generated StoreTransaction has
    // somewhere to post legs.
    clients[0].execute(ledger::OpenAccount{.ledgerId = ledger::LedgerId{1}, .name = "Checking",
                                            .kind = ledger::AccountKind::Asset, .currency = ledger::Currency::USD});
    clients[0].execute(ledger::OpenAccount{.ledgerId = ledger::LedgerId{1}, .name = "Groceries",
                                            .kind = ledger::AccountKind::Expense, .currency = ledger::Currency::USD});

    SeededScript script{seed, nActions};
    script.addGenerator(1.0, [](auto& rng) {
        // Generates a balanced two-leg StoreTransaction between the two
        // fixed accounts above, amount varying by rng, always summing to
        // exactly zero by construction (the generator, not the model,
        // guarantees balance here -- the model's own invariant is what
        // this test is actually probing for correctness under
        // concurrency, not generating known-bad input).
        return ledger::StoreTransaction{/* ... populated from rng ... */};
    });

    for (int i = 0; i < nClients; ++i) {
        script.runOn(clients[i]);
    }
    script.joinAll();

    // Per-burst invariant hook: every account's committed legs, summed,
    // equal zero per currency -- the same assertion StoreTransaction
    // itself makes per-call, now checked against the database's final
    // state after concurrent execution.
    for (const auto& currencyTotal : /* query all legs, sum by currency */ std::vector<int>{}) {
        (void)currencyTotal;  // replace with the real per-currency Rational sum assertion
    }

    requireConverged(clients, std::chrono::seconds{10});
}
```

Confirm `SeededScript`/`ClientPool`/`requireConverged`'s exact
constructor and method signatures against `examples/common/testkit/
action_driver.hpp`, `client_pool.hpp`, `convergence.hpp` before
finalizing — this is this plan's best-grounded guess from `TESTING.md`'s
own description of each component's role (Section "Multi-client stress
harness"); adjust call shapes to match the real headers, keeping the
test's actual assertions (zero-sum holds after concurrent execution,
clients converge) intact.

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --preset cl-debug -R "multiclient" --output-on-failure`
Expected: FAIL to compile.

- [ ] **Step 3: Fix the test body against the real testkit signatures, then run again**

Adjust the placeholder call shapes above to match
`action_driver.hpp`/`client_pool.hpp`/`convergence.hpp`'s actual
signatures (read those three headers in full first), and replace the
per-currency sum placeholder with a real `Query<TransactionLegRecord>` +
`Rational` summation assertion, mirroring `StoreTransaction`'s own
zero-sum check (Task 8) but run against the database's final state.

- [ ] **Step 4: Run test to verify it passes**

Run: `ctest --preset cl-debug -R "multiclient" --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add examples/ledger/tests/test_multiclient.cpp examples/ledger/CMakeLists.txt
git commit -m "ledger: multi-client concurrent-transaction stress test"
```

---

## Task 24: Sync-philosophy benchmark write-up (design spec §10)

**Files:**
- Create: `examples/ledger/SYNC-BENCHMARK.md`
- Modify: `examples/ledger/tests/test_ledger_offline.cpp` (Scenario A/B tests)

**Interfaces:**
- Produces: `examples/ledger/SYNC-BENCHMARK.md` containing the four
  numbered items from design spec §10 (Scenario A, Scenario B, the
  clock-skew test, and the explicit "server arrival order, full stop"
  statement), plus two new tests reproducing Scenarios A and B.

- [ ] **Step 1: Write the failing test for Scenario A**

```cpp
// Append to examples/ledger/tests/test_ledger_offline.cpp
TEST_CASE("Scenario A: two offline clients edit different fields of the same transaction", "[ledger][sync-benchmark]") {
    // Two LedgerModel clients over OfflineRig, both go offline, client 1
    // edits description, client 2 edits a leg's category-linked field,
    // both reconnect. Assert: whichever action reaches the server first
    // (server arrival order) applies in full; the second either applies
    // cleanly (non-overlapping fields) or fails validation against
    // changed state (overlapping fields), surfaced via onBackendChanged.
}
```

- [ ] **Step 2: Run test to verify it fails, implement, verify it passes**

Run: `ctest --preset cl-debug -R "Scenario.A" --output-on-failure`
Expected: PASS.

- [ ] **Step 3: Write, implement, and pass the Scenario B test**

```cpp
TEST_CASE("Scenario B: stale base-version edit is rejected outright, never merged", "[ledger][sync-benchmark]") {
    // Two clients fetch the same journal; one commits a change (bumping
    // an implicit base version); the second's queued edit with a stale
    // base is rejected outright. Assert the typed rejection, not a
    // silent overwrite or merge.
}
```

Run: `ctest --preset cl-debug -R "Scenario.B" --output-on-failure`
Expected: PASS.

- [ ] **Step 4: Write the clock-skew test**

```cpp
TEST_CASE("Clock-skew: audit view orders by journal order, labels claimed timestamps as non-authoritative", "[ledger][sync-benchmark]") {
    // Two clients with injected TokenVerifier-clock skew of +-5 minutes
    // both write to one ledger; assert the activity/audit view's ordering
    // uses journal (server arrival) order, never the claimed timestamp.
}
```

Run: `ctest --preset cl-debug -R "clock.skew" --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Write `SYNC-BENCHMARK.md`**

Transcribe design spec §10's four numbered points into
`examples/ledger/SYNC-BENCHMARK.md` as the rung's written deliverable,
citing the three tests above by file/name as the reproduced evidence for
Scenarios A and B and the clock-skew claim.

- [ ] **Step 6: Commit**

```bash
git add examples/ledger/SYNC-BENCHMARK.md examples/ledger/tests/test_ledger_offline.cpp
git commit -m "ledger: sync-philosophy benchmark write-up + Scenario A/B/clock-skew tests (design spec §10)"
```

---

## Task 25: Coverage gate, README/spec reconciliation, full test suite

**Files:**
- Modify: `codecov.yml`
- Modify: `examples/ledger/README.md` (mark implemented build-order steps)
- Modify: `docs/superpowers/specs/2026-08-19-ledger-rung5-design.md` (only
  if implementation surfaced a genuine deviation — the spec is
  authoritative, so a mismatch found here is usually a plan/code bug to
  fix, not a spec update; update the spec only for a deliberate,
  discovered-during-build design change)

**Interfaces:**
- Produces: a green `codecov.yml` component for
  `examples/ledger/src/models/` + `include/ledger/models/`, scoped and
  targeted per `IMPLEMENTATION.md` rule 5's measured-ceiling guidance; a
  fully passing `ctest -L ladder-ledger`.

- [ ] **Step 1: Run the full ledger test suite**

```bash
ctest --preset cl-debug -L ladger-ledger --output-on-failure
```

Expected: all green.

- [ ] **Step 2: Measure model coverage and wire the `codecov.yml` component**

Follow `TESTING.md`'s "Coverage wiring" section exactly: build the
`clang-coverage` CI leg locally if possible (or run `scripts/coverage.sh`
against a coverage-instrumented build), compute the real ceiling via
`llvm-cov export`'s JSON (`covered / total`, not the rounded percentage),
and add a `component_management.individual_components` entry to
`codecov.yml` scoped to `examples/ledger/src/models/` +
`include/ledger/models/`, `informational: false`, `target:` set a small
margin below the measured ceiling with every known-artifact line
documented in a comment.

- [ ] **Step 3: Update `examples/ledger/README.md`'s build-order list**

Mark steps 1–7 as implemented (per this plan's scope), leaving step 8
(the sync-philosophy benchmark) noted as delivered via
`SYNC-BENCHMARK.md` + Task 24's tests, matching kanban's own README
update pattern once its rung completed.

- [ ] **Step 4: Run the full test suite one final time**

```bash
ctest --preset cl-debug -L ladder-ledger --output-on-failure
```

Expected: all green, coverage gate passing.

- [ ] **Step 5: Commit**

```bash
git add codecov.yml examples/ledger/README.md
git commit -m "ledger: coverage gate wired, README reconciled with implemented scope"
```

---

## Task 26: Rebase onto master once PR #121 merges (deferred, tracked here for visibility)

This task is **not executed as part of this plan's initial pass** — it is
recorded here so the eventual rebase is not forgotten. Once PR #121
(kanban, rung 4) merges into `master`:

- [ ] Rebase `ladder-ledger-rung5` onto `master`.
- [ ] Confirm all four of Task 0's cherry-picked commits
      (`causalParentId`/`isReplaying()`, `action_driver.hpp`,
      `offline_rig.hpp`, `client_pool.hpp`+`convergence.hpp`) become
      no-ops via patch-id match, per design spec §5.
- [ ] Resolve the `codecov.yml` merge conflict (two independently-added
      components, mechanical per design spec §12).
- [ ] Re-run the full ledger test suite once more post-rebase.
- [ ] Open the PR for this branch (`ledger: rung 5 -- ledger`, mirroring
      PR #121's title convention), citing the design spec and this plan
      in its description, per kanban's own PR body shape.
